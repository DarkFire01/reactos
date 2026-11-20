/*
 * PROJECT:     ReactOS Power Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winsvc.h>
#define NTOS_MODE_USER
#include <ndk/pofuncs.h>
#include <ndk/rtlfuncs.h>
#include <ndk/setypes.h>
#include <strsafe.h>

#define POWER_SERVICE_NAME          L"PowerSvc"

/* Registry keys hold GUIDs as 36 characters, without braces */
#define POWER_GUID_CHARS            36

/* main.c */
extern HANDLE PwStopEvent;

/* scheme.c */
BOOL
PwSchemeInitialize(VOID);

VOID
PwSchemeUninitialize(VOID);

VOID
PwSchemePushAll(VOID);

HANDLE
PwSchemeChangeEvent(VOID);

BOOL
PwSchemeWatch(VOID);
