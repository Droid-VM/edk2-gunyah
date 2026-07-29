/** @file
  Serial port hook.

  Copyright (c) DroidVM contributors. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Base.h>
#include <Library/BaseLib.h>

VOID SerialConvertWriteChar(IN UINT8 *Ch) {

}

VOID SerialConvertReadChar(IN UINT8 *Ch) {
  switch (*Ch) {
    case 0xa: //
      *Ch = 0xd;
      break;
    default:;
  }
}
