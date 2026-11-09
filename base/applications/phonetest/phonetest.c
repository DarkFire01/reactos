/*
 * PROJECT:     ReactOS Phone Test
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Minimal call and SMS front end for the phone service
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <wingdi.h>
#include <stdlib.h>
#include <strsafe.h>

#include <telephony/phoneom.h>

#define WM_PHONE_EVENT              (WM_APP + 1)

enum
{
    IDC_RADIO_STATUS = 100,
    IDC_RADIO_ON,
    IDC_RADIO_OFF,
    IDC_NUMBER,
    IDC_DIAL,
    IDC_ANSWER,
    IDC_HANGUP,
    IDC_SWAP,
    IDC_DTMF,
    IDC_SPEAKER,
    IDC_MUTE,
    IDC_CALLS,
    IDC_SMS_TO,
    IDC_SMS_TEXT,
    IDC_SMS_SEND,
    IDC_MESSAGES,
    IDC_LOG
};

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

static const LPCWSTR PtStateNames[] =
{
    L"idle", L"dialing", L"incoming", L"active", L"held", L"ended"
};

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

static
VOID
PtCheck(
    _In_ LPCWSTR What,
    _In_ HRESULT hr)
{
    if (FAILED(hr))
        PtLog(L"%s failed 0x%08lx", What, hr);
    else
        PtLog(L"%s ok", What);
}

/* Event callback runs on the phoneom dispatch thread, the window owns the data from here */
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
        SetDlgItemTextW(PtWindow, IDC_RADIO_STATUS, L"Radio: no phone service");
        return;
    }

    StringCchPrintfW(Text,
                     _countof(Text),
                     L"Radio: %s, %s",
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
    }
}

static
HWND
PtControl(
    _In_ HWND Parent,
    _In_ LPCWSTR Class,
    _In_opt_ LPCWSTR Text,
    _In_ DWORD Style,
    _In_ int X,
    _In_ int Y,
    _In_ int Width,
    _In_ int Height,
    _In_ UINT Id)
{
    HWND Control;
    DWORD ExStyle = 0;

    if (!wcscmp(Class, L"EDIT") || !wcscmp(Class, L"LISTBOX"))
        ExStyle = WS_EX_CLIENTEDGE;

    Control = CreateWindowExW(ExStyle,
                              Class,
                              Text,
                              WS_CHILD | WS_VISIBLE | Style,
                              X,
                              Y,
                              Width,
                              Height,
                              Parent,
                              (HMENU)(ULONG_PTR)Id,
                              PtInstance,
                              NULL);
    if (Control)
        SendMessageW(Control, WM_SETFONT, (WPARAM)PtFont, FALSE);
    return Control;
}

static
VOID
PtCreateControls(
    _In_ HWND Window)
{
    PtControl(Window, L"STATIC", L"Radio: ?", 0, 10, 12, 260, 20, IDC_RADIO_STATUS);
    PtControl(Window, L"BUTTON", L"Radio on", WS_TABSTOP, 280, 8, 90, 26, IDC_RADIO_ON);
    PtControl(Window, L"BUTTON", L"Radio off", WS_TABSTOP, 376, 8, 90, 26, IDC_RADIO_OFF);

    PtControl(Window, L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, 10, 44, 200, 24, IDC_NUMBER);
    PtControl(Window, L"BUTTON", L"Dial", WS_TABSTOP, 216, 42, 60, 26, IDC_DIAL);
    PtControl(Window, L"BUTTON", L"Answer", WS_TABSTOP, 280, 42, 60, 26, IDC_ANSWER);
    PtControl(Window, L"BUTTON", L"Hang up", WS_TABSTOP, 344, 42, 60, 26, IDC_HANGUP);
    PtControl(Window, L"BUTTON", L"Swap", WS_TABSTOP, 408, 42, 58, 26, IDC_SWAP);
    PtControl(Window, L"BUTTON", L"Tone", WS_TABSTOP, 10, 74, 60, 24, IDC_DTMF);
    PtControl(Window, L"BUTTON", L"Speaker", WS_TABSTOP | BS_AUTOCHECKBOX, 80, 76, 90, 20, IDC_SPEAKER);
    PtControl(Window, L"BUTTON", L"Mute", WS_TABSTOP | BS_AUTOCHECKBOX, 176, 76, 80, 20, IDC_MUTE);
    PtControl(Window, L"LISTBOX", NULL, WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, 10, 104, 456, 70, IDC_CALLS);

    PtControl(Window, L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, 10, 184, 140, 24, IDC_SMS_TO);
    PtControl(Window, L"EDIT", NULL, WS_TABSTOP | ES_AUTOHSCROLL, 156, 184, 244, 24, IDC_SMS_TEXT);
    PtControl(Window, L"BUTTON", L"Send", WS_TABSTOP, 406, 182, 60, 26, IDC_SMS_SEND);
    PtControl(Window, L"LISTBOX", NULL, WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT, 10, 214, 456, 130, IDC_MESSAGES);

    PtControl(Window, L"LISTBOX", NULL, WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 10, 350, 456, 110, IDC_LOG);
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
            PtOpenPhone();
            if (!PtPhone)
                SetTimer(Window, 1, 3000, NULL);
            return 0;

        case WM_TIMER:
            PtOpenPhone();
            if (PtPhone)
                KillTimer(Window, 1);
            return 0;

        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED)
                PtCommand(LOWORD(wParam));
            return 0;

        case WM_PHONE_EVENT:
            PtHandleEvent((PPT_EVENT)lParam);
            HeapFree(GetProcessHeap(), 0, (PVOID)lParam);
            return 0;

        case WM_DESTROY:
            KillTimer(Window, 1);
            if (PtPhone)
            {
                PhoneOmClose(PtPhone);
                PtPhone = NULL;
            }
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
    RECT Rect = { 0, 0, 476, 470 };
    HWND Window;
    MSG Msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    PtInstance = hInstance;
    PtFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    ZeroMemory(&Class, sizeof(Class));
    Class.cbSize = sizeof(Class);
    Class.lpfnWndProc = PtWndProc;
    Class.hInstance = hInstance;
    Class.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    Class.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    Class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    Class.lpszClassName = L"ReactOSPhoneTest";
    if (!RegisterClassExW(&Class))
        return 1;

    AdjustWindowRect(&Rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    Window = CreateWindowExW(0,
                             Class.lpszClassName,
                             L"Phone Test",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT,
                             CW_USEDEFAULT,
                             Rect.right - Rect.left,
                             Rect.bottom - Rect.top,
                             NULL,
                             NULL,
                             hInstance,
                             NULL);
    if (!Window)
        return 1;

    ShowWindow(Window, nShowCmd);
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
