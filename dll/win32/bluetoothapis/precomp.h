/*
 * PROJECT:     ReactOS Bluetooth API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Shared declarations
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef _BLUETOOTHAPIS_PCH_
#define _BLUETOOTHAPIS_PCH_

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winnls.h>
#include <winreg.h>
#include <winuser.h>
#include <winioctl.h>
#include <setupapi.h>
#include <bthsdpdef.h>
#include <bluetoothapis.h>
#include <bthioctl.h>

/* radio.c */
DWORD
BthpQueryLocalRadio(
    _In_ HANDLE Radio,
    _Out_ PBTH_LOCAL_RADIO_INFO Info);

DWORD
BthpForwardToRadio(
    _In_ HANDLE Radio);

HANDLE
BthpOpenAnyRadio(VOID);

#endif /* _BLUETOOTHAPIS_PCH_ */
