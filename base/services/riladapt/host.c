/*
 * PROJECT:     ReactOS RIL Adaptation Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hosts the OEM RIL library and serves broker clients
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "riladapt.h"

#define NDEBUG
#include <debug.h>

#define RIL_HOST_EVENT_BUFFER       (sizeof(MSRIL_HOST_EVENT) + RIL_RECORD_MAX_LENGTH)
#define RIL_QUEUE_INITIAL_SIZE      4096
#define RIL_COMMAND_ID_MAX          0x7FFFFFFF

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
        return FALSE;

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
    LeaveCriticalSection(&Modem->DeliverLock);

    HeapFree(GetProcessHeap(), 0, Delivery);
    return Result;
}

static
VOID
RilRespond(
    _In_ PRIL_HOST_CLIENT Client,
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
    RilDeliver(Client->Modem, Client->ClientId, &Response, sizeof(Response), Data, DataSize);
}

static
VOID
RilRespondError(
    _In_ PRIL_HOST_CLIENT Client,
    _In_ DWORD Command,
    _In_ ULONGLONG Context,
    _In_ HRESULT Error)
{
    RIL_ERROR_RESULT Result;

    Result.Size = sizeof(Result);
    Result.Params = 0;
    Result.Error = Error;
    Result.Reserved = 0;
    RilRespond(Client, RIL_RESULT_ERROR, Command, Context, &Result, sizeof(Result));
}

static
PRIL_HOST_COMMAND
RilTakeCommand(
    _In_ PRIL_HOST_CLIENT Client,
    _In_ DWORD CommandId)
{
    PRIL_HOST_COMMAND Found = NULL;
    PLIST_ENTRY Entry;

    EnterCriticalSection(&Client->CommandLock);
    for (Entry = Client->Commands.Flink; Entry != &Client->Commands; Entry = Entry->Flink)
    {
        PRIL_HOST_COMMAND Command = CONTAINING_RECORD(Entry, RIL_HOST_COMMAND, Link);
        if (Command->CommandId == CommandId)
        {
            RemoveEntryList(&Command->Link);
            Found = Command;
            break;
        }
    }
    LeaveCriticalSection(&Client->CommandLock);

    return Found;
}

/* Caller holds CommandLock */
static
BOOL
RilCommandIdInUse(
    _In_ PRIL_HOST_CLIENT Client,
    _In_ DWORD CommandId)
{
    PLIST_ENTRY Entry;

    for (Entry = Client->Commands.Flink; Entry != &Client->Commands; Entry = Entry->Flink)
    {
        if (CONTAINING_RECORD(Entry, RIL_HOST_COMMAND, Link)->CommandId == CommandId)
            return TRUE;
    }

    return FALSE;
}

static
BOOL
RilIsCodeEnabled(
    _In_ PRIL_HOST_CLIENT Client,
    _In_ DWORD Code)
{
    BOOL Enabled = FALSE;
    DWORD i;

    EnterCriticalSection(&Client->CommandLock);
    for (i = 0; i < Client->EnabledCount; i++)
    {
        if (Client->EnabledCodes[i] == Code)
        {
            Enabled = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&Client->CommandLock);

    return Enabled;
}

/* The OEM library broadcasts everything, the per client selection lives in the host */
static
HRESULT
RilEnableCodes(
    _In_ PRIL_HOST_CLIENT Client,
    _In_reads_bytes_(Size) const BYTE *Payload,
    _In_ DWORD Size)
{
    DWORD Count, i, j, Added;
    const DWORD *Codes;
    PDWORD Merged;

    if (Size < sizeof(DWORD))
        return E_INVALIDARG;

    Count = *(const DWORD *)Payload;
    if (Count > (Size - sizeof(DWORD)) / sizeof(DWORD))
        return E_INVALIDARG;

    Codes = (const DWORD *)(Payload + sizeof(DWORD));

    EnterCriticalSection(&Client->CommandLock);
    Merged = Client->EnabledCodes ?
             HeapReAlloc(GetProcessHeap(), 0, Client->EnabledCodes,
                         (Client->EnabledCount + Count + 1) * sizeof(DWORD)) :
             HeapAlloc(GetProcessHeap(), 0, (Count + 1) * sizeof(DWORD));
    if (!Merged)
    {
        LeaveCriticalSection(&Client->CommandLock);
        return E_OUTOFMEMORY;
    }

    Added = Client->EnabledCount;
    for (i = 0; i < Count; i++)
    {
        for (j = 0; j < Added && Merged[j] != Codes[i]; j++);
        if (j == Added)
            Merged[Added++] = Codes[i];
    }
    Client->EnabledCodes = Merged;
    Client->EnabledCount = Added;
    LeaveCriticalSection(&Client->CommandLock);

    return S_OK;
}

static
VOID
RilForwardQueued(
    _In_ PRIL_HOST_CLIENT Client,
    _In_ DWORD Code,
    _In_ DWORD CommandId,
    _In_reads_bytes_opt_(DataSize) const BYTE *Data,
    _In_ DWORD DataSize)
{
    RIL_NOTIFICATION_RECORD Notification;
    PRIL_HOST_COMMAND Command;

    if (CommandId)
    {
        Command = RilTakeCommand(Client, CommandId);
        if (!Command)
        {
            DPRINT1("Client %lu: result for unknown id %lu\n", Client->ClientId, CommandId);
            return;
        }

        RilRespond(Client, Code, Command->Command, Command->Context, Data, DataSize);
        HeapFree(GetProcessHeap(), 0, Command);
        return;
    }

    if (!RilIsCodeEnabled(Client, Code))
        return;

    Notification.Type = RIL_RECORD_NOTIFICATION;
    Notification.Code = Code;
    RilDeliver(Client->Modem, Client->ClientId, &Notification, sizeof(Notification), Data, DataSize);
}

/* Drains everything the OEM library queued for one client */
static
VOID
RilDrainQueue(
    _In_ PRIL_HOST_CLIENT Client)
{
    PRIL_MODEM Modem = Client->Modem;
    DWORD Actual, HeaderSize, Required, DataSize, Code, CommandId;
    PDWORD Out;
    PBYTE Larger;

    HeaderSize = (Modem->Version >= 2) ? sizeof(RIL_HOST_QUEUED_V2) : sizeof(RIL_HOST_QUEUED_V1);

    for (;;)
    {
        Out = (PDWORD)Client->QueueBuffer;
        ZeroMemory(Out, HeaderSize);
        Out[0] = Client->QueueBufferSize;

        Actual = 0;
        if (!Modem->RilIoControl(Client->OemClient, RilCmdGetNextNotification, NULL, 0,
                                 Client->QueueBuffer, Client->QueueBufferSize, &Actual))
        {
            break;
        }

        Required = ((PRIL_HOST_QUEUED_V1)Out)->Required;
        if (Required > Client->QueueBufferSize)
        {
            /* The entry stays queued until a large enough buffer is offered */
            Larger = HeapReAlloc(GetProcessHeap(), 0, Client->QueueBuffer, Required);
            if (!Larger)
                break;

            Client->QueueBuffer = Larger;
            Client->QueueBufferSize = Required;
            continue;
        }

        if (Modem->Version >= 2)
        {
            PRIL_HOST_QUEUED_V2 Entry = (PRIL_HOST_QUEUED_V2)Out;
            Code = Entry->Code;
            CommandId = Entry->CommandId;
            DataSize = Entry->DataSize;
        }
        else
        {
            PRIL_HOST_QUEUED_V1 Entry = (PRIL_HOST_QUEUED_V1)Out;
            Code = Entry->Code;
            CommandId = Entry->CommandId;
            DataSize = Entry->DataSize;
        }

        if (DataSize > Client->QueueBufferSize - HeaderSize)
            DataSize = Client->QueueBufferSize - HeaderSize;

        RilForwardQueued(Client, Code, CommandId,
                         DataSize ? Client->QueueBuffer + HeaderSize : NULL, DataSize);
    }
}

static
VOID
CALLBACK
RilNotifySignaled(
    _Inout_ PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID Context,
    _Inout_ PTP_WAIT Wait,
    _In_ TP_WAIT_RESULT WaitResult)
{
    PRIL_HOST_CLIENT Client = Context;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(WaitResult);

    RilDrainQueue(Client);
    SetThreadpoolWait(Wait, Client->NotifyEvent, NULL);
}

static
VOID
RilDestroyClient(
    _In_ PRIL_HOST_CLIENT Client)
{
    PRIL_MODEM Modem = Client->Modem;
    PRIL_HOST_COMMAND Command;

    if (Client->NotifyWait)
    {
        SetThreadpoolWait(Client->NotifyWait, NULL, NULL);
        WaitForThreadpoolWaitCallbacks(Client->NotifyWait, TRUE);
        CloseThreadpoolWait(Client->NotifyWait);
    }

    if (Client->OemClient)
        Modem->RilClose(Client->OemClient);

    while (!IsListEmpty(&Client->Commands))
    {
        Command = CONTAINING_RECORD(RemoveHeadList(&Client->Commands), RIL_HOST_COMMAND, Link);
        HeapFree(GetProcessHeap(), 0, Command);
    }

    if (Client->NotifyEvent)
        CloseHandle(Client->NotifyEvent);
    if (Client->CancelEvent)
        CloseHandle(Client->CancelEvent);
    if (Client->QueueBuffer)
        HeapFree(GetProcessHeap(), 0, Client->QueueBuffer);
    if (Client->EnabledCodes)
        HeapFree(GetProcessHeap(), 0, Client->EnabledCodes);

    DeleteCriticalSection(&Client->CommandLock);
    HeapFree(GetProcessHeap(), 0, Client);
}

static
PRIL_HOST_CLIENT
RilFindClient(
    _In_ PRIL_MODEM Modem,
    _In_ DWORD ClientId)
{
    PLIST_ENTRY Entry;

    for (Entry = Modem->Clients.Flink; Entry != &Modem->Clients; Entry = Entry->Flink)
    {
        PRIL_HOST_CLIENT Client = CONTAINING_RECORD(Entry, RIL_HOST_CLIENT, Link);
        if (Client->ClientId == ClientId)
            return Client;
    }

    return NULL;
}

static
VOID
RilClientOpened(
    _In_ PRIL_MODEM Modem,
    _In_ PMSRIL_HOST_EVENT Event)
{
    PRIL_HOST_CLIENT Client;
    ULONGLONG CancelHandle, NotifyHandle = 0;
    DWORD Actual = 0;

    Client = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Client));
    if (!Client)
        return;

    Client->Modem = Modem;
    Client->ClientId = Event->ClientId;
    Client->ProcessId = Event->ProcessId;
    InitializeCriticalSection(&Client->CommandLock);
    InitializeListHead(&Client->Commands);
    InsertTailList(&Modem->Clients, &Client->Link);

    /* A client the library refused keeps an entry so its commands get a clean error */
    Client->OemClient = Modem->RilOpen(Modem->OemDriver);
    if (!Client->OemClient)
    {
        DPRINT1("RIL_Open failed for client %lu\n", Client->ClientId);
        return;
    }

    Client->CancelEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Client->QueueBufferSize = RIL_QUEUE_INITIAL_SIZE;
    Client->QueueBuffer = HeapAlloc(GetProcessHeap(), 0, Client->QueueBufferSize);
    if (!Client->CancelEvent || !Client->QueueBuffer)
        goto Fail;

    CancelHandle = (ULONG_PTR)Client->CancelEvent;
    if (!Modem->RilIoControl(Client->OemClient, RilCmdInitNotifications,
                             (PBYTE)&CancelHandle, sizeof(CancelHandle),
                             (PBYTE)&NotifyHandle, sizeof(NotifyHandle), &Actual) ||
        Actual != sizeof(NotifyHandle) || !NotifyHandle)
    {
        DPRINT1("Notification setup failed for client %lu\n", Client->ClientId);
        goto Fail;
    }

    Client->NotifyEvent = (HANDLE)(ULONG_PTR)NotifyHandle;
    Client->NotifyWait = CreateThreadpoolWait(RilNotifySignaled, Client, NULL);
    if (!Client->NotifyWait)
        goto Fail;

    SetThreadpoolWait(Client->NotifyWait, Client->NotifyEvent, NULL);
    return;

Fail:
    Modem->RilClose(Client->OemClient);
    Client->OemClient = 0;
}

static
VOID
RilClientClosed(
    _In_ PRIL_MODEM Modem,
    _In_ PMSRIL_HOST_EVENT Event)
{
    PRIL_HOST_CLIENT Client = RilFindClient(Modem, Event->ClientId);

    if (!Client)
        return;

    RemoveEntryList(&Client->Link);
    RilDestroyClient(Client);
}

static
VOID
RilClientCommand(
    _In_ PRIL_MODEM Modem,
    _In_ PMSRIL_HOST_EVENT Event,
    _In_reads_bytes_(Event->DataLength) PBYTE Data)
{
    PRIL_COMMAND_RECORD Header = (PRIL_COMMAND_RECORD)Data;
    PRIL_HOST_CLIENT Client;
    PRIL_HOST_COMMAND Command;
    DWORD Status = 0, Actual = 0;
    HRESULT hr;

    Client = RilFindClient(Modem, Event->ClientId);
    if (!Client || Event->DataLength < sizeof(*Header) || Header->Type != RIL_RECORD_COMMAND)
        return;

    if (!Client->OemClient)
    {
        RilRespondError(Client, Header->Command, Header->Context, RIL_E_RADIONOTAVAILABLE);
        return;
    }

    /* Notification setup and fetching belong to the host */
    if (Header->Command == RilCmdInitNotifications || Header->Command == RilCmdGetNextNotification)
    {
        RilRespondError(Client, Header->Command, Header->Context, E_INVALIDARG);
        return;
    }

    if (Header->Command == RilCmdEnableNotifications)
    {
        hr = RilEnableCodes(Client, Data + sizeof(*Header), Event->DataLength - sizeof(*Header));
        if (FAILED(hr))
            RilRespondError(Client, Header->Command, Header->Context, hr);
        else
            RilRespond(Client, RIL_RESULT_OK, Header->Command, Header->Context, NULL, 0);
        return;
    }

    Command = HeapAlloc(GetProcessHeap(), 0, sizeof(*Command));
    if (!Command)
    {
        RilRespondError(Client, Header->Command, Header->Context, E_OUTOFMEMORY);
        return;
    }

    Command->Command = Header->Command;
    Command->Context = Header->Context;

    /* Registered before the call, the library can complete on another thread first */
    EnterCriticalSection(&Client->CommandLock);
    do
    {
        if (++Client->NextCommandId > RIL_COMMAND_ID_MAX)
            Client->NextCommandId = 1;
    } while (RilCommandIdInUse(Client, Client->NextCommandId));
    Command->CommandId = Client->NextCommandId;
    InsertTailList(&Client->Commands, &Command->Link);
    LeaveCriticalSection(&Client->CommandLock);

    Status = Command->CommandId;
    if (!Modem->RilIoControl(Client->OemClient,
                             Header->Command,
                             Event->DataLength > sizeof(*Header) ? Data + sizeof(*Header) : NULL,
                             Event->DataLength - sizeof(*Header),
                             (PBYTE)&Status,
                             sizeof(Status),
                             &Actual))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(hr))
            hr = E_FAIL;
        if ((HRESULT)Status < 0)
            hr = (HRESULT)Status;
    }
    else
    {
        hr = (HRESULT)Status;
    }

    if (FAILED(hr))
    {
        Command = RilTakeCommand(Client, Command->CommandId);
        if (Command)
        {
            RilRespondError(Client, Command->Command, Command->Context, hr);
            HeapFree(GetProcessHeap(), 0, Command);
        }
    }
}

static
DWORD
WINAPI
RilModemThread(
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
        goto Exit;

    Event = (PMSRIL_HOST_EVENT)Buffer;
    for (;;)
    {
        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Overlapped.hEvent = IoEvent;
        Returned = 0;

        Result = DeviceIoControl(Modem->Broker, IOCTL_MSRIL_HOST_GET_EVENT, NULL, 0,
                                 Buffer, RIL_HOST_EVENT_BUFFER, NULL, &Overlapped);
        if (!Result && GetLastError() == ERROR_IO_PENDING)
            Result = GetOverlappedResult(Modem->Broker, &Overlapped, &Returned, TRUE);
        else if (Result)
            GetOverlappedResult(Modem->Broker, &Overlapped, &Returned, FALSE);

        if (!Result)
        {
            DPRINT("Broker fetch ended with %lu\n", GetLastError());
            break;
        }

        if (Returned < sizeof(*Event) || Event->DataLength > Returned - sizeof(*Event))
            continue;

        switch (Event->EventType)
        {
            case MsRilClientOpened:
                RilClientOpened(Modem, Event);
                break;

            case MsRilClientClosed:
                RilClientClosed(Modem, Event);
                break;

            case MsRilClientRecord:
                RilClientCommand(Modem, Event, (PBYTE)(Event + 1));
                break;
        }
    }

Exit:
    if (IoEvent)
        CloseHandle(IoEvent);
    if (Buffer)
        HeapFree(GetProcessHeap(), 0, Buffer);
    return 0;
}

static
BOOL
RilLoadLibrary(
    _Inout_ PRIL_MODEM Modem)
{
    Modem->Library = LoadLibraryExW(Modem->Config.LibraryPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!Modem->Library)
    {
        DPRINT1("Cannot load %S (%lu)\n", Modem->Config.LibraryPath, GetLastError());
        return FALSE;
    }

    Modem->RilVersion = (PFN_RIL_VERSION)GetProcAddress(Modem->Library, "RIL_Version");
    Modem->RilInit = (PFN_RIL_INIT)GetProcAddress(Modem->Library, "RIL_Init");
    Modem->RilOpen = (PFN_RIL_OPEN)GetProcAddress(Modem->Library, "RIL_Open");
    Modem->RilClose = (PFN_RIL_CLOSE)GetProcAddress(Modem->Library, "RIL_Close");
    Modem->RilIoControl = (PFN_RIL_IOCONTROL)GetProcAddress(Modem->Library, "RIL_IOControl");

    if (!Modem->RilInit || !Modem->RilOpen || !Modem->RilClose || !Modem->RilIoControl)
    {
        DPRINT1("%S is not a RIL library\n", Modem->Config.LibraryPath);
        return FALSE;
    }

    /* Libraries without version negotiation speak the first host interface */
    Modem->Version = Modem->RilVersion ? Modem->RilVersion(RILADAPT_VERSION_MIN, RILADAPT_VERSION_MAX) : 1;
    if (Modem->Version < RILADAPT_VERSION_MIN || Modem->Version > RILADAPT_VERSION_MAX)
    {
        DPRINT1("No common host interface version\n");
        return FALSE;
    }

    Modem->OemDriver = Modem->RilInit(Modem->Config.InitIndex);
    if (!Modem->OemDriver)
    {
        DPRINT1("RIL_Init(%lu) failed\n", Modem->Config.InitIndex);
        return FALSE;
    }

    return TRUE;
}

DWORD
RilStartModem(
    _Inout_ PRIL_MODEM Modem,
    _In_ HANDLE StopEvent)
{
    WCHAR HostPath[MAX_PATH];

    UNREFERENCED_PARAMETER(StopEvent);

    InitializeListHead(&Modem->Clients);
    InitializeCriticalSection(&Modem->DeliverLock);
    Modem->Broker = INVALID_HANDLE_VALUE;

    Modem->DeliverEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Modem->DeliverEvent)
        return GetLastError();

    if (!RilLoadLibrary(Modem))
        return ERROR_DLL_INIT_FAILED;

    if (!RilEnsureBrokerDevice(&Modem->Config) ||
        !RilGetBrokerHostPath(&Modem->Config, HostPath, _countof(HostPath)))
    {
        return ERROR_DEVICE_NOT_AVAILABLE;
    }

    Modem->Broker = CreateFileW(HostPath,
                                GENERIC_READ | GENERIC_WRITE,
                                0,
                                NULL,
                                OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED,
                                NULL);
    if (Modem->Broker == INVALID_HANDLE_VALUE)
    {
        DPRINT1("Cannot attach to %S (%lu)\n", HostPath, GetLastError());
        return GetLastError();
    }

    Modem->Thread = CreateThread(NULL, 0, RilModemThread, Modem, 0, NULL);
    if (!Modem->Thread)
        return GetLastError();

    return ERROR_SUCCESS;
}

VOID
RilStopModem(
    _Inout_ PRIL_MODEM Modem)
{
    PRIL_HOST_CLIENT Client;
    HANDLE Broker;

    /* Closing the host handle cancels the pending fetch and detaches every client */
    Broker = InterlockedExchangePointer(&Modem->Broker, INVALID_HANDLE_VALUE);
    if (Broker != INVALID_HANDLE_VALUE)
        CloseHandle(Broker);

    if (Modem->Thread)
    {
        WaitForSingleObject(Modem->Thread, INFINITE);
        CloseHandle(Modem->Thread);
        Modem->Thread = NULL;
    }

    while (!IsListEmpty(&Modem->Clients))
    {
        Client = CONTAINING_RECORD(RemoveHeadList(&Modem->Clients), RIL_HOST_CLIENT, Link);
        RilDestroyClient(Client);
    }

    if (Modem->DeliverEvent)
    {
        CloseHandle(Modem->DeliverEvent);
        Modem->DeliverEvent = NULL;
    }

    DeleteCriticalSection(&Modem->DeliverLock);

    /* WMRil keeps worker threads alive, so the library stays mapped for the life of the process */
}
