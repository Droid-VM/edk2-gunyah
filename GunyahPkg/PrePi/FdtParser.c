/*
 * Copyright (c) 2015, Linaro Ltd. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>

BOOLEAN
FindMemnode (
  IN  VOID    *DeviceTreeBlob,
  OUT UINT64  *SystemMemoryBase,
  OUT UINT64  *SystemMemorySize,
  IN  UINT64  FirmwareBase
  )
{
  INT32        MemoryNode;
  INT32        AddressCells;
  INT32        SizeCells;
  INT32        Length;
  CONST INT32  *Prop;
  INT32        Cells;
  INT32        Index;
  UINT64       Base;
  UINT64       Size;

  if (FdtCheckHeader (DeviceTreeBlob) != 0) {
    return FALSE;
  }

  //
  // Look for a node called "memory" at the lowest level of the tree
  //
  MemoryNode = FdtPathOffset (DeviceTreeBlob, "/memory");
  if (MemoryNode <= 0) {
    return FALSE;
  }

  //
  // Retrieve the #address-cells and #size-cells properties
  // from the root node, or use the default if not provided.
  //
  AddressCells = 1;
  SizeCells    = 1;

  Prop = FdtGetProp (DeviceTreeBlob, 0, "#address-cells", &Length);
  if (Length == 4) {
    AddressCells = Fdt32ToCpu (*Prop);
  }

  Prop = FdtGetProp (DeviceTreeBlob, 0, "#size-cells", &Length);
  if (Length == 4) {
    SizeCells = Fdt32ToCpu (*Prop);
  }

  //
  // Select the RAM bank containing the loaded firmware. A lower-address
  // framebuffer or shared region may precede RAM in crosvm's /memory node.
  //
  Prop = FdtGetProp (DeviceTreeBlob, MemoryNode, "reg", &Length);

  if ((AddressCells < 1) || (AddressCells > 2) ||
      (SizeCells < 1) || (SizeCells > 2)) {
    return FALSE;
  }

  Cells = AddressCells + SizeCells;
  if ((Prop == NULL) || (Length < Cells * sizeof (INT32)) ||
      ((Length % (Cells * sizeof (INT32))) != 0)) {
    return FALSE;
  }

  for (Index = 0; Index < Length / sizeof (INT32); Index += Cells) {
    Base = Fdt32ToCpu (Prop[Index]);
    if (AddressCells == 2) {
      Base = (Base << 32) | Fdt32ToCpu (Prop[Index + 1]);
    }

    Size = Fdt32ToCpu (Prop[Index + AddressCells]);
    if (SizeCells == 2) {
      Size = (Size << 32) | Fdt32ToCpu (Prop[Index + AddressCells + 1]);
    }

    // Subtraction avoids overflowing Base + Size for malformed ranges.
    if ((FirmwareBase >= Base) && (FirmwareBase - Base < Size)) {
      *SystemMemoryBase = Base;
      *SystemMemorySize = Size;
      return TRUE;
    }
  }

  return FALSE;
}

VOID
CopyFdt (
  IN    VOID  *FdtDest,
  IN    VOID  *FdtSource
  )
{
  INT32        MemoryNode;
  INT32        FramebufferNode;
  INT32        Length;
  INT32        FramebufferLength;
  INT32        AddressCells;
  INT32        SizeCells;
  INT32        Cells;
  INT32        Index;
  INT32        Kept;
  CONST INT32  *Prop;
  CONST INT32  *FramebufferReg;
  INT32        MemoryReg[256];

  FdtPack (FdtSource);
  CopyMem (FdtDest, FdtSource, FdtTotalSize (FdtSource));

  // A standalone simple-framebuffer allocation is device memory, not RAM.
  // Remove only exact matching banks; a framebuffer carved out of a larger
  // RAM bank (as on Gunyah) must leave that bank intact.
  Prop = FdtGetProp (FdtDest, 0, "#address-cells", &Length);
  AddressCells = ((Prop != NULL) && (Length == 4)) ? Fdt32ToCpu (*Prop) : 1;
  Prop = FdtGetProp (FdtDest, 0, "#size-cells", &Length);
  SizeCells = ((Prop != NULL) && (Length == 4)) ? Fdt32ToCpu (*Prop) : 1;
  if ((AddressCells < 1) || (AddressCells > 2) ||
      (SizeCells < 1) || (SizeCells > 2)) {
    return;
  }

  Cells = AddressCells + SizeCells;
  MemoryNode = FdtPathOffset (FdtDest, "/memory");
  Prop = FdtGetProp (FdtDest, MemoryNode, "reg", &Length);
  if ((Prop == NULL) || (Length <= 0) || (Length > sizeof (MemoryReg)) ||
      ((Length % (Cells * sizeof (INT32))) != 0)) {
    return;
  }

  Kept = 0;
  for (Index = 0; Index < Length / sizeof (INT32); Index += Cells) {
    FramebufferNode = -1;
    while ((FramebufferNode = FdtNodeOffsetByCompatible (
              FdtDest, FramebufferNode, "simple-framebuffer")) >= 0) {
      // Only root-level nodes share the root's reg cell encoding.
      if (FdtParentOffset (FdtDest, FramebufferNode) != 0) {
        continue;
      }

      FramebufferReg = FdtGetProp (FdtDest, FramebufferNode, "reg", &FramebufferLength);
      if ((FramebufferReg != NULL) &&
          (FramebufferLength == Cells * sizeof (INT32)) &&
          (CompareMem (&Prop[Index], FramebufferReg, FramebufferLength) == 0)) {
        break;
      }
    }

    if (FramebufferNode < 0) {
      CopyMem (&MemoryReg[Kept], &Prop[Index], Cells * sizeof (INT32));
      Kept += Cells;
    }
  }

  if ((Kept > 0) && (Kept < Length / sizeof (INT32))) {
    FdtSetProp (FdtDest, MemoryNode, "reg", MemoryReg, Kept * sizeof (INT32));
    FdtPack (FdtDest);
  }
}
