/** @file
  Gunyah SSDT restricted DMA pool updater.

  Builds an SSDT with \_SB_.RDPA and \_SB_.RDPS based on
  device tree compatible="restricted-dma-pool" reg.

  Copyright (c) 2026, Kancy Joe.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Library/AmlLib/AmlLib.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/FdtClient.h>

#define GUNYAH_RESTRICTED_DMA_POOL_COMPAT  "restricted-dma-pool"
#define SSDT_OEM_ID                        "ARMLTD"
#define SSDT_OEM_TABLE_ID                  "DMAPOOL"
#define SSDT_OEM_REVISION                  1

STATIC
EFI_STATUS
GetRestrictedDmaPool (
  OUT UINT64 *BaseAddress,
  OUT UINT64 *Length
  )
{
  EFI_STATUS          Status;
  FDT_CLIENT_PROTOCOL *FdtClient;
  INT32               Node;
  CONST VOID         *Prop;
  UINT32              Len;
  UINT64              Address;
  UINT64              Size;

  if ((BaseAddress == NULL) || (Length == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->LocateProtocol (&gFdtClientProtocolGuid, NULL, (VOID **)&FdtClient);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahRestrictedDmaPool: FdtClient not available (%r)\n", Status));
    return Status;
  }

  Status = FdtClient->FindCompatibleNode (FdtClient, GUNYAH_RESTRICTED_DMA_POOL_COMPAT, &Node);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_INFO, "GunyahRestrictedDmaPool: no compatible node\n"));
    return Status;
  }

  Status = FdtClient->GetNodeProperty (FdtClient, Node, "reg", &Prop, &Len);
  if (EFI_ERROR (Status) || (Len < sizeof (UINT64) * 2)) {
    DEBUG ((DEBUG_ERROR, "GunyahRestrictedDmaPool: invalid reg property %r\n", Status));
    return EFI_NOT_FOUND;
  }

  if (Len < sizeof (UINT64) * 2) {
    return EFI_NOT_FOUND;
  }

  Address = SwapBytes64 (((CONST UINT64 *)Prop)[0]);
  Size    = SwapBytes64 (((CONST UINT64 *)Prop)[1]);

  if ((Address == 0) || (Size == 0)) {
    return EFI_NOT_FOUND;
  }

  *BaseAddress = Address;
  *Length      = Size;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
InstallRestrictedDmaPoolSsdt (VOID)
{
  EFI_STATUS                     Status;
  UINT64                         BaseAddress;
  UINT64                         Length;
  AML_ROOT_NODE_HANDLE           RootNode;
  AML_OBJECT_NODE_HANDLE         ScopeNode;
  EFI_ACPI_DESCRIPTION_HEADER   *Table;
  EFI_ACPI_TABLE_PROTOCOL       *AcpiTableProtocol;
  UINTN                          TableKey;

  Status = GetRestrictedDmaPool (&BaseAddress, &Length);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = AmlCodeGenDefinitionBlock (
             "SSDT",
             SSDT_OEM_ID,
             SSDT_OEM_TABLE_ID,
             SSDT_OEM_REVISION,
             &RootNode
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahRestrictedDmaPool: AmlCodeGenDefinitionBlock failed %r\n", Status));
    return Status;
  }

  Status = AmlCodeGenScope ("_SB_", RootNode, &ScopeNode);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahRestrictedDmaPool: AmlCodeGenScope failed %r\n", Status));
    AmlDeleteTree (RootNode);
    return Status;
  }

  Status = AmlCodeGenNameInteger ("RDPA", BaseAddress, ScopeNode, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahRestrictedDmaPool: AmlCodeGenNameInteger RDPA failed %r\n", Status));
    AmlDeleteTree (RootNode);
    return Status;
  }

  Status = AmlCodeGenNameInteger ("RDPS", Length, ScopeNode, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahRestrictedDmaPool: AmlCodeGenNameInteger RDPS failed %r\n", Status));
    AmlDeleteTree (RootNode);
    return Status;
  }

  Status = AmlSerializeDefinitionBlock (RootNode, &Table);
  AmlDeleteTree (RootNode);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahRestrictedDmaPool: AmlSerializeDefinitionBlock failed %r\n", Status));
    return Status;
  }

  Status = gBS->LocateProtocol (&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&AcpiTableProtocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahRestrictedDmaPool: ACPI table protocol not available %r\n", Status));
    FreePool (Table);
    return Status;
  }

  Status = AcpiTableProtocol->InstallAcpiTable (AcpiTableProtocol, Table, Table->Length, &TableKey);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GunyahRestrictedDmaPool: InstallAcpiTable failed %r\n", Status));
    FreePool (Table);
    return Status;
  }

  DEBUG ((DEBUG_INFO, "GunyahRestrictedDmaPool: SSDT installed (TableKey=0x%lx) RDPA=0x%lx RDPS=0x%lx\n", TableKey, BaseAddress, Length));
  FreePool (Table);

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
GunyahRestrictedDmaPoolAcpiDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS Status;

  Status = InstallRestrictedDmaPoolSsdt ();
  if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND) {
    DEBUG ((DEBUG_ERROR, "GunyahRestrictedDmaPool: SSDT install failed %r\n", Status));
  }

  return EFI_SUCCESS;
}
