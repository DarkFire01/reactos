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

/* A hardware button event, posted to the bar by the buttons thread */
#define WM_MS_BUTTON            (WM_APP + 1)
#define MS_VOLUME_TIMER         3

/* A new battery reading for the launcher, see MsBatteryUnpack */
#define WM_MS_BATTERY           (WM_APP + 2)

/* The volume worker changed the level, so the bar repaints */
#define WM_MS_VOLUME            (WM_APP + 3)

#define MS_BATTERY_PACK(Present, Charging, Percent) \
    ((WPARAM)(((Present) ? 0x200 : 0) | ((Charging) ? 0x100 : 0) | ((Percent) & 0xFF)))
#define MS_BATTERY_PRESENT(Packed)  (((Packed) & 0x200) != 0)
#define MS_BATTERY_CHARGING(Packed) (((Packed) & 0x100) != 0)
#define MS_BATTERY_PERCENT(Packed)  ((ULONG)((Packed) & 0xFF))

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
VOID
NTAPI
MsBatteryStart(
    _In_ HWND Notify);

/* buttons.c */
VOID
NTAPI
MsButtonsStart(
    _In_ HWND Notify);

BOOL
NTAPI
MsButtonsHandle(
    _In_ HWND Bar,
    _In_ WPARAM State,
    _In_ LPARAM Usage);

VOID
NTAPI
MsButtonsHide(
    _In_ HWND Bar);

VOID
NTAPI
MsButtonsPaint(
    _In_ HDC Dc,
    _In_ const RECT *Area,
    _In_ HFONT Font);

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
