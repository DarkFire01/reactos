/*
 * PROJECT:     ReactOS RIL Adaptation Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hosts the OEM RIL library and serves broker clients
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "riladapt.h"

#define RIL_HOST_EVENT_BUFFER       (sizeof(MSRIL_HOST_EVENT) + RIL_RECORD_MAX_LENGTH)
#define RIL_QUEUE_INITIAL_SIZE      4096
#define RIL_COMMAND_ID_MAX          0x7FFFFFFF

/* Command payloads this large never reach the library */
#define RIL_PAYLOAD_LIMIT           0x3C00

/* Commands with this bit drive modem logging, which the host does not offer */
#define RIL_MODEM_LOG_COMMAND       0x400

/* RilCmdSendMsgAck input size and the answer to an acknowledgement already sent */
#define RIL_MSGACK_INPUT_SIZE       28
#define RIL_E_DUPLICATE_ACK         RIL_ERROR(0x26)

/* RIL_NOTIFY_MESSAGE payloads that carry a full RIL_MESSAGE_NOTIFICATION */
#define RIL_MESSAGE_PAYLOAD_SHORT   2668
#define RIL_MESSAGE_PAYLOAD_LONG    2672

/* RIL_MESSAGE.Params bits for a deliver message that has a user data header */
#define RIL_MSGPARAM_UDH_LENGTH     0x00800000
#define RIL_MSGPARAM_UDH_DATA       0x04000000

/* User data header elements carrying application ports, and the WAP push port */
#define RIL_UDH_PORTS_8BIT          4
#define RIL_UDH_PORTS_16BIT         5
#define RIL_WAP_PUSH_PORT           2948

static
BOOL
RilDeliver(
    _In_ PRIL_MODEM Modem,
    _In_ DWORD ClientId,
    _In_reads_bytes_(HeaderSize) const VOID *Header,
    _In_ DWORD HeaderSize,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize)
{
    PMSRIL_HOST_DELIVERY Delivery;
    OVERLAPPED Overlapped;
    DWORD Size, Returned;
    BOOL Result;

    if (DataSize > RIL_RECORD_MAX_LENGTH - HeaderSize)
    {
        RilLog("modem %lu: record of %lu bytes for client %lu is too large\n",
               Modem->Index, HeaderSize + DataSize, ClientId);
        return FALSE;
    }

    Size = sizeof(*Delivery) + HeaderSize + DataSize;
    Delivery = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Delivery)
        return FALSE;

    Delivery->ClientId = ClientId;
    Delivery->DataLength = HeaderSize + DataSize;
    CopyMemory(Delivery + 1, Header, HeaderSize);
    if (DataSize)
        CopyMemory((PBYTE)(Delivery + 1) + HeaderSize, Data, DataSize);

    EnterCriticalSection(&Modem->DeliverLock);
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = Modem->DeliverEvent;
    Result = DeviceIoControl(Modem->Broker, IOCTL_MSRIL_HOST_DELIVER, Delivery, Size,
                             NULL, 0, NULL, &Overlapped);
    if (!Result && GetLastError() == ERROR_IO_PENDING)
        Result = GetOverlappedResult(Modem->Broker, &Overlapped, &Returned, TRUE);
    if (!Result)
        RilLog("modem %lu: delivery to client %lu failed %lu\n", Modem->Index, ClientId, GetLastError());
    LeaveCriticalSection(&Modem->DeliverLock);

    HeapFree(GetProcessHeap(), 0, Delivery);
    return Result;
}

static
VOID
RilRespond(
    _In_ PRIL_MODEM Modem,
    _In_ DWORD ClientId,
    _In_ DWORD ResultType,
    _In_ DWORD Command,
    _In_ ULONGLONG Context,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize)
{
    RIL_RESPONSE_RECORD Response;

    Response.Type = RIL_RECORD_RESPONSE;
    Response.ResultType = ResultType;
    Response.Command = Command;
    Response.Context = Context;
    RilDeliver(Modem, ClientId, &Response, sizeof(Response), Data, DataSize);
}

static
VOID
RilRespondError(
    _In_ PRIL_MODEM Modem,
    _In_ DWORD ClientId,
    _In_ DWORD Command,
    _In_ ULONGLONG Context,
    _In_ HRESULT Error)
{
    RIL_ERROR_RESULT Result;

    Result.Size = sizeof(Result);
    Result.Params = 0;
    Result.Error = Error;
    Result.Reserved = 0;

    RilLog("modem %lu: client %lu command 0x%lx context 0x%I64x result error 0x%08lx\n",
           Modem->Index, ClientId, Command, Context, Error);
    RilRespond(Modem, ClientId, RIL_RESULT_ERROR, Command, Context, &Result, sizeof(Result));
}

static
VOID
RilBroadcast(
    _In_ PRIL_MODEM Modem,
    _In_ DWORD Code,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize)
{
    RIL_NOTIFICATION_RECORD Notification;

    Notification.Type = RIL_RECORD_NOTIFICATION;
    Notification.Code = Code;

    RilLog("modem %lu: notification 0x%08lx (%lu bytes) broadcast\n", Modem->Index, Code, DataSize);
    RilDeliver(Modem, MSRIL_CLIENT_BROADCAST, &Notification, sizeof(Notification), Data, DataSize);
}

static
VOID
CALLBACK
RilWatchdogExpired(
    _Inout_ PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID Context,
    _Inout_ PTP_TIMER Timer)
{
    PRIL_MODEM Modem = Context;
    PCSTR Kind = (Timer == Modem->CommandWatchdog) ? "command" : "notification fetch";

    UNREFERENCED_PARAMETER(Instance);

    if (IsDebuggerPresent())
    {
        RilLog("modem %lu: %s watchdog expired with a debugger attached, continuing\n", Modem->Index, Kind);
        return;
    }

    RilFatal("modem %lu: %s watchdog expired after %lu s\n", Modem->Index, Kind, Modem->Config.WatchdogTimeout);
}

/* Calls into the library with the watchdog running */
static
BOOL
RilCallLibrary(
    _In_ PRIL_MODEM Modem,
    _In_ PTP_TIMER Watchdog,
    _In_ DWORD Code,
    _In_reads_bytes_opt_(InSize) PBYTE In,
    _In_ DWORD InSize,
    _Out_writes_bytes_(OutSize) PBYTE Out,
    _In_ DWORD OutSize,
    _Out_ PDWORD Actual)
{
    ULARGE_INTEGER Due;
    FILETIME DueTime;
    BOOL Result;

    if (Modem->Config.WatchdogTimeout)
    {
        Due.QuadPart = (ULONGLONG)(-(LONGLONG)Modem->Config.WatchdogTimeout * 10000000);
        DueTime.dwLowDateTime = Due.LowPart;
        DueTime.dwHighDateTime = Due.HighPart;
        SetThreadpoolTimer(Watchdog, &DueTime, 0, 0);
    }

    *Actual = 0;
    Result = Modem->RilIoControl(Modem->OemClient, Code, In, InSize, Out, OutSize, Actual);

    SetThreadpoolTimer(Watchdog, NULL, 0, 0);
    return Result;
}

static
PRIL_HOST_COMMAND
RilTakeCommand(
    _In_ PRIL_MODEM Modem,
    _In_ DWORD CommandId)
{
    PRIL_HOST_COMMAND Found = NULL;
    PRIL_HOST_COMMAND Command;
    PLIST_ENTRY Entry;

    EnterCriticalSection(&Modem->CommandLock);
    for (Entry = Modem->Commands.Flink; Entry != &Modem->Commands; Entry = Entry->Flink)
    {
        Command = CONTAINING_RECORD(Entry, RIL_HOST_COMMAND, Link);
        if (Command->CommandId == CommandId)
        {
            RemoveEntryList(&Command->Link);
            Found = Command;
            break;
        }
    }
    LeaveCriticalSection(&Modem->CommandLock);

    return Found;
}

/* Caller holds CommandLock */
static
BOOL
RilCommandIdInUse(
    _In_ PRIL_MODEM Modem,
    _In_ DWORD CommandId)
{
    PLIST_ENTRY Entry;

    for (Entry = Modem->Commands.Flink; Entry != &Modem->Commands; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, RIL_HOST_COMMAND, Link)->CommandId == CommandId)
            return TRUE;
    }

    return FALSE;
}

/* Returns TRUE when the acknowledgement was sent recently, otherwise remembers it */
static
BOOL
RilAckAlreadySent(
    _In_ PRIL_MODEM Modem,
    _In_ ULONGLONG Key)
{
    BOOL Seen = FALSE;
    DWORD i;

    EnterCriticalSection(&Modem->CommandLock);
    for (i = 0; i < RILADAPT_ACK_HISTORY; i++)
    {
        if (Modem->AckValid[i] && Modem->AckKeys[i] == Key)
        {
            Seen = TRUE;
            break;
        }
    }

    if (!Seen)
    {
        Modem->AckKeys[Modem->AckNext] = Key;
        Modem->AckValid[Modem->AckNext] = TRUE;
        Modem->AckNext = (Modem->AckNext + 1) % RILADAPT_ACK_HISTORY;
    }
    LeaveCriticalSection(&Modem->CommandLock);

    return Seen;
}

/* A new message with this key may be acknowledged again */
static
VOID
RilForgetAck(
    _In_ PRIL_MODEM Modem,
    _In_ ULONGLONG Key)
{
    DWORD i;

    EnterCriticalSection(&Modem->CommandLock);
    for (i = 0; i < RILADAPT_ACK_HISTORY; i++)
    {
        if (Modem->AckValid[i] && Modem->AckKeys[i] == Key)
            Modem->AckValid[i] = FALSE;
    }
    LeaveCriticalSection(&Modem->CommandLock);
}

static
ULONGLONG
RilMessageKey(
    _In_ DWORD High,
    _In_ DWORD Low)
{
    return ((ULONGLONG)High << 32) | Low;
}

/* Looks for one user data header element and copies it out, returns its length or -1 */
static
LONG
RilFindUdhElement(
    _In_reads_bytes_(Length) const BYTE *Udh,
    _In_ DWORD Length,
    _In_ BYTE Id,
    _Out_writes_bytes_(OutSize) PBYTE Out,
    _In_ DWORD OutSize)
{
    DWORD Offset = 0, ElementLength;

    while (Offset + 2 <= Length)
    {
        ElementLength = Udh[Offset + 1];
        if (Offset + 2 + ElementLength > Length)
            break;

        if (Udh[Offset] == Id)
        {
            if (ElementLength > OutSize)
                return -1;
            CopyMemory(Out, &Udh[Offset + 2], ElementLength);
            return (LONG)ElementLength;
        }

        Offset += 2 + ElementLength;
    }

    return -1;
}

/* WAP push messages wait for the messaging stack, see RilHandleNotification */
static
BOOL
RilIsHeldMessage(
    _In_reads_bytes_(DataSize) const BYTE *Data,
    _In_ DWORD DataSize)
{
    const RIL_MESSAGE_NOTIFICATION *Notification = (const RIL_MESSAGE_NOTIFICATION *)Data;
    const RIL_MESSAGE *Message = &Notification->Message;
    BYTE Ports[4];
    DWORD Port;

    if (DataSize != RIL_MESSAGE_PAYLOAD_SHORT && DataSize != RIL_MESSAGE_PAYLOAD_LONG)
        return FALSE;

    if (!(Message->Params & RIL_MSGPARAM_TYPE) || Message->Type != RIL_MSGTYPE_GW_DELIVER)
        return FALSE;

    if (!(Message->Params & RIL_MSGPARAM_DCS) || Message->Dcs.Alphabet != RIL_ALPHABET_8BIT)
        return FALSE;

    if ((Message->Params & (RIL_MSGPARAM_UDH_LENGTH | RIL_MSGPARAM_UDH_DATA)) !=
        (RIL_MSGPARAM_UDH_LENGTH | RIL_MSGPARAM_UDH_DATA))
    {
        return FALSE;
    }

    if (RilFindUdhElement(Message->u.Deliver.Header,
                          min(Message->u.Deliver.HeaderLength, RIL_MESSAGE_HEADER_MAX),
                          RIL_UDH_PORTS_8BIT, Ports, 2) == 2)
    {
        Port = Ports[0];
    }
    else if (RilFindUdhElement(Message->u.Deliver.Header,
                               min(Message->u.Deliver.HeaderLength, RIL_MESSAGE_HEADER_MAX),
                               RIL_UDH_PORTS_16BIT, Ports, 4) == 4)
    {
        Port = (Ports[0] << 8) | Ports[1];
    }
    else
    {
        return FALSE;
    }

    return Port == RIL_WAP_PUSH_PORT;
}

static
VOID
RilReleaseHeld(
    _In_ PRIL_MODEM Modem,
    _In_ BOOL Deliver)
{
    PRIL_HELD_NOTIFICATION Held;

    while (!IsListEmpty(&Modem->Held))
    {
        Held = CONTAINING_RECORD(RemoveHeadList(&Modem->Held), RIL_HELD_NOTIFICATION, Link);
        if (Deliver)
            RilBroadcast(Modem, Held->Code, Held->Data, Held->DataSize);
        HeapFree(GetProcessHeap(), 0, Held);
    }
}

static
VOID
RilHandleResult(
    _In_ PRIL_MODEM Modem,
    _In_ DWORD ResultType,
    _In_ DWORD CommandId,
    _In_reads_bytes_opt_(DataSize) const BYTE *Data,
    _In_ DWORD DataSize)
{
    PRIL_HOST_COMMAND Command;
    HRESULT Error = S_OK;

    Command = RilTakeCommand(Modem, CommandId);
    if (!Command)
        RilFatal("modem %lu: result type %lu for unknown command id %lu\n", Modem->Index, ResultType, CommandId);

    if (ResultType == RIL_RESULT_ERROR && DataSize >= FIELD_OFFSET(RIL_ERROR_RESULT, Reserved))
        Error = ((const RIL_ERROR_RESULT *)Data)->Error;

    RilLog("modem %lu: client %lu command 0x%lx context 0x%I64x async result type %lu hr 0x%08lx (%lu bytes)\n",
           Modem->Index, Command->ClientId, Command->Command, Command->Context, ResultType, Error, DataSize);

    RilRespond(Modem, Command->ClientId, ResultType, Command->Command, Command->Context, Data, DataSize);
    HeapFree(GetProcessHeap(), 0, Command);
}

static
VOID
RilHandleNotification(
    _In_ PRIL_MODEM Modem,
    _In_ DWORD Code,
    _In_reads_bytes_opt_(DataSize) const BYTE *Data,
    _In_ DWORD DataSize)
{
    PRIL_HELD_NOTIFICATION Held;
    LARGE_INTEGER Due;
    const DWORD *Words = (const DWORD *)Data;

    /* There is no messaging ready signal, the hold timer alone releases held messages */
    if (Code == RIL_NOTIFY_MESSAGE && !Modem->HoldReleased && RilIsHeldMessage(Data, DataSize))
    {
        Held = HeapAlloc(GetProcessHeap(), 0, FIELD_OFFSET(RIL_HELD_NOTIFICATION, Data) + DataSize);
        if (!Held)
            RilFatal("modem %lu: no memory to hold a message\n", Modem->Index);

        Held->Code = Code;
        Held->DataSize = DataSize;
        CopyMemory(Held->Data, Data, DataSize);

        if (IsListEmpty(&Modem->Held))
        {
            Due.QuadPart = -(LONGLONG)Modem->Config.MessageHoldTimeout * 10000000;
            if (!SetWaitableTimer(Modem->HoldTimer, &Due, 0, NULL, NULL, FALSE))
                RilFatal("modem %lu: cannot arm the message hold timer (%lu)\n", Modem->Index, GetLastError());
        }
        InsertTailList(&Modem->Held, &Held->Link);

        RilLog("modem %lu: WAP push message held for up to %lu s\n", Modem->Index, Modem->Config.MessageHoldTimeout);
        return;
    }

    if (Code == RIL_NOTIFY_MESSAGE && DataSize >= FIELD_OFFSET(RIL_MESSAGE_NOTIFICATION, OverIms))
        RilForgetAck(Modem, RilMessageKey(Words[2], Words[3]));

    RilBroadcast(Modem, Code, Data, DataSize);
}

/* One fetch per wake of the library notification event */
static
VOID
RilFetchNotification(
    _In_ PRIL_MODEM Modem)
{
    PRIL_HOST_QUEUED_V1 Header = (PRIL_HOST_QUEUED_V1)Modem->QueueBuffer;
    DWORD Actual, DataSize;
    PBYTE Larger;

    ZeroMemory(Header, sizeof(*Header));
    Header->Size = Modem->QueueBufferSize;

    /* The reference host treats FALSE as fatal, here it means the queue ran dry */
    if (!RilCallLibrary(Modem, Modem->FetchWatchdog, RilCmdGetNextNotification, NULL, 0,
                        Modem->QueueBuffer, Modem->QueueBufferSize, &Actual))
    {
        return;
    }

    if (Header->Required > Modem->QueueBufferSize)
    {
        /* The entry stays queued until a large enough buffer is offered */
        Larger = HeapAlloc(GetProcessHeap(), 0, Header->Required);
        if (!Larger)
            RilFatal("modem %lu: no memory for a %lu byte notification\n", Modem->Index, Header->Required);

        RilLog("modem %lu: notification buffer grown to %lu bytes\n", Modem->Index, Header->Required);
        HeapFree(GetProcessHeap(), 0, Modem->QueueBuffer);
        Modem->QueueBuffer = Larger;
        Modem->QueueBufferSize = Header->Required;
        return;
    }

    if (Actual < sizeof(*Header))
    {
        RilLog("modem %lu: short notification entry (%lu bytes)\n", Modem->Index, Actual);
        return;
    }

    DataSize = min(Header->DataSize, Modem->QueueBufferSize - sizeof(*Header));

    if (Header->Code & RIL_NOTIFY(0))
    {
        RilHandleNotification(Modem, Header->Code, DataSize ? (PBYTE)(Header + 1) : NULL, DataSize);
    }
    else
    {
        RilHandleResult(Modem, Header->Code, Header->CommandId,
                        DataSize ? (PBYTE)(Header + 1) : NULL, DataSize);
    }
}

static
DWORD
WINAPI
RilNotificationThread(
    _In_ LPVOID Parameter)
{
    PRIL_MODEM Modem = Parameter;
    HANDLE Waits[4];
    DWORD Result;

    Waits[0] = Modem->StopEvent;
    Waits[1] = Modem->CancelEvent;
    Waits[2] = Modem->NotifyEvent;
    Waits[3] = Modem->HoldTimer;

    for (;;)
    {
        Result = WaitForMultipleObjects(_countof(Waits), Waits, FALSE, INFINITE);
        switch (Result)
        {
            case WAIT_OBJECT_0:
                RilReleaseHeld(Modem, FALSE);
                return 0;

            case WAIT_OBJECT_0 + 1:
                RilReleaseHeld(Modem, FALSE);
                RilFatal("modem %lu: the RIL library canceled the modem\n", Modem->Index);

            case WAIT_OBJECT_0 + 2:
                RilFetchNotification(Modem);
                break;

            case WAIT_OBJECT_0 + 3:
                RilLog("modem %lu: message hold timeout, releasing held messages\n", Modem->Index);
                Modem->HoldReleased = TRUE;
                RilReleaseHeld(Modem, TRUE);
                break;

            default:
                RilFatal("modem %lu: notification wait failed %lu\n", Modem->Index, GetLastError());
        }
    }
}

static
VOID
RilClientCommand(
    _In_ PRIL_MODEM Modem,
    _In_ PMSRIL_HOST_EVENT Event,
    _In_reads_bytes_(Event->DataLength) PBYTE Data)
{
    PRIL_COMMAND_RECORD Header = (PRIL_COMMAND_RECORD)Data;
    PRIL_HOST_COMMAND Command;
    DWORD InSize, Out, Actual, CommandId;
    PDWORD In;

    if (Event->DataLength < sizeof(*Header) || Header->Type != RIL_RECORD_COMMAND)
    {
        RilLog("modem %lu: client %lu sent a malformed record (%lu bytes)\n",
               Modem->Index, Event->ClientId, Event->DataLength);
        return;
    }

    InSize = Event->DataLength - sizeof(*Header);
    In = InSize ? (PDWORD)(Header + 1) : NULL;

    RilLog("modem %lu: client %lu command 0x%lx context 0x%I64x (%lu bytes)\n",
           Modem->Index, Event->ClientId, Header->Command, Header->Context, InSize);

    if (InSize >= RIL_PAYLOAD_LIMIT)
    {
        RilRespondError(Modem, Event->ClientId, Header->Command, Header->Context, E_INVALIDARG);
        return;
    }

    /* The notification channel belongs to the host, a client never drives it */
    if (Header->Command == RilCmdInitNotifications || Header->Command == RilCmdGetNextNotification)
    {
        RilRespondError(Modem, Event->ClientId, Header->Command, Header->Context, E_INVALIDARG);
        return;
    }

    if (Header->Command & RIL_MODEM_LOG_COMMAND)
    {
        RilLog("modem %lu: modem log command 0x%lx not forwarded\n", Modem->Index, Header->Command);
        RilRespondError(Modem, Event->ClientId, Header->Command, Header->Context, RIL_E_OPNOTSUPPORTED);
        return;
    }

    if (Header->Command == RilCmdSendMsgAck && InSize == RIL_MSGACK_INPUT_SIZE &&
        RilAckAlreadySent(Modem, RilMessageKey(In[2], In[3])))
    {
        RilLog("modem %lu: duplicate message acknowledgement 0x%lx:0x%lx dropped\n", Modem->Index, In[2], In[3]);
        RilRespondError(Modem, Event->ClientId, Header->Command, Header->Context, RIL_E_DUPLICATE_ACK);
        return;
    }

    Command = HeapAlloc(GetProcessHeap(), 0, sizeof(*Command));
    if (!Command)
        RilFatal("modem %lu: no memory for a command\n", Modem->Index);

    Command->ClientId = Event->ClientId;
    Command->Command = Header->Command;
    Command->Context = Header->Context;

    /* Registered before the call, the library can complete on its own thread first */
    EnterCriticalSection(&Modem->CommandLock);
    do
    {
        if (++Modem->NextCommandId > RIL_COMMAND_ID_MAX)
            Modem->NextCommandId = 1;
    } while (RilCommandIdInUse(Modem, Modem->NextCommandId));
    Command->CommandId = Modem->NextCommandId;
    InsertTailList(&Modem->Commands, &Command->Link);
    LeaveCriticalSection(&Modem->CommandLock);

    CommandId = Command->CommandId;
    Out = CommandId;
    if (!RilCallLibrary(Modem, Modem->CommandWatchdog, Header->Command, (PBYTE)In, InSize,
                        (PBYTE)&Out, sizeof(Out), &Actual))
    {
        RilFatal("modem %lu: RIL_IOControl(0x%lx) failed %lu\n", Modem->Index, Header->Command, GetLastError());
    }

    /* An untouched id means the answer comes through the notification queue */
    if (Out == CommandId)
    {
        RilLog("modem %lu: client %lu command 0x%lx pending as id %lu\n",
               Modem->Index, Event->ClientId, Header->Command, CommandId);
        return;
    }

    Command = RilTakeCommand(Modem, CommandId);
    if (!Command)
        RilFatal("modem %lu: command id %lu vanished\n", Modem->Index, CommandId);

    if ((HRESULT)Out >= 0)
        RilFatal("modem %lu: RIL_IOControl(0x%lx) returned 0x%lx\n", Modem->Index, Header->Command, Out);

    RilRespondError(Modem, Command->ClientId, Command->Command, Command->Context, (HRESULT)Out);
    HeapFree(GetProcessHeap(), 0, Command);
}

static
DWORD
WINAPI
RilBrokerThread(
    _In_ LPVOID Parameter)
{
    PRIL_MODEM Modem = Parameter;
    PMSRIL_HOST_EVENT Event;
    OVERLAPPED Overlapped;
    HANDLE IoEvent;
    DWORD Returned;
    PBYTE Buffer;
    BOOL Result;

    Buffer = HeapAlloc(GetProcessHeap(), 0, RIL_HOST_EVENT_BUFFER);
    IoEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Buffer || !IoEvent)
        RilFatal("modem %lu: no resources for the broker thread\n", Modem->Index);

    Event = (PMSRIL_HOST_EVENT)Buffer;
    for (;;)
    {
        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Overlapped.hEvent = IoEvent;
        Returned = 0;

        Result = DeviceIoControl(Modem->Broker, IOCTL_MSRIL_HOST_GET_EVENT, NULL, 0,
                                 Buffer, RIL_HOST_EVENT_BUFFER, NULL, &Overlapped);
        if (Result || GetLastError() == ERROR_IO_PENDING)
            Result = GetOverlappedResult(Modem->Broker, &Overlapped, &Returned, TRUE);

        if (!Result)
        {
            if (Modem->Stopping)
                break;
            RilFatal("modem %lu: broker fetch failed %lu\n", Modem->Index, GetLastError());
        }

        if (Returned < sizeof(*Event) || Event->DataLength > Returned - sizeof(*Event))
            continue;

        switch (Event->EventType)
        {
            case MsRilClientOpened:
                RilLog("modem %lu: client %lu opened (pid %lu)\n", Modem->Index, Event->ClientId, Event->ProcessId);
                break;

            case MsRilClientClosed:
                RilLog("modem %lu: client %lu closed (pid %lu)\n", Modem->Index, Event->ClientId, Event->ProcessId);
                break;

            case MsRilClientRecord:
                RilClientCommand(Modem, Event, (PBYTE)(Event + 1));
                break;
        }
    }

    CloseHandle(IoEvent);
    HeapFree(GetProcessHeap(), 0, Buffer);
    return 0;
}

static
BOOL
RilLibraryInSystem32(
    _In_ PCWSTR Path)
{
    WCHAR System[MAX_PATH];
    UINT Length;

    Length = GetSystemDirectoryW(System, _countof(System));
    if (!Length || Length >= _countof(System))
        return FALSE;

    return !_wcsnicmp(Path, System, Length) && Path[Length] == L'\\' && !wcschr(&Path[Length + 1], L'\\');
}

static
VOID
RilLoadLibrary(
    _Inout_ PRIL_MODEM Modem)
{
    DWORD Flags;

    Flags = RilLibraryInSystem32(Modem->Config.LibraryPath) ? LOAD_LIBRARY_SEARCH_SYSTEM32
                                                            : LOAD_WITH_ALTERED_SEARCH_PATH;
    Modem->Library = LoadLibraryExW(Modem->Config.LibraryPath, NULL, Flags);
    if (!Modem->Library)
        RilFatal("modem %lu: cannot load %S (%lu)\n", Modem->Index, Modem->Config.LibraryPath, GetLastError());

    Modem->RilVersion = (PFN_RIL_VERSION)GetProcAddress(Modem->Library, "RIL_Version");
    Modem->RilInit = (PFN_RIL_INIT)GetProcAddress(Modem->Library, "RIL_Init");
    Modem->RilOpen = (PFN_RIL_OPEN)GetProcAddress(Modem->Library, "RIL_Open");
    Modem->RilClose = (PFN_RIL_CLOSE)GetProcAddress(Modem->Library, "RIL_Close");
    Modem->RilIoControl = (PFN_RIL_IOCONTROL)GetProcAddress(Modem->Library, "RIL_IOControl");

    if (!Modem->RilInit || !Modem->RilOpen || !Modem->RilClose || !Modem->RilIoControl)
        RilFatal("modem %lu: %S is not a RIL library\n", Modem->Index, Modem->Config.LibraryPath);

    RilLog("modem %lu: loaded %S\n", Modem->Index, Modem->Config.LibraryPath);

    /* A library without version negotiation speaks the first host interface */
    Modem->Version = Modem->RilVersion ? Modem->RilVersion(RIL_HOST_VERSION, RIL_HOST_VERSION) : RIL_HOST_VERSION;
    RilLog("modem %lu: RIL_Version(%u, %u) -> %lu\n", Modem->Index, RIL_HOST_VERSION, RIL_HOST_VERSION, Modem->Version);
    if (Modem->Version != RIL_HOST_VERSION)
        RilFatal("modem %lu: library host interface version %lu is not supported\n", Modem->Index, Modem->Version);

    Modem->OemDriver = Modem->RilInit(Modem->Config.InitIndex);
    RilLog("modem %lu: RIL_Init(%lu) -> 0x%lx\n", Modem->Index, Modem->Config.InitIndex, Modem->OemDriver);
    if (!Modem->OemDriver)
        RilFatal("modem %lu: RIL_Init(%lu) failed\n", Modem->Index, Modem->Config.InitIndex);
}

/* Opens the single library client and sets up its notification channel */
static
VOID
RilOpenLibrary(
    _Inout_ PRIL_MODEM Modem)
{
    ULONGLONG CancelHandle, NotifyHandle = 0;
    DWORD Actual;

    Modem->OemClient = Modem->RilOpen(Modem->OemDriver, 0, 0);
    RilLog("modem %lu: RIL_Open(0x%lx, 0, 0) -> 0x%lx\n", Modem->Index, Modem->OemDriver, Modem->OemClient);
    if (!Modem->OemClient)
        RilFatal("modem %lu: RIL_Open failed\n", Modem->Index);

    Modem->CancelEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Modem->CancelEvent)
        RilFatal("modem %lu: cannot create the cancel event (%lu)\n", Modem->Index, GetLastError());

    /* The library owns the duplicate, the host keeps waiting on the original */
    if (!DuplicateHandle(GetCurrentProcess(), Modem->CancelEvent, GetCurrentProcess(),
                         &Modem->CancelHandle, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        RilFatal("modem %lu: cannot duplicate the cancel event (%lu)\n", Modem->Index, GetLastError());
    }

    CancelHandle = (ULONG_PTR)Modem->CancelHandle;
    if (!RilCallLibrary(Modem, Modem->CommandWatchdog, RilCmdInitNotifications,
                        (PBYTE)&CancelHandle, sizeof(CancelHandle),
                        (PBYTE)&NotifyHandle, sizeof(NotifyHandle), &Actual))
    {
        RilFatal("modem %lu: notification setup failed %lu\n", Modem->Index, GetLastError());
    }

    if (Actual != sizeof(NotifyHandle) || !NotifyHandle)
        RilFatal("modem %lu: notification setup returned no event\n", Modem->Index);

    Modem->NotifyEvent = (HANDLE)(ULONG_PTR)NotifyHandle;
    RilLog("modem %lu: notifications initialized\n", Modem->Index);
}

VOID
RilStartModem(
    _Inout_ PRIL_MODEM Modem)
{
    WCHAR HostPath[MAX_PATH];

    InitializeCriticalSection(&Modem->DeliverLock);
    InitializeCriticalSection(&Modem->CommandLock);
    InitializeListHead(&Modem->Commands);
    InitializeListHead(&Modem->Held);
    Modem->Broker = INVALID_HANDLE_VALUE;

    Modem->DeliverEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Modem->StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Modem->HoldTimer = CreateWaitableTimerW(NULL, FALSE, NULL);
    Modem->CommandWatchdog = CreateThreadpoolTimer(RilWatchdogExpired, Modem, NULL);
    Modem->FetchWatchdog = CreateThreadpoolTimer(RilWatchdogExpired, Modem, NULL);
    Modem->QueueBufferSize = RIL_QUEUE_INITIAL_SIZE;
    Modem->QueueBuffer = HeapAlloc(GetProcessHeap(), 0, Modem->QueueBufferSize);
    if (!Modem->DeliverEvent || !Modem->StopEvent || !Modem->HoldTimer ||
        !Modem->CommandWatchdog || !Modem->FetchWatchdog || !Modem->QueueBuffer)
    {
        RilFatal("modem %lu: no resources to start (%lu)\n", Modem->Index, GetLastError());
    }

    RilLoadLibrary(Modem);
    RilOpenLibrary(Modem);

    if (!RilEnsureBrokerDevice(&Modem->Config) ||
        !RilGetBrokerHostPath(&Modem->Config, HostPath, _countof(HostPath)))
    {
        RilFatal("modem %lu: broker %S is not available\n", Modem->Index, Modem->Config.BrokerName);
    }

    Modem->Broker = CreateFileW(HostPath,
                                GENERIC_READ | GENERIC_WRITE,
                                0,
                                NULL,
                                OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED,
                                NULL);
    if (Modem->Broker == INVALID_HANDLE_VALUE)
        RilFatal("modem %lu: cannot attach to %S (%lu)\n", Modem->Index, HostPath, GetLastError());

    RilLog("modem %lu: attached to %S\n", Modem->Index, HostPath);

    Modem->NotifyThread = CreateThread(NULL, 0, RilNotificationThread, Modem, 0, NULL);
    Modem->BrokerThread = CreateThread(NULL, 0, RilBrokerThread, Modem, 0, NULL);
    if (!Modem->NotifyThread || !Modem->BrokerThread)
        RilFatal("modem %lu: cannot start the modem threads (%lu)\n", Modem->Index, GetLastError());
}

VOID
RilStopModem(
    _Inout_ PRIL_MODEM Modem)
{
    HANDLE Broker;

    Modem->Stopping = TRUE;
    SetEvent(Modem->StopEvent);

    /* Closing the host handle fails the pending fetch and detaches every client */
    Broker = InterlockedExchangePointer(&Modem->Broker, INVALID_HANDLE_VALUE);
    if (Broker != INVALID_HANDLE_VALUE)
        CloseHandle(Broker);

    if (Modem->BrokerThread)
    {
        WaitForSingleObject(Modem->BrokerThread, INFINITE);
        CloseHandle(Modem->BrokerThread);
        Modem->BrokerThread = NULL;
    }

    if (Modem->NotifyThread)
    {
        WaitForSingleObject(Modem->NotifyThread, INFINITE);
        CloseHandle(Modem->NotifyThread);
        Modem->NotifyThread = NULL;
    }

    RilLog("modem %lu: stopped\n", Modem->Index);

    /* The library keeps worker threads alive, the process exits instead of unloading it */
}
