/*
 * msosv10.h
 *
 * This file is part of the ReactOS PSDK package.
 *
 * Contributors:
 *   Adam Słaboń.
 *
 * THIS SOFTWARE IS NOT COPYRIGHTED
 *
 * This source code is offered for use in the public domain. You may
 * use, modify or distribute it freely.
 *
 * This code is distributed in the hope that it will be useful but
 * WITHOUT ANY WARRANTY. ALL WARRANTIES, EXPRESS OR IMPLIED ARE HEREBY
 * DISCLAIMED. This includes but is not limited to warranties of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 */

#pragma once

/* Helper macro to enable gcc's extension. */
#ifndef __GNU_EXTENSION
#ifdef __GNUC__
#define __GNU_EXTENSION __extension__
#else
#define __GNU_EXTENSION
#endif
#endif

#include <pshpack1.h>

typedef struct _OS_FEATURE_EXTENDED_COMPATIBLE_ID_DESCRIPTOR {
  ULONG dwLength;
  USHORT bcdVersion;
  USHORT wIndex;
  UCHAR bCount;
  UCHAR Reserved[7];
  UCHAR bFirstInterfaceNumber;
  UCHAR bNumInterfaces;
  CHAR compatibleID[8];
  CHAR subCompatibleID[8];
  UCHAR Reserved0[6];
} OS_FEATURE_EXTENDED_COMPATIBLE_ID_DESCRIPTOR, *POS_FEATURE_EXTENDED_COMPATIBLE_ID_DESCRIPTOR;

#include <poppack.h>
