/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     A display, in the one mode everything can already talk to
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Text, eighty by twenty five, out of the page at B eight zero zero zero. Two
 * bytes a cell, the character and then what colour to draw it in.
 *
 * There is nothing to draw on, so what the guest writes is turned back into
 * lines and sent wherever the device was told to send them. That is enough to
 * watch an operating system come up, which is what a display is for at this
 * stage, and a window can be put on the other end later without the guest
 * knowing anything changed.
 */

/* INCLUDES *******************************************************************/

#include <windows.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtvm_device.h"

/* DEFINES ********************************************************************/

/* Where the text page is, and how much of it there is */
#define VIDEO_TEXT_BASE     0x000B8000
#define VIDEO_TEXT_SIZE     0x00008000

#define VIDEO_COLUMNS       80
#define VIDEO_ROWS          25
#define VIDEO_CELL_SIZE     2
#define VIDEO_PAGE_SIZE     (VIDEO_COLUMNS * VIDEO_ROWS * VIDEO_CELL_SIZE)

/* The registers, which are reached by naming one and then reading it */
#define VIDEO_CRTC_ADDRESS  0x03D4
#define VIDEO_CRTC_DATA     0x03D5
#define VIDEO_STATUS        0x03DA

/* The two that say where the cursor is */
#define CRTC_CURSOR_HIGH    0x0E
#define CRTC_CURSOR_LOW     0x0F
#define CRTC_START_HIGH     0x0C
#define CRTC_START_LOW      0x0D

#define CRTC_REGISTER_COUNT 32

/* How long the screen must be still before it is worth sending again */
#define VIDEO_SETTLE_MS     120

/* TYPES **********************************************************************/

typedef struct _VIDEO_DEVICE
{
    RTVM_DEVICE Device;

    /* The page itself, which the guest writes straight into */
    UCHAR Text[VIDEO_TEXT_SIZE];

    UCHAR CrtcAddress;
    UCHAR Crtc[CRTC_REGISTER_COUNT];

    /* Flipped every time the status register is read, because it has to be */
    UCHAR Retrace;

    /* What was last sent, so that only a screen that changed is sent again */
    UCHAR Sent[VIDEO_PAGE_SIZE];
    BOOLEAN Dirty;
    ULONG DirtyAt;

    HANDLE Output;
    HANDLE Painter;
    volatile LONG Stopping;

    CRITICAL_SECTION Lock;
    CHAR Backend[MAX_PATH];
} VIDEO_DEVICE, *PVIDEO_DEVICE;

/* GLOBALS ********************************************************************/

static const RTVM_DEVICE_VTABLE VideoVtable;

/* FUNCTIONS ******************************************************************/

static
VOID
VideoWrite(
    _In_ PVIDEO_DEVICE Video,
    _In_reads_bytes_(Length) const VOID *Buffer,
    _In_ ULONG Length)
{
    DWORD Written;

    if (Video->Output != INVALID_HANDLE_VALUE)
        WriteFile(Video->Output, Buffer, Length, &Written, NULL);
}

/**
 * @brief
 * Hands the page to whatever the manager has for a display.
 *
 * @remarks
 * Every look, not only the ones that settle: a window is watched while it
 * changes, and a screen being rewritten is exactly what the operator wants to
 * see happening. The log is the one that waits for it to hold still.
 */
static
VOID
VideoPresent(
    _Inout_ PVIDEO_DEVICE Video)
{
    const RTVM_HOST_INTERFACE *Host = Video->Device.Host;
    RTVM_TEXT_PAGE Page;
    ULONG Offset;

    if (!RTVM_CARRIES(Host, RTVM_HOST_INTERFACE, PresentText))
        return;

    Offset = ((ULONG)Video->Crtc[CRTC_CURSOR_HIGH] << 8) |
             Video->Crtc[CRTC_CURSOR_LOW];

    Page.Size = sizeof(Page);
    Page.Columns = VIDEO_COLUMNS;
    Page.Rows = VIDEO_ROWS;
    Page.CursorColumn = Offset % VIDEO_COLUMNS;
    Page.CursorRow = Offset / VIDEO_COLUMNS;
    Page.CursorVisible = (Page.CursorRow < VIDEO_ROWS);
    Page.Cells = Video->Text;

    Host->PresentText(Host->Context, &Page);
}

/**
 * @brief
 * Turns the page back into lines and sends them.
 *
 * @remarks
 * Trailing blanks are dropped, because a screen is mostly empty and eighty
 * columns of nothing on every line would bury what is actually on it.
 */
static
VOID
VideoPaint(
    _Inout_ PVIDEO_DEVICE Video)
{
    CHAR Line[VIDEO_COLUMNS + 3];
    CHAR Header[64];
    ULONG Row;
    ULONG Column;
    ULONG Length;

    StringCchPrintfA(Header, sizeof(Header),
                     "\r\n--- screen ---------------------------------\r\n");
    VideoWrite(Video, Header, (ULONG)strlen(Header));

    for (Row = 0; Row < VIDEO_ROWS; Row++)
    {
        Length = 0;

        for (Column = 0; Column < VIDEO_COLUMNS; Column++)
        {
            UCHAR Character = Video->Text[(Row * VIDEO_COLUMNS + Column) *
                                          VIDEO_CELL_SIZE];

            /* Anything that is not a printable character is shown as a space */
            if ((Character < 0x20) || (Character > 0x7E))
                Character = ' ';

            Line[Length++] = (CHAR)Character;
        }

        while ((Length > 0) && (Line[Length - 1] == ' '))
            Length--;

        Line[Length++] = '\r';
        Line[Length++] = '\n';

        VideoWrite(Video, Line, Length);
    }

    memcpy(Video->Sent, Video->Text, VIDEO_PAGE_SIZE);
    Video->Dirty = FALSE;
}

/**
 * @brief
 * Sends the screen once it has stopped changing.
 *
 * @remarks
 * A guest clearing the screen writes two thousand cells one at a time, and
 * sending after each would be unreadable. Waiting for it to go quiet turns
 * that into one screen.
 */
static
DWORD
WINAPI
VideoPainter(
    _In_ LPVOID Parameter)
{
    PVIDEO_DEVICE Video = (PVIDEO_DEVICE)Parameter;
    const RTVM_HOST_INTERFACE *Host = Video->Device.Host;

    while (InterlockedCompareExchange(&Video->Stopping, 0, 0) == 0)
    {
        Sleep(40);

        EnterCriticalSection(&Video->Lock);

        /* Whatever the guest has put there since the last look */
        if (Host->ReadGuestMemory(Host->Context, VIDEO_TEXT_BASE,
                                  Video->Text, VIDEO_PAGE_SIZE) == RtvmOk)
        {
            VideoPresent(Video);

            if (memcmp(Video->Sent, Video->Text, VIDEO_PAGE_SIZE) != 0)
            {
                /*
                 * Changed, but not drawn straight away: a screen being
                 * rewritten changes constantly, and one drawn per change would
                 * be unreadable. It is drawn once it holds still.
                 */
                if (!Video->Dirty)
                {
                    Video->Dirty = TRUE;
                    Video->DirtyAt = GetTickCount();
                }
                else if ((GetTickCount() - Video->DirtyAt) >= VIDEO_SETTLE_MS)
                {
                    VideoPaint(Video);
                }
            }
            else
            {
                Video->Dirty = FALSE;
            }
        }

        LeaveCriticalSection(&Video->Lock);
    }

    return 0;
}

/* THE DEVICE *****************************************************************/

static
RTVM_STATUS
RTVMAPI
VideoStart(
    _In_ PRTVM_DEVICE Device)
{
    PVIDEO_DEVICE Video = (PVIDEO_DEVICE)Device->DeviceContext;
    const RTVM_HOST_INTERFACE *Host = Device->Host;
    RTVM_STATUS Status;

    /*
     * The page is deliberately not claimed. Claiming it would turn every write
     * into an exit for the manager to carry out, and a guest clears a screen
     * with one repeated store of two thousand cells: without an instruction
     * decoder behind it nothing would be written at all, and the instruction
     * would never finish.
     *
     * Reading the page instead costs one copy per repaint and works with
     * whatever the guest chooses to write it with. A display that had to know
     * the moment a pixel changed would need something cleverer; a page of text
     * does not.
     */
    Status = Host->ClaimPortRange(Host->Context, Device, VIDEO_CRTC_ADDRESS, 2);
    if (!RTVM_SUCCESS(Status))
        return Status;

    Status = Host->ClaimPortRange(Host->Context, Device, VIDEO_STATUS, 1);
    if (!RTVM_SUCCESS(Status))
        return Status;

    Video->Painter = CreateThread(NULL, 0, VideoPainter, Video, 0, NULL);

    Host->Log(Host->Context, RtvmLogInfo,
              "%s: %ux%u text at %08x, %s\n",
              Device->Name,
              VIDEO_COLUMNS,
              VIDEO_ROWS,
              VIDEO_TEXT_BASE,
              Video->Backend);

    return RtvmOk;
}

static
VOID
RTVMAPI
VideoStop(
    _In_ PRTVM_DEVICE Device)
{
    PVIDEO_DEVICE Video = (PVIDEO_DEVICE)Device->DeviceContext;

    InterlockedExchange(&Video->Stopping, 1);

    if (Video->Painter != NULL)
    {
        WaitForSingleObject(Video->Painter, 2000);
        CloseHandle(Video->Painter);
        Video->Painter = NULL;
    }

    /* Whatever is on it when the machine stops is worth having */
    EnterCriticalSection(&Video->Lock);

    if (Device->Host->ReadGuestMemory(Device->Host->Context, VIDEO_TEXT_BASE,
                                      Video->Text, VIDEO_PAGE_SIZE) == RtvmOk)
    {
        if (memcmp(Video->Sent, Video->Text, VIDEO_PAGE_SIZE) != 0)
            VideoPaint(Video);
    }

    LeaveCriticalSection(&Video->Lock);
}

static
VOID
RTVMAPI
VideoReset(
    _In_ PRTVM_DEVICE Device)
{
    PVIDEO_DEVICE Video = (PVIDEO_DEVICE)Device->DeviceContext;
    ULONG Index;

    EnterCriticalSection(&Video->Lock);

    /* Blank, in the colour a screen comes up in */
    for (Index = 0; Index < VIDEO_TEXT_SIZE; Index += VIDEO_CELL_SIZE)
    {
        Video->Text[Index] = ' ';
        Video->Text[Index + 1] = 0x07;
    }

    memset(Video->Crtc, 0, sizeof(Video->Crtc));
    Video->CrtcAddress = 0;
    Video->Retrace = 0;
    Video->Dirty = FALSE;

    memcpy(Video->Sent, Video->Text, VIDEO_PAGE_SIZE);

    LeaveCriticalSection(&Video->Lock);
}

static
VOID
RTVMAPI
VideoDestroy(
    _In_ PRTVM_DEVICE Device)
{
    PVIDEO_DEVICE Video = (PVIDEO_DEVICE)Device->DeviceContext;

    VideoStop(Device);

    if (Video->Output != INVALID_HANDLE_VALUE)
        CloseHandle(Video->Output);

    DeleteCriticalSection(&Video->Lock);
    free(Video);
}

static
RTVM_STATUS
RTVMAPI
VideoIoRead(
    _In_ PRTVM_DEVICE Device,
    _In_ USHORT Port,
    _In_ ULONG Width,
    _Out_ PULONG Value)
{
    PVIDEO_DEVICE Video = (PVIDEO_DEVICE)Device->DeviceContext;
    UCHAR Result = 0;

    UNREFERENCED_PARAMETER(Width);

    EnterCriticalSection(&Video->Lock);

    switch (Port)
    {
        case VIDEO_CRTC_ADDRESS:
            Result = Video->CrtcAddress;
            break;

        case VIDEO_CRTC_DATA:
            if (Video->CrtcAddress < CRTC_REGISTER_COUNT)
                Result = Video->Crtc[Video->CrtcAddress];
            break;

        case VIDEO_STATUS:
            /*
             * Something is always about to happen. A guest waiting for the
             * beam to come back round is waiting on this, and a register that
             * never changes is one it waits on forever.
             */
            Video->Retrace ^= 0x09;
            Result = Video->Retrace;
            break;
    }

    LeaveCriticalSection(&Video->Lock);

    *Value = Result;
    return RtvmOk;
}

static
RTVM_STATUS
RTVMAPI
VideoIoWrite(
    _In_ PRTVM_DEVICE Device,
    _In_ USHORT Port,
    _In_ ULONG Width,
    _In_ ULONG Value)
{
    PVIDEO_DEVICE Video = (PVIDEO_DEVICE)Device->DeviceContext;

    UNREFERENCED_PARAMETER(Width);

    EnterCriticalSection(&Video->Lock);

    switch (Port)
    {
        case VIDEO_CRTC_ADDRESS:
            Video->CrtcAddress = (UCHAR)(Value & 0xFF);
            break;

        case VIDEO_CRTC_DATA:
            if (Video->CrtcAddress < CRTC_REGISTER_COUNT)
                Video->Crtc[Video->CrtcAddress] = (UCHAR)(Value & 0xFF);
            break;
    }

    LeaveCriticalSection(&Video->Lock);
    return RtvmOk;
}

static const RTVM_DEVICE_VTABLE VideoVtable =
{
    sizeof(VideoVtable),
    VideoStart,
    VideoStop,
    VideoReset,
    VideoDestroy,
    VideoIoRead,
    VideoIoWrite,
    NULL,
    NULL,
    NULL
};

/* MAKING ONE *****************************************************************/

static
PCSTR
VideoSetting(
    _In_opt_ PCSTR Parameters,
    _In_ PCSTR Name)
{
    SIZE_T Length = strlen(Name);
    PCSTR Walk = Parameters;

    while ((Walk != NULL) && (*Walk != '\0'))
    {
        if ((_strnicmp(Walk, Name, Length) == 0) &&
            ((Walk[Length] == '=') || (Walk[Length] == ',') || (Walk[Length] == '\0')))
        {
            return (Walk[Length] == '=') ? &Walk[Length + 1] : &Walk[Length];
        }

        Walk = strchr(Walk, ',');
        if (Walk != NULL)
            Walk++;
    }

    return NULL;
}

static
VOID
VideoCopySetting(
    _In_ PCSTR Value,
    _Out_writes_z_(Size) PSTR Buffer,
    _In_ SIZE_T Size)
{
    SIZE_T Index = 0;

    while ((Index + 1 < Size) && (Value[Index] != '\0') && (Value[Index] != ','))
    {
        Buffer[Index] = Value[Index];
        Index++;
    }

    Buffer[Index] = '\0';
}

static
RTVM_STATUS
RTVMAPI
VideoCreate(
    _In_ const RTVM_HOST_INTERFACE *Host,
    _In_opt_ PCSTR Parameters,
    _Outptr_ PRTVM_DEVICE *Device)
{
    PVIDEO_DEVICE Video;
    CHAR Path[MAX_PATH];
    PCSTR Value;

    Video = (PVIDEO_DEVICE)calloc(1, sizeof(*Video));
    if (Video == NULL)
        return RtvmNoMemory;

    Video->Device.Size = sizeof(Video->Device);
    Video->Device.Vtable = &VideoVtable;
    Video->Device.Host = Host;
    Video->Device.DeviceContext = Video;
    Video->Output = INVALID_HANDLE_VALUE;
    StringCchCopyA(Video->Device.Name, sizeof(Video->Device.Name), "video");

    Value = VideoSetting(Parameters, "file");
    if ((Value != NULL) && (*Value != '\0'))
    {
        VideoCopySetting(Value, Path, sizeof(Path));

        Video->Output = CreateFileA(Path,
                                    FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL,
                                    OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    NULL);
    }

    if (Video->Output != INVALID_HANDLE_VALUE)
        StringCchPrintfA(Video->Backend, sizeof(Video->Backend), "drawn to %s", Path);
    else
        StringCchCopyA(Video->Backend, sizeof(Video->Backend), "not drawn anywhere");

    InitializeCriticalSection(&Video->Lock);
    VideoReset(&Video->Device);

    *Device = &Video->Device;
    return RtvmOk;
}

/* WHAT THIS MODULE HAS *******************************************************/

static const RTVM_DEVICE_CLASS VideoClasses[] =
{
    {
        "video",
        "Eighty by twenty five text, drawn wherever it is told",
        VideoCreate
    }
};

static const RTVM_DEVICE_MODULE VideoModule =
{
    sizeof(VideoModule),
    RTVM_DEVICE_ABI_VERSION,
    "rtvmvideo",
    "Displays",
    RTL_NUMBER_OF(VideoClasses),
    VideoClasses
};

RTVM_DEVICE_EXPORT
const RTVM_DEVICE_MODULE *
RTVMAPI
RtvmDeviceModuleEntry(
    _In_ ULONG HostAbiVersion)
{
    if (HostAbiVersion != RTVM_DEVICE_ABI_VERSION)
        return NULL;

    return &VideoModule;
}

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ DWORD Reason,
    _In_ LPVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);

    return TRUE;
}

/* EOF */
