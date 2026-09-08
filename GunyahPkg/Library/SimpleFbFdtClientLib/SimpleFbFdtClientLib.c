/** @file
  FDT client library for Simple Framebuffer driver

  Copyright (c) 2026, Kancy Joe. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/FdtClient.h>

/* SimpleFB runs on a8r8g8b8 for WoA devices, the same fact SimpleFbDxe states as VIDEO_BPP32.
   Named here so the stride arithmetic below does not spell out a 4 nobody can search for. */
#define SIMPLEFB_BYTES_PER_PIXEL  4

RETURN_STATUS
ReadPropertyInFdt(
  IN FDT_CLIENT_PROTOCOL *FdtClient,
  IN INT32 Node,
  IN CONST CHAR8 *PropertyName,
  IN UINT32 ExpectedSize,
  OUT UINT64 *Value
  )
{
  CONST VOID *Property;
  UINT32 PropertySize;
  EFI_STATUS Status;

  if (FdtClient == NULL || PropertyName == NULL || Value == NULL || ExpectedSize == 0) {
    return RETURN_INVALID_PARAMETER;
  }

  Status = FdtClient->GetNodeProperty (
                          FdtClient,
                          Node,
                          PropertyName,
                          &Property,
                          &PropertySize
                          );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: No '%a' property found in 'simple-framebuffer' compatible DT node\n",
      __func__,
      PropertyName
      ));
    return (RETURN_STATUS)Status;
  }

  if (PropertySize < ExpectedSize) {
    DEBUG ((
      DEBUG_WARN,
      "%a: '%a' property size expected %u got %u\n",
      __func__,
      PropertyName,
      ExpectedSize,
      PropertySize
      ));
    return RETURN_INVALID_PARAMETER;
  }

  if (PropertySize == sizeof (UINT32)) {
      *Value = SwapBytes32 (*(CONST UINT32 *)Property);
  } else if (PropertySize >= sizeof (UINT64)) {
    // Only read the first 64 bits if the property is larger than 64 bits
    *Value = SwapBytes64 (*(CONST UINT64 *)Property);
  }

  return RETURN_SUCCESS;
}


RETURN_STATUS
EFIAPI
SimpleFbFdtClientLibConstructor (
  VOID
  )
{
  EFI_STATUS           Status;
  FDT_CLIENT_PROTOCOL  *FdtClient;
  INT32                Node;
  UINT64               PropertyValue;
  UINT64               FramebufferBaseAddress;
  UINT32               FramebufferHeight;
  UINT32               FramebufferWidth;
  UINT32               FramebufferStride;
  RETURN_STATUS        PcdStatus;

  Status = gBS->LocateProtocol (
                  &gFdtClientProtocolGuid,
                  NULL,
                  (VOID **)&FdtClient
                  );
  ASSERT_EFI_ERROR (Status);

  Status = FdtClient->FindCompatibleNode (FdtClient, "simple-framebuffer", &Node);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: No 'simple-framebuffer' compatible DT node found\n",
      __func__
      ));
    return EFI_SUCCESS;
  }

  /* Framebuffer base address */
  Status = ReadPropertyInFdt (FdtClient, Node, "reg", sizeof (UINT64), &PropertyValue);
  if (RETURN_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: No 'reg' property found in 'simple-framebuffer' compatible DT node\n",
      __func__
      ));
    return EFI_SUCCESS;
  }

  FramebufferBaseAddress = PropertyValue;
  ASSERT (FramebufferBaseAddress != 0);

  PcdStatus = PcdSet64S (PcdFrameBufferBaseAddress, FramebufferBaseAddress);
  ASSERT_RETURN_ERROR (PcdStatus);

  DEBUG ((DEBUG_INFO, "Found Simple Framebuffer @ 0x%Lx\n", FramebufferBaseAddress));

  /* Framebuffer height */
  Status = ReadPropertyInFdt (FdtClient, Node, "height", sizeof (UINT32), &PropertyValue);
  if (RETURN_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: No 'height' property found\n", __func__));
    return EFI_SUCCESS;
  }

  FramebufferHeight = (UINT32)PropertyValue;
  ASSERT (FramebufferHeight != 0);

  PcdStatus = PcdSet32S (PcdFrameBufferHeight, FramebufferHeight);
  ASSERT_RETURN_ERROR (PcdStatus);

  /* Framebuffer width */
  Status = ReadPropertyInFdt (FdtClient, Node, "width", sizeof (UINT32), &PropertyValue);
  if (RETURN_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "%a: No 'width' property found\n", __func__));
    return EFI_SUCCESS;
  }

  FramebufferWidth = (UINT32)PropertyValue;
  ASSERT (FramebufferWidth != 0);

  PcdStatus = PcdSet32S (PcdFrameBufferWidth, FramebufferWidth);
  ASSERT_RETURN_ERROR (PcdStatus);

  /* Framebuffer stride: bytes from one row to the next, which is NOT width * 4 when the host
     pads its rows. It pads them because the GPU transport imports this same memory as a LINEAR
     dmabuf and the importer cannot express every pitch; a width that does not land on that
     alignment therefore arrives here with padding, and computing the pitch ourselves would
     shear the picture by exactly the padding on every row.

     Optional on purpose. The Linux simple-framebuffer binding has always carried 'stride', but a
     device tree from a host that never padded describes a packed layout and may omit it; the
     fallback is the value this driver assumed for its whole life, so an old DT boots unchanged. */
  Status = ReadPropertyInFdt (FdtClient, Node, "stride", sizeof (UINT32), &PropertyValue);
  if (RETURN_ERROR (Status)) {
    FramebufferStride = FramebufferWidth * SIMPLEFB_BYTES_PER_PIXEL;
    DEBUG ((
      DEBUG_WARN,
      "%a: No 'stride' property found, assuming packed %u bytes per row\n",
      __func__,
      FramebufferStride
      ));
  } else {
    FramebufferStride = (UINT32)PropertyValue;
  }

  /* A stride below the visible row is not a layout anything can draw into: it would put row N+1
     on top of row N. Refuse the number rather than the framebuffer -- the packed layout is still
     a coherent one, and a firmware that draws is worth more than one that is right about why it
     could not. */
  if (FramebufferStride < FramebufferWidth * SIMPLEFB_BYTES_PER_PIXEL) {
    DEBUG ((
      DEBUG_WARN,
      "%a: 'stride' %u is below %u bytes for a %u-pixel row; using the packed layout\n",
      __func__,
      FramebufferStride,
      FramebufferWidth * SIMPLEFB_BYTES_PER_PIXEL,
      FramebufferWidth
      ));
    FramebufferStride = FramebufferWidth * SIMPLEFB_BYTES_PER_PIXEL;
  }

  PcdStatus = PcdSet32S (PcdFrameBufferStride, FramebufferStride);
  ASSERT_RETURN_ERROR (PcdStatus);

  return EFI_SUCCESS;
}
