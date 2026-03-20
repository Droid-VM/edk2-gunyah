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
// #include <Library/FdtSerialPortAddressLib.h>

#include <Guid/Early16550UartBaseAddress.h>
#include <Guid/FdtHob.h>

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
  // FDT_SERIAL_PORTS          Ports;
  // EFI_STATUS                Status;

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

  // Status = FdtSerialGetPorts (Base, "ns16550a", &Ports);
  // if (!EFI_ERROR (Status)) {
  //   if (Ports.NumberOfPorts == 1) {
  //     //
  //     // Just one UART; direct both SerialPortLib+console and DebugLib to it.
  //     //
  //     *UartHobData = Ports.BaseAddress[0];
  //   } else {
  //     UINT64  ConsoleAddress;

  //     Status = FdtSerialGetConsolePort (Base, &ConsoleAddress);
  //     if (EFI_ERROR (Status)) {
  //       //
  //       // At least two UARTs; but failed to get the console preference. Use the
  //       // first UART for SerialPortLib+console, and the second one for
  //       // DebugLib.
  //       //
  //       *UartHobData = Ports.BaseAddress[0];
  //     } else {
  //       //
  //       // At least two UARTs; and console preference available. Use the
  //       // preferred UART for SerialPortLib+console, and *another* UART for
  //       // DebugLib.
  //       //
  //       *UartHobData = ConsoleAddress;
  //     }
  //   }

  // }

  BuildFvHob (PcdGet64 (PcdFvBaseAddress), PcdGet32 (PcdFvSize));

  return EFI_SUCCESS;
}
