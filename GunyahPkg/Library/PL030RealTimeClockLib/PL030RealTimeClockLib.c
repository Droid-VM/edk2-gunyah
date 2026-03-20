/** @file
  Implement EFI RealTimeClock runtime services via RTC Lib.

  Copyright (c) 2008 - 2010, Apple Inc. All rights reserved.<BR>
  Copyright (c) 2011 - 2020, Arm Limited. All rights reserved.<BR>
  Copyright (c) 2019, Linaro Ltd. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>

#include <Guid/EventGroup.h>
#include <Guid/GlobalVariable.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/RealTimeClockLib.h>
#include <Library/TimeBaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>

#include "PL030RealTimeClock.h"

STATIC BOOLEAN    mPL030Initialized = FALSE;
STATIC EFI_EVENT  mRtcVirtualAddrChangeEvent;
STATIC UINTN      mPL030RtcBase;

EFI_STATUS
InitializePL030 (
  VOID
  )
{
  // Ensure interrupts are masked. We do not want RTC interrupts in UEFI
  if ((MmioRead32(mPL030RtcBase + PL030_RTC_EOI_REGISTER) & PL030_IRQ_TRIGGERED) == PL030_IRQ_TRIGGERED)
    MmioWrite32 (mPL030RtcBase + PL030_RTC_EOI_REGISTER, 0);
  if ((MmioRead32(mPL030RtcBase + PL030_RTC_CR_CONTROL_REGISTER) & PL030_SET_IRQ_MASK) == PL030_SET_IRQ_MASK)
    MmioWrite32 (mPL030RtcBase + PL030_RTC_CR_CONTROL_REGISTER, 0);

  mPL030Initialized = TRUE;

  return EFI_SUCCESS;
}

/**
  Returns the current time and date information, and the time-keeping capabilities
  of the hardware platform.

  @param  Time                   A pointer to storage to receive a snapshot of the current time.
  @param  Capabilities           An optional pointer to a buffer to receive the real time clock
                                 device's capabilities.

  @retval EFI_SUCCESS            The operation completed successfully.
  @retval EFI_INVALID_PARAMETER  Time is NULL.
  @retval EFI_DEVICE_ERROR       The time could not be retrieved due to hardware error.
  @retval EFI_UNSUPPORTED        This call is not supported by this platform at the time the call is made.
                                 The platform should describe this runtime service as unsupported at runtime
                                 via an EFI_RT_PROPERTIES_TABLE configuration table.

**/
EFI_STATUS
EFIAPI
LibGetTime (
  OUT EFI_TIME               *Time,
  OUT EFI_TIME_CAPABILITIES  *Capabilities
  )
{
  EFI_STATUS  Status;
  UINT32      EpochSeconds;

  // Ensure Time is a valid pointer
  if (Time == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  // Initialize the hardware if not already done
  if (!mPL030Initialized) {
    Status = InitializePL030 ();
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  EpochSeconds = MmioRead32 (mPL030RtcBase + PL030_RTC_DR_DATA_REGISTER);

  // Adjust for the correct time zone
  // The timezone setting also reflects the DST setting of the clock
  if (Time->TimeZone != EFI_UNSPECIFIED_TIMEZONE) {
    EpochSeconds += Time->TimeZone * SEC_PER_MIN;
  } else if ((Time->Daylight & EFI_TIME_IN_DAYLIGHT) == EFI_TIME_IN_DAYLIGHT) {
    // Convert to adjusted time, i.e. spring forwards one hour
    EpochSeconds += SEC_PER_HOUR;
  }

  // Convert from internal 32-bit time to UEFI time
  EpochToEfiTime (EpochSeconds, Time);

  // Update the Capabilities info
  if (Capabilities != NULL) {
    // PL030 runs at frequency 1Hz
    Capabilities->Resolution = PL030_COUNTS_PER_SECOND;
    // Accuracy in ppm multiplied by 1,000,000, e.g. for 50ppm set 50,000,000
    Capabilities->Accuracy = (UINT32)PcdGet32 (PcdPL030RtcPpmAccuracy);
    // FALSE: Setting the time does not clear the values below the resolution level
    Capabilities->SetsToZero = FALSE;
  }

  return EFI_SUCCESS;
}

/**
  Sets the current local time and date information.

  @param  Time                  A pointer to the current time.

  @retval EFI_SUCCESS           The operation completed successfully.
  @retval EFI_INVALID_PARAMETER A time field is out of range.
  @retval EFI_DEVICE_ERROR      The time could not be set due to hardware error.
  @retval EFI_UNSUPPORTED       This call is not supported by this platform at the time the call is made.
                                The platform should describe this runtime service as unsupported at runtime
                                via an EFI_RT_PROPERTIES_TABLE configuration table.

**/
EFI_STATUS
EFIAPI
LibSetTime (
  IN  EFI_TIME  *Time
  )
{
  EFI_STATUS  Status;
  UINT32      EpochSeconds;

  // Because the PL030 is a 32-bit counter counting seconds,
  // the maximum time span is just over 136 years.
  // Time is stored in Unix Epoch format, so it starts in 1970,
  // Therefore it can not exceed the year 2106.
  if ((Time->Year < 1970) || (Time->Year >= 2106)) {
    return EFI_UNSUPPORTED;
  }

  // Initialize the hardware if not already done
  if (!mPL030Initialized) {
    Status = InitializePL030 ();
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  EpochSeconds = (UINT32)EfiTimeToEpoch (Time);

  // Adjust for the correct time zone, i.e. convert to UTC time zone
  // The timezone setting also reflects the DST setting of the clock
  if (Time->TimeZone != EFI_UNSPECIFIED_TIMEZONE) {
    EpochSeconds -= Time->TimeZone * SEC_PER_MIN;
  } else if ((Time->Daylight & EFI_TIME_IN_DAYLIGHT) == EFI_TIME_IN_DAYLIGHT) {
    // Convert to un-adjusted time, i.e. fall back one hour
    EpochSeconds -= SEC_PER_HOUR;
  }

  // Set the PL030
  MmioWrite32 (mPL030RtcBase + PL030_RTC_LR_LOAD_REGISTER, EpochSeconds);

  return EFI_SUCCESS;
}

/**
  Returns the current wakeup alarm clock setting.

  @param  Enabled               Indicates if the alarm is currently enabled or disabled.
  @param  Pending               Indicates if the alarm signal is pending and requires acknowledgement.
  @param  Time                  The current alarm setting.

  @retval EFI_SUCCESS           The alarm settings were returned.
  @retval EFI_INVALID_PARAMETER Enabled is NULL.
  @retval EFI_INVALID_PARAMETER Pending is NULL.
  @retval EFI_INVALID_PARAMETER Time is NULL.
  @retval EFI_DEVICE_ERROR      The wakeup time could not be retrieved due to a hardware error.
  @retval EFI_UNSUPPORTED       This call is not supported by this platform at the time the call is made.
                                The platform should describe this runtime service as unsupported at runtime
                                via an EFI_RT_PROPERTIES_TABLE configuration table.

**/
EFI_STATUS
EFIAPI
LibGetWakeupTime (
  OUT BOOLEAN   *Enabled,
  OUT BOOLEAN   *Pending,
  OUT EFI_TIME  *Time
  )
{
  // Not a required feature
  return EFI_UNSUPPORTED;
}

/**
  Sets the system wakeup alarm clock time.

  @param  Enabled               Enable or disable the wakeup alarm.
  @param  Time                  If Enable is TRUE, the time to set the wakeup alarm for.

  @retval EFI_SUCCESS           If Enable is TRUE, then the wakeup alarm was enabled. If
                                Enable is FALSE, then the wakeup alarm was disabled.
  @retval EFI_INVALID_PARAMETER Enabled is NULL.
  @retval EFI_INVALID_PARAMETER Pending is NULL.
  @retval EFI_INVALID_PARAMETER Time is NULL.
  @retval EFI_DEVICE_ERROR      The wakeup time could not be set due to a hardware error.
  @retval EFI_UNSUPPORTED       This call is not supported by this platform at the time the call is made.
                                The platform should describe this runtime service as unsupported at runtime
                                via an EFI_RT_PROPERTIES_TABLE configuration table.

**/
EFI_STATUS
EFIAPI
LibSetWakeupTime (
  IN BOOLEAN    Enabled,
  OUT EFI_TIME  *Time
  )
{
  // Not a required feature
  return EFI_UNSUPPORTED;
}

/**
  Fixup internal data so that EFI can be call in virtual mode.
  Call the passed in Child Notify event and convert any pointers in
  lib to virtual mode.

  @param[in]    Event   The Event that is being processed
  @param[in]    Context Event Context
**/
STATIC
VOID
EFIAPI
VirtualNotifyEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  //
  // Only needed if you are going to support the OS calling RTC functions in virtual mode.
  // You will need to call EfiConvertPointer (). To convert any stored physical addresses
  // to virtual address. After the OS transitions to calling in virtual mode, all future
  // runtime calls will be made in virtual mode.
  //
  EfiConvertPointer (0x0, (VOID **)&mPL030RtcBase);
  return;
}

/**
  This is the declaration of an EFI image entry point. This can be the entry point to an application
  written to this specification, an EFI boot service driver, or an EFI runtime driver.

  @param  ImageHandle           Handle that identifies the loaded image.
  @param  SystemTable           System Table for this image.

  @retval EFI_SUCCESS           The operation completed successfully.

**/
EFI_STATUS
EFIAPI
LibRtcInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  // Initialize RTC Base Address
  mPL030RtcBase = PcdGet32 (PcdPL030RtcBase);

  // Declare the controller as EFI_MEMORY_RUNTIME
  Status = gDS->AddMemorySpace (
                  EfiGcdMemoryTypeMemoryMappedIo,
                  mPL030RtcBase,
                  SIZE_4KB,
                  EFI_MEMORY_UC | EFI_MEMORY_RUNTIME | EFI_MEMORY_XP
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gDS->SetMemorySpaceAttributes (mPL030RtcBase, SIZE_4KB, EFI_MEMORY_UC | EFI_MEMORY_RUNTIME | EFI_MEMORY_XP);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Register for the virtual address change event
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  VirtualNotifyEvent,
                  NULL,
                  &gEfiEventVirtualAddressChangeGuid,
                  &mRtcVirtualAddrChangeEvent
                  );
  ASSERT_EFI_ERROR (Status);

  return Status;
}
