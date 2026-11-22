/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     Cellular radio status from the phone service, polled off the caller's thread
 */

#pragma once

#include <windows.h>
#include <telephony/phoneom.h>

typedef struct _CELL_STATUS
{
    /* phoneom.dll loaded and a session to the phone service open */
    BOOL ServiceUp;
    HRESULT LastError;
    ULONG Generation;

    BOOL HaveRadio;
    BOOL HaveSim;
    BOOL HaveNetwork;
    BOOL HaveDevice;
    BOOL HaveData;
    BOOL HaveDataStatus;
    HRESULT ActionResult;

    PHONEOM_RADIO Radio;
    PHONEOM_SUBSCRIBER Sim;
    PHONEOM_NETWORK Network;
    PHONEOM_DEVICE_INFO Device;
    PHONEOM_DATA_CONFIG Data;
    PHONEOM_DATA_STATUS DataStatus;
} CELL_STATUS, *PCELL_STATUS;

#define CELL_MESSAGE_QUEUE      8
#define CELL_MESSAGE_PREVIEW    96

/* A message that just arrived, enough for a notification */
typedef struct _CELL_MESSAGE
{
    DWORD MessageId;
    BOOL Picture;
    WCHAR Address[PHONEOM_MAX_NUMBER];
    WCHAR Preview[CELL_MESSAGE_PREVIEW];
} CELL_MESSAGE, *PCELL_MESSAGE;

typedef VOID
(CALLBACK *PCELL_NOTIFY)(
    _In_opt_ PVOID Context);

/* Five bars from what the service reports, or from the level when it gives no bars */
inline
ULONG
CellSignalBars(
    _In_ const CELL_STATUS *Status)
{
    LONG Dbm = Status->Network.SignalDbm;

    if (!Status->HaveNetwork ||
        (Status->Network.RegState != PHONEOM_REG_HOME && Status->Network.RegState != PHONEOM_REG_ROAMING))
    {
        return 0;
    }

    if (Status->Network.SignalBars)
        return min(Status->Network.SignalBars, 5UL);
    if (Dbm >= 0 || Dbm <= -125)
        return 0;
    if (Dbm >= -85)
        return 5;
    if (Dbm >= -95)
        return 4;
    if (Dbm >= -105)
        return 3;
    if (Dbm >= -115)
        return 2;
    return 1;
}

inline
PCWSTR
CellRatLabel(
    _In_ ULONG Rat)
{
    switch (Rat)
    {
        case PHONEOM_RAT_LTE: return L"4G";
        case PHONEOM_RAT_UMTS: return L"3G";
        case PHONEOM_RAT_TDSCDMA: return L"3G";
        case PHONEOM_RAT_EVDO: return L"3G";
        case PHONEOM_RAT_GSM: return L"2G";
        case PHONEOM_RAT_CDMA: return L"1X";
        default: return L"";
    }
}

class CCellMonitor
{
public:
    CCellMonitor() = default;

    ~CCellMonitor()
    {
        Stop();
    }

    CCellMonitor(const CCellMonitor &) = delete;
    CCellMonitor &operator=(const CCellMonitor &) = delete;

    /* Details adds the device and data settings, which only a settings page needs */
    BOOL
    Start(
        _In_ ULONG PeriodMs,
        _In_ BOOL Details,
        _In_opt_ PCELL_NOTIFY Notify,
        _In_opt_ PVOID Context)
    {
        if (m_Thread)
            return TRUE;

        InitializeSRWLock(&m_Lock);
        m_Period = PeriodMs;
        m_Details = Details;
        m_Notify = Notify;
        m_Context = Context;
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
    Snapshot(
        _Out_ PCELL_STATUS Status)
    {
        AcquireSRWLockShared(&m_Lock);
        *Status = m_Status;
        ReleaseSRWLockShared(&m_Lock);
    }

    VOID
    Refresh(VOID)
    {
        if (m_KickEvent)
            SetEvent(m_KickEvent);
    }

    VOID
    SetRadioPower(
        _In_ BOOL On)
    {
        AcquireSRWLockExclusive(&m_Lock);
        m_PendingRadio = On ? 1 : 0;
        ReleaseSRWLockExclusive(&m_Lock);
        Refresh();
    }

    /* Goes up by one for every call that starts ringing, the shell brings Phone up when it moves */
    LONG
    IncomingSequence(VOID) const
    {
        return m_IncomingSequence;
    }

    /* Moves the messages that arrived since the last call into Messages, oldest first */
    ULONG
    TakeMessages(
        _Out_writes_to_(Max, return) PCELL_MESSAGE Messages,
        _In_ ULONG Max)
    {
        ULONG Count;

        AcquireSRWLockExclusive(&m_Lock);
        Count = min(m_MessageCount, Max);
        CopyMemory(Messages, m_Messages, Count * sizeof(*Messages));
        m_MessageCount = 0;
        ReleaseSRWLockExclusive(&m_Lock);
        return Count;
    }

    /* Saves the settings, then brings the data connection up or down to match Enabled */
    VOID
    SetDataConfig(
        _In_ const PHONEOM_DATA_CONFIG *Config)
    {
        AcquireSRWLockExclusive(&m_Lock);
        m_PendingConfig = *Config;
        m_PendingData = TRUE;
        ReleaseSRWLockExclusive(&m_Lock);
        Refresh();
    }

private:
    typedef HRESULT (WINAPI *PFN_OPEN)(PPHONEOM_EVENT_CALLBACK, PVOID, HPHONEOM *);
    typedef VOID (WINAPI *PFN_CLOSE)(HPHONEOM);
    typedef HRESULT (WINAPI *PFN_GET)(HPHONEOM, PVOID);
    typedef HRESULT (WINAPI *PFN_SET_RADIO)(HPHONEOM, BOOL);
    typedef HRESULT (WINAPI *PFN_SET_DATA)(HPHONEOM, const PHONEOM_DATA_CONFIG *);
    typedef HRESULT (WINAPI *PFN_CONNECT)(HPHONEOM, BOOL, PPHONEOM_DATA_STATUS);

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
        CCellMonitor *This = static_cast<CCellMonitor *>(Context);

        UNREFERENCED_PARAMETER(Phone);

        if (Event == PhoneEventCall && Data && DataSize >= sizeof(PHONEOM_CALL))
        {
            const PHONEOM_CALL *Call = static_cast<const PHONEOM_CALL *>(Data);

            if (Call->State == PhoneCallIncoming &&
                InterlockedExchange(&This->m_RingingCall, static_cast<LONG>(Call->CallId)) != static_cast<LONG>(Call->CallId))
            {
                InterlockedIncrement(&This->m_IncomingSequence);
            }
            else if (Call->State != PhoneCallIncoming)
            {
                InterlockedCompareExchange(&This->m_RingingCall, 0, static_cast<LONG>(Call->CallId));
            }
        }

        if (Event == PhoneEventMessage && Data && DataSize >= sizeof(PHONEOM_MESSAGE))
            This->QueueMessage(static_cast<const PHONEOM_MESSAGE *>(Data));

        /* The session is closed on the monitor thread, never from its own callback */
        if (Event == PhoneEventDisconnected)
            InterlockedExchange(&This->m_Lost, TRUE);
        if (Event == PhoneEventRadio || Event == PhoneEventDisconnected)
            This->Refresh();
    }

    /* A picture message is announced again once it is downloaded, the id keeps it to one note */
    VOID
    QueueMessage(
        _In_ const PHONEOM_MESSAGE *Message)
    {
        PCELL_MESSAGE Entry;
        SIZE_T Length;

        if (!(Message->Flags & PHONEOM_MESSAGE_INCOMING))
            return;

        AcquireSRWLockExclusive(&m_Lock);
        for (ULONG Index = 0; Index < ARRAYSIZE(m_RecentIds); Index++)
        {
            if (m_RecentIds[Index] == Message->MessageId)
            {
                ReleaseSRWLockExclusive(&m_Lock);
                return;
            }
        }
        m_RecentIds[m_RecentNext++ % ARRAYSIZE(m_RecentIds)] = Message->MessageId;

        if (m_MessageCount == CELL_MESSAGE_QUEUE)
        {
            MoveMemory(&m_Messages[0], &m_Messages[1], (CELL_MESSAGE_QUEUE - 1) * sizeof(m_Messages[0]));
            m_MessageCount--;
        }

        Entry = &m_Messages[m_MessageCount++];
        ZeroMemory(Entry, sizeof(*Entry));
        Entry->MessageId = Message->MessageId;
        Entry->Picture = (Message->Flags & PHONEOM_MESSAGE_MMS) != 0;
        CopyMemory(Entry->Address, Message->Address, sizeof(Entry->Address) - sizeof(WCHAR));

        /* First line only, trimmed with an ellipsis */
        Length = 0;
        while (Length < PHONEOM_MAX_TEXT && Message->Text[Length] && Message->Text[Length] != L'\n')
            Length++;
        if (Length >= CELL_MESSAGE_PREVIEW - 1)
        {
            CopyMemory(Entry->Preview, Message->Text, (CELL_MESSAGE_PREVIEW - 2) * sizeof(WCHAR));
            Entry->Preview[CELL_MESSAGE_PREVIEW - 2] = L'\x2026';
        }
        else
        {
            CopyMemory(Entry->Preview, Message->Text, Length * sizeof(WCHAR));
        }
        ReleaseSRWLockExclusive(&m_Lock);
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
        m_GetRadio = reinterpret_cast<PFN_GET>(GetProcAddress(m_Module, "PhoneOmGetRadio"));
        m_GetSubscriber = reinterpret_cast<PFN_GET>(GetProcAddress(m_Module, "PhoneOmGetSubscriber"));
        m_GetNetwork = reinterpret_cast<PFN_GET>(GetProcAddress(m_Module, "PhoneOmGetNetwork"));
        m_GetDevice = reinterpret_cast<PFN_GET>(GetProcAddress(m_Module, "PhoneOmGetDeviceInfo"));
        m_GetData = reinterpret_cast<PFN_GET>(GetProcAddress(m_Module, "PhoneOmGetDataConfig"));
        m_SetRadio = reinterpret_cast<PFN_SET_RADIO>(GetProcAddress(m_Module, "PhoneOmSetRadioPower"));
        m_SetData = reinterpret_cast<PFN_SET_DATA>(GetProcAddress(m_Module, "PhoneOmSetDataConfig"));
        m_Connect = reinterpret_cast<PFN_CONNECT>(GetProcAddress(m_Module, "PhoneOmConnectData"));

        if (!m_Open || !m_Close || !m_GetRadio || !m_GetSubscriber || !m_GetNetwork || !m_GetDevice ||
            !m_GetData || !m_SetRadio || !m_SetData || !m_Connect)
        {
            FreeLibrary(m_Module);
            m_Module = NULL;
            SetLastError(ERROR_PROC_NOT_FOUND);
            return FALSE;
        }
        return TRUE;
    }

    VOID
    Poll(VOID)
    {
        CELL_STATUS Status = {};
        PHONEOM_DATA_CONFIG Config = {};
        LONG Radio;
        BOOL Data;
        HRESULT hr;

        AcquireSRWLockShared(&m_Lock);
        Status.Generation = m_Status.Generation + 1;
        Status.DataStatus = m_Status.DataStatus;
        Status.HaveDataStatus = m_Status.HaveDataStatus;
        Status.ActionResult = m_Status.ActionResult;
        ReleaseSRWLockShared(&m_Lock);

        if (InterlockedExchange(&m_Lost, FALSE) && m_Phone)
        {
            m_Close(m_Phone);
            m_Phone = NULL;
        }

        if (!Load())
        {
            Status.LastError = HRESULT_FROM_WIN32(GetLastError());
        }
        else if (!m_Phone && FAILED(hr = m_Open(OnPhoneEvent, this, &m_Phone)))
        {
            m_Phone = NULL;
            Status.LastError = hr;
        }

        if (m_Phone)
        {
            Status.ServiceUp = TRUE;

            AcquireSRWLockExclusive(&m_Lock);
            Radio = m_PendingRadio;
            Data = m_PendingData;
            Config = m_PendingConfig;
            m_PendingRadio = -1;
            m_PendingData = FALSE;
            ReleaseSRWLockExclusive(&m_Lock);

            if (Radio >= 0)
                Status.ActionResult = m_SetRadio(m_Phone, Radio != 0);
            if (Data)
            {
                Status.ActionResult = m_SetData(m_Phone, &Config);
                if (SUCCEEDED(Status.ActionResult))
                {
                    Status.ActionResult = m_Connect(m_Phone, Config.Enabled != 0, &Status.DataStatus);
                    Status.HaveDataStatus = SUCCEEDED(Status.ActionResult);
                }
            }

            Status.HaveRadio = SUCCEEDED(Status.LastError = m_GetRadio(m_Phone, &Status.Radio));
            if (Status.HaveRadio && Status.Radio.Present)
            {
                Status.HaveSim = SUCCEEDED(m_GetSubscriber(m_Phone, &Status.Sim));
                Status.HaveNetwork = SUCCEEDED(m_GetNetwork(m_Phone, &Status.Network));
                if (m_Details)
                    Status.HaveDevice = SUCCEEDED(m_GetDevice(m_Phone, &Status.Device));
            }
            if (m_Details)
                Status.HaveData = SUCCEEDED(m_GetData(m_Phone, &Status.Data));
        }

        AcquireSRWLockExclusive(&m_Lock);
        m_Status = Status;
        ReleaseSRWLockExclusive(&m_Lock);

        if (m_Notify)
            m_Notify(m_Context);
    }

    static
    DWORD
    WINAPI
    ThreadProc(
        _In_ LPVOID Parameter)
    {
        CCellMonitor *This = static_cast<CCellMonitor *>(Parameter);
        HANDLE Events[2] = { This->m_StopEvent, This->m_KickEvent };

        do
        {
            This->Poll();
        } while (WaitForMultipleObjects(ARRAYSIZE(Events), Events, FALSE, This->m_Period) != WAIT_OBJECT_0);

        if (This->m_Phone)
            This->m_Close(This->m_Phone);
        This->m_Phone = NULL;
        if (This->m_Module)
            FreeLibrary(This->m_Module);
        This->m_Module = NULL;
        return 0;
    }

    SRWLOCK m_Lock = SRWLOCK_INIT;
    HANDLE m_Thread = NULL;
    HANDLE m_StopEvent = NULL;
    HANDLE m_KickEvent = NULL;
    ULONG m_Period = 5000;
    BOOL m_Details = FALSE;
    PCELL_NOTIFY m_Notify = NULL;
    PVOID m_Context = NULL;
    volatile LONG m_Lost = FALSE;
    volatile LONG m_IncomingSequence = 0;
    CELL_MESSAGE m_Messages[CELL_MESSAGE_QUEUE] = {};
    ULONG m_MessageCount = 0;
    DWORD m_RecentIds[16] = {};
    ULONG m_RecentNext = 0;
    volatile LONG m_RingingCall = 0;

    CELL_STATUS m_Status = {};
    LONG m_PendingRadio = -1;
    BOOL m_PendingData = FALSE;
    PHONEOM_DATA_CONFIG m_PendingConfig = {};

    HMODULE m_Module = NULL;
    HPHONEOM m_Phone = NULL;
    PFN_OPEN m_Open = NULL;
    PFN_CLOSE m_Close = NULL;
    PFN_GET m_GetRadio = NULL;
    PFN_GET m_GetSubscriber = NULL;
    PFN_GET m_GetNetwork = NULL;
    PFN_GET m_GetDevice = NULL;
    PFN_GET m_GetData = NULL;
    PFN_SET_RADIO m_SetRadio = NULL;
    PFN_SET_DATA m_SetData = NULL;
    PFN_CONNECT m_Connect = NULL;
};
