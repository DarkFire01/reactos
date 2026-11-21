/*
 * PROJECT:     ReactOS Phone Test
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Call, SMS, status and data front end for the phone service
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "phonetest.h"
#include <telephony/phoneom.h>
#include <telephony/rilinfo.h>

/* Dark theme shared with the mobile shell */
#define PT_BG                       RGB(0x12, 0x12, 0x14)
#define PT_TILE                     RGB(0x24, 0x25, 0x2A)
#define PT_ACCENT                   RGB(0x3A, 0x96, 0xDD)
#define PT_TEXT                     RGB(0xF2, 0xF2, 0xF4)
#define PT_SUBTEXT                  RGB(0x9A, 0x9B, 0xA3)
#define PT_FIELD                    RGB(0x1A, 0x1A, 0x1E)

typedef struct _PT_EVENT
{
    DWORD Event;
    DWORD Size;
    BYTE Data[ANYSIZE_ARRAY];
} PT_EVENT, *PPT_EVENT;

static HINSTANCE PtInstance;
static HWND PtWindow;
static HPHONEOM PtPhone;
static HFONT PtFont;
static HFONT PtBigFont;
static HFONT PtButtonFont;
static HFONT PtHeadFont;
static HFONT PtSmallFont;
static HBRUSH PtBgBrush;
static HBRUSH PtFieldBrush;
static int PtPage;
static WCHAR PtStatusText[2048];

/* Touch sized layout metrics in pixels, chosen for a high density phone panel */
#define PT_MARGIN                   28
/* Keep the top row clear of the camera cutout at the top of the panel */
#define PT_SAFE_TOP                 120
#define PT_GAP                      16
#define PT_ROW                      88
#define PT_PILL_H                   56
#define PT_TAB_H                    76
#define PT_HEAD_H                   64
#define PT_RADIUS                   18

#define PT_PAGE_PHONE               0
#define PT_PAGE_STATUS              1
#define PT_PAGE_DATA                2
#define PT_PAGE_MODEM               3

static const LPCWSTR PtStateNames[] =
{
    L"idle", L"dialing", L"incoming", L"active", L"held", L"ended"
};

static const LPCWSTR PtSimNames[] =
{
    L"absent", L"ready", L"PIN required", L"PUK required", L"error"
};

static const LPCWSTR PtRegNames[] =
{
    L"unknown", L"not registered", L"home", L"searching", L"denied", L"roaming", L"roaming (domestic)"
};

static const LPCWSTR PtPageHeadings[] =
{
    L"Phone", L"Status", L"Mobile data", L"Modem"
};

static const UINT PtPhoneControls[] =
{
    IDC_RADIO_STATUS, IDC_RADIO_ON, IDC_RADIO_OFF, IDC_NUMBER, IDC_DIAL,
    IDC_ANSWER, IDC_HANGUP, IDC_SWAP, IDC_DTMF, IDC_SPEAKER, IDC_MUTE,
    IDC_CALLS, IDC_SMS_TO, IDC_SMS_TEXT, IDC_SMS_SEND, IDC_MESSAGES, IDC_LOG
};

static const UINT PtStatusControls[] =
{
    IDC_STATUS_REFRESH
};

static const UINT PtDataControls[] =
{
    IDC_DATA_APN_LABEL, IDC_DATA_APN, IDC_DATA_USER_LABEL, IDC_DATA_USER,
    IDC_DATA_PASS_LABEL, IDC_DATA_PASS, IDC_DATA_AUTH_LABEL, IDC_DATA_AUTH,
    IDC_DATA_ENABLED, IDC_DATA_SAVE, IDC_DATA_CONNECT, IDC_DATA_DISCONNECT,
    IDC_DATA_STATUS
};

static const UINT PtModemControls[] =
{
    IDC_MODEM_STATUS, IDC_MODEM_SERVICES, IDC_MODEM_MODE, IDC_MODEM_ONLINE,
    IDC_MODEM_LOW_POWER, IDC_MODEM_OFFLINE, IDC_MODEM_RESET, IDC_MODEM_SIGNAL,
    IDC_MODEM_SERVING, IDC_MODEM_SIM, IDC_MODEM_IDENTITY, IDC_MODEM_RAW,
    IDC_MODEM_SEND, IDC_MODEM_CLEAR, IDC_MODEM_ACTIVATE, IDC_MODEM_LOG
};

static
BOOL
PtIsToggle(
    _In_ UINT Id)
{
    return Id == IDC_SPEAKER || Id == IDC_MUTE || Id == IDC_DATA_ENABLED;
}

static
VOID
PtFillRound(
    _In_ HDC Dc,
    _In_ const RECT *Rect,
    _In_ int Radius,
    _In_ COLORREF Color)
{
    HBRUSH Brush = CreateSolidBrush(Color);
    HGDIOBJ OldBrush = SelectObject(Dc, Brush);
    HGDIOBJ OldPen = SelectObject(Dc, GetStockObject(NULL_PEN));

    RoundRect(Dc, Rect->left, Rect->top, Rect->right + 1, Rect->bottom + 1, Radius, Radius);

    SelectObject(Dc, OldPen);
    SelectObject(Dc, OldBrush);
    DeleteObject(Brush);
}

static
VOID
PtLog(
    _In_ LPCWSTR Format,
    ...)
{
    WCHAR Line[512];
    va_list Args;
    HWND Log;
    LRESULT Index;

    va_start(Args, Format);
    StringCchVPrintfW(Line, _countof(Line), Format, Args);
    va_end(Args);

    Log = GetDlgItem(PtWindow, IDC_LOG);
    Index = SendMessageW(Log, LB_ADDSTRING, 0, (LPARAM)Line);
    SendMessageW(Log, LB_SETCURSEL, Index, 0);
}

/* Names the RIL errors, the low byte of a modem error is its QMI error code */
static
LPCWSTR
PtRilErrorText(
    _In_ HRESULT hr)
{
    if ((hr & 0xFFFFFF00) != 0x81000000)
        return NULL;

    if (hr & 0x80)
    {
        switch (hr & 0x7F)
        {
            case 0x01: return L"the modem could not parse the request";
            case 0x03: return L"the modem reported an internal error";
            case 0x0D: return L"no network found";
            case 0x0E: return L"the call failed";
            case 0x10: return L"the SIM is not provisioned";
            case 0x11: return L"the request is missing an argument";
            default: return L"the modem refused the request";
        }
    }

    switch (hr & 0xFF)
    {
        case 0x04: return L"operation not allowed";
        case 0x05: return L"operation not supported";
        case 0x06: return L"no SIM inserted";
        case 0x0D: return L"not found";
        case 0x11: return L"number too long";
        case 0x12: return L"not a dialable number";
        case 0x15: return L"emergency calls only";
        case 0x50: return L"the radio is not available";
        case 0x51: return L"the radio is off";
        default: return NULL;
    }
}

static
VOID
PtCheck(
    _In_ LPCWSTR What,
    _In_ HRESULT hr)
{
    LPCWSTR Reason = PtRilErrorText(hr);

    if (FAILED(hr) && Reason)
        PtLog(L"%s failed 0x%08lx, %s", What, hr, Reason);
    else if (FAILED(hr))
        PtLog(L"%s failed 0x%08lx", What, hr);
    else
        PtLog(L"%s ok", What);
}

static
VOID
CALLBACK
PtPhoneEvent(
    _In_ HPHONEOM Phone,
    _In_ DWORD Event,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize,
    _In_opt_ PVOID Context)
{
    PPT_EVENT Copy;

    UNREFERENCED_PARAMETER(Phone);
    UNREFERENCED_PARAMETER(Context);

    Copy = HeapAlloc(GetProcessHeap(), 0, FIELD_OFFSET(PT_EVENT, Data) + DataSize);
    if (!Copy)
        return;

    Copy->Event = Event;
    Copy->Size = DataSize;
    if (Data && DataSize)
        CopyMemory(Copy->Data, Data, DataSize);

    if (!PostMessageW(PtWindow, WM_PHONE_EVENT, 0, (LPARAM)Copy))
        HeapFree(GetProcessHeap(), 0, Copy);
}

static
VOID
PtRefreshRadio(VOID)
{
    PHONEOM_RADIO Radio;
    WCHAR Text[128];

    if (!PtPhone || FAILED(PhoneOmGetRadio(PtPhone, &Radio)))
    {
        SetDlgItemTextW(PtWindow, IDC_RADIO_STATUS, L"No phone service");
        return;
    }

    StringCchPrintfW(Text,
                     _countof(Text),
                     L"Radio %s  \x2022  %s",
                     Radio.Present ? L"present" : L"absent",
                     Radio.PoweredOn ? L"on" : L"off");
    SetDlgItemTextW(PtWindow, IDC_RADIO_STATUS, Text);
}

static
VOID
PtRefreshCalls(VOID)
{
    PHONEOM_CALL Calls[PHONEOM_MAX_CALLS];
    WCHAR Line[256];
    HWND List = GetDlgItem(PtWindow, IDC_CALLS);
    DWORD Count = 0, i;
    LRESULT Index;

    SendMessageW(List, LB_RESETCONTENT, 0, 0);
    if (!PtPhone || FAILED(PhoneOmGetCalls(PtPhone, Calls, _countof(Calls), &Count)))
        return;

    for (i = 0; i < Count; i++)
    {
        StringCchPrintfW(Line,
                         _countof(Line),
                         L"#%lu %s %s %s%s",
                         Calls[i].CallId,
                         Calls[i].State < _countof(PtStateNames) ? PtStateNames[Calls[i].State] : L"?",
                         Calls[i].Number,
                         (Calls[i].Flags & PHONEOM_CALL_INCOMING) ? L"(in)" : L"(out)",
                         (Calls[i].Flags & PHONEOM_CALL_EMERGENCY) ? L" emergency" : L"");

        Index = SendMessageW(List, LB_ADDSTRING, 0, (LPARAM)Line);
        SendMessageW(List, LB_SETITEMDATA, Index, Calls[i].CallId);
    }

    if (Count)
        SendMessageW(List, LB_SETCURSEL, 0, 0);
}

static
VOID
PtAddMessageLine(
    _In_ const PHONEOM_MESSAGE *Message)
{
    WCHAR Line[PHONEOM_MAX_TEXT + 128];
    LPCWSTR Status = L"";

    if (Message->Flags & PHONEOM_MESSAGE_FAILED)
        Status = L" [failed]";
    else if (Message->Flags & PHONEOM_MESSAGE_SENT)
        Status = L" [sent]";
    else if (!(Message->Flags & PHONEOM_MESSAGE_INCOMING))
        Status = L" [sending]";

    StringCchPrintfW(Line,
                     _countof(Line),
                     L"%lu %s %s%s: %s",
                     Message->MessageId,
                     (Message->Flags & PHONEOM_MESSAGE_INCOMING) ? L"from" : L"to",
                     Message->Address,
                     Status,
                     Message->Text);

    SendDlgItemMessageW(PtWindow, IDC_MESSAGES, LB_ADDSTRING, 0, (LPARAM)Line);
}

static
VOID
PtRefreshMessages(VOID)
{
    PPHONEOM_MESSAGE Messages;
    DWORD Count, Next = 1, i;
    const DWORD Batch = 16;

    SendDlgItemMessageW(PtWindow, IDC_MESSAGES, LB_RESETCONTENT, 0, 0);
    if (!PtPhone)
        return;

    Messages = HeapAlloc(GetProcessHeap(), 0, Batch * sizeof(*Messages));
    if (!Messages)
        return;

    for (;;)
    {
        if (FAILED(PhoneOmGetMessages(PtPhone, Next, Messages, Batch, &Count)) || Count == 0)
            break;

        for (i = 0; i < Count; i++)
            PtAddMessageLine(&Messages[i]);

        Next = Messages[Count - 1].MessageId + 1;
        if (Count < Batch)
            break;
    }

    HeapFree(GetProcessHeap(), 0, Messages);
}

static
LPCWSTR
PtName(
    _In_ const LPCWSTR *Table,
    _In_ DWORD Count,
    _In_ DWORD Index)
{
    return (Index < Count) ? Table[Index] : L"?";
}

/* The system type is a bit, one per radio technology */
static
LPCWSTR
PtRatName(
    _In_ DWORD Rat)
{
    switch (Rat)
    {
        case RIL_RAT_NONE: return L"none";
        case RIL_RAT_CDMA: return L"CDMA";
        case RIL_RAT_EVDO: return L"EV-DO";
        case RIL_RAT_GSM: return L"GSM";
        case RIL_RAT_UMTS: return L"UMTS";
        case RIL_RAT_LTE: return L"LTE";
        case RIL_RAT_TDSCDMA: return L"TD-SCDMA";
        default: return L"?";
    }
}

static
VOID
PtRefreshStatus(VOID)
{
    PHONEOM_DEVICE_INFO Device;
    PHONEOM_SUBSCRIBER Subscriber;
    PHONEOM_NETWORK Network;
    WCHAR Line[256];

    PtStatusText[0] = UNICODE_NULL;

    if (!PtPhone)
    {
        StringCchCopyW(PtStatusText, _countof(PtStatusText), L"No phone service.");
        goto Done;
    }

    if (SUCCEEDED(PhoneOmGetNetwork(PtPhone, &Network)))
    {
        StringCchPrintfW(Line, _countof(Line),
                         L"%s\n%s%s  \x2022  %s\n%ld dBm  (%lu/5)\nMCC/MNC %03lu/%02lu\n\n",
                         Network.Operator[0] ? Network.Operator : L"No service",
                         PtName(PtRegNames, _countof(PtRegNames), Network.RegState),
                         Network.Roaming ? L" (roaming)" : L"",
                         PtRatName(Network.Rat),
                         Network.SignalDbm, Network.SignalBars,
                         Network.Mcc, Network.Mnc);
        StringCchCatW(PtStatusText, _countof(PtStatusText), Line);
    }

    if (SUCCEEDED(PhoneOmGetSubscriber(PtPhone, &Subscriber)))
    {
        StringCchPrintfW(Line, _countof(Line),
                         L"SIM  \x2022  %s\nPhone number  %s\nIMSI  %s\n\n",
                         PtName(PtSimNames, _countof(PtSimNames), Subscriber.SimState),
                         Subscriber.Number[0] ? Subscriber.Number : L"unknown",
                         Subscriber.Imsi[0] ? Subscriber.Imsi : L"unknown");
        StringCchCatW(PtStatusText, _countof(PtStatusText), Line);
    }

    if (SUCCEEDED(PhoneOmGetDeviceInfo(PtPhone, &Device)))
    {
        StringCchPrintfW(Line, _countof(Line),
                         L"%s %s\nFirmware  %s\nIMEI  %s",
                         Device.Manufacturer, Device.Model, Device.Revision, Device.Imei);
        StringCchCatW(PtStatusText, _countof(PtStatusText), Line);
    }

    if (PtStatusText[0] == UNICODE_NULL)
        StringCchCopyW(PtStatusText, _countof(PtStatusText), L"Modem information is not available yet.");

Done:
    InvalidateRect(PtWindow, NULL, FALSE);
}

static
VOID
PtLoadData(VOID)
{
    PHONEOM_DATA_CONFIG Config;

    if (!PtPhone || FAILED(PhoneOmGetDataConfig(PtPhone, &Config)))
        return;

    SetDlgItemTextW(PtWindow, IDC_DATA_APN, Config.Apn);
    SetDlgItemTextW(PtWindow, IDC_DATA_USER, Config.Username);
    SetDlgItemTextW(PtWindow, IDC_DATA_PASS, Config.Password);
    SendDlgItemMessageW(PtWindow, IDC_DATA_AUTH, CB_SETCURSEL, Config.AuthType, 0);
    CheckDlgButton(PtWindow, IDC_DATA_ENABLED, Config.Enabled ? BST_CHECKED : BST_UNCHECKED);
}

static
VOID
PtSaveData(VOID)
{
    PHONEOM_DATA_CONFIG Config;
    LRESULT Auth;

    if (!PtPhone)
        return;

    ZeroMemory(&Config, sizeof(Config));
    GetDlgItemTextW(PtWindow, IDC_DATA_APN, Config.Apn, _countof(Config.Apn));
    GetDlgItemTextW(PtWindow, IDC_DATA_USER, Config.Username, _countof(Config.Username));
    GetDlgItemTextW(PtWindow, IDC_DATA_PASS, Config.Password, _countof(Config.Password));
    Auth = SendDlgItemMessageW(PtWindow, IDC_DATA_AUTH, CB_GETCURSEL, 0, 0);
    Config.AuthType = (Auth == CB_ERR) ? PHONEOM_AUTH_NONE : (DWORD)Auth;
    Config.Enabled = (IsDlgButtonChecked(PtWindow, IDC_DATA_ENABLED) == BST_CHECKED);

    PtCheck(L"Save data profile", PhoneOmSetDataConfig(PtPhone, &Config));
}

static
VOID
PtRefreshData(VOID)
{
    PHONEOM_DATA_STATUS Status;

    if (!PtPhone || FAILED(PhoneOmConnectData(PtPhone, FALSE, &Status)))
    {
        SetDlgItemTextW(PtWindow, IDC_DATA_STATUS, L"Data status unknown");
        return;
    }

    SetDlgItemTextW(PtWindow, IDC_DATA_STATUS,
                    Status.Connected ? L"Data connected" : L"Data disconnected");
}

static
DWORD
PtSelectedCall(VOID)
{
    HWND List = GetDlgItem(PtWindow, IDC_CALLS);
    LRESULT Index = SendMessageW(List, LB_GETCURSEL, 0, 0);

    if (Index == LB_ERR)
        return 0;

    return (DWORD)SendMessageW(List, LB_GETITEMDATA, Index, 0);
}

static
VOID
PtOpenPhone(VOID)
{
    HRESULT hr;

    if (PtPhone)
        return;

    hr = PhoneOmOpen(PtPhoneEvent, NULL, &PtPhone);
    if (FAILED(hr))
    {
        PtPhone = NULL;
        PtLog(L"Phone service unavailable 0x%08lx", hr);
        return;
    }

    PtLog(L"Connected to the phone service");
    PtRefreshRadio();
    PtRefreshCalls();
    PtRefreshMessages();
    PtLoadData();
}

static
VOID
PtHandleEvent(
    _In_ PPT_EVENT Event)
{
    const PHONEOM_CALL *Call;
    const PHONEOM_MESSAGE *Message;

    switch (Event->Event)
    {
        case PhoneEventRadio:
            PtRefreshRadio();
            break;

        case PhoneEventCall:
            if (Event->Size < sizeof(*Call))
                break;

            Call = (const PHONEOM_CALL *)Event->Data;
            PtLog(L"Call %lu %s %s",
                  Call->CallId,
                  Call->State < _countof(PtStateNames) ? PtStateNames[Call->State] : L"?",
                  Call->Number);

            if (Call->State == PhoneCallIncoming)
            {
                SetDlgItemTextW(PtWindow, IDC_NUMBER, Call->Number);
                FlashWindow(PtWindow, TRUE);
                MessageBeep(MB_ICONASTERISK);
            }
            PtRefreshCalls();
            break;

        case PhoneEventMessage:
            if (Event->Size < sizeof(*Message))
                break;

            Message = (const PHONEOM_MESSAGE *)Event->Data;
            PtLog(L"Message from %s", Message->Address);
            PtAddMessageLine(Message);
            MessageBeep(MB_OK);
            break;

        case PhoneEventMessageStatus:
            if (Event->Size < sizeof(*Message))
                break;

            Message = (const PHONEOM_MESSAGE *)Event->Data;
            PtLog(L"Message %lu %s", Message->MessageId, (Message->Flags & PHONEOM_MESSAGE_SENT) ? L"sent" : L"failed");
            PtRefreshMessages();
            break;

        case PhoneEventDisconnected:
            PtLog(L"Phone service went away");
            PhoneOmClose(PtPhone);
            PtPhone = NULL;
            PtRefreshRadio();
            SetTimer(PtWindow, 1, 3000, NULL);
            break;
    }
}

static
VOID
PtCommand(
    _In_ UINT Id)
{
    WCHAR Number[PHONEOM_MAX_NUMBER];
    WCHAR Text[PHONEOM_MAX_TEXT];
    DWORD Value;
    HRESULT hr;

    if (Id >= IDC_TAB_PHONE && Id <= IDC_TAB_MODEM)
        return;

    /* The modem page talks to the router itself, it works without the phone service */
    if (PtModemIsCommand(Id))
    {
        PtModemCommand(Id);
        return;
    }

    if (!PtPhone)
    {
        PtOpenPhone();
        if (!PtPhone)
            return;
    }

    switch (Id)
    {
        case IDC_RADIO_ON:
        case IDC_RADIO_OFF:
            PtCheck(L"Radio power", PhoneOmSetRadioPower(PtPhone, Id == IDC_RADIO_ON));
            PtRefreshRadio();
            break;

        case IDC_DIAL:
            GetDlgItemTextW(PtWindow, IDC_NUMBER, Number, _countof(Number));
            if (!Number[wcsspn(Number, L" ")])
            {
                PtLog(L"Enter a number to dial");
                break;
            }

            hr = PhoneOmDial(PtPhone, Number, &Value);
            if (SUCCEEDED(hr))
                PtLog(L"Dialing %s as call %lu", Number, Value);
            else
                PtCheck(L"Dial", hr);
            PtRefreshCalls();
            break;

        case IDC_ANSWER:
            PtCheck(L"Answer", PhoneOmAnswer(PtPhone, PtSelectedCall()));
            break;

        case IDC_HANGUP:
            PtCheck(L"Hang up", PhoneOmHangUp(PtPhone, PtSelectedCall()));
            break;

        case IDC_SWAP:
            PtCheck(L"Swap", PhoneOmSwap(PtPhone));
            break;

        case IDC_DTMF:
            GetDlgItemTextW(PtWindow, IDC_NUMBER, Number, _countof(Number));
            if (Number[0])
                PtCheck(L"DTMF", PhoneOmSendDtmf(PtPhone, Number[wcslen(Number) - 1]));
            break;

        case IDC_SPEAKER:
            Value = (IsDlgButtonChecked(PtWindow, IDC_SPEAKER) == BST_CHECKED);
            PtCheck(L"Audio route", PhoneOmSetAudioRoute(PtPhone, Value ? PhoneAudioSpeaker : PhoneAudioEarpiece));
            break;

        case IDC_MUTE:
            Value = (IsDlgButtonChecked(PtWindow, IDC_MUTE) == BST_CHECKED);
            PtCheck(L"Mute", PhoneOmSetMute(PtPhone, Value));
            break;

        case IDC_SMS_SEND:
            GetDlgItemTextW(PtWindow, IDC_SMS_TO, Number, _countof(Number));
            GetDlgItemTextW(PtWindow, IDC_SMS_TEXT, Text, _countof(Text));
            hr = PhoneOmSendMessage(PtPhone, Number, Text, &Value);
            PtCheck(L"Send message", hr);
            if (SUCCEEDED(hr))
                SetDlgItemTextW(PtWindow, IDC_SMS_TEXT, L"");
            PtRefreshMessages();
            break;

        case IDC_STATUS_REFRESH:
            PtRefreshStatus();
            break;

        case IDC_DATA_SAVE:
            PtSaveData();
            break;

        case IDC_DATA_CONNECT:
        {
            PHONEOM_DATA_STATUS Status;
            PtSaveData();
            PtCheck(L"Data connect", PhoneOmConnectData(PtPhone, TRUE, &Status));
            PtRefreshData();
            break;
        }

        case IDC_DATA_DISCONNECT:
        {
            PHONEOM_DATA_STATUS Status;
            PtCheck(L"Data disconnect", PhoneOmConnectData(PtPhone, FALSE, &Status));
            PtRefreshData();
            break;
        }
    }
}

static
HFONT
PtMakeFont(
    _In_ int Height,
    _In_ int Weight)
{
    return CreateFontW(Height,
                       0, 0, 0,
                       Weight,
                       FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE,
                       L"Tahoma");
}

static
HWND
PtControl(
    _In_ HWND Parent,
    _In_ LPCWSTR Class,
    _In_opt_ LPCWSTR Text,
    _In_ DWORD Style,
    _In_ UINT Id,
    _In_opt_ HFONT Font)
{
    HWND Control;

    Control = CreateWindowExW(0,
                              Class,
                              Text,
                              WS_CHILD | Style,
                              0, 0, 10, 10,
                              Parent,
                              (HMENU)(ULONG_PTR)Id,
                              PtInstance,
                              NULL);
    if (Control)
        SendMessageW(Control, WM_SETFONT, (WPARAM)(Font ? Font : PtFont), FALSE);
    return Control;
}

/* An owner drawn pill button, flat with an accent fill when active or pressed */
static
HWND
PtButton(
    _In_ HWND Parent,
    _In_ LPCWSTR Text,
    _In_ DWORD Style,
    _In_ UINT Id)
{
    return PtControl(Parent, L"BUTTON", Text, Style | BS_OWNERDRAW, Id, PtButtonFont);
}

static
VOID
PtDrawButton(
    _In_ const DRAWITEMSTRUCT *Item)
{
    WCHAR Text[64];
    RECT Rect = Item->rcItem;
    UINT Id = Item->CtlID;
    BOOL Pressed = (Item->itemState & ODS_SELECTED) != 0;
    BOOL Active = FALSE;
    COLORREF Fill, Ink;
    int Radius = (Rect.bottom - Rect.top) / 3;

    if (Id >= IDC_TAB_PHONE && Id <= IDC_TAB_MODEM)
        Active = (PtPage == (int)(Id - IDC_TAB_PHONE));
    else if (PtIsToggle(Id))
        Active = (SendMessageW(Item->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED);

    if (Active || Pressed)
    {
        Fill = PT_ACCENT;
        Ink = PT_TEXT;
    }
    else
    {
        Fill = PT_TILE;
        Ink = (Id >= IDC_TAB_PHONE && Id <= IDC_TAB_MODEM) ? PT_SUBTEXT : PT_TEXT;
    }

    FillRect(Item->hDC, &Rect, PtBgBrush);
    PtFillRound(Item->hDC, &Rect, Radius, Fill);

    GetWindowTextW(Item->hwndItem, Text, _countof(Text));
    SetBkMode(Item->hDC, TRANSPARENT);
    SetTextColor(Item->hDC, Ink);
    SelectObject(Item->hDC, PtButtonFont);
    DrawTextW(Item->hDC, Text, -1, &Rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
}

static
VOID
PtCreateControls(
    _In_ HWND Window)
{
    PtButton(Window, L"Phone", WS_VISIBLE, IDC_TAB_PHONE);
    PtButton(Window, L"Status", WS_VISIBLE, IDC_TAB_STATUS);
    PtButton(Window, L"Data", WS_VISIBLE, IDC_TAB_DATA);
    PtButton(Window, L"Modem", WS_VISIBLE, IDC_TAB_MODEM);

    /* Phone page */
    PtControl(Window, L"STATIC", L"Radio", SS_CENTERIMAGE, IDC_RADIO_STATUS, PtFont);
    PtButton(Window, L"Radio on", WS_TABSTOP, IDC_RADIO_ON);
    PtButton(Window, L"Radio off", WS_TABSTOP, IDC_RADIO_OFF);
    PtControl(Window, L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, IDC_NUMBER, PtBigFont);
    PtButton(Window, L"Dial", WS_TABSTOP, IDC_DIAL);
    PtButton(Window, L"Answer", WS_TABSTOP, IDC_ANSWER);
    PtButton(Window, L"Hang up", WS_TABSTOP, IDC_HANGUP);
    PtButton(Window, L"Swap", WS_TABSTOP, IDC_SWAP);
    PtButton(Window, L"Tone", WS_TABSTOP, IDC_DTMF);
    PtButton(Window, L"Speaker", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_SPEAKER);
    PtButton(Window, L"Mute", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_MUTE);
    PtControl(Window, L"LISTBOX", NULL, WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, IDC_CALLS, PtFont);
    PtControl(Window, L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, IDC_SMS_TO, PtFont);
    PtControl(Window, L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, IDC_SMS_TEXT, PtFont);
    PtButton(Window, L"Send", WS_TABSTOP, IDC_SMS_SEND);
    PtControl(Window, L"LISTBOX", NULL, WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT, IDC_MESSAGES, PtFont);
    PtControl(Window, L"LISTBOX", NULL, WS_VSCROLL | LBS_NOINTEGRALHEIGHT, IDC_LOG, PtFont);

    /* Status page draws its own text, only the refresh button is a control */
    PtButton(Window, L"Refresh", WS_TABSTOP, IDC_STATUS_REFRESH);

    /* Data page */
    PtControl(Window, L"STATIC", L"Access point (APN)", SS_CENTERIMAGE, IDC_DATA_APN_LABEL, PtFont);
    PtControl(Window, L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, IDC_DATA_APN, PtFont);
    PtControl(Window, L"STATIC", L"User name", SS_CENTERIMAGE, IDC_DATA_USER_LABEL, PtFont);
    PtControl(Window, L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, IDC_DATA_USER, PtFont);
    PtControl(Window, L"STATIC", L"Password", SS_CENTERIMAGE, IDC_DATA_PASS_LABEL, PtFont);
    PtControl(Window, L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, IDC_DATA_PASS, PtFont);
    PtControl(Window, L"STATIC", L"Authentication", SS_CENTERIMAGE, IDC_DATA_AUTH_LABEL, PtFont);
    PtControl(Window, L"COMBOBOX", NULL, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, IDC_DATA_AUTH, PtFont);
    PtButton(Window, L"Mobile data enabled", WS_TABSTOP | BS_AUTOCHECKBOX, IDC_DATA_ENABLED);
    PtButton(Window, L"Save", WS_TABSTOP, IDC_DATA_SAVE);
    PtButton(Window, L"Connect", WS_TABSTOP, IDC_DATA_CONNECT);
    PtButton(Window, L"Disconnect", WS_TABSTOP, IDC_DATA_DISCONNECT);
    PtControl(Window, L"STATIC", L"Data status unknown", SS_CENTERIMAGE, IDC_DATA_STATUS, PtFont);

    /* Modem page, QMI straight on the router */
    PtControl(Window, L"STATIC", L"IPC router closed", SS_CENTERIMAGE, IDC_MODEM_STATUS, PtFont);
    PtButton(Window, L"Services", WS_TABSTOP, IDC_MODEM_SERVICES);
    PtButton(Window, L"Mode", WS_TABSTOP, IDC_MODEM_MODE);
    PtButton(Window, L"Online", WS_TABSTOP, IDC_MODEM_ONLINE);
    PtButton(Window, L"Low pwr", WS_TABSTOP, IDC_MODEM_LOW_POWER);
    PtButton(Window, L"Offline", WS_TABSTOP, IDC_MODEM_OFFLINE);
    PtButton(Window, L"Reset", WS_TABSTOP, IDC_MODEM_RESET);
    PtButton(Window, L"Signal", WS_TABSTOP, IDC_MODEM_SIGNAL);
    PtButton(Window, L"Network", WS_TABSTOP, IDC_MODEM_SERVING);
    PtButton(Window, L"SIM", WS_TABSTOP, IDC_MODEM_SIM);
    PtButton(Window, L"Identity", WS_TABSTOP, IDC_MODEM_IDENTITY);
    PtControl(Window, L"EDIT", L"2 2d", WS_TABSTOP | ES_AUTOHSCROLL, IDC_MODEM_RAW, PtFont);
    PtButton(Window, L"Send", WS_TABSTOP, IDC_MODEM_SEND);
    PtButton(Window, L"Clear", WS_TABSTOP, IDC_MODEM_CLEAR);
    PtButton(Window, L"Activate SIM", WS_TABSTOP, IDC_MODEM_ACTIVATE);
    PtControl(Window, L"LISTBOX", NULL, WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT, IDC_MODEM_LOG, PtSmallFont);
    SendDlgItemMessageW(Window, IDC_MODEM_LOG, LB_SETHORIZONTALEXTENT, 2400, 0);

    /* Always visible, returns to the shell */
    PtButton(Window, L"Exit", WS_VISIBLE | WS_TABSTOP, IDC_EXIT);

    SendDlgItemMessageW(Window, IDC_DATA_AUTH, CB_ADDSTRING, 0, (LPARAM)L"None");
    SendDlgItemMessageW(Window, IDC_DATA_AUTH, CB_ADDSTRING, 0, (LPARAM)L"PAP");
    SendDlgItemMessageW(Window, IDC_DATA_AUTH, CB_ADDSTRING, 0, (LPARAM)L"CHAP");
    SendDlgItemMessageW(Window, IDC_DATA_AUTH, CB_SETCURSEL, 0, 0);
}

static
VOID
PtMove(
    _In_ HWND Window,
    _In_ UINT Id,
    _In_ int X,
    _In_ int Y,
    _In_ int Width,
    _In_ int Height)
{
    MoveWindow(GetDlgItem(Window, Id), X, Y, Width, Height, TRUE);
}

static
VOID
PtContentRect(
    _In_ HWND Window,
    _Out_ RECT *Area)
{
    RECT Client;

    GetClientRect(Window, &Client);
    Area->left = PT_MARGIN;
    Area->right = Client.right - PT_MARGIN;
    Area->top = PT_SAFE_TOP + PT_TAB_H + PT_GAP + PT_HEAD_H;
    /* The exit button owns the bottom row on every page */
    Area->bottom = Client.bottom - PT_MARGIN - PT_ROW - PT_GAP;
}

static
VOID
PtLayoutPhone(
    _In_ HWND Window,
    _In_ const RECT *Area)
{
    int Width = Area->right - Area->left;
    int Half = (Width - PT_GAP) / 2;
    int Quarter = (Width - 3 * PT_GAP) / 4;
    int Third = (Width - 2 * PT_GAP) / 3;
    int Left = Area->left;
    int Y = Area->top;
    int ListH;

    ListH = (Area->bottom - Area->top - 7 * PT_GAP - PT_PILL_H - 5 * PT_ROW) / 3;
    if (ListH < PT_ROW)
        ListH = PT_ROW;

    PtMove(Window, IDC_RADIO_STATUS, Left, Y, Width, PT_PILL_H);
    Y += PT_PILL_H + PT_GAP;
    PtMove(Window, IDC_RADIO_ON, Left, Y, Half, PT_ROW);
    PtMove(Window, IDC_RADIO_OFF, Left + Half + PT_GAP, Y, Half, PT_ROW);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_NUMBER, Left, Y, Width, PT_ROW);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_DIAL, Left, Y, Quarter, PT_ROW);
    PtMove(Window, IDC_ANSWER, Left + (Quarter + PT_GAP), Y, Quarter, PT_ROW);
    PtMove(Window, IDC_HANGUP, Left + 2 * (Quarter + PT_GAP), Y, Quarter, PT_ROW);
    PtMove(Window, IDC_SWAP, Left + 3 * (Quarter + PT_GAP), Y, Quarter, PT_ROW);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_DTMF, Left, Y, Third, PT_ROW);
    PtMove(Window, IDC_SPEAKER, Left + (Third + PT_GAP), Y, Third, PT_ROW);
    PtMove(Window, IDC_MUTE, Left + 2 * (Third + PT_GAP), Y, Third, PT_ROW);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_CALLS, Left, Y, Width, ListH);
    Y += ListH + PT_GAP;
    PtMove(Window, IDC_SMS_TO, Left, Y, Third, PT_ROW);
    PtMove(Window, IDC_SMS_TEXT, Left + (Third + PT_GAP), Y, Third, PT_ROW);
    PtMove(Window, IDC_SMS_SEND, Left + 2 * (Third + PT_GAP), Y, Third, PT_ROW);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_MESSAGES, Left, Y, Width, ListH);
    Y += ListH + PT_GAP;
    PtMove(Window, IDC_LOG, Left, Y, Width, ListH);
}

static
VOID
PtLayoutStatus(
    _In_ HWND Window,
    _In_ const RECT *Area)
{
    PtMove(Window, IDC_STATUS_REFRESH, Area->left, Area->bottom - PT_ROW,
           Area->right - Area->left, PT_ROW);
}

static
VOID
PtLayoutData(
    _In_ HWND Window,
    _In_ const RECT *Area)
{
    int Width = Area->right - Area->left;
    int Third = (Width - 2 * PT_GAP) / 3;
    int Left = Area->left;
    int Y = Area->top;

    PtMove(Window, IDC_DATA_APN_LABEL, Left, Y, Width, PT_PILL_H);
    Y += PT_PILL_H;
    PtMove(Window, IDC_DATA_APN, Left, Y, Width, PT_ROW);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_DATA_USER_LABEL, Left, Y, Width, PT_PILL_H);
    Y += PT_PILL_H;
    PtMove(Window, IDC_DATA_USER, Left, Y, Width, PT_ROW);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_DATA_PASS_LABEL, Left, Y, Width, PT_PILL_H);
    Y += PT_PILL_H;
    PtMove(Window, IDC_DATA_PASS, Left, Y, Width, PT_ROW);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_DATA_AUTH_LABEL, Left, Y, Width, PT_PILL_H);
    Y += PT_PILL_H;
    PtMove(Window, IDC_DATA_AUTH, Left, Y, Width, 240);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_DATA_ENABLED, Left, Y, Width, PT_ROW);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_DATA_SAVE, Left, Y, Third, PT_ROW);
    PtMove(Window, IDC_DATA_CONNECT, Left + (Third + PT_GAP), Y, Third, PT_ROW);
    PtMove(Window, IDC_DATA_DISCONNECT, Left + 2 * (Third + PT_GAP), Y, Third, PT_ROW);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_DATA_STATUS, Left, Y, Width, PT_PILL_H);
}

static
VOID
PtLayoutModem(
    _In_ HWND Window,
    _In_ const RECT *Area)
{
    int Width = Area->right - Area->left;
    int Half = (Width - PT_GAP) / 2;
    int Quarter = (Width - 3 * PT_GAP) / 4;
    int Left = Area->left;
    int Y = Area->top;
    int LogH;

    PtMove(Window, IDC_MODEM_STATUS, Left, Y, Width, PT_PILL_H);
    Y += PT_PILL_H + PT_GAP;
    PtMove(Window, IDC_MODEM_SERVICES, Left, Y, Half, PT_ROW);
    PtMove(Window, IDC_MODEM_MODE, Left + Half + PT_GAP, Y, Half, PT_ROW);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_MODEM_ONLINE, Left, Y, Quarter, PT_ROW);
    PtMove(Window, IDC_MODEM_LOW_POWER, Left + (Quarter + PT_GAP), Y, Quarter, PT_ROW);
    PtMove(Window, IDC_MODEM_OFFLINE, Left + 2 * (Quarter + PT_GAP), Y, Quarter, PT_ROW);
    PtMove(Window, IDC_MODEM_RESET, Left + 3 * (Quarter + PT_GAP), Y, Quarter, PT_ROW);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_MODEM_SIGNAL, Left, Y, Quarter, PT_ROW);
    PtMove(Window, IDC_MODEM_SERVING, Left + (Quarter + PT_GAP), Y, Quarter, PT_ROW);
    PtMove(Window, IDC_MODEM_SIM, Left + 2 * (Quarter + PT_GAP), Y, Quarter, PT_ROW);
    PtMove(Window, IDC_MODEM_IDENTITY, Left + 3 * (Quarter + PT_GAP), Y, Quarter, PT_ROW);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_MODEM_RAW, Left, Y, Width - 2 * (Quarter + PT_GAP), PT_ROW);
    PtMove(Window, IDC_MODEM_SEND, Left + Width - 2 * Quarter - PT_GAP, Y, Quarter, PT_ROW);
    PtMove(Window, IDC_MODEM_CLEAR, Left + Width - Quarter, Y, Quarter, PT_ROW);
    Y += PT_ROW + PT_GAP;
    PtMove(Window, IDC_MODEM_ACTIVATE, Left, Y, Width, PT_ROW);
    Y += PT_ROW + PT_GAP;

    LogH = Area->bottom - Y;
    if (LogH < PT_ROW)
        LogH = PT_ROW;
    PtMove(Window, IDC_MODEM_LOG, Left, Y, Width, LogH);
}

static
VOID
PtLayout(
    _In_ HWND Window)
{
    RECT Client, Area;
    int Width, Segment, i;
    static const UINT Tabs[] = { IDC_TAB_PHONE, IDC_TAB_STATUS, IDC_TAB_DATA, IDC_TAB_MODEM };

    GetClientRect(Window, &Client);
    Width = Client.right - 2 * PT_MARGIN;
    if (Width < 120)
        return;

    Segment = (Width - ((int)_countof(Tabs) - 1) * PT_GAP) / (int)_countof(Tabs);
    for (i = 0; i < (int)_countof(Tabs); i++)
        PtMove(Window, Tabs[i], PT_MARGIN + i * (Segment + PT_GAP), PT_SAFE_TOP, Segment, PT_TAB_H);

    PtMove(Window, IDC_EXIT, PT_MARGIN, Client.bottom - PT_MARGIN - PT_ROW, Width, PT_ROW);

    PtContentRect(Window, &Area);
    if (Area.bottom - Area.top < PT_ROW)
        return;

    switch (PtPage)
    {
        case PT_PAGE_PHONE: PtLayoutPhone(Window, &Area); break;
        case PT_PAGE_STATUS: PtLayoutStatus(Window, &Area); break;
        case PT_PAGE_DATA: PtLayoutData(Window, &Area); break;
        case PT_PAGE_MODEM: PtLayoutModem(Window, &Area); break;
    }
}

static
VOID
PtPaint(
    _In_ HWND Window)
{
    PAINTSTRUCT Paint;
    RECT Client, Head, Area;
    HDC Dc = BeginPaint(Window, &Paint);

    GetClientRect(Window, &Client);
    FillRect(Dc, &Client, PtBgBrush);

    /* Heading band under the tab bar */
    Head.left = PT_MARGIN;
    Head.right = Client.right - PT_MARGIN;
    Head.top = PT_SAFE_TOP + PT_TAB_H + PT_GAP;
    Head.bottom = Head.top + PT_HEAD_H;
    SetBkMode(Dc, TRANSPARENT);
    SetTextColor(Dc, PT_TEXT);
    SelectObject(Dc, PtHeadFont);
    DrawTextW(Dc, PtPageHeadings[PtPage], -1, &Head, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

    if (PtPage == PT_PAGE_STATUS)
    {
        PtContentRect(Window, &Area);
        Area.bottom -= PT_ROW + PT_GAP;
        SetTextColor(Dc, PT_TEXT);
        SelectObject(Dc, PtFont);
        DrawTextW(Dc, PtStatusText, -1, &Area, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
    }

    EndPaint(Window, &Paint);
}

static
VOID
PtShowGroup(
    _In_ HWND Window,
    _In_ const UINT *Ids,
    _In_ DWORD Count,
    _In_ BOOL Show)
{
    DWORD i;

    for (i = 0; i < Count; i++)
        ShowWindow(GetDlgItem(Window, Ids[i]), Show ? SW_SHOW : SW_HIDE);
}

static
VOID
PtSelectPage(
    _In_ HWND Window,
    _In_ int Page)
{
    UINT Tab;

    PtPage = Page;

    PtShowGroup(Window, PtPhoneControls, _countof(PtPhoneControls), Page == PT_PAGE_PHONE);
    PtShowGroup(Window, PtStatusControls, _countof(PtStatusControls), Page == PT_PAGE_STATUS);
    PtShowGroup(Window, PtDataControls, _countof(PtDataControls), Page == PT_PAGE_DATA);
    PtShowGroup(Window, PtModemControls, _countof(PtModemControls), Page == PT_PAGE_MODEM);

    PtLayout(Window);
    InvalidateRect(Window, NULL, FALSE);

    /* The tabs are child windows, the parent repaint does not reach them */
    for (Tab = IDC_TAB_PHONE; Tab <= IDC_TAB_MODEM; Tab++)
        InvalidateRect(GetDlgItem(Window, Tab), NULL, FALSE);

    if (Page == PT_PAGE_STATUS)
        PtRefreshStatus();
    else if (Page == PT_PAGE_DATA)
    {
        PtLoadData();
        PtRefreshData();
    }
    else if (Page == PT_PAGE_MODEM)
        PtModemOpen(Window);
}

static
LRESULT
CALLBACK
PtWndProc(
    _In_ HWND Window,
    _In_ UINT Message,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    switch (Message)
    {
        case WM_CREATE:
            PtWindow = Window;
            PtCreateControls(Window);
            PtSelectPage(Window, PT_PAGE_PHONE);
            PtOpenPhone();
            if (!PtPhone)
                SetTimer(Window, 1, 3000, NULL);
            return 0;

        case WM_SIZE:
            PtLayout(Window);
            InvalidateRect(Window, NULL, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            PtPaint(Window);
            return 0;

        case WM_DRAWITEM:
            PtDrawButton((const DRAWITEMSTRUCT *)lParam);
            return TRUE;

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetTextColor((HDC)wParam, PT_TEXT);
            SetBkColor((HDC)wParam, PT_FIELD);
            return (LRESULT)PtFieldBrush;

        case WM_CTLCOLORSTATIC:
            SetTextColor((HDC)wParam, PT_SUBTEXT);
            SetBkColor((HDC)wParam, PT_BG);
            return (LRESULT)PtBgBrush;

        case WM_TIMER:
            PtOpenPhone();
            if (PtPhone)
                KillTimer(Window, 1);
            return 0;

        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED)
            {
                UINT Id = LOWORD(wParam);
                if (Id == IDC_EXIT)
                    DestroyWindow(Window);
                else if (Id >= IDC_TAB_PHONE && Id <= IDC_TAB_MODEM)
                    PtSelectPage(Window, (int)(Id - IDC_TAB_PHONE));
                else
                    PtCommand(Id);
            }
            return 0;

        case WM_PHONE_EVENT:
            PtHandleEvent((PPT_EVENT)lParam);
            HeapFree(GetProcessHeap(), 0, (PVOID)lParam);
            return 0;

        case WM_MODEM_PACKET:
            PtModemPacket((const PT_MODEM_PACKET *)lParam);
            HeapFree(GetProcessHeap(), 0, (PVOID)lParam);
            return 0;

        case WM_DESTROY:
            KillTimer(Window, 1);
            PtModemClose();
            if (PtPhone)
            {
                PhoneOmClose(PtPhone);
                PtPhone = NULL;
            }
            if (PtFont)
                DeleteObject(PtFont);
            if (PtBigFont)
                DeleteObject(PtBigFont);
            if (PtButtonFont)
                DeleteObject(PtButtonFont);
            if (PtHeadFont)
                DeleteObject(PtHeadFont);
            if (PtSmallFont)
                DeleteObject(PtSmallFont);
            if (PtBgBrush)
                DeleteObject(PtBgBrush);
            if (PtFieldBrush)
                DeleteObject(PtFieldBrush);
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(Window, Message, wParam, lParam);
}

int
WINAPI
wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nShowCmd)
{
    WNDCLASSEXW Class;
    HWND Window;
    MSG Msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nShowCmd);

    PtInstance = hInstance;
    PtFont = PtMakeFont(34, FW_NORMAL);
    PtBigFont = PtMakeFont(54, FW_NORMAL);
    PtButtonFont = PtMakeFont(32, FW_SEMIBOLD);
    PtHeadFont = PtMakeFont(48, FW_SEMIBOLD);
    PtSmallFont = PtMakeFont(24, FW_NORMAL);
    if (!PtFont)
        PtFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (!PtBigFont)
        PtBigFont = PtFont;
    if (!PtButtonFont)
        PtButtonFont = PtFont;
    if (!PtHeadFont)
        PtHeadFont = PtFont;
    if (!PtSmallFont)
        PtSmallFont = PtFont;

    PtBgBrush = CreateSolidBrush(PT_BG);
    PtFieldBrush = CreateSolidBrush(PT_FIELD);

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = PtWndProc;
    Class.hInstance = hInstance;
    Class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    Class.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    Class.hbrBackground = PtBgBrush;
    Class.lpszClassName = L"ReactOSPhoneTest";
    if (!RegisterClassExW(&Class))
        return 1;

    Window = CreateWindowExW(0,
                             Class.lpszClassName,
                             L"Phone",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT,
                             CW_USEDEFAULT,
                             CW_USEDEFAULT,
                             CW_USEDEFAULT,
                             NULL,
                             NULL,
                             hInstance,
                             NULL);
    if (!Window)
        return 1;

    ShowWindow(Window, SW_SHOWMAXIMIZED);
    UpdateWindow(Window);

    while (GetMessageW(&Msg, NULL, 0, 0) > 0)
    {
        if (!IsDialogMessageW(Window, &Msg))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
    }

    return (int)Msg.wParam;
}
