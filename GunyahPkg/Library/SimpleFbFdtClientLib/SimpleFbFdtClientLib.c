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

  return EFI_SUCCESS;
}
