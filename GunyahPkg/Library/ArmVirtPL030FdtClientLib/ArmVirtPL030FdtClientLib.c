/** @file
  FDT client library for ARM's PL030 RTC driver

  Copyright (c) 2016, Linaro Ltd. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/FdtClient.h>

RETURN_STATUS
EFIAPI
ArmVirtPL030FdtClientLibConstructor (
  VOID
  )
{
  EFI_STATUS           Status;
  FDT_CLIENT_PROTOCOL  *FdtClient;
  INT32                Node;
  CONST UINT64         *Reg;
  UINT32               RegSize;
  UINT64               RegBase;
  CONST UINT32         *PeriphId;
  UINT32               PeriphIdVal;
  UINT32               PeriphIdSize;
  BOOLEAN              PeriphIdFound;
  UINTN                PeriphIndex;
  RETURN_STATUS        PcdStatus;

  Status = gBS->LocateProtocol (
                  &gFdtClientProtocolGuid,
                  NULL,
                  (VOID **)&FdtClient
                  );
  ASSERT_EFI_ERROR (Status);

  Status = FdtClient->FindCompatibleNode (FdtClient, "arm,primecell", &Node);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: No 'arm,primecell' compatible DT node found\n",
      __func__
      ));
    return EFI_SUCCESS;
  }

  PeriphIdFound = FALSE;
  for (;;) {
    Status = FdtClient->GetNodeProperty (
                          FdtClient,
                          Node,
                          "arm,primecell-periphid",
                          (CONST VOID **)&PeriphId,
                          &PeriphIdSize
                          );

    if (!EFI_ERROR (Status) && (PeriphIdSize != 0) && ((PeriphIdSize % sizeof (UINT32)) == 0)) {
      for (PeriphIndex = 0; PeriphIndex < (PeriphIdSize / sizeof (UINT32)); PeriphIndex++) {
        PeriphIdVal = SwapBytes32 (PeriphId[PeriphIndex]);
        if (PeriphIdVal == 0x41030) {
          PeriphIdFound = TRUE;
          break;
        }
      }
    }

    if (PeriphIdFound) {
      break;
    }

    Status = FdtClient->FindNextCompatibleNode (FdtClient, "arm,primecell", Node, &Node);
    if (EFI_ERROR (Status)) {
      break;
    }
  }

  if (!PeriphIdFound) {
    DEBUG ((
      DEBUG_WARN,
      "%a: no matching 'arm,primecell-periphid' entry found in any 'arm,primecell' node\n",
      __func__
      ));
    return EFI_SUCCESS;
  }

  Status = FdtClient->GetNodeProperty (
                        FdtClient,
                        Node,
                        "reg",
                        (CONST VOID **)&Reg,
                        &RegSize
                        );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: No 'reg' property found in 'arm,primecell' compatible DT node\n",
      __func__
      ));
    return EFI_SUCCESS;
  }

  ASSERT (RegSize == 16);

  RegBase = SwapBytes64 (Reg[0]);
  ASSERT (RegBase < MAX_UINT32);

  PcdStatus = PcdSet32S (PcdPL030RtcBase, (UINT32)RegBase);
  ASSERT_RETURN_ERROR (PcdStatus);

  DEBUG ((DEBUG_INFO, "Found PL030 RTC @ 0x%Lx\n", RegBase));

  //
  // UEFI takes ownership of the RTC hardware, and exposes its functionality
  // through the UEFI Runtime Services GetTime, SetTime, etc. This means we
  // need to disable it in the device tree to prevent the OS from attaching
  // its device driver as well.
  //
  Status = FdtClient->SetNodeProperty (
                        FdtClient,
                        Node,
                        "status",
                        "disabled",
                        sizeof ("disabled")
                        );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "Failed to set PL030 status to 'disabled'\n"));
  }

  return EFI_SUCCESS;
}
