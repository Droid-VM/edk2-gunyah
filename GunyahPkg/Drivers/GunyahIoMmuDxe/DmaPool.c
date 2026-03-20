/** @file

  Gunyah restricted DMA pool bitmap allocator.

  Discovers the "restricted-dma-pool" region from FDT via FdtClientProtocol
  and manages page allocations using a bitmap.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/FdtClient.h>

#include "DmaPool.h"

STATIC EFI_PHYSICAL_ADDRESS  mPoolBase;
STATIC UINTN                 mPoolTotalPages;

//
// Bitmap: one bit per page.  Bit set = allocated.
//
STATIC UINT8   *mBitmap;
STATIC UINTN   mBitmapBytes;

STATIC
BOOLEAN
BitmapTestBit (
  IN UINTN  Index
  )
{
  return (mBitmap[Index / 8] & (1U << (Index % 8))) != 0;
}

STATIC
VOID
BitmapSetBit (
  IN UINTN  Index
  )
{
  mBitmap[Index / 8] |= (UINT8)(1U << (Index % 8));
}

STATIC
VOID
BitmapClearBit (
  IN UINTN  Index
  )
{
  mBitmap[Index / 8] &= (UINT8)~(1U << (Index % 8));
}

/**
  Find a contiguous run of free pages.
**/
STATIC
BOOLEAN
BitmapFindFreeRun (
  IN  UINTN  NumPages,
  OUT UINTN  *StartPage
  )
{
  UINTN  RunStart;
  UINTN  RunLen;
  UINTN  i;

  RunStart = 0;
  RunLen   = 0;

  for (i = 0; i < mPoolTotalPages; i++) {
    if (BitmapTestBit (i)) {
      RunLen   = 0;
      RunStart = i + 1;
    } else {
      RunLen++;
      if (RunLen >= NumPages) {
        *StartPage = RunStart;
        return TRUE;
      }
    }
  }

  return FALSE;
}

EFI_STATUS
DmaPoolInit (
  VOID
  )
{
  EFI_STATUS           Status;
  FDT_CLIENT_PROTOCOL  *FdtClient;
  INT32                Node;
  CONST UINT64         *RegProp;
  UINT32               RegSize;
  UINT64               Base;
  UINT64               Size;

  Status = gBS->LocateProtocol (
                  &gFdtClientProtocolGuid,
                  NULL,
                  (VOID **)&FdtClient
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: FdtClient not available: %r\n", __func__, Status));
    return EFI_NOT_FOUND;
  }

  Status = FdtClient->FindCompatibleNode (
                        FdtClient,
                        "restricted-dma-pool",
                        &Node
                        );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: restricted-dma-pool not found in FDT\n", __func__));
    return EFI_NOT_FOUND;
  }

  Status = FdtClient->GetNodeProperty (
                        FdtClient,
                        Node,
                        "reg",
                        (CONST VOID **)&RegProp,
                        &RegSize
                        );
  if (EFI_ERROR (Status) || (RegSize < sizeof (UINT64) * 2)) {
    DEBUG ((DEBUG_ERROR, "%a: bad 'reg' property (%r, size %u)\n",
            __func__, Status, RegSize));
    return EFI_NOT_FOUND;
  }

  Base = SwapBytes64 (RegProp[0]);
  Size = SwapBytes64 (RegProp[1]);

  if ((Size == 0) || (Base == 0)) {
    DEBUG ((DEBUG_ERROR, "%a: invalid pool region Base=0x%lx Size=0x%lx\n",
            __func__, Base, Size));
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_INFO, "%a: restricted-dma-pool @ 0x%lx size 0x%lx (%lu MB)\n",
          __func__, Base, Size, Size / (1024 * 1024)));

  mPoolBase       = Base;
  mPoolTotalPages = EFI_SIZE_TO_PAGES (Size);

  mBitmapBytes = (mPoolTotalPages + 7) / 8;
  mBitmap      = AllocateZeroPool (mBitmapBytes);
  if (mBitmap == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
DmaPoolAllocatePages (
  IN  UINTN                 NumPages,
  OUT EFI_PHYSICAL_ADDRESS  *Address
  )
{
  UINTN  StartPage;
  UINTN  i;

  if (!BitmapFindFreeRun (NumPages, &StartPage)) {
    DEBUG ((DEBUG_ERROR, "%a: pool exhausted (requested %u pages, total %u)\n",
            __func__, NumPages, mPoolTotalPages));
    return EFI_OUT_OF_RESOURCES;
  }

  for (i = StartPage; i < StartPage + NumPages; i++) {
    BitmapSetBit (i);
  }

  *Address = mPoolBase + EFI_PAGES_TO_SIZE (StartPage);

  DEBUG ((DEBUG_VERBOSE, "%a: allocated %u pages @ 0x%lx\n",
          __func__, NumPages, *Address));

  return EFI_SUCCESS;
}

VOID
DmaPoolFreePages (
  IN UINTN                 NumPages,
  IN EFI_PHYSICAL_ADDRESS  Address
  )
{
  UINTN  PageOffset;
  UINTN  StartPage;
  UINTN  i;

  if (Address < mPoolBase) {
    DEBUG ((DEBUG_ERROR, "%a: address 0x%lx below pool base\n", __func__, Address));
    return;
  }

  PageOffset = (UINTN)(Address - mPoolBase);
  StartPage  = EFI_SIZE_TO_PAGES (PageOffset);

  if ((StartPage + NumPages) > mPoolTotalPages) {
    DEBUG ((DEBUG_ERROR, "%a: free out of range @ 0x%lx\n", __func__, Address));
    return;
  }

  for (i = StartPage; i < StartPage + NumPages; i++) {
    BitmapClearBit (i);
  }

  DEBUG ((DEBUG_VERBOSE, "%a: freed %u pages @ 0x%lx\n",
          __func__, NumPages, Address));
}
