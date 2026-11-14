/*
 * PROJECT:     ReactOS Win32k subsystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Touch screen input and WM_TOUCH
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <win32k.h>
DBG_DEFAULT_CHANNEL(UserInput);

#define HID_PAGE_GENERIC            0x01
#define HID_PAGE_DIGITIZER          0x0D
#define HID_USAGE_X                 0x30
#define HID_USAGE_Y                 0x31
#define HID_USAGE_TOUCH_SCREEN      0x04
#define HID_USAGE_FINGER            0x22
#define HID_USAGE_TIP_SWITCH        0x42
#define HID_USAGE_CONTACT_ID        0x51
#define HID_USAGE_CONTACT_COUNT     0x54

/* Mouse messages promoted from touch carry this in their extra info */
#define TOUCH_PROMOTED_EXTRA_INFO   0xFF515780

#define TOUCH_MAX_CONTACTS          16
#define TOUCH_MAX_USAGES            32

typedef struct _TOUCH_FINGER_LINK
{
    USHORT LinkCollection;
    LONG MinX;
    LONG MaxX;
    LONG MinY;
    LONG MaxY;
} TOUCH_FINGER_LINK, *PTOUCH_FINGER_LINK;

typedef struct _TOUCH_SAMPLE
{
    ULONG ContactId;
    BOOLEAN Tip;
    POINT Point;
} TOUCH_SAMPLE, *PTOUCH_SAMPLE;

typedef struct _TOUCH_DEVICE
{
    ULONG FingerCount;
    TOUCH_FINGER_LINK Fingers[TOUCH_MAX_CONTACTS];

    /* Hybrid digitizers spread one frame over several reports */
    ULONG Expected;
    ULONG Collected;
    TOUCH_SAMPLE Frame[TOUCH_MAX_CONTACTS];
} TOUCH_DEVICE, *PTOUCH_DEVICE;

typedef struct _TOUCH_CONTACT
{
    BOOLEAN Active;
    BOOLEAN Primary;
    BOOLEAN Seen;
    PINPUT_DEVICE_INFO Device;
    ULONG ContactId;
    DWORD TouchId;
    DWORD Flags;
    HWND hwndTarget;
    POINT Point;
} TOUCH_CONTACT, *PTOUCH_CONTACT;

/* A HTOUCHINPUT, owned by the thread of the window it was posted to */
typedef struct _TOUCH_INPUT_OBJECT
{
    THROBJHEAD head;
    UINT Count;
    TOUCHINPUT Inputs[ANYSIZE_ARRAY];
} TOUCH_INPUT_OBJECT, *PTOUCH_INPUT_OBJECT;

static TOUCH_CONTACT gTouchContacts[TOUCH_MAX_CONTACTS];
static DWORD gNextTouchId = 1;

/**
 * @brief Finds the finger collections of a touch screen and their X and Y ranges.
 */
static
BOOLEAN
TouchDescribeFingers(
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_ PHIDP_CAPS Caps,
    _Out_ PTOUCH_DEVICE Touch)
{
    PHIDP_LINK_COLLECTION_NODE Nodes;
    HIDP_VALUE_CAPS ValueCaps;
    ULONG NodeCount = Caps->NumberLinkCollectionNodes;
    USHORT CapsCount;
    ULONG Index;
    PTOUCH_FINGER_LINK Finger;

    RtlZeroMemory(Touch, sizeof(*Touch));

    if (NodeCount == 0)
        return FALSE;

    Nodes = ExAllocatePoolWithTag(PagedPool, NodeCount * sizeof(*Nodes), USERTAG_TOUCHINPUTINFO);
    if (!Nodes)
        return FALSE;

    if (HidP_GetLinkCollectionNodes(Nodes, &NodeCount, PreparsedData) != HIDP_STATUS_SUCCESS)
    {
        ExFreePoolWithTag(Nodes, USERTAG_TOUCHINPUTINFO);
        return FALSE;
    }

    for (Index = 0; Index < NodeCount && Touch->FingerCount < TOUCH_MAX_CONTACTS; Index++)
    {
        if (Nodes[Index].LinkUsagePage != HID_PAGE_DIGITIZER || Nodes[Index].LinkUsage != HID_USAGE_FINGER)
            continue;

        Finger = &Touch->Fingers[Touch->FingerCount];
        Finger->LinkCollection = (USHORT)Index;

        CapsCount = 1;
        if (HidP_GetSpecificValueCaps(HidP_Input, HID_PAGE_GENERIC, (USHORT)Index, HID_USAGE_X,
                                      &ValueCaps, &CapsCount, PreparsedData) != HIDP_STATUS_SUCCESS)
            continue;
        Finger->MinX = ValueCaps.LogicalMin;
        Finger->MaxX = ValueCaps.LogicalMax;

        CapsCount = 1;
        if (HidP_GetSpecificValueCaps(HidP_Input, HID_PAGE_GENERIC, (USHORT)Index, HID_USAGE_Y,
                                      &ValueCaps, &CapsCount, PreparsedData) != HIDP_STATUS_SUCCESS)
            continue;
        Finger->MinY = ValueCaps.LogicalMin;
        Finger->MaxY = ValueCaps.LogicalMax;

        if (Finger->MaxX <= Finger->MinX || Finger->MaxY <= Finger->MinY)
            continue;

        Touch->FingerCount++;
    }

    ExFreePoolWithTag(Nodes, USERTAG_TOUCHINPUTINFO);
    return Touch->FingerCount != 0;
}

/**
 * @brief Decides whether a HID collection is a touch screen and prepares it for reading.
 */
BOOLEAN
NTAPI
UserInitTouchDevice(
    _Inout_ PINPUT_DEVICE_INFO DeviceInfo)
{
    PHID_DEVICE_INFO Hid = &DeviceInfo->Hid;
    PTOUCH_DEVICE Touch;

    if (Hid->Caps.UsagePage != HID_PAGE_DIGITIZER || Hid->Caps.Usage != HID_USAGE_TOUCH_SCREEN)
        return FALSE;

    if (Hid->Caps.InputReportByteLength == 0)
        return FALSE;

    Touch = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Touch), USERTAG_TOUCHINPUTINFO);
    if (!Touch)
        return FALSE;

    if (!TouchDescribeFingers(Hid->PreparsedData, &Hid->Caps, Touch))
    {
        ExFreePoolWithTag(Touch, USERTAG_TOUCHINPUTINFO);
        return FALSE;
    }

    Hid->ReportLength = Hid->Caps.InputReportByteLength;
    Hid->ReportBuffer = ExAllocatePoolWithTag(NonPagedPool, Hid->ReportLength, USERTAG_TOUCHINPUTINFO);
    if (!Hid->ReportBuffer)
    {
        ExFreePoolWithTag(Touch, USERTAG_TOUCHINPUTINFO);
        return FALSE;
    }

    Hid->TouchContext = Touch;
    TRACE("Touch screen '%wZ' with %lu contacts\n", &DeviceInfo->DeviceName, Touch->FingerCount);
    return TRUE;
}

VOID
NTAPI
UserCleanupTouchDevice(
    _Inout_ PINPUT_DEVICE_INFO DeviceInfo)
{
    ULONG Index;

    /* Contacts of a vanished device cannot end on their own */
    for (Index = 0; Index < TOUCH_MAX_CONTACTS; Index++)
    {
        if (gTouchContacts[Index].Device == DeviceInfo)
            RtlZeroMemory(&gTouchContacts[Index], sizeof(gTouchContacts[Index]));
    }

    if (DeviceInfo->Hid.TouchContext)
        ExFreePoolWithTag(DeviceInfo->Hid.TouchContext, USERTAG_TOUCHINPUTINFO);
    if (DeviceInfo->Hid.ReportBuffer)
        ExFreePoolWithTag(DeviceInfo->Hid.ReportBuffer, USERTAG_TOUCHINPUTINFO);

    DeviceInfo->Hid.TouchContext = NULL;
    DeviceInfo->Hid.ReportBuffer = NULL;
}

static
LONG
TouchScale(
    _In_ LONG Value,
    _In_ LONG Minimum,
    _In_ LONG Maximum,
    _In_ LONG Pixels)
{
    if (Value <= Minimum)
        return 0;
    if (Value >= Maximum)
        return Pixels - 1;

    return (LONG)(((LONGLONG)(Value - Minimum) * Pixels) / ((LONGLONG)Maximum - Minimum + 1));
}

static
PTOUCH_CONTACT
TouchFindContact(
    _In_ PINPUT_DEVICE_INFO DeviceInfo,
    _In_ ULONG ContactId)
{
    ULONG Index;

    for (Index = 0; Index < TOUCH_MAX_CONTACTS; Index++)
    {
        if (gTouchContacts[Index].Active &&
            gTouchContacts[Index].Device == DeviceInfo &&
            gTouchContacts[Index].ContactId == ContactId)
        {
            return &gTouchContacts[Index];
        }
    }

    return NULL;
}

static
BOOLEAN
TouchAnyActive(VOID)
{
    ULONG Index;

    for (Index = 0; Index < TOUCH_MAX_CONTACTS; Index++)
    {
        if (gTouchContacts[Index].Active)
            return TRUE;
    }

    return FALSE;
}

static
PTOUCH_CONTACT
TouchStartContact(
    _In_ PINPUT_DEVICE_INFO DeviceInfo,
    _In_ PTOUCH_SAMPLE Sample)
{
    PTOUCH_CONTACT Contact = NULL;
    USHORT HitTest;
    PWND pwnd;
    ULONG Index;

    for (Index = 0; Index < TOUCH_MAX_CONTACTS; Index++)
    {
        if (!gTouchContacts[Index].Active)
        {
            Contact = &gTouchContacts[Index];
            break;
        }
    }

    if (!Contact)
        return NULL;

    RtlZeroMemory(Contact, sizeof(*Contact));
    Contact->Primary = !TouchAnyActive();
    Contact->Active = TRUE;
    Contact->Device = DeviceInfo;
    Contact->ContactId = Sample->ContactId;
    Contact->TouchId = gNextTouchId++;
    Contact->Point = Sample->Point;
    Contact->Flags = TOUCHEVENTF_DOWN;

    /* The window a contact lands on keeps it until it lifts */
    pwnd = co_WinPosWindowFromPoint(NULL, &Sample->Point, &HitTest, FALSE);
    if (pwnd)
        Contact->hwndTarget = UserHMGetHandle(pwnd);

    return Contact;
}

static
VOID
TouchPromotePrimary(
    _In_ PTOUCH_CONTACT Contact)
{
    MOUSEINPUT Input;
    LONG Width = UserGetSystemMetrics(SM_CXSCREEN);
    LONG Height = UserGetSystemMetrics(SM_CYSCREEN);

    if (Width <= 0 || Height <= 0)
        return;

    RtlZeroMemory(&Input, sizeof(Input));
    Input.dx = (LONG)((((LONGLONG)Contact->Point.x << 16) + Width - 1) / Width);
    Input.dy = (LONG)((((LONGLONG)Contact->Point.y << 16) + Height - 1) / Height);
    Input.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    Input.dwExtraInfo = TOUCH_PROMOTED_EXTRA_INFO;

    if (Contact->Flags & TOUCHEVENTF_DOWN)
        Input.dwFlags |= MOUSEEVENTF_LEFTDOWN;
    else if (Contact->Flags & TOUCHEVENTF_UP)
        Input.dwFlags |= MOUSEEVENTF_LEFTUP;

    UserSendMouseInput(&Input, FALSE);
}

static
BOOLEAN
TouchWindowWantsTouch(
    _In_ PWND pwnd)
{
    return (pwnd->ExStyle2 & WS_EX2_TOUCHREGISTERED) != 0;
}

/**
 * @brief Posts one WM_TOUCH with every contact a window currently holds.
 */
static
VOID
TouchPostToWindow(
    _In_ PWND pwnd,
    _In_ DWORD Time)
{
    PTOUCH_INPUT_OBJECT Object;
    PTOUCHINPUT Input;
    HTOUCHINPUT hTouchInput;
    HWND hwnd = UserHMGetHandle(pwnd);
    MSG Msg;
    UINT Count = 0;
    ULONG Index;

    for (Index = 0; Index < TOUCH_MAX_CONTACTS; Index++)
    {
        if (gTouchContacts[Index].Active && gTouchContacts[Index].hwndTarget == hwnd)
            Count++;
    }

    if (Count == 0)
        return;

    Object = UserCreateObject(gHandleTable,
                              pwnd->head.rpdesk,
                              pwnd->head.pti,
                              (PHANDLE)&hTouchInput,
                              TYPE_TOUCHINPUTINFO,
                              FIELD_OFFSET(TOUCH_INPUT_OBJECT, Inputs[Count]));
    if (!Object)
        return;

    Object->Count = 0;
    for (Index = 0; Index < TOUCH_MAX_CONTACTS; Index++)
    {
        PTOUCH_CONTACT Contact = &gTouchContacts[Index];

        if (!Contact->Active || Contact->hwndTarget != hwnd)
            continue;

        Input = &Object->Inputs[Object->Count++];
        Input->x = Contact->Point.x * 100;
        Input->y = Contact->Point.y * 100;
        Input->hSource = (HANDLE)Contact->Device;
        Input->dwID = Contact->TouchId;
        Input->dwFlags = Contact->Flags;
        if (!(Contact->Flags & TOUCHEVENTF_UP))
            Input->dwFlags |= TOUCHEVENTF_INRANGE;
        if (Contact->Primary)
            Input->dwFlags |= TOUCHEVENTF_PRIMARY;
        Input->dwTime = Time;
    }

    Msg.hwnd = hwnd;
    Msg.message = WM_TOUCH;
    Msg.wParam = MAKEWPARAM(Object->Count, 0);
    Msg.lParam = (LPARAM)hTouchInput;
    Msg.time = Time;
    Msg.pt = gpsi->ptCursor;

    if (!MsqPostMessage(pwnd->head.pti, &Msg, FALSE, QS_POSTMESSAGE | QS_ALLPOSTMESSAGE, 0, 0))
        UserDeleteObject(hTouchInput, TYPE_TOUCHINPUTINFO);

    /* Drop the creation reference, the handle keeps the object until it is closed */
    UserDereferenceObject(Object);
}

/**
 * @brief Delivers one complete frame from a touch screen.
 */
static
VOID
TouchDispatchFrame(
    _In_ PINPUT_DEVICE_INFO DeviceInfo,
    _In_reads_(Count) PTOUCH_SAMPLE Samples,
    _In_ ULONG Count)
{
    PTOUCH_CONTACT Contact;
    DWORD Time = EngGetTickCount32();
    ULONG Index;
    ULONG Other;
    PWND pwnd;

    for (Index = 0; Index < TOUCH_MAX_CONTACTS; Index++)
        gTouchContacts[Index].Seen = FALSE;

    for (Index = 0; Index < Count; Index++)
    {
        Contact = TouchFindContact(DeviceInfo, Samples[Index].ContactId);

        if (Samples[Index].Tip)
        {
            if (!Contact)
            {
                Contact = TouchStartContact(DeviceInfo, &Samples[Index]);
                if (!Contact)
                    continue;
            }
            else
            {
                Contact->Point = Samples[Index].Point;
                Contact->Flags = TOUCHEVENTF_MOVE;
            }
        }
        else if (Contact)
        {
            Contact->Point = Samples[Index].Point;
            Contact->Flags = TOUCHEVENTF_UP;
        }
        else
        {
            continue;
        }

        Contact->Seen = TRUE;
    }

    /* A contact the frame no longer reports has lifted */
    for (Index = 0; Index < TOUCH_MAX_CONTACTS; Index++)
    {
        Contact = &gTouchContacts[Index];
        if (Contact->Active && Contact->Device == DeviceInfo && !Contact->Seen)
            Contact->Flags = TOUCHEVENTF_UP;
    }

    /* One WM_TOUCH per registered window that holds a contact */
    for (Index = 0; Index < TOUCH_MAX_CONTACTS; Index++)
    {
        Contact = &gTouchContacts[Index];
        if (!Contact->Active || Contact->Device != DeviceInfo || !Contact->hwndTarget)
            continue;

        for (Other = 0; Other < Index; Other++)
        {
            if (gTouchContacts[Other].Active &&
                gTouchContacts[Other].Device == DeviceInfo &&
                gTouchContacts[Other].hwndTarget == Contact->hwndTarget)
            {
                break;
            }
        }

        if (Other != Index)
            continue;

        pwnd = UserGetWindowObject(Contact->hwndTarget);
        if (pwnd && TouchWindowWantsTouch(pwnd))
            TouchPostToWindow(pwnd, Time);
    }

    /* Legacy applications see the primary contact as the left button */
    for (Index = 0; Index < TOUCH_MAX_CONTACTS; Index++)
    {
        Contact = &gTouchContacts[Index];
        if (Contact->Active && Contact->Device == DeviceInfo && Contact->Primary)
            TouchPromotePrimary(Contact);
    }

    for (Index = 0; Index < TOUCH_MAX_CONTACTS; Index++)
    {
        Contact = &gTouchContacts[Index];
        if (Contact->Active && Contact->Device == DeviceInfo && (Contact->Flags & TOUCHEVENTF_UP))
            RtlZeroMemory(Contact, sizeof(*Contact));
    }
}

/**
 * @brief Parses one input report and dispatches the frame once it is complete.
 */
VOID
NTAPI
UserProcessTouchReport(
    _In_ PINPUT_DEVICE_INFO DeviceInfo,
    _In_reads_bytes_(Length) PCHAR Report,
    _In_ ULONG Length)
{
    PHID_DEVICE_INFO Hid = &DeviceInfo->Hid;
    PTOUCH_DEVICE Touch = Hid->TouchContext;
    PTOUCH_FINGER_LINK Finger;
    PTOUCH_SAMPLE Sample;
    USAGE Usages[TOUCH_MAX_USAGES];
    ULONG UsageCount;
    ULONG ContactCount = 0;
    ULONG Value;
    ULONG Index;
    ULONG Usage;
    LONG Width = UserGetSystemMetrics(SM_CXSCREEN);
    LONG Height = UserGetSystemMetrics(SM_CYSCREEN);

    if (!Touch)
        return;

    /* A nonzero contact count starts a frame, zero continues the current one */
    if (HidP_GetUsageValue(HidP_Input, HID_PAGE_DIGITIZER, HIDP_LINK_COLLECTION_ROOT, HID_USAGE_CONTACT_COUNT,
                           &ContactCount, Hid->PreparsedData, Report, Length) != HIDP_STATUS_SUCCESS)
    {
        ContactCount = Touch->FingerCount;
    }

    if (ContactCount != 0)
    {
        Touch->Expected = min(ContactCount, TOUCH_MAX_CONTACTS);
        Touch->Collected = 0;
    }
    else if (Touch->Expected == 0)
    {
        /* Every contact lifted */
        TouchDispatchFrame(DeviceInfo, Touch->Frame, 0);
        return;
    }

    for (Index = 0; Index < Touch->FingerCount && Touch->Collected < Touch->Expected; Index++)
    {
        Finger = &Touch->Fingers[Index];
        Sample = &Touch->Frame[Touch->Collected];

        if (HidP_GetUsageValue(HidP_Input, HID_PAGE_DIGITIZER, Finger->LinkCollection, HID_USAGE_CONTACT_ID,
                               &Value, Hid->PreparsedData, Report, Length) != HIDP_STATUS_SUCCESS)
        {
            Value = Index;
        }
        Sample->ContactId = Value;

        UsageCount = RTL_NUMBER_OF(Usages);
        Sample->Tip = FALSE;
        if (HidP_GetUsages(HidP_Input, HID_PAGE_DIGITIZER, Finger->LinkCollection, Usages, &UsageCount,
                           Hid->PreparsedData, Report, Length) == HIDP_STATUS_SUCCESS)
        {
            for (Usage = 0; Usage < UsageCount; Usage++)
            {
                if (Usages[Usage] == HID_USAGE_TIP_SWITCH)
                    Sample->Tip = TRUE;
            }
        }

        if (HidP_GetUsageValue(HidP_Input, HID_PAGE_GENERIC, Finger->LinkCollection, HID_USAGE_X,
                               &Value, Hid->PreparsedData, Report, Length) != HIDP_STATUS_SUCCESS)
            continue;
        Sample->Point.x = TouchScale((LONG)Value, Finger->MinX, Finger->MaxX, Width);

        if (HidP_GetUsageValue(HidP_Input, HID_PAGE_GENERIC, Finger->LinkCollection, HID_USAGE_Y,
                               &Value, Hid->PreparsedData, Report, Length) != HIDP_STATUS_SUCCESS)
            continue;
        Sample->Point.y = TouchScale((LONG)Value, Finger->MinY, Finger->MaxY, Height);

        Touch->Collected++;
    }

    if (Touch->Collected < Touch->Expected)
        return;

    TouchDispatchFrame(DeviceInfo, Touch->Frame, Touch->Collected);
    Touch->Expected = 0;
    Touch->Collected = 0;
}

/* TYPE_TOUCHINPUTINFO destroy callback */
BOOLEAN
UserDestroyTouchInput(
    _In_ PVOID Object)
{
    return UserDeleteObject(((PHEAD)Object)->h, TYPE_TOUCHINPUTINFO);
}

BOOL
APIENTRY
NtUserGetTouchInputInfo(
    _In_ HTOUCHINPUT hTouchInput,
    _In_ UINT cInputs,
    _Out_writes_(cInputs) PTOUCHINPUT pInputs,
    _In_ int cbSize)
{
    PTOUCH_INPUT_OBJECT Object;
    UINT Count;
    BOOL Ret = FALSE;

    if (cbSize != sizeof(TOUCHINPUT) || cInputs == 0 || pInputs == NULL)
    {
        EngSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    UserEnterShared();

    Object = UserGetObject(gHandleTable, hTouchInput, TYPE_TOUCHINPUTINFO);
    if (!Object || Object->head.pti != PsGetCurrentThreadWin32Thread())
    {
        EngSetLastError(ERROR_INVALID_HANDLE);
        goto Exit;
    }

    Count = min(cInputs, Object->Count);

    _SEH2_TRY
    {
        ProbeForWrite(pInputs, Count * sizeof(TOUCHINPUT), sizeof(ULONG));
        RtlCopyMemory(pInputs, Object->Inputs, Count * sizeof(TOUCHINPUT));
        Ret = TRUE;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        SetLastNtError(_SEH2_GetExceptionCode());
    }
    _SEH2_END;

Exit:
    UserLeave();
    return Ret;
}

BOOL
APIENTRY
NtUserCloseTouchInputHandle(
    _In_ HTOUCHINPUT hTouchInput)
{
    PTOUCH_INPUT_OBJECT Object;
    BOOL Ret = FALSE;

    UserEnterExclusive();

    Object = UserGetObject(gHandleTable, hTouchInput, TYPE_TOUCHINPUTINFO);
    if (!Object || Object->head.pti != PsGetCurrentThreadWin32Thread())
        EngSetLastError(ERROR_INVALID_HANDLE);
    else
        Ret = UserDeleteObject(hTouchInput, TYPE_TOUCHINPUTINFO);

    UserLeave();
    return Ret;
}

BOOL
APIENTRY
NtUserIsTouchWindow(
    _In_ HWND hwnd,
    _Out_opt_ PULONG pulFlags)
{
    PWND pwnd;
    ULONG Flags = 0;
    BOOL Ret = FALSE;

    UserEnterShared();

    pwnd = UserGetWindowObject(hwnd);
    if (pwnd && TouchWindowWantsTouch(pwnd))
    {
        Ret = TRUE;
        if (pwnd->ExStyle2 & WS_EX2_TOUCHFINE)
            Flags |= TWF_FINETOUCH;
        if (pwnd->ExStyle2 & WS_EX2_TOUCHWANTPALM)
            Flags |= TWF_WANTPALM;
    }

    UserLeave();

    if (pulFlags)
    {
        _SEH2_TRY
        {
            ProbeForWrite(pulFlags, sizeof(*pulFlags), sizeof(ULONG));
            *pulFlags = Flags;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            SetLastNtError(_SEH2_GetExceptionCode());
            Ret = FALSE;
        }
        _SEH2_END;
    }

    return Ret;
}

/**
 * @brief Registers or unregisters a window for WM_TOUCH.
 *
 * Only the creating process may change a window.
 */
BOOL
APIENTRY
NtUserModifyWindowTouchCapability(
    _In_ HWND hwnd,
    _In_ ULONG ulFlags,
    _In_ BOOL Register)
{
    PWND pwnd;
    BOOL Ret = FALSE;

    if (Register && (ulFlags & ~(TWF_FINETOUCH | TWF_WANTPALM)))
    {
        EngSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    UserEnterExclusive();

    pwnd = UserGetWindowObject(hwnd);
    if (!pwnd)
        goto Exit;

    if (pwnd->head.pti->ppi != PsGetCurrentProcessWin32Process())
    {
        EngSetLastError(ERROR_ACCESS_DENIED);
        goto Exit;
    }

    pwnd->ExStyle2 &= ~(WS_EX2_TOUCHREGISTERED | WS_EX2_TOUCHFINE | WS_EX2_TOUCHWANTPALM);
    if (Register)
    {
        pwnd->ExStyle2 |= WS_EX2_TOUCHREGISTERED;
        if (ulFlags & TWF_FINETOUCH)
            pwnd->ExStyle2 |= WS_EX2_TOUCHFINE;
        if (ulFlags & TWF_WANTPALM)
            pwnd->ExStyle2 |= WS_EX2_TOUCHWANTPALM;
    }

    Ret = TRUE;

Exit:
    UserLeave();
    return Ret;
}
