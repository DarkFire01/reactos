/*
 * spbfuncenum.h
 *
 * Index of each SpbCx API within the class extension's function table.
 *
 * This file is part of the ReactOS DDK package.
 *
 * Contributors:
 *   Created by Justin Miller <justinmiller100@gmail.com>
 *
 * THIS SOFTWARE IS NOT COPYRIGHTED
 *
 * This source code is offered for use in the public domain. You may
 * use, modify or distribute it freely.
 *
 * This code is distributed in the hope that it will be useful but
 * WITHOUT ANY WARRANTY. ALL WARRANTIES, EXPRESS OR IMPLIED ARE HEREBY
 * DISCLAIMED. This includes but is not limited to warranties of
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 */

#ifndef _SPBFUNCENUM_H_
#define _SPBFUNCENUM_H_

/*
 * The order here is the wire format between a client and SpbCx: the client's
 * stub asks the class extension to fill a table of this many slots, and every
 * SpbXxx() inline in <spbcx.h> indexes into it. Never reorder or insert, since a
 * new API goes on the end and bumps the minor version.
 *
 * SpbCx v1.0 and v1.1 both publish these thirteen; the reference's CxBindClient
 * checks for exactly that count on either version.
 */
typedef enum _SPBFUNCENUM {
  SpbDeviceInitConfigTableIndex = 0,
  SpbDeviceInitializeTableIndex = 1,
  SpbControllerSetIoOtherCallbackTableIndex = 2,
  SpbControllerSetRequestAttributesTableIndex = 3,
  SpbControllerSetTargetAttributesTableIndex = 4,
  SpbTargetGetConnectionParametersTableIndex = 5,
  SpbTargetGetFileObjectTableIndex = 6,
  SpbRequestGetTargetTableIndex = 7,
  SpbRequestGetControllerTableIndex = 8,
  SpbRequestGetParametersTableIndex = 9,
  SpbRequestGetTransferParametersTableIndex = 10,
  SpbRequestCompleteTableIndex = 11,
  SpbRequestCaptureIoOtherTransferListTableIndex = 12,
  SpbFunctionTableNumEntries = 13,
} SPBFUNCENUM;

extern PSPB_DRIVER_GLOBALS SpbDriverGlobals;

#endif /* _SPBFUNCENUM_H_ */
