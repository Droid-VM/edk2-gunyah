/** @file

  Copyright (c) 2011 - 2014, ARM Limited. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef __PL030_REAL_TIME_CLOCK_H__
#define __PL030_REAL_TIME_CLOCK_H__

// PL030 Registers
#define PL030_RTC_DR_DATA_REGISTER                  0x000
#define PL030_RTC_MR_MATCH_REGISTER                 0x004
#define PL030_RTC_EOI_REGISTER                      0x008
#define PL030_RTC_LR_LOAD_REGISTER                  0x00C
#define PL030_RTC_CR_CONTROL_REGISTER               0x010

// PL030 Values
#define PL030_SET_IRQ_MASK   0x00000001
#define PL030_IRQ_TRIGGERED  0x00000001

#define PL030_COUNTS_PER_SECOND  1

#endif
