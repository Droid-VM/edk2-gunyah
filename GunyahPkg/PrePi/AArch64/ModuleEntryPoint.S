//
//  Copyright (c) 2011-2013, ARM Limited. All rights reserved.
//  Copyright (c) 2015-2016, Linaro Limited. All rights reserved.
//
//  SPDX-License-Identifier: BSD-2-Clause-Patent
//
//

#include <AsmMacroLib.h>

ASM_FUNC(_ModuleEntryPoint)
  //
  // If we are booting from RAM using the Linux kernel boot protocol, x0 will
  // point to the DTB image in memory. Otherwise, use the default value defined
  // by the platform.
  //
  cbnz  x0, 0f
  ldr   x0, PcdGet64 (PcdDeviceTreeInitialBaseAddress)

0:mov   x28, x0             // preserve DTB pointer
  mov   x27, x1             // preserve base of image pointer

  // Enable Floating Point. This needs to be done before entering C code, which
  // may use FP/SIMD registers.
  bl    ArmEnableVFP

  bl    ASM_PFX(DiscoverDramFromDt)

  // Get ID of this CPU in Multicore system
  bl    ASM_PFX(ArmReadMpidr)
  // Keep a copy of the MpId register value
  mov   x20, x0

// Place the UEFI memory region (including stacks) right after the Firmware Device,
// to avoid conflicting with reserved memory at the top of DRAM (swiotlb, simplefb, etc.)
_SetupStackPosition:
  // Calculate end of Firmware Device
  ldr   x2, PcdGet64 (PcdFdBaseAddress)
  MOV32 (w3, FixedPcdGet32 (PcdFdSize))
  add   x21, x2, x3      // x21 = UefiMemoryBase = PcdFdBaseAddress + PcdFdSize

  // UEFI Memory Size (stacks are allocated at the top of this region)
  MOV32 (x4, FixedPcdGet32(PcdSystemMemoryUefiRegionSize))

  // Stack top = UefiMemoryBase + UefiRegionSize
  add   x1, x21, x4

_GetStackBase:
  // x1 = The top of the Mpcore Stacks
  mov   sp, x1

  // Stack for the primary core = PrimaryCoreStack
  MOV32 (x2, FixedPcdGet32(PcdCPUCorePrimaryStackSize))
  sub   x22, x1, x2

  mov   x0, x20
  mov   x1, x21
  mov   x2, x22

  // Set the frame pointer to NULL so any backtraces terminate here
  mov   x29, xzr

  // Jump to PrePiCore C code
  //    x0 = MpId
  //    x1 = UefiMemoryBase
  //    x2 = StacksBase
  bl    ASM_PFX(CEntryPoint)

_NeverReturn:
  b _NeverReturn

// VOID
// DiscoverDramFromDt (
//   VOID   *DeviceTreeBaseAddress,   // passed by loader in x0, preserved in x28
//   VOID   *ImageBase                // passed by FDF trampoline in x1, preserved in x27
//   );
ASM_PFX(DiscoverDramFromDt):
  mov   x29, x30            // preserve LR

  //
  // The base of the runtime image has been preserved in x27. Check whether
  // the expected magic number can be found in the header.
  //
  ldr   w8, .LArm64LinuxMagic
  ldr   w9, [x27, #0x38]
  cmp   w8, w9
  bne   .Lout

  //
  //
  // OK, so far so good. We have confirmed that we likely have a DTB and are
  // booting via the arm64 Linux boot protocol. Update the base-of-image PCD
  // to the actual relocated value, and add the shift of PcdFdBaseAddress to
  // PcdFvBaseAddress as well
  //
  adr   x8, PcdGet64 (PcdFdBaseAddress)
  adr   x9, PcdGet64 (PcdFvBaseAddress)
  ldr   x6, [x8]
  ldr   x7, [x9]
  sub   x7, x7, x6
  add   x7, x7, x27
  str   x27, [x8]
  str   x7, [x9]

  //
  // The runtime address may be different from the link time address so fix
  // up the PE/COFF relocations. Since we are calling a C function, use the
  // window at the beginning of the FD image as a temp stack.
  //
  mov   x0, x7
  adr   x1, PeCoffLoaderImageReadFromMemory
  mov   sp, x7
  bl    RelocatePeCoffImage

  //
  // Discover the memory size and offset from the DTB, and record in the
  // respective PCDs. This will also return false if a corrupt DTB is
  // encountered.
  //
  mov   x0, x28
  adr   x1, PcdGet64 (PcdSystemMemoryBase)
  adr   x2, PcdGet64 (PcdSystemMemorySize)
  bl    FindMemnode
  cbz   x0, .Lout

  //
  // Copy the DTB to the slack space right after the 64 byte arm64/Linux style
  // image header at the base of this image (defined in the FDF), and record the
  // pointer in PcdDeviceTreeInitialBaseAddress.
  //
  adr   x8, PcdGet64 (PcdDeviceTreeInitialBaseAddress)
  add   x27, x27, #0x40
  str   x27, [x8]

  mov   x0, x27
  mov   x1, x28
  bl    CopyFdt

.Lout:
  ret    x29

.LArm64LinuxMagic:
  .byte   0x41, 0x52, 0x4d, 0x64
