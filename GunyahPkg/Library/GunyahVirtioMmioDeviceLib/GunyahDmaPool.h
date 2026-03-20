/** @file

  Gunyah restricted DMA pool allocator for protected VM support.

  When running under Gunyah hypervisor in protected mode, guest memory regions
  are donated to the hypervisor and become inaccessible to the host (VMM).
  Only the StaticSwiotlbRegion remains host-accessible. The hypervisor
  communicates this region to the guest via the FDT "restricted-dma-pool"
  binding under reserved-memory.

  This module discovers the restricted-dma-pool from FDT and provides a
  page-granularity allocator so that VirtIO shared buffers (queues, request
  headers, etc.) are placed in host-accessible memory.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef GUNYAH_DMA_POOL_H_
#define GUNYAH_DMA_POOL_H_

#include <Uefi.h>

/**
  Initialize the DMA pool by discovering the restricted-dma-pool region
  from the FDT. Must be called from DXE context where Boot Services and
  the FDT Client Protocol are available.

  @retval EFI_SUCCESS            DMA pool discovered and reserved.
  @retval EFI_NOT_FOUND          No restricted-dma-pool in FDT (non-protected VM).
  @retval EFI_OUT_OF_RESOURCES   Failed to reserve pool memory.
**/
EFI_STATUS
GunyahDmaPoolInit (
  VOID
  );

/**
  Check whether the DMA pool is available (i.e. restricted-dma-pool was found).

  @retval TRUE   DMA pool is available; shared pages must come from it.
  @retval FALSE  No DMA pool; use normal AllocatePages.
**/
BOOLEAN
GunyahDmaPoolIsAvailable (
  VOID
  );

/**
  Allocate pages from the restricted DMA pool.

  @param[in]  NumPages     Number of 4 KiB pages to allocate.
  @param[out] HostAddress  On success, the allocated virtual (= physical) address.

  @retval EFI_SUCCESS           Allocation succeeded.
  @retval EFI_OUT_OF_RESOURCES  Pool exhausted.
**/
EFI_STATUS
GunyahDmaPoolAllocatePages (
  IN  UINTN  NumPages,
  OUT VOID   **HostAddress
  );

/**
  Free pages previously allocated from the DMA pool.
  Currently a no-op for the bump allocator; provided for future extension.

  @param[in] NumPages     Number of pages to free.
  @param[in] HostAddress  Address returned by GunyahDmaPoolAllocatePages.
**/
VOID
GunyahDmaPoolFreePages (
  IN UINTN  NumPages,
  IN VOID   *HostAddress
  );

#endif // GUNYAH_DMA_POOL_H_
