/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hardware volume and camera keys, the volume level pill on the bar
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "mobileshell.h"
#include <winioctl.h>
#include <mmsystem.h>
#include <qcom/buttonsif.h>

#define MS_VOLUME_STEP          10
#define MS_VOLUME_SHOW_MS       1500
#define MS_BUTTONS_RETRY_MS     3000

static ULONG MsVolumePercent;
static BOOL MsVolumeKnown;
static BOOL MsVolumeShown;
static BOOL MsVolumeNoDevice;

/* Steps the volume worker still has to apply, up is positive */
static volatile LONG MsVolumePendingSteps;
static HANDLE MsVolumeWake;
static HWND MsVolumeBar;

static
DWORD
WINAPI
MsVolumeThread(
    _In_ LPVOID Parameter);

/* The buttons device shows up once its driver starts, so keep trying to open it */
static
DWORD
WINAPI
MsButtonsThread(
    _In_ LPVOID Parameter)
{
    HWND Notify = Parameter;
    QCOM_BUTTON_EVENT Event;
    HANDLE Device;
    DWORD Returned;

    for (;;)
    {
        Device = CreateFileW(QCOM_BUTTONS_DEVICE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL, OPEN_EXISTING, 0, NULL);
        if (Device == INVALID_HANDLE_VALUE)
        {
            Sleep(MS_BUTTONS_RETRY_MS);
            continue;
        }

        while (DeviceIoControl(Device, IOCTL_QCOM_BUTTONS_WAIT, NULL, 0, &Event, sizeof(Event), &Returned, NULL))
        {
            if (Returned >= sizeof(Event))
                PostMessageW(Notify, WM_MS_BUTTON, Event.State, MAKELPARAM(Event.Usage, Event.UsagePage));
        }

        CloseHandle(Device);
        Sleep(MS_BUTTONS_RETRY_MS);
    }
}

VOID
NTAPI
MsButtonsStart(
    _In_ HWND Notify)
{
    HANDLE Thread = CreateThread(NULL, 0, MsButtonsThread, Notify, 0, NULL);

    if (Thread)
        CloseHandle(Thread);

    MsVolumeBar = Notify;
    MsVolumeWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!MsVolumeWake)
        return;

    Thread = CreateThread(NULL, 0, MsVolumeThread, NULL, 0, NULL);
    if (Thread)
    {
        CloseHandle(Thread);
    }
    else
    {
        CloseHandle(MsVolumeWake);
        MsVolumeWake = NULL;
    }
}

/* Steps the first output device, the one the wave mapper plays on */
static
VOID
MsVolumeStep(
    _In_ BOOL Up)
{
    DWORD Value;
    ULONG Percent;

    if (!waveOutGetNumDevs() || waveOutGetVolume((HWAVEOUT)0, &Value) != MMSYSERR_NOERROR)
    {
        MsVolumeNoDevice = TRUE;
        return;
    }

    MsVolumeNoDevice = FALSE;
    Percent = ((Value & 0xFFFF) * 100 + 0x7FFF) / 0xFFFF;
    Percent = (Percent + MS_VOLUME_STEP / 2) / MS_VOLUME_STEP * MS_VOLUME_STEP;

    if (Up)
        Percent = min(Percent + MS_VOLUME_STEP, 100);
    else
        Percent = (Percent > MS_VOLUME_STEP) ? Percent - MS_VOLUME_STEP : 0;

    Value = 0xFFFF * Percent / 100;
    waveOutSetVolume((HWAVEOUT)0, Value | (Value << 16));

    MsVolumePercent = Percent;
    MsVolumeKnown = TRUE;
}

/* The wave mapper may block while the audio stack is missing, so the level changes away from the bar */
static
DWORD
WINAPI
MsVolumeThread(
    _In_ LPVOID Parameter)
{
    LONG Steps;

    UNREFERENCED_PARAMETER(Parameter);

    for (;;)
    {
        WaitForSingleObject(MsVolumeWake, INFINITE);

        Steps = InterlockedExchange(&MsVolumePendingSteps, 0);
        for (; Steps > 0; Steps--)
            MsVolumeStep(TRUE);
        for (; Steps < 0; Steps++)
            MsVolumeStep(FALSE);

        PostMessageW(MsVolumeBar, WM_MS_VOLUME, 0, 0);
    }
}

/* Returns TRUE when the bar has something new to show */
BOOL
NTAPI
MsButtonsHandle(
    _In_ HWND Bar,
    _In_ WPARAM State,
    _In_ LPARAM Usage)
{
    BOOL Up;

    if (HIWORD(Usage) != QCOM_BUTTON_PAGE_CONSUMER || State == QCOM_BUTTON_RELEASED)
        return FALSE;

    if (LOWORD(Usage) != QCOM_BUTTON_VOLUME_UP && LOWORD(Usage) != QCOM_BUTTON_VOLUME_DOWN)
        return FALSE;

    Up = (LOWORD(Usage) == QCOM_BUTTON_VOLUME_UP);

    /* Apps see the key as on any keyboard, the shell owns the level itself */
    keybd_event(Up ? VK_VOLUME_UP : VK_VOLUME_DOWN, 0, 0, 0);
    keybd_event(Up ? VK_VOLUME_UP : VK_VOLUME_DOWN, 0, KEYEVENTF_KEYUP, 0);

    if (MsVolumeWake)
    {
        InterlockedAdd(&MsVolumePendingSteps, Up ? 1 : -1);
        SetEvent(MsVolumeWake);
    }
    else
    {
        MsVolumeNoDevice = TRUE;
    }
    MsVolumeShown = TRUE;
    SetTimer(Bar, MS_VOLUME_TIMER, MS_VOLUME_SHOW_MS, NULL);
    return TRUE;
}

VOID
NTAPI
MsButtonsHide(
    _In_ HWND Bar)
{
    KillTimer(Bar, MS_VOLUME_TIMER);
    MsVolumeShown = FALSE;
}

/* The level pill, drawn on the left of the bar while a volume key was pressed recently */
VOID
NTAPI
MsButtonsPaint(
    _In_ HDC Dc,
    _In_ const RECT *Area,
    _In_ HFONT Font)
{
    WCHAR Text[32];
    RECT Pill, Track, Level, Label;
    INT Height, Pad;

    if (!MsVolumeShown || Area->right - Area->left < 40)
        return;

    Pill = *Area;
    Height = Pill.bottom - Pill.top;
    Pad = Height / 4;
    MsFillRoundRect(Dc, &Pill, Height, MS_COLOR_TILE);

    if (MsVolumeNoDevice || !MsVolumeKnown)
        StringCchCopyW(Text, ARRAYSIZE(Text), L"No sound");
    else
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu%%", MsVolumePercent);

    Label = Pill;
    Label.left += Pad;
    Label.right = Label.left + (Pill.right - Pill.left) / 3;
    SetBkMode(Dc, TRANSPARENT);
    SetTextColor(Dc, MS_COLOR_TEXT);
    SelectObject(Dc, Font);
    DrawTextW(Dc, Text, -1, &Label, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    if (MsVolumeNoDevice || !MsVolumeKnown)
        return;

    Track.left = Label.right + Pad;
    Track.right = Pill.right - Pad * 2;
    Track.top = Pill.top + Height / 2 - Pad / 2;
    Track.bottom = Track.top + Pad;
    if (Track.right <= Track.left)
        return;

    MsFillRoundRect(Dc, &Track, Pad, MS_COLOR_KEY);

    Level = Track;
    Level.right = Track.left + (Track.right - Track.left) * (LONG)MsVolumePercent / 100;
    if (Level.right > Level.left)
        MsFillRoundRect(Dc, &Level, Pad, MS_COLOR_ACCENT);
}
