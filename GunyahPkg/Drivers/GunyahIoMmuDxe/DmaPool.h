/** @file

  Gunyah restricted DMA pool allocator for protected VM support.

  Discovers the "restricted-dma-pool" region from FDT and provides a
  page-granularity bitmap allocator so that all DMA buffers are placed
  in host-accessible memory.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef GUNYAH_IOMMU_DMA_POOL_H_
#define GUNYAH_IOMMU_DMA_POOL_H_

#include <Uefi.h>

/**
  Initialize the DMA pool by discovering the restricted-dma-pool region
  from the FDT.

  @retval EFI_SUCCESS            Pool initialized.
  @retval EFI_NOT_FOUND          No restricted-dma-pool in FDT.
  @retval EFI_OUT_OF_RESOURCES   Failed to allocate bitmap.
**/
EFI_STATUS
DmaPoolInit (
  VOID
  );

/**
  Allocate contiguous pages from the restricted DMA pool.

  @param[in]  NumPages     Number of 4 KiB pages to allocate.
  @param[out] Address      Physical address of allocated region.

  @retval EFI_SUCCESS           Allocation succeeded.
  @retval EFI_OUT_OF_RESOURCES  Pool exhausted.
**/
EFI_STATUS
DmaPoolAllocatePages (
  IN  UINTN                 NumPages,
  OUT EFI_PHYSICAL_ADDRESS  *Address
  );

/**
  Free pages previously allocated from the DMA pool.

  @param[in] NumPages  Number of pages to free.
  @param[in] Address   Address returned by DmaPoolAllocatePages.
**/
VOID
DmaPoolFreePages (
  IN UINTN                 NumPages,
  IN EFI_PHYSICAL_ADDRESS  Address
  );

#endif // GUNYAH_IOMMU_DMA_POOL_H_
