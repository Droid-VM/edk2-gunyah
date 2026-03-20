/** @file

  Gunyah IOMMU DXE driver for protected VM DMA isolation.

  In Gunyah protected VM mode, guest memory regions are donated to the
  hypervisor and become inaccessible to the host VMM.  Only the
  "restricted-dma-pool" region (StaticSwiotlbRegion) remains shared.
  This driver implements EDKII_IOMMU_PROTOCOL to ensure all PCI DMA
  buffers are allocated from that shared region, preventing SIGBUS
  faults when the host accesses virtio queue memory.

  For BusMasterRead/Write operations, bounce buffers are allocated from
  the DMA pool and data is copied as needed.  For CommonBuffer operations,
  the buffer (previously allocated via AllocateBuffer from the pool) is
  identity-mapped.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/IoMmu.h>

#include "DmaPool.h"

//
// Tracking structure for each Map() operation
//
#define MAP_INFO_SIG  SIGNATURE_32 ('G', 'I', 'M', 'P')

typedef struct {
  UINT32                   Signature;
  LIST_ENTRY               Link;
  EDKII_IOMMU_OPERATION    Operation;
  UINTN                    NumberOfBytes;
  UINTN                    NumberOfPages;
  EFI_PHYSICAL_ADDRESS     HostAddress;       // caller's original buffer
  EFI_PHYSICAL_ADDRESS     BounceAddress;     // bounce buffer in DMA pool
  BOOLEAN                  BounceUsed;        // TRUE if bounce buffer allocated
} MAP_INFO;

#define MAP_INFO_FROM_LINK(a) \
  CR (a, MAP_INFO, Link, MAP_INFO_SIG)

STATIC LIST_ENTRY  mMapInfoList = INITIALIZE_LIST_HEAD_VARIABLE (mMapInfoList);

/**
  Determine whether the operation is a CommonBuffer type.
**/
STATIC
BOOLEAN
IsCommonBufferOperation (
  IN EDKII_IOMMU_OPERATION  Operation
  )
{
  return (Operation == EdkiiIoMmuOperationBusMasterCommonBuffer) ||
         (Operation == EdkiiIoMmuOperationBusMasterCommonBuffer64);
}

/**
  Determine whether the operation involves device reading from memory
  (i.e., host wrote data, device reads it).
**/
STATIC
BOOLEAN
IsBusMasterReadOperation (
  IN EDKII_IOMMU_OPERATION  Operation
  )
{
  return (Operation == EdkiiIoMmuOperationBusMasterRead) ||
         (Operation == EdkiiIoMmuOperationBusMasterRead64);
}

/**
  Set IOMMU attribute for a system memory.
  No hardware IOMMU — this is a no-op.
**/
STATIC
EFI_STATUS
EFIAPI
GunyahIoMmuSetAttribute (
  IN EDKII_IOMMU_PROTOCOL  *This,
  IN EFI_HANDLE            DeviceHandle,
  IN VOID                  *Mapping,
  IN UINT64                IoMmuAccess
  )
{
  return EFI_SUCCESS;
}

/**
  Map a system buffer for DMA access.

  - CommonBuffer: identity-map (buffer already in pool from AllocateBuffer).
  - BusMasterRead: allocate bounce buffer in pool, copy data from host.
  - BusMasterWrite: allocate bounce buffer in pool (device will write).
**/
STATIC
EFI_STATUS
EFIAPI
GunyahIoMmuMap (
  IN     EDKII_IOMMU_PROTOCOL   *This,
  IN     EDKII_IOMMU_OPERATION  Operation,
  IN     VOID                   *HostAddress,
  IN OUT UINTN                  *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS   *DeviceAddress,
  OUT    VOID                   **Mapping
  )
{
  EFI_STATUS            Status;
  MAP_INFO              *MapInfo;
  EFI_PHYSICAL_ADDRESS  Bounce;

  if ((HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (DeviceAddress == NULL) || (Mapping == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  if ((UINT32)Operation >= EdkiiIoMmuOperationMaximum) {
    return EFI_INVALID_PARAMETER;
  }

  MapInfo = AllocateZeroPool (sizeof (*MapInfo));
  if (MapInfo == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  MapInfo->Signature     = MAP_INFO_SIG;
  MapInfo->Operation     = Operation;
  MapInfo->NumberOfBytes  = *NumberOfBytes;
  MapInfo->HostAddress   = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  MapInfo->BounceUsed    = FALSE;

  if (IsCommonBufferOperation (Operation)) {
    //
    // CommonBuffer: the buffer was allocated via AllocateBuffer() and is
    // already in the DMA pool.  Identity-map it.
    //
    *DeviceAddress = MapInfo->HostAddress;
  } else {
    //
    // BusMasterRead or BusMasterWrite: allocate a bounce buffer from the
    // DMA pool and copy data if the device needs to read.
    //
    MapInfo->NumberOfPages = EFI_SIZE_TO_PAGES (*NumberOfBytes);
    if (EFI_PAGES_TO_SIZE (MapInfo->NumberOfPages) < *NumberOfBytes) {
      MapInfo->NumberOfPages++;
    }

    Status = DmaPoolAllocatePages (MapInfo->NumberOfPages, &Bounce);
    if (EFI_ERROR (Status)) {
      FreePool (MapInfo);
      return EFI_OUT_OF_RESOURCES;
    }

    MapInfo->BounceAddress = Bounce;
    MapInfo->BounceUsed    = TRUE;

    //
    // If the device will read from the buffer, copy host data into the
    // bounce buffer now.
    //
    if (IsBusMasterReadOperation (Operation)) {
      CopyMem ((VOID *)(UINTN)Bounce, HostAddress, *NumberOfBytes);
    }

    *DeviceAddress = Bounce;
  }

  InsertTailList (&mMapInfoList, &MapInfo->Link);
  *Mapping = MapInfo;

  return EFI_SUCCESS;
}

/**
  Unmap a previously mapped buffer.

  - CommonBuffer: no action.
  - BusMasterWrite: copy data from bounce buffer back to host buffer.
  - Free the bounce buffer.
**/
STATIC
EFI_STATUS
EFIAPI
GunyahIoMmuUnmap (
  IN EDKII_IOMMU_PROTOCOL  *This,
  IN VOID                  *Mapping
  )
{
  MAP_INFO  *MapInfo;

  if (Mapping == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  MapInfo = (MAP_INFO *)Mapping;
  if (MapInfo->Signature != MAP_INFO_SIG) {
    return EFI_INVALID_PARAMETER;
  }

  if (MapInfo->BounceUsed) {
    //
    // BusMasterWrite: device wrote data into bounce buffer.
    // Copy it back to the caller's original buffer.
    //
    if (!IsBusMasterReadOperation (MapInfo->Operation)) {
      CopyMem (
        (VOID *)(UINTN)MapInfo->HostAddress,
        (VOID *)(UINTN)MapInfo->BounceAddress,
        MapInfo->NumberOfBytes
        );
    }

    DmaPoolFreePages (MapInfo->NumberOfPages, MapInfo->BounceAddress);
  }

  RemoveEntryList (&MapInfo->Link);
  FreePool (MapInfo);

  return EFI_SUCCESS;
}

/**
  Allocate a buffer suitable for CommonBuffer DMA from the restricted
  DMA pool.
**/
STATIC
EFI_STATUS
EFIAPI
GunyahIoMmuAllocateBuffer (
  IN     EDKII_IOMMU_PROTOCOL  *This,
  IN     EFI_ALLOCATE_TYPE     Type,
  IN     EFI_MEMORY_TYPE       MemoryType,
  IN     UINTN                 Pages,
  IN OUT VOID                  **HostAddress,
  IN     UINT64                Attributes
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Address;

  if (HostAddress == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Attributes & EDKII_IOMMU_ATTRIBUTE_INVALID_FOR_ALLOCATE_BUFFER) != 0) {
    return EFI_UNSUPPORTED;
  }

  Status = DmaPoolAllocatePages (Pages, &Address);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem ((VOID *)(UINTN)Address, EFI_PAGES_TO_SIZE (Pages));
  *HostAddress = (VOID *)(UINTN)Address;

  return EFI_SUCCESS;
}

/**
  Free a buffer previously allocated with AllocateBuffer().
**/
STATIC
EFI_STATUS
EFIAPI
GunyahIoMmuFreeBuffer (
  IN EDKII_IOMMU_PROTOCOL  *This,
  IN UINTN                 Pages,
  IN VOID                  *HostAddress
  )
{
  if (HostAddress == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  DmaPoolFreePages (Pages, (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress);
  return EFI_SUCCESS;
}

//
// IOMMU protocol instance
//
STATIC EDKII_IOMMU_PROTOCOL  mGunyahIoMmu = {
  EDKII_IOMMU_PROTOCOL_REVISION,
  GunyahIoMmuSetAttribute,
  GunyahIoMmuMap,
  GunyahIoMmuUnmap,
  GunyahIoMmuAllocateBuffer,
  GunyahIoMmuFreeBuffer,
};

/**
  Entry point.  Discover the restricted-dma-pool from FDT and, if present,
  install the IOMMU protocol so PciHostBridgeDxe routes all DMA through us.
**/
EFI_STATUS
EFIAPI
GunyahIoMmuDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  Status = DmaPoolInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO,
            "%a: No restricted-dma-pool found, IOMMU not needed (%r)\n",
            __func__, Status));
    //
    // Not a protected VM — no IOMMU needed.
    // Return success so the driver loads silently.
    //
    return EFI_SUCCESS;
  }

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEdkiiIoMmuProtocolGuid,
                  &mGunyahIoMmu,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to install IOMMU protocol: %r\n",
            __func__, Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "%a: Gunyah IOMMU protocol installed\n", __func__));
  return EFI_SUCCESS;
}
