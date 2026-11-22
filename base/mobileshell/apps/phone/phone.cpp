/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Phone: call history, dial pad and the in-call screen over the phone service
 */

#include <appclient.h>
#include <records.h>
#include <shellapi.h>
#include <strsafe.h>
#include <telephony/phoneom.h>

/*
 * Markup contract (phone.uix):
 *   Elements: PageHistory, PageDial, PageCall, ServiceDown, HistoryEmpty, DialHint, CallButtons,
 *             IncomingButtons, InCallPad
 *   Repeaters: HistoryList (Id, Glyph, Name, Detail, Tone)
 *   Bindings: ServiceText, DialText, CallName, CallNumber, CallState, MuteFill, SpeakerFill,
 *             KeypadFill, HoldFill, MuteInk, SpeakerInk, KeypadInk, HoldInk, PadText
 *   Commands: key:<c>, back, dial, pad, history, contacts, call:<n>, mute, speaker, keypad, hold,
 *             end, answer, ignore
 */

#define LINK_IDLE_MS        5000
#define LINK_CALL_MS        1000
#define HISTORY_FILE        L"calls.txt"
#define HISTORY_MAX         100

typedef struct _CALL_SNAPSHOT
{
    BOOL ServiceUp = FALSE;
    HRESULT Error = S_OK;
    ULONG Generation = 0;
    std::vector<PHONEOM_CALL> Calls;
} CALL_SNAPSHOT;

typedef enum _CALL_OP_KIND
{
    CallOpDial,
    CallOpAnswer,
    CallOpHangUp,
    CallOpDtmf,
    CallOpSpeaker,
    CallOpMute,
    CallOpSwap
} CALL_OP_KIND;

typedef struct _CALL_OP
{
    CALL_OP_KIND Kind;
    DWORD Value;
    std::wstring Number;
} CALL_OP;

typedef VOID
(*PCALL_WAKE)(
    _In_opt_ PVOID Context);

/* Owns the phone service session on its own thread, polls every second while a call is up */
class CCallLink
{
public:
    ~CCallLink()
    {
        Stop();
    }

    BOOL
    Start(
        _In_ PCALL_WAKE WakeRoutine,
        _In_opt_ PVOID Context)
    {
        if (m_Thread)
            return TRUE;

        m_Wake = WakeRoutine;
        m_WakeContext = Context;
        m_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        m_KickEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!m_StopEvent || !m_KickEvent)
            return FALSE;

        m_Thread = CreateThread(NULL, 0, ThreadProc, this, 0, NULL);
        return m_Thread != NULL;
    }

    VOID
    Stop(VOID)
    {
        if (m_Thread)
        {
            SetEvent(m_StopEvent);
            WaitForSingleObject(m_Thread, INFINITE);
            CloseHandle(m_Thread);
            m_Thread = NULL;
        }
        if (m_StopEvent)
            CloseHandle(m_StopEvent);
        if (m_KickEvent)
            CloseHandle(m_KickEvent);
        m_StopEvent = m_KickEvent = NULL;
    }

    VOID
    Queue(
        _In_ CALL_OP_KIND Kind,
        _In_ DWORD Value,
        _In_opt_z_ PCWSTR Number = NULL)
    {
        CALL_OP Op;

        Op.Kind = Kind;
        Op.Value = Value;
        if (Number)
            Op.Number = Number;

        AcquireSRWLockExclusive(&m_Lock);
        m_Ops.push_back(Op);
        ReleaseSRWLockExclusive(&m_Lock);
        Kick();
    }

    VOID
    Kick(VOID)
    {
        if (m_KickEvent)
            SetEvent(m_KickEvent);
    }

    BOOL
    Snapshot(
        _Inout_ CALL_SNAPSHOT &Out)
    {
        BOOL Changed;

        AcquireSRWLockShared(&m_Lock);
        Changed = (Out.Generation != m_Snapshot.Generation);
        if (Changed)
            Out = m_Snapshot;
        ReleaseSRWLockShared(&m_Lock);
        return Changed;
    }

private:
    typedef HRESULT (WINAPI *PFN_OPEN)(PPHONEOM_EVENT_CALLBACK, PVOID, HPHONEOM *);
    typedef VOID (WINAPI *PFN_CLOSE)(HPHONEOM);
    typedef HRESULT (WINAPI *PFN_DIAL)(HPHONEOM, LPCWSTR, PDWORD);
    typedef HRESULT (WINAPI *PFN_ID)(HPHONEOM, DWORD);
    typedef HRESULT (WINAPI *PFN_NONE)(HPHONEOM);
    typedef HRESULT (WINAPI *PFN_DTMF)(HPHONEOM, WCHAR);
    typedef HRESULT (WINAPI *PFN_CALLS)(HPHONEOM, PPHONEOM_CALL, DWORD, PDWORD);
    typedef HRESULT (WINAPI *PFN_BOOL)(HPHONEOM, BOOL);

    static
    VOID
    CALLBACK
    OnPhoneEvent(
        _In_ HPHONEOM Phone,
        _In_ DWORD Event,
        _In_reads_bytes_opt_(DataSize) const VOID *Data,
        _In_ DWORD DataSize,
        _In_opt_ PVOID Context)
    {
        CCallLink *This = static_cast<CCallLink *>(Context);

        UNREFERENCED_PARAMETER(Phone);
        UNREFERENCED_PARAMETER(Data);
        UNREFERENCED_PARAMETER(DataSize);

        /* The session is closed on the link thread, never from its own callback */
        if (Event == PhoneEventDisconnected)
            InterlockedExchange(&This->m_Lost, TRUE);
        if (Event == PhoneEventCall || Event == PhoneEventDisconnected)
            This->Kick();
    }

    BOOL
    Load(VOID)
    {
        if (m_Module)
            return TRUE;

        m_Module = LoadLibraryW(L"phoneom.dll");
        if (!m_Module)
            return FALSE;

        m_Open = reinterpret_cast<PFN_OPEN>(GetProcAddress(m_Module, "PhoneOmOpen"));
        m_Close = reinterpret_cast<PFN_CLOSE>(GetProcAddress(m_Module, "PhoneOmClose"));
        m_Dial = reinterpret_cast<PFN_DIAL>(GetProcAddress(m_Module, "PhoneOmDial"));
        m_Answer = reinterpret_cast<PFN_ID>(GetProcAddress(m_Module, "PhoneOmAnswer"));
        m_HangUp = reinterpret_cast<PFN_ID>(GetProcAddress(m_Module, "PhoneOmHangUp"));
        m_Swap = reinterpret_cast<PFN_NONE>(GetProcAddress(m_Module, "PhoneOmSwap"));
        m_Dtmf = reinterpret_cast<PFN_DTMF>(GetProcAddress(m_Module, "PhoneOmSendDtmf"));
        m_Calls = reinterpret_cast<PFN_CALLS>(GetProcAddress(m_Module, "PhoneOmGetCalls"));
        m_Route = reinterpret_cast<PFN_ID>(GetProcAddress(m_Module, "PhoneOmSetAudioRoute"));
        m_Mute = reinterpret_cast<PFN_BOOL>(GetProcAddress(m_Module, "PhoneOmSetMute"));

        if (!m_Open || !m_Close || !m_Dial || !m_Answer || !m_HangUp || !m_Swap || !m_Dtmf || !m_Calls ||
            !m_Route || !m_Mute)
        {
            FreeLibrary(m_Module);
            m_Module = NULL;
            SetLastError(ERROR_PROC_NOT_FOUND);
            return FALSE;
        }
        return TRUE;
    }

    VOID
    Drain(VOID)
    {
        CALL_OP Op;

        for (;;)
        {
            AcquireSRWLockExclusive(&m_Lock);
            if (m_Ops.empty())
            {
                ReleaseSRWLockExclusive(&m_Lock);
                return;
            }
            Op = m_Ops.front();
            m_Ops.erase(m_Ops.begin());
            ReleaseSRWLockExclusive(&m_Lock);

            switch (Op.Kind)
            {
                case CallOpDial:
                    m_Dial(m_Phone, Op.Number.c_str(), NULL);
                    break;

                case CallOpAnswer:
                    m_Answer(m_Phone, Op.Value);
                    break;

                case CallOpHangUp:
                    m_HangUp(m_Phone, Op.Value);
                    break;

                case CallOpDtmf:
                    m_Dtmf(m_Phone, static_cast<WCHAR>(Op.Value));
                    break;

                case CallOpSpeaker:
                    m_Route(m_Phone, Op.Value ? PhoneAudioSpeaker : PhoneAudioEarpiece);
                    break;

                case CallOpMute:
                    m_Mute(m_Phone, Op.Value != 0);
                    break;

                case CallOpSwap:
                    m_Swap(m_Phone);
                    break;
            }
        }
    }

    /* Returns TRUE while a call is up so the next poll comes a second later */
    BOOL
    Poll(VOID)
    {
        CALL_SNAPSHOT Next;
        PHONEOM_CALL Calls[PHONEOM_MAX_CALLS];
        DWORD Count = 0;
        HRESULT hr = S_OK;

        if (InterlockedExchange(&m_Lost, FALSE) && m_Phone)
        {
            m_Close(m_Phone);
            m_Phone = NULL;
        }

        if (!Load())
            hr = HRESULT_FROM_WIN32(GetLastError());
        else if (!m_Phone && FAILED(hr = m_Open(OnPhoneEvent, this, &m_Phone)))
            m_Phone = NULL;

        if (m_Phone)
        {
            Next.ServiceUp = TRUE;
            Drain();

            hr = m_Calls(m_Phone, Calls, ARRAYSIZE(Calls), &Count);
            if (SUCCEEDED(hr))
            {
                for (DWORD Index = 0; Index < Count && Index < ARRAYSIZE(Calls); Index++)
                {
                    Calls[Index].Number[PHONEOM_MAX_NUMBER - 1] = UNICODE_NULL;
                    Calls[Index].Name[PHONEOM_MAX_NAME - 1] = UNICODE_NULL;
                    if (Calls[Index].State != PhoneCallIdle && Calls[Index].State != PhoneCallEnded)
                        Next.Calls.push_back(Calls[Index]);
                }
            }
        }
        Next.Error = hr;

        AcquireSRWLockExclusive(&m_Lock);
        Next.Generation = m_Snapshot.Generation + 1;
        m_Snapshot = Next;
        ReleaseSRWLockExclusive(&m_Lock);

        m_Wake(m_WakeContext);
        return !Next.Calls.empty();
    }

    static
    DWORD
    WINAPI
    ThreadProc(
        _In_ LPVOID Parameter)
    {
        CCallLink *This = static_cast<CCallLink *>(Parameter);
        HANDLE Events[2] = { This->m_StopEvent, This->m_KickEvent };
        DWORD Period;

        do
        {
            Period = This->Poll() ? LINK_CALL_MS : LINK_IDLE_MS;
        } while (WaitForMultipleObjects(ARRAYSIZE(Events), Events, FALSE, Period) != WAIT_OBJECT_0);

        if (This->m_Phone)
            This->m_Close(This->m_Phone);
        This->m_Phone = NULL;
        if (This->m_Module)
            FreeLibrary(This->m_Module);
        This->m_Module = NULL;
        return 0;
    }

    PCALL_WAKE m_Wake = NULL;
    PVOID m_WakeContext = NULL;
    SRWLOCK m_Lock = SRWLOCK_INIT;
    HANDLE m_Thread = NULL;
    HANDLE m_StopEvent = NULL;
    HANDLE m_KickEvent = NULL;
    volatile LONG m_Lost = FALSE;
    CALL_SNAPSHOT m_Snapshot;
    std::vector<CALL_OP> m_Ops;

    HMODULE m_Module = NULL;
    HPHONEOM m_Phone = NULL;
    PFN_OPEN m_Open = NULL;
    PFN_CLOSE m_Close = NULL;
    PFN_DIAL m_Dial = NULL;
    PFN_ID m_Answer = NULL;
    PFN_ID m_HangUp = NULL;
    PFN_NONE m_Swap = NULL;
    PFN_DTMF m_Dtmf = NULL;
    PFN_CALLS m_Calls = NULL;
    PFN_ID m_Route = NULL;
    PFN_BOOL m_Mute = NULL;
};

typedef enum _PHONE_PAGE
{
    PageHistory,
    PageDial,
    PageCall
} PHONE_PAGE;

/* One line of calls.txt: number, name, kind (in, out, missed), start time and seconds talked */
typedef struct _CALL_ENTRY
{
    std::wstring Number;
    std::wstring Name;
    std::wstring Kind;
    FILETIME Start = {};
    ULONG Seconds = 0;
} CALL_ENTRY;

/* What the app watched of a call, kept until the service drops it */
typedef struct _CALL_TRACK
{
    DWORD CallId;
    BOOL Incoming;
    BOOL Answered;
    FILETIME Start;
    ULONGLONG ActiveSince;
    std::wstring Number;
    std::wstring Name;
} CALL_TRACK;

static
std::wstring
NumberKey(
    _In_ const std::wstring &Number)
{
    std::wstring Digits;

    for (WCHAR Char : Number)
    {
        if (iswdigit(Char))
            Digits += Char;
    }

    if (Digits.empty())
        return Number;
    if (Digits.size() > 10)
        Digits.erase(0, Digits.size() - 10);
    return Digits;
}

static
std::wstring
FormatWhen(
    _In_ const FILETIME &Utc)
{
    FILETIME Local;
    SYSTEMTIME When, Now;
    WCHAR Time[32] = L"";
    WCHAR Date[64] = L"";

    if (!Utc.dwLowDateTime && !Utc.dwHighDateTime)
        return std::wstring();

    FileTimeToLocalFileTime(&Utc, &Local);
    FileTimeToSystemTime(&Local, &When);
    GetLocalTime(&Now);

    GetTimeFormatEx(NULL, TIME_NOSECONDS, &When, NULL, Time, ARRAYSIZE(Time));
    if (When.wYear == Now.wYear && When.wMonth == Now.wMonth && When.wDay == Now.wDay)
        return Time;

    GetDateFormatEx(NULL, DATE_SHORTDATE, &When, NULL, Date, ARRAYSIZE(Date), NULL);
    return std::wstring(Date) + L" " + Time;
}

static
std::wstring
FormatDuration(
    _In_ ULONG Seconds)
{
    WCHAR Text[32];

    if (Seconds >= 3600)
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu:%02lu:%02lu", Seconds / 3600, (Seconds / 60) % 60, Seconds % 60);
    else
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu:%02lu", Seconds / 60, Seconds % 60);
    return Text;
}

class CPhoneApp : public CAppClient
{
protected:
    VOID OnActivate(VOID) override;

    VOID OnCommand(
        _In_z_ PCWSTR Command) override;

    VOID OnSoftKey(
        _In_ SHELL_SOFTKEY Key) override;

    VOID OnCloseButton(VOID) override;

    VOID OnChar(
        _In_ WCHAR Char) override;

    VOID OnKey(
        _In_ UINT VirtualKey) override;

    VOID OnOpenDocument(
        _In_z_ PCWSTR Path) override;

    VOID OnWake(VOID) override;

private:
    static
    VOID
    LinkWake(
        _In_opt_ PVOID Context)
    {
        static_cast<CPhoneApp *>(Context)->Wake();
    }

    VOID
    LoadNames(VOID);

    std::wstring
    NameFor(
        _In_ const std::wstring &Number) const;

    VOID
    LoadHistory(VOID);

    VOID
    SaveHistory(VOID);

    VOID
    ShowPage(
        _In_ PHONE_PAGE Page);

    VOID
    RefreshHistory(VOID);

    VOID
    RefreshDial(VOID);

    VOID
    RefreshCall(VOID);

    VOID
    TrackCalls(VOID);

    const PHONEOM_CALL *
    CurrentCall(VOID) const;

    VOID
    Dial(
        _In_ const std::wstring &Number);

    VOID
    PressKey(
        _In_ WCHAR Key);

    VOID
    SetToggle(
        _In_z_ PCWSTR Name,
        _In_ BOOL On);

    CCallLink m_Link;
    CALL_SNAPSHOT m_Snapshot;
    std::vector<CALL_TRACK> m_Tracks;
    std::vector<CALL_ENTRY> m_History;
    std::vector<std::pair<std::wstring, std::wstring>> m_Names;
    PHONE_PAGE m_Page = PageHistory;
    PHONE_PAGE m_IdlePage = PageHistory;
    std::wstring m_Number;
    std::wstring m_Pad;
    BOOL m_Mute = FALSE;
    BOOL m_Speaker = FALSE;
    BOOL m_PadOpen = FALSE;
    BOOL m_Started = FALSE;
};

VOID
CPhoneApp::LoadNames(VOID)
{
    std::vector<APP_RECORD> Records;

    /* People keeps name and mobile number as its first two fields */
    m_Names.clear();
    if (!LoadRecords(L"contacts.txt", Records))
        return;

    for (const APP_RECORD &Record : Records)
    {
        if (Record.size() >= 2 && !Record[0].empty() && !Record[1].empty())
            m_Names.emplace_back(NumberKey(Record[1]), Record[0]);
    }
}

std::wstring
CPhoneApp::NameFor(
    _In_ const std::wstring &Number) const
{
    std::wstring Key = NumberKey(Number);

    for (const auto &Entry : m_Names)
    {
        if (Entry.first == Key)
            return Entry.second;
    }
    return std::wstring();
}

VOID
CPhoneApp::LoadHistory(VOID)
{
    std::vector<APP_RECORD> Records;
    ULARGE_INTEGER Time;

    m_History.clear();
    if (!LoadRecords(HISTORY_FILE, Records))
        return;

    for (const APP_RECORD &Record : Records)
    {
        CALL_ENTRY Entry;

        if (Record.size() < 5)
            continue;

        Entry.Number = Record[0];
        Entry.Name = Record[1];
        Entry.Kind = Record[2];
        Time.QuadPart = _wcstoui64(Record[3].c_str(), NULL, 10);
        Entry.Start.dwLowDateTime = Time.LowPart;
        Entry.Start.dwHighDateTime = Time.HighPart;
        Entry.Seconds = wcstoul(Record[4].c_str(), NULL, 10);
        m_History.push_back(Entry);
    }
}

VOID
CPhoneApp::SaveHistory(VOID)
{
    std::vector<APP_RECORD> Records;
    ULARGE_INTEGER Time;
    WCHAR Text[32];

    while (m_History.size() > HISTORY_MAX)
        m_History.erase(m_History.begin());

    for (const CALL_ENTRY &Entry : m_History)
    {
        APP_RECORD Record;

        Time.LowPart = Entry.Start.dwLowDateTime;
        Time.HighPart = Entry.Start.dwHighDateTime;
        Record.push_back(Entry.Number);
        Record.push_back(Entry.Name);
        Record.push_back(Entry.Kind);
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%I64u", Time.QuadPart);
        Record.push_back(Text);
        StringCchPrintfW(Text, ARRAYSIZE(Text), L"%lu", Entry.Seconds);
        Record.push_back(Text);
        Records.push_back(Record);
    }
    SaveRecords(HISTORY_FILE, Records);
}

VOID
CPhoneApp::ShowPage(
    _In_ PHONE_PAGE Page)
{
    m_Page = Page;
    if (Page != PageCall)
        m_IdlePage = Page;

    Scene()->SetVisible(L"PageHistory", Page == PageHistory);
    Scene()->SetVisible(L"PageDial", Page == PageDial);
    Scene()->SetVisible(L"PageCall", Page == PageCall);
    Scene()->SetSelected(NULL);

    if (Page == PageHistory)
    {
        RefreshHistory();
        SetChrome(L"Phone", L"Keypad", L"Contacts", ShellCloseOk);
    }
    else if (Page == PageDial)
    {
        RefreshDial();
        SetChrome(L"Phone", L"Call", L"History", ShellCloseOk);
    }
    else
    {
        RefreshCall();
        SetChrome(L"Phone", L"", L"", ShellCloseNone);
    }
}

VOID
CPhoneApp::RefreshHistory(VOID)
{
    static PCWSTR const Fields[] = { L"Id", L"Glyph", L"Name", L"Detail", L"Tone" };
    std::vector<std::wstring> Storage;
    std::vector<PCWSTR> Values;
    WCHAR Id[16];
    ULONG Count = 0;

    /* Newest first */
    for (SIZE_T Index = m_History.size(); Index-- > 0;)
    {
        const CALL_ENTRY &Entry = m_History[Index];
        BOOL Missed = (Entry.Kind == L"missed");
        std::wstring Detail = FormatWhen(Entry.Start);

        if (Missed)
            Detail = L"Missed, " + Detail;
        else if (Entry.Seconds)
            Detail += L", " + FormatDuration(Entry.Seconds);

        StringCchPrintfW(Id, ARRAYSIZE(Id), L"%Iu", Index);
        Storage.push_back(Id);
        Storage.push_back(Entry.Kind == L"out" ? L"\xE8B6" : L"\xE8B5");
        Storage.push_back(Entry.Name.empty() ? Entry.Number : Entry.Name);
        Storage.push_back(Detail);
        Storage.push_back(Missed ? L"@AccentText" : L"@AppSubText");
        Count++;
    }

    for (const std::wstring &Value : Storage)
        Values.push_back(Value.c_str());

    Scene()->SetItems(L"HistoryList", Count, ARRAYSIZE(Fields), Fields, Values.data());
    Scene()->SetVisible(L"HistoryEmpty", Count == 0 && m_Snapshot.ServiceUp);
    Scene()->SetBinding(L"ServiceText", m_Started ?
                        L"The phone service isn't running, so calls can't be made right now." :
                        L"Connecting to the phone service...");
    Scene()->SetVisible(L"ServiceDown", !m_Snapshot.ServiceUp);
}

VOID
CPhoneApp::RefreshDial(VOID)
{
    Scene()->SetBinding(L"DialText", m_Number.c_str());
    Scene()->SetVisible(L"DialHint", m_Number.empty());
    Scene()->SetVisible(L"ServiceDown", !m_Snapshot.ServiceUp);
}

VOID
CPhoneApp::SetToggle(
    _In_z_ PCWSTR Name,
    _In_ BOOL On)
{
    std::wstring Fill = std::wstring(Name) + L"Fill";
    std::wstring Ink = std::wstring(Name) + L"Ink";

    Scene()->SetBinding(Fill.c_str(), On ? L"@Accent" : L"@ChromeMediumLow");
    Scene()->SetBinding(Ink.c_str(), On ? L"@BaseWhite" : L"@AppText");
}

const PHONEOM_CALL *
CPhoneApp::CurrentCall(VOID) const
{
    static const DWORD Order[] = { PhoneCallIncoming, PhoneCallActive, PhoneCallDialing, PhoneCallHeld };

    for (DWORD State : Order)
    {
        for (const PHONEOM_CALL &Call : m_Snapshot.Calls)
        {
            if (Call.State == State)
                return &Call;
        }
    }
    return NULL;
}

VOID
CPhoneApp::RefreshCall(VOID)
{
    const PHONEOM_CALL *Call = CurrentCall();
    std::wstring Name, Number, State;
    BOOL Incoming = FALSE;
    BOOL Held = FALSE;

    if (Call)
    {
        Number = Call->Number;
        Name = Call->Name[0] ? std::wstring(Call->Name) : NameFor(Number);
        if (Name.empty())
        {
            Name = Number.empty() ? L"Unknown" : Number;
            Number.clear();
        }

        Incoming = (Call->State == PhoneCallIncoming);
        switch (Call->State)
        {
            case PhoneCallIncoming:
                State = L"Incoming call";
                break;

            case PhoneCallDialing:
                State = L"Calling...";
                break;

            case PhoneCallHeld:
                State = L"On hold";
                Held = TRUE;
                break;

            default:
                for (const CALL_TRACK &Track : m_Tracks)
                {
                    if (Track.CallId == Call->CallId && Track.ActiveSince)
                        State = FormatDuration(static_cast<ULONG>((GetTickCount64() - Track.ActiveSince) / 1000));
                }
                if (State.empty())
                    State = L"0:00";
                break;
        }

        if (Call->Flags & PHONEOM_CALL_EMERGENCY)
            State = L"Emergency call, " + State;
        if (m_Snapshot.Calls.size() > 1)
            State += L" (" + std::to_wstring(m_Snapshot.Calls.size()) + L" calls)";
    }

    Scene()->SetBinding(L"CallName", Name.c_str());
    Scene()->SetBinding(L"CallNumber", Number.c_str());
    Scene()->SetBinding(L"CallState", State.c_str());
    Scene()->SetBinding(L"PadText", m_Pad.c_str());
    Scene()->SetVisible(L"IncomingButtons", Incoming);
    Scene()->SetVisible(L"CallButtons", !Incoming);
    Scene()->SetVisible(L"InCallPad", !Incoming && m_PadOpen);
    SetToggle(L"Mute", m_Mute);
    SetToggle(L"Speaker", m_Speaker);
    SetToggle(L"Keypad", m_PadOpen);
    SetToggle(L"Hold", Held);
}

/* Follows each call from first sight to when the service drops it and logs it then */
VOID
CPhoneApp::TrackCalls(VOID)
{
    BOOL Changed = FALSE;

    for (const PHONEOM_CALL &Call : m_Snapshot.Calls)
    {
        CALL_TRACK *Found = NULL;

        for (CALL_TRACK &Track : m_Tracks)
        {
            if (Track.CallId == Call.CallId)
                Found = &Track;
        }

        if (!Found)
        {
            CALL_TRACK Track;

            Track.CallId = Call.CallId;
            Track.Incoming = (Call.Flags & PHONEOM_CALL_INCOMING) != 0 || Call.State == PhoneCallIncoming;
            Track.Answered = FALSE;
            GetSystemTimeAsFileTime(&Track.Start);
            Track.ActiveSince = 0;
            Track.Number = Call.Number;
            Track.Name = Call.Name[0] ? std::wstring(Call.Name) : NameFor(Track.Number);
            m_Tracks.push_back(Track);
            Found = &m_Tracks.back();
        }

        if ((Call.State == PhoneCallActive || Call.State == PhoneCallHeld) && !Found->ActiveSince)
        {
            Found->Answered = TRUE;
            Found->ActiveSince = GetTickCount64();
        }
    }

    for (SIZE_T Index = m_Tracks.size(); Index-- > 0;)
    {
        BOOL Present = FALSE;
        CALL_ENTRY Entry;

        for (const PHONEOM_CALL &Call : m_Snapshot.Calls)
        {
            if (Call.CallId == m_Tracks[Index].CallId)
                Present = TRUE;
        }
        if (Present)
            continue;

        Entry.Number = m_Tracks[Index].Number;
        Entry.Name = m_Tracks[Index].Name;
        Entry.Start = m_Tracks[Index].Start;
        if (!m_Tracks[Index].Incoming)
            Entry.Kind = L"out";
        else
            Entry.Kind = m_Tracks[Index].Answered ? L"in" : L"missed";
        if (m_Tracks[Index].ActiveSince)
            Entry.Seconds = static_cast<ULONG>((GetTickCount64() - m_Tracks[Index].ActiveSince) / 1000);

        m_History.push_back(Entry);
        m_Tracks.erase(m_Tracks.begin() + Index);
        Changed = TRUE;
    }

    if (Changed)
        SaveHistory();
}

VOID
CPhoneApp::Dial(
    _In_ const std::wstring &Number)
{
    if (Number.empty())
    {
        /* Call with nothing typed brings back the last number, as phones do */
        if (!m_History.empty())
        {
            m_Number = m_History.back().Number;
            RefreshDial();
        }
        return;
    }

    m_Link.Queue(CallOpDial, 0, Number.c_str());
    m_Mute = m_Speaker = m_PadOpen = FALSE;
    m_Pad.clear();
    m_Number.clear();
}

VOID
CPhoneApp::PressKey(
    _In_ WCHAR Key)
{
    if (m_Page == PageCall)
    {
        m_Pad += Key;
        m_Link.Queue(CallOpDtmf, Key);
        RefreshCall();
        return;
    }

    if (m_Page != PageDial)
        ShowPage(PageDial);
    if (m_Number.size() < PHONEOM_MAX_NUMBER - 1)
        m_Number += Key;
    RefreshDial();
}

VOID
CPhoneApp::OnOpenDocument(
    _In_z_ PCWSTR Path)
{
    /* Other apps hand a number over as the document, a tel: prefix is allowed */
    std::wstring Number = Path;

    if (_wcsnicmp(Number.c_str(), L"tel:", 4) == 0)
        Number.erase(0, 4);
    m_Number = Number;
    ShowPage(PageDial);
    Present();
}

VOID
CPhoneApp::OnActivate(VOID)
{
    LoadNames();
    LoadHistory();

    if (!m_Started)
    {
        m_Link.Start(LinkWake, this);
        m_Started = TRUE;
    }
    ShowPage(m_Page);
}

VOID
CPhoneApp::OnWake(VOID)
{
    BOOL HadCall = (m_Page == PageCall);

    if (!m_Link.Snapshot(m_Snapshot))
        return;

    TrackCalls();

    if (!m_Snapshot.Calls.empty())
    {
        if (!HadCall)
            ShowPage(PageCall);
        else
            RefreshCall();
    }
    else if (HadCall)
    {
        m_Mute = m_Speaker = m_PadOpen = FALSE;
        m_Pad.clear();
        ShowPage(PageHistory);
    }
    else if (m_Page == PageHistory)
    {
        RefreshHistory();
    }
    else
    {
        RefreshDial();
    }
}

VOID
CPhoneApp::OnCommand(
    _In_z_ PCWSTR Command)
{
    const PHONEOM_CALL *Call = CurrentCall();

    if (wcsncmp(Command, L"key:", 4) == 0 && Command[4])
    {
        PressKey(Command[4]);
    }
    else if (wcscmp(Command, L"back") == 0)
    {
        if (!m_Number.empty())
            m_Number.pop_back();
        RefreshDial();
    }
    else if (wcscmp(Command, L"dial") == 0)
    {
        Dial(m_Number);
    }
    else if (wcscmp(Command, L"pad") == 0)
    {
        ShowPage(PageDial);
    }
    else if (wcscmp(Command, L"history") == 0)
    {
        ShowPage(PageHistory);
    }
    else if (wcscmp(Command, L"contacts") == 0)
    {
        Launch(L"Contacts");
    }
    else if (wcsncmp(Command, L"call:", 5) == 0)
    {
        SIZE_T Index = wcstoul(Command + 5, NULL, 10);

        if (Index < m_History.size())
            Dial(m_History[Index].Number);
    }
    else if (wcscmp(Command, L"mute") == 0)
    {
        m_Mute = !m_Mute;
        m_Link.Queue(CallOpMute, m_Mute);
        RefreshCall();
    }
    else if (wcscmp(Command, L"speaker") == 0)
    {
        m_Speaker = !m_Speaker;
        m_Link.Queue(CallOpSpeaker, m_Speaker);
        RefreshCall();
    }
    else if (wcscmp(Command, L"keypad") == 0)
    {
        m_PadOpen = !m_PadOpen;
        RefreshCall();
    }
    else if (wcscmp(Command, L"hold") == 0)
    {
        m_Link.Queue(CallOpSwap, 0);
    }
    else if (wcscmp(Command, L"end") == 0 || wcscmp(Command, L"ignore") == 0)
    {
        m_Link.Queue(CallOpHangUp, Call ? Call->CallId : 0);
    }
    else if (wcscmp(Command, L"answer") == 0)
    {
        if (Call)
            m_Link.Queue(CallOpAnswer, Call->CallId);
    }
    else
    {
        CAppClient::OnCommand(Command);
        return;
    }
    Present();
}

VOID
CPhoneApp::OnSoftKey(
    _In_ SHELL_SOFTKEY Key)
{
    if (m_Page == PageHistory)
    {
        if (Key == ShellSoftKeyLeft)
            ShowPage(PageDial);
        else if (Key == ShellSoftKeyRight)
            Launch(L"Contacts");
    }
    else if (m_Page == PageDial)
    {
        if (Key == ShellSoftKeyLeft)
            Dial(m_Number);
        else if (Key == ShellSoftKeyRight)
            ShowPage(PageHistory);
    }
    Present();
}

VOID
CPhoneApp::OnCloseButton(VOID)
{
    /* A call keeps the app up, the shell can hide it but closing would orphan the call screen */
    if (!m_Snapshot.Calls.empty())
    {
        RequestHide();
        return;
    }
    if (m_Page == PageDial)
    {
        ShowPage(PageHistory);
        Present();
        return;
    }
    RequestClose();
}

VOID
CPhoneApp::OnChar(
    _In_ WCHAR Char)
{
    if ((Char >= L'0' && Char <= L'9') || Char == L'*' || Char == L'#' || Char == L'+')
    {
        PressKey(Char);
        Present();
    }
}

VOID
CPhoneApp::OnKey(
    _In_ UINT VirtualKey)
{
    if (VirtualKey == VK_BACK && m_Page == PageDial)
    {
        OnCommand(L"back");
    }
    else if (VirtualKey == VK_RETURN && m_Page == PageDial)
    {
        OnCommand(L"dial");
    }
    else
    {
        CAppClient::OnKey(VirtualKey);
    }
}

INT
WINAPI
wWinMain(
    _In_ HINSTANCE Instance,
    _In_opt_ HINSTANCE Previous,
    _In_ PWSTR CommandLine,
    _In_ INT ShowCommand)
{
    CPhoneApp App;
    PWSTR *Arguments;
    INT Count = 0;
    INT Result = 1;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CommandLine);
    UNREFERENCED_PARAMETER(ShowCommand);

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    Arguments = CommandLineToArgvW(GetCommandLineW(), &Count);
    if (Arguments && SUCCEEDED(App.Initialize(Count, Arguments)))
        Result = App.Run();
    LocalFree(Arguments);
    CoUninitialize();
    return Result;
}
