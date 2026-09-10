/** @file
 * SimpleFbDxe: Simple FrameBuffer
 *
 * Copyright (c) DuoWoA authors. All rights reserved.
 * Copyright (c) AlohaWoA authors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
*/

#include <PiDxe.h>
#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/GraphicsOutput.h>
#include <Protocol/Cpu.h>

/// Defines
/*
 * Convert enum video_log2_bpp to bytes and bits. Note we omit the outer
 * brackets to allow multiplication by fractional pixels.
 */
#define VNBYTES(bpix) (1 << (bpix)) / 8
#define VNBITS(bpix) (1 << (bpix))

#define POS_TO_FB(posX, posY)                                                  \
  ((UINT8                                                                      \
        *)((UINTN)This->Mode->FrameBufferBase + (posY)*This->Mode->Info->PixelsPerScanLine * FB_BYTES_PER_PIXEL + (posX)*FB_BYTES_PER_PIXEL))

#define FB_BITS_PER_PIXEL (32)
#define FB_BYTES_PER_PIXEL (FB_BITS_PER_PIXEL / 8)
#define DISPLAYDXE_PHYSICALADDRESS32(_x_) (UINTN)((_x_)&0xFFFFFFFF)

#define DISPLAYDXE_RED_MASK 0xFF0000
#define DISPLAYDXE_GREEN_MASK 0x00FF00
#define DISPLAYDXE_BLUE_MASK 0x0000FF
#define DISPLAYDXE_ALPHA_MASK 0x000000

/*
 * Bits per pixel selector. Each value n is such that the bits-per-pixel is
 * 2 ^ n
 */
enum video_log2_bpp {
  VIDEO_BPP1 = 0,
  VIDEO_BPP2,
  VIDEO_BPP4,
  VIDEO_BPP8,
  VIDEO_BPP16,
  VIDEO_BPP32,
};

typedef struct {
  VENDOR_DEVICE_PATH DisplayDevicePath;
  EFI_DEVICE_PATH    EndDevicePath;
} DISPLAY_DEVICE_PATH;

DISPLAY_DEVICE_PATH mDisplayDevicePath = {
    {{HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      {
          (UINT8)(sizeof(VENDOR_DEVICE_PATH)),
          (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8),
      }},
     EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID},
    {END_DEVICE_PATH_TYPE,
     END_ENTIRE_DEVICE_PATH_SUBTYPE,
     {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0}}};

/// Declares

STATIC FRAME_BUFFER_CONFIGURE *mFrameBufferBltLibConfigure;
STATIC UINTN mFrameBufferBltLibConfigureSize;

STATIC
EFI_STATUS
EFIAPI
DisplayQueryMode(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber,
    OUT UINTN *SizeOfInfo, OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);

STATIC
EFI_STATUS
EFIAPI
DisplaySetMode(IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber);

STATIC
EFI_STATUS
EFIAPI
DisplayBlt(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
    OPTIONAL IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    IN UINTN SourceX, IN UINTN SourceY, IN UINTN DestinationX,
    IN UINTN DestinationY, IN UINTN Width, IN UINTN Height,
    IN UINTN Delta OPTIONAL);

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL mDisplay = {
    DisplayQueryMode, DisplaySetMode, DisplayBlt, NULL};

STATIC
EFI_STATUS
EFIAPI
DisplayQueryMode(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber,
    OUT UINTN *SizeOfInfo, OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info)
{
  EFI_STATUS Status;
  Status = gBS->AllocatePool(
      EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
      (VOID **)Info);

  ASSERT_EFI_ERROR(Status);

  *SizeOfInfo                   = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  (*Info)->Version              = This->Mode->Info->Version;
  (*Info)->HorizontalResolution = This->Mode->Info->HorizontalResolution;
  (*Info)->VerticalResolution   = This->Mode->Info->VerticalResolution;
  (*Info)->PixelFormat          = This->Mode->Info->PixelFormat;
  (*Info)->PixelsPerScanLine    = This->Mode->Info->PixelsPerScanLine;

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DisplaySetMode(IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This, IN UINT32 ModeNumber)
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DisplayBlt(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
    OPTIONAL IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    IN UINTN SourceX, IN UINTN SourceY, IN UINTN DestinationX,
    IN UINTN DestinationY, IN UINTN Width, IN UINTN Height,
    IN UINTN Delta OPTIONAL)
{
  RETURN_STATUS Status;
  EFI_TPL       Tpl;
  //
  // We have to raise to TPL_NOTIFY, so we make an atomic write to the frame
  // buffer. We would not want a timer based event (Cursor, ...) to come in
  // while we are doing this operation.
  //
  Tpl    = gBS->RaiseTPL(TPL_NOTIFY);
  Status = FrameBufferBlt(
      mFrameBufferBltLibConfigure, BltBuffer, BltOperation, SourceX, SourceY,
      DestinationX, DestinationY, Width, Height, Delta);
  gBS->RestoreTPL(Tpl);

  return RETURN_ERROR(Status) ? EFI_INVALID_PARAMETER : EFI_SUCCESS;
}

/**
  Map the framebuffer, and give it the attributes a framebuffer wants.

  This is not only about attributes. Where the framebuffer sits outside the
  memory the firmware was told is RAM -- a Gunyah pseudo-unprotected VM, whose
  /memory names only the window the shim accepted, and whose framebuffer is the
  region just above it -- this call is what creates the mapping at all. Nothing
  else does: the driver takes the address from a device-tree node and writes to
  it directly.

  Which is why the range has to be a whole number of pages. The page tables are
  updated a page at a time and a request that is not page-aligned is refused
  outright, so a framebuffer whose byte size is not a multiple of 4 KiB --
  1400x1050x32 is 0x59B940 -- would leave the mapping uncreated. In a VM where
  the framebuffer happened to fall inside RAM that went unnoticed, because the
  mapping was already there; where it does not, the first write to the
  framebuffer took a synchronous data abort and the firmware died on the spot.
**/
STATIC
EFI_STATUS
SetSimpleFrameBufferMemoryAttributes(
    IN EFI_PHYSICAL_ADDRESS FrameBufferBase,
    IN UINTN FrameBufferSize)
{
  EFI_CPU_ARCH_PROTOCOL *CpuArch = NULL;
  EFI_PHYSICAL_ADDRESS  AlignedBase;
  UINT64                AlignedSize;
  EFI_STATUS            Status;

  Status = gBS->LocateProtocol(&gEfiCpuArchProtocolGuid, NULL, (VOID **)&CpuArch);
  if (EFI_ERROR(Status) || CpuArch == NULL) {
    DEBUG((DEBUG_WARN, "%a: gEfiCpuArchProtocolGuid not available\n", __func__));
    return Status;
  }

  AlignedBase = FrameBufferBase & ~((EFI_PHYSICAL_ADDRESS)EFI_PAGE_SIZE - 1);
  AlignedSize = ALIGN_VALUE(FrameBufferBase + FrameBufferSize, EFI_PAGE_SIZE) -
                AlignedBase;

  Status = CpuArch->SetMemoryAttributes(
      CpuArch,
      AlignedBase,
      AlignedSize,
      EFI_MEMORY_WT | EFI_MEMORY_XP);

  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_WARN,
           "%a: Failed to set framebuffer memory attributes (WT | XP): %r\n",
           __func__,
           Status));
  }

  return Status;
}

EFI_STATUS
EFIAPI
SimpleFbDxeInitialize(
    IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{

  EFI_STATUS Status             = EFI_SUCCESS;
  EFI_HANDLE hUEFIDisplayHandle = NULL;

  /* Retrieve simple frame buffer from pre-SEC bootloader */
  DEBUG(
      (EFI_D_INFO,
       "SimpleFbDxe: Retrieve MIPI FrameBuffer parameters from PCD\n"));

  UINT64 MipiFrameBufferAddr = PcdGet64(PcdFrameBufferBaseAddress);
  UINT32 MipiFrameBufferWidth  = PcdGet32(PcdFrameBufferWidth);
  UINT32 MipiFrameBufferHeight = PcdGet32(PcdFrameBufferHeight);
  UINT32 MipiFrameBufferStride = PcdGet32(PcdFrameBufferStride);

  /* Sanity check */
  if (MipiFrameBufferAddr == 0 || MipiFrameBufferWidth == 0 ||
      MipiFrameBufferHeight == 0) {
    DEBUG((EFI_D_ERROR, "SimpleFbDxe: Invalid FrameBuffer parameters\n"));
    return EFI_DEVICE_ERROR;
  }

  /* Prepare struct */
  if (mDisplay.Mode == NULL) {
    Status = gBS->AllocatePool(
        EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE),
        (VOID **)&mDisplay.Mode);

    ASSERT_EFI_ERROR(Status);
    if (EFI_ERROR(Status))
      return Status;

    ZeroMem(mDisplay.Mode, sizeof(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE));
  }

  if (mDisplay.Mode->Info == NULL) {
    Status = gBS->AllocatePool(
        EfiBootServicesData, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION),
        (VOID **)&mDisplay.Mode->Info);

    ASSERT_EFI_ERROR(Status);
    if (EFI_ERROR(Status))
      return Status;

    ZeroMem(mDisplay.Mode->Info, sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION));
  }

  /* Set information */
  mDisplay.Mode->MaxMode       = 1;
  mDisplay.Mode->Mode          = 0;
  mDisplay.Mode->Info->Version = 0;

  mDisplay.Mode->Info->HorizontalResolution = MipiFrameBufferWidth;
  mDisplay.Mode->Info->VerticalResolution   = MipiFrameBufferHeight;

  /* SimpleFB runs on a8r8g8b8 (VIDEO_BPP32) for WoA devices */

  /* The row pitch comes from the device tree, not from the width: the host pads its rows so that
     the same memory can be imported as a LINEAR dmabuf by the GPU display path, whose importer
     cannot express every pitch. A width that does not land on that alignment therefore has
     padding after each visible row, and computing LineLength as width * 4 would walk one row
     short of the truth -- shearing the picture progressively down the screen.

     A zero (no 'stride' in the DT) or a nonsense value means packed, which is what this driver
     assumed before the property existed. PixelsPerScanLine is the field GOP has for exactly this
     and is what Windows' Basic Display driver reads, so a padded framebuffer needs nothing
     further downstream. */
  /* FB_BYTES_PER_PIXEL, not VNBYTES: the macro above deliberately omits its outer brackets so
     that `x * VNBYTES(bpp)` stays exact for sub-byte depths, which makes it correct in a
     multiplication and wrong in every divisor. `a / VNBYTES(VIDEO_BPP32)` expands to
     `a / (1 << 5) / 8`, i.e. a / 256 -- and `a % VNBYTES(...)` to `(a % 32) / 8`, which calls a
     perfectly aligned 6000-byte stride misaligned. Both were measured: PixelsPerScanLine came out
     20 instead of 1280, so the whole screen was written into the first eleven rows. */
  UINT32               PackedLineLength = MipiFrameBufferWidth * FB_BYTES_PER_PIXEL;
  UINT32               LineLength       = MipiFrameBufferStride;

  if (LineLength < PackedLineLength || (LineLength % FB_BYTES_PER_PIXEL) != 0) {
    if (LineLength != 0) {
      DEBUG((EFI_D_WARN, "SimpleFbDxe: unusable stride %u, using packed %u\n", LineLength,
             PackedLineLength));
    }
    LineLength = PackedLineLength;
  }

  UINT32               FrameBufferSize    = LineLength * MipiFrameBufferHeight;
  EFI_PHYSICAL_ADDRESS FrameBufferAddress = MipiFrameBufferAddr;

  DEBUG((EFI_D_INFO, "SimpleFbDxe: %ux%u stride=%u (%u px per scan line)\n",
         MipiFrameBufferWidth, MipiFrameBufferHeight, LineLength,
         LineLength / FB_BYTES_PER_PIXEL));

  mDisplay.Mode->Info->PixelsPerScanLine = LineLength / FB_BYTES_PER_PIXEL;
  mDisplay.Mode->Info->PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
  mDisplay.Mode->SizeOfInfo      = sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);
  mDisplay.Mode->FrameBufferBase = FrameBufferAddress;
  mDisplay.Mode->FrameBufferSize = FrameBufferSize;

  DEBUG((EFI_D_WARN, "0"));

  /* Memory propery configuration */
  Status = SetSimpleFrameBufferMemoryAttributes(
      (EFI_PHYSICAL_ADDRESS)FrameBufferAddress,
      FrameBufferSize);
  if (EFI_ERROR(Status)) {
    /*
     * Without that mapping there is nowhere to draw. Everything below this
     * point writes to the framebuffer -- ZeroMem first -- so carrying on would
     * not produce a display, it would produce a data abort in a phase where the
     * only thing that reports one is the exception handler.
     */
    DEBUG((EFI_D_ERROR,
           "SimpleFbDxe: framebuffer at 0x%lx is not mapped; no GOP\n",
           (UINT64)FrameBufferAddress));
    return Status;
  }

  DEBUG((EFI_D_WARN, "1"));

  /* Create the FrameBufferBltLib configuration. */
  Status = FrameBufferBltConfigure(
      (VOID *)(UINTN)mDisplay.Mode->FrameBufferBase, mDisplay.Mode->Info,
      mFrameBufferBltLibConfigure, &mFrameBufferBltLibConfigureSize);

      DEBUG((EFI_D_WARN, "2"));

  if (Status == RETURN_BUFFER_TOO_SMALL) {
    mFrameBufferBltLibConfigure = AllocatePool(mFrameBufferBltLibConfigureSize);
    if (mFrameBufferBltLibConfigure != NULL) {
      Status = FrameBufferBltConfigure(
          (VOID *)(UINTN)mDisplay.Mode->FrameBufferBase, mDisplay.Mode->Info,
          mFrameBufferBltLibConfigure, &mFrameBufferBltLibConfigureSize);
    }
  }
  ASSERT_EFI_ERROR(Status);

  DEBUG((EFI_D_WARN, "3 0x%08X", FrameBufferAddress));
  ZeroMem((VOID *)FrameBufferAddress, FrameBufferSize);
  DEBUG((EFI_D_WARN, "4"));

  /* Register handle */
  Status = gBS->InstallMultipleProtocolInterfaces(
      &hUEFIDisplayHandle, &gEfiDevicePathProtocolGuid, &mDisplayDevicePath,
      &gEfiGraphicsOutputProtocolGuid, &mDisplay, NULL);

      DEBUG((EFI_D_WARN, "5"));

  ASSERT_EFI_ERROR(Status);
      DEBUG((EFI_D_WARN, "6"));

  return Status;
}