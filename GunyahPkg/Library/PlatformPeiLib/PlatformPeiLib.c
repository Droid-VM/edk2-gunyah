/** @file
*
*  Copyright (c) 2011-2014, ARM Limited. All rights reserved.
*  Copyright (c) 2014-2020, Linaro Limited. All rights reserved.
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
*
**/

#include <PiPei.h>

#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/PeiServicesLib.h>
#include <Guid/Early16550UartBaseAddress.h>
#include <Guid/FdtHob.h>

STATIC
UINT64
ReadFdtCells (
  IN CONST UINT32  *Prop,
  IN INT32         NumCells
  )
{
  if (NumCells == 2) {
    return ((UINT64)Fdt32ToCpu (Prop[0]) << 32) | Fdt32ToCpu (Prop[1]);
  }

  return Fdt32ToCpu (Prop[0]);
}

STATIC
VOID
BuildReservedMemoryHob (
  VOID *FdtBase
  )
{
  INT32        RsvdNode;
  INT32        SubNode;
  INT32        AddrCells;
  INT32        SizeCells;
  INT32        PropLen;
  CONST UINT32 *RegProp;
  BOOLEAN      NoMap;

  RsvdNode = FdtPathOffset (FdtBase, "/reserved-memory");
  if (RsvdNode < 0) {
    DEBUG ((DEBUG_WARN, "%a: /reserved-memory not found\n", __func__));
    return;
  }

  AddrCells = FdtAddressCells (FdtBase, RsvdNode);
  SizeCells = FdtSizeCells (FdtBase, RsvdNode);
  if ((AddrCells < 1) || (SizeCells < 1)) {
    DEBUG ((DEBUG_ERROR, "%a: invalid #address-cells/%a-cells\n", __func__, "#size"));
    return;
  }

  FdtForEachSubnode (SubNode, FdtBase, RsvdNode) {
    EFI_PHYSICAL_ADDRESS  Base;
    UINT64                Size;

    RegProp = FdtGetProp (FdtBase, SubNode, "reg", &PropLen);
    if ((RegProp == NULL) || (PropLen < (AddrCells + SizeCells) * (INT32)sizeof (UINT32))) {
      continue;
    }

    Base = ReadFdtCells (RegProp, AddrCells);
    Size = ReadFdtCells (RegProp + AddrCells, SizeCells);

    NoMap = FdtGetProp (FdtBase, SubNode, "no-map", NULL) != NULL;
  
    DEBUG ((
      DEBUG_INFO,
      "%a: reserved region %a @ 0x%lx size 0x%lx%a\n",
      __func__,
      FdtGetName(FdtBase, SubNode, NULL),
      Base, Size, NoMap ? " (no-map)" : ""
      ));

    BuildMemoryAllocationHob (Base, Size, NoMap ? EfiReservedMemoryType : EfiACPIReclaimMemory);
  }
}

EFI_STATUS
EFIAPI
PlatformPeim (
  VOID
  )
{
  VOID                      *Base;
  VOID                      *NewBase;
  UINTN                     FdtSize;
  UINTN                     FdtPages;
  UINT64                    *FdtHobData;
  UINT64                    *UartHobData;

  Base = (VOID *)(UINTN)PcdGet64 (PcdDeviceTreeInitialBaseAddress);
  ASSERT (Base != NULL);
  ASSERT (FdtCheckHeader (Base) == 0);

  FdtSize  = FdtTotalSize (Base) + PcdGet32 (PcdDeviceTreeAllocationPadding);
  FdtPages = EFI_SIZE_TO_PAGES (FdtSize);
  NewBase  = AllocatePages (FdtPages);
  ASSERT (NewBase != NULL);
  FdtOpenInto (Base, NewBase, EFI_PAGES_TO_SIZE (FdtPages));

  FdtHobData = BuildGuidHob (&gFdtHobGuid, sizeof *FdtHobData);
  ASSERT (FdtHobData != NULL);
  *FdtHobData = (UINTN)NewBase;

  UartHobData = BuildGuidHob (&gEarly16550UartBaseAddressGuid, sizeof *UartHobData);
  ASSERT (UartHobData != NULL);
  SetMem (UartHobData, sizeof *UartHobData, 0);

  *UartHobData = PcdGet64 (PcdSerialRegisterBase);
  DEBUG ((DEBUG_INFO, "%a: NS16550A UART @ 0x%lx\n", __func__, *UartHobData));

  BuildFvHob (PcdGet64 (PcdFvBaseAddress), PcdGet32 (PcdFvSize));

  BuildReservedMemoryHob (NewBase);

  return EFI_SUCCESS;
}
