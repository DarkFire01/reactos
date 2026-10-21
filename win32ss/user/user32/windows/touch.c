/*
 * PROJECT:     ReactOS user32.dll
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Touch injection, delivered as the mouse input touch is promoted to
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <user32.h>

/* user32 builds for an older WINVER than the pointer API, so the layouts it reads are spelled out here */
#define TOUCH_POINTER_TYPE          2       /* PT_TOUCH */
#define TOUCH_FLAG_PRIMARY          0x00002000
#define TOUCH_FLAG_DOWN             0x00010000
#define TOUCH_FLAG_UPDATE           0x00020000
#define TOUCH_FLAG_UP               0x00040000
#define TOUCH_CONTACTS_MAXIMUM      256
#define TOUCH_FEEDBACK_FIRST        1       /* TOUCH_FEEDBACK_DEFAULT */
#define TOUCH_FEEDBACK_LAST         3       /* TOUCH_FEEDBACK_NONE */

/* The layout of POINTER_TOUCH_INFO */
typedef struct _TOUCH_CONTACT
{
    DWORD PointerType;
    UINT32 PointerId;
    UINT32 FrameId;
    UINT32 PointerFlags;
    HANDLE SourceDevice;
    HWND Target;
    POINT PixelLocation;
    POINT HimetricLocation;
    POINT PixelLocationRaw;
    POINT HimetricLocationRaw;
    DWORD Time;
    UINT32 HistoryCount;
    INT32 InputData;
    DWORD KeyStates;
    UINT64 PerformanceCount;
    DWORD ButtonChangeType;
    UINT32 TouchFlags;
    UINT32 TouchMask;
    RECT Contact;
    RECT ContactRaw;
    UINT32 Orientation;
    UINT32 Pressure;
} TOUCH_CONTACT;

C_ASSERT(sizeof(TOUCH_CONTACT) == (sizeof(PVOID) == 8 ? 144 : 136));

/* The most contacts a frame may carry, 0 until InitializeTouchInjection succeeds */
static UINT32 TouchInjectionMaximum;

/* The contact that drives the mouse, and whether it is pressed */
static UINT32 TouchPromotedId;
static BOOL TouchPromotedDown;

/* One of down, update or up describes what a contact did in this frame */
static
BOOL
IsValidTouchContact(
    _In_ const TOUCH_CONTACT *Contact)
{
    UINT32 Change = Contact->PointerFlags & (TOUCH_FLAG_DOWN | TOUCH_FLAG_UPDATE | TOUCH_FLAG_UP);

    if (Contact->PointerType != TOUCH_POINTER_TYPE)
        return FALSE;
    return Change == TOUCH_FLAG_DOWN || Change == TOUCH_FLAG_UPDATE || Change == TOUCH_FLAG_UP;
}

/* Moves the mouse to the contact and presses or lets go of the left button with it */
static
BOOL
PromoteTouchContact(
    _In_ const TOUCH_CONTACT *Contact)
{
    UINT32 Flags = Contact->PointerFlags;
    LONG Left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    LONG Top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    LONG Width = max(GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1, 1);
    LONG Height = max(GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1, 1);
    INPUT Input;

    RtlZeroMemory(&Input, sizeof(Input));
    Input.type = INPUT_MOUSE;
    Input.mi.dx = MulDiv(Contact->PixelLocation.x - Left, 65535, Width);
    Input.mi.dy = MulDiv(Contact->PixelLocation.y - Top, 65535, Height);
    Input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;

    if ((Flags & TOUCH_FLAG_DOWN) && !TouchPromotedDown)
    {
        Input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
        TouchPromotedDown = TRUE;
    }
    else if ((Flags & TOUCH_FLAG_UP) && TouchPromotedDown)
    {
        Input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
        TouchPromotedDown = FALSE;
    }

    return SendInput(1, &Input, sizeof(Input)) == 1;
}

/**
 * @brief
 * Prepares the calling process to inject touch input.
 *
 * @param[in] maxCount
 * The most contacts one call to InjectTouchInput may carry, from 1 to MAX_TOUCH_COUNT.
 *
 * @param[in] dwMode
 * One of the TOUCH_FEEDBACK values.
 *
 * @return
 * TRUE on success, FALSE with ERROR_INVALID_PARAMETER for a bad count or mode.
 */
BOOL
WINAPI
InitializeTouchInjection(
    _In_ UINT32 maxCount,
    _In_ DWORD dwMode)
{
    if (maxCount < 1 || maxCount > TOUCH_CONTACTS_MAXIMUM ||
        dwMode < TOUCH_FEEDBACK_FIRST || dwMode > TOUCH_FEEDBACK_LAST)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    TouchInjectionMaximum = maxCount;
    return TRUE;
}

/**
 * @brief
 * Injects one frame of touch contacts. There is no touch input stack, so the primary contact
 * reaches applications the way Windows promotes touch to the mouse.
 *
 * @param[in] count
 * The number of contacts, at most the count given to InitializeTouchInjection.
 *
 * @param[in] contacts
 * The touch contacts of the frame, as POINTER_TOUCH_INFO.
 *
 * @return
 * TRUE on success, FALSE with ERROR_INVALID_PARAMETER when injection is not set up or a
 * contact is malformed.
 */
BOOL
WINAPI
InjectTouchInput(
    _In_ UINT32 count,
    _In_reads_(count) const VOID *contacts)
{
    const TOUCH_CONTACT *Contacts = contacts;
    const TOUCH_CONTACT *Primary = NULL;
    UINT32 i;

    if (!TouchInjectionMaximum || !count || count > TouchInjectionMaximum || !Contacts)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    for (i = 0; i < count; i++)
    {
        if (!IsValidTouchContact(&Contacts[i]))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        if (Contacts[i].PointerFlags & TOUCH_FLAG_PRIMARY)
            Primary = &Contacts[i];
    }

    /* A new primary contact takes the mouse over */
    if (!Primary)
        return TRUE;
    if (Primary->PointerId != TouchPromotedId && (Primary->PointerFlags & TOUCH_FLAG_DOWN))
        TouchPromotedId = Primary->PointerId;
    if (Primary->PointerId != TouchPromotedId)
        return TRUE;

    return PromoteTouchContact(Primary);
}

/* EOF */
