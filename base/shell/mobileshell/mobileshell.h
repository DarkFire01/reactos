/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <wingdi.h>
#include <winuser.h>
#include <winnls.h>
#include <strsafe.h>

#include "resource.h"

/* Dark theme */
#define MS_COLOR_BACKGROUND     RGB(0x12, 0x12, 0x14)
#define MS_COLOR_TILE           RGB(0x24, 0x25, 0x2A)
#define MS_COLOR_ACCENT         RGB(0x3A, 0x96, 0xDD)
#define MS_COLOR_TEXT           RGB(0xF2, 0xF2, 0xF4)
#define MS_COLOR_SUBTEXT        RGB(0x9A, 0x9B, 0xA3)
#define MS_COLOR_BAR            RGB(0x08, 0x08, 0x0A)
#define MS_COLOR_KEYBOARD       RGB(0x1A, 0x1A, 0x1E)
#define MS_COLOR_KEY            RGB(0x34, 0x35, 0x3B)
#define MS_COLOR_KEY_SPECIAL    RGB(0x26, 0x27, 0x2C)

#define MS_LAUNCHER_CLASS       L"ReactOSMobileShellLauncher"
#define MS_BAR_CLASS            L"ReactOSMobileShellBar"
#define MS_KEYBOARD_CLASS       L"ReactOSMobileShellKeyboard"

/* How often the bar and keyboard put themselves back on top */
#define MS_TOPMOST_INTERVAL_MS  500

extern HINSTANCE MsInstance;
extern HWND MsLauncherWindow;
extern HWND MsBarWindow;

/* main.c */
HFONT
NTAPI
MsCreateFont(
    _In_ INT Height,
    _In_ INT Weight);

VOID
NTAPI
MsFillRoundRect(
    _In_ HDC Dc,
    _In_ const RECT *Rect,
    _In_ INT Radius,
    _In_ COLORREF Color);

VOID
NTAPI
MsUpdateWorkArea(VOID);

/* battery.c */
BOOL
NTAPI
MsBatteryRead(
    _Out_ PULONG Percent,
    _Out_ PBOOL Charging);

/* keyboard.c */
BOOL
NTAPI
MsKeyboardRegister(VOID);

HWND
NTAPI
MsKeyboardCreate(
    _In_ INT Width,
    _In_ INT Bottom);

VOID
NTAPI
MsKeyboardToggle(VOID);

VOID
NTAPI
MsKeyboardDestroy(VOID);

INT
NTAPI
MsKeyboardVisibleHeight(VOID);

VOID
NTAPI
MsKeyboardKeepOnTop(VOID);
