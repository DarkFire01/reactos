/*
 * PROJECT:     ReactOS RIL Proxy
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Connection to the MSRil broker, request and record dispatch
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "rilproxy.h"

#define NDEBUG
#include <debug.h>

static
VOID
CALLBACK
RilpReadCompleted(
    _Inout_ PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID Context,
    _Inout_ PTP_WAIT Wait,
    _In_ TP_WAIT_RESULT WaitResult);

static
HRESULT
RilpHresultFromLastError(VOID)
{
    DWORD Error = GetLastError();

    if (Error == ERROR_SUCCESS)
        return E_FAIL;

    return HRESULT_FROM_WIN32(Error);
}

static
VOID
RilpNotifyPresence(
    _In_ PRIL_CLIENT Client,
    _In_ DWORD Presence)
{
    if (Client->NotifyCallback)
    {
        Client->NotifyCallback(Client,
                               RIL_NOTIFY_RADIOPRESENCE,
                               &Presence,
                               sizeof(Presence),
                               Client->Param);
    }
}

static
BOOL
RilpTakePending(
    _In_ PRIL_CLIENT Client,
    _In_ ULONGLONG Context,
    _Out_opt_ PDWORD Command)
{
    PLIST_ENTRY Entry;
    PRIL_PENDING Pending;
    BOOL Found = FALSE;

    EnterCriticalSection(&Client->PendingLock);
    for (Entry = Client->Pending.Flink; Entry != &Client->Pending; Entry = Entry->Flink)
    {
        Pending = CONTAINING_RECORD(Entry, RIL_PENDING, Link);
        if (Pending->Context == Context)
        {
            RemoveEntryList(&Pending->Link);
            if (Command)
                *Command = Pending->Command;
            HeapFree(GetProcessHeap(), 0, Pending);
            Found = TRUE;
            break;
        }
    }
    LeaveCriticalSection(&Client->PendingLock);

    return Found;
}

/* Every command still in flight is completed with a radio unavailable error */
static
VOID
RilpFailAllPending(
    _In_ PRIL_CLIENT Client)
{
    RIL_ERROR_RESULT ErrorResult;
    PRIL_PENDING Pending;
    ULONGLONG Context;

    ErrorResult.Size = sizeof(ErrorResult);
    ErrorResult.Params = 0;
    ErrorResult.Error = RIL_E_RADIONOTAVAILABLE;
    ErrorResult.Reserved = 0;

    for (;;)
    {
        EnterCriticalSection(&Client->PendingLock);
        if (IsListEmpty(&Client->Pending))
        {
            LeaveCriticalSection(&Client->PendingLock);
            break;
        }

        Pending = CONTAINING_RECORD(RemoveHeadList(&Client->Pending), RIL_PENDING, Link);
        LeaveCriticalSection(&Client->PendingLock);

        Context = Pending->Context;
        HeapFree(GetProcessHeap(), 0, Pending);

        if (Client->ResultCallback)
        {
            Client->ResultCallback(Client,
                                   RIL_RESULT_ERROR,
                                   Context,
                                   &ErrorResult,
                                   sizeof(ErrorResult),
                                   Client->Param);
        }
    }
}

static
BOOL
RilpIssueRead(
    _In_ PRIL_CLIENT Client)
{
    DWORD Error;

    ZeroMemory(&Client->ReadOverlapped, sizeof(Client->ReadOverlapped));
    Client->ReadOverlapped.hEvent = Client->ReadEvent;

    Client->ReadOutstanding = TRUE;
    if (!ReadFile(Client->Device,
                  Client->ReadBuffer,
                  Client->ReadBufferSize,
                  NULL,
                  &Client->ReadOverlapped))
    {
        Error = GetLastError();
        if (Error != ERROR_IO_PENDING)
        {
            Client->ReadOutstanding = FALSE;
            SetLastError(Error);
            return FALSE;
        }
    }

    /* The event is signaled for both synchronous and pending completion */
    SetThreadpoolWait(Client->ReadWait, Client->ReadEvent, NULL);
    return TRUE;
}

/* Caller holds DeviceLock */
static
VOID
RilpCloseDeviceLocked(
    _In_ PRIL_CLIENT Client)
{
    if (Client->Device == INVALID_HANDLE_VALUE)
        return;

    /* Closing the handle makes the broker cancel the outstanding read */
    CloseHandle(Client->Device);
    Client->Device = INVALID_HANDLE_VALUE;
}

static
BOOL
RilpTryConnect(
    _In_ PRIL_CLIENT Client)
{
    PWSTR List = NULL;
    ULONG Length = 0;
    HANDLE Device;
    CONFIGRET Cr;

    Cr = CM_Get_Device_Interface_List_SizeW(&Length,
                                            (LPGUID)&GUID_DEVINTERFACE_RIL_CLIENT,
                                            NULL,
                                            CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES);
    if (Cr != CR_SUCCESS || Length <= 5)
        return FALSE;

    List = HeapAlloc(GetProcessHeap(), 0, Length * sizeof(WCHAR));
    if (!List)
        return FALSE;

    Cr = CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_RIL_CLIENT,
                                       NULL,
                                       List,
                                       Length,
                                       CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES);
    if (Cr != CR_SUCCESS)
    {
        HeapFree(GetProcessHeap(), 0, List);
        return FALSE;
    }

    Device = CreateFileW(List,
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED,
                         NULL);
    HeapFree(GetProcessHeap(), 0, List);
    if (Device == INVALID_HANDLE_VALUE)
        return FALSE;

    SetFileCompletionNotificationModes(Device, FILE_SKIP_SET_EVENT_ON_HANDLE);

    EnterCriticalSection(&Client->DeviceLock);
    if (Client->ShuttingDown || Client->Device != INVALID_HANDLE_VALUE)
    {
        LeaveCriticalSection(&Client->DeviceLock);
        CloseHandle(Device);
        return FALSE;
    }

    Client->Device = Device;
    if (!RilpIssueRead(Client))
    {
        RilpCloseDeviceLocked(Client);
        LeaveCriticalSection(&Client->DeviceLock);
        return FALSE;
    }
    LeaveCriticalSection(&Client->DeviceLock);

    RilpNotifyPresence(Client, RIL_RADIO_PRESENT);
    return TRUE;
}

static
VOID
CALLBACK
RilpReconnectTimer(
    _Inout_ PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID Context,
    _Inout_ PTP_TIMER Timer)
{
    PRIL_CLIENT Client = Context;

    UNREFERENCED_PARAMETER(Instance);

    if (RilpTryConnect(Client))
        SetThreadpoolTimer(Timer, NULL, 0, 0);
}

static
VOID
RilpStartReconnect(
    _In_ PRIL_CLIENT Client)
{
    FILETIME DueTime;
    ULARGE_INTEGER Due;

    Due.QuadPart = (ULONGLONG)(-(LONGLONG)RIL_RECONNECT_PERIOD_MS * 10000);
    DueTime.dwLowDateTime = Due.LowPart;
    DueTime.dwHighDateTime = Due.HighPart;

    SetThreadpoolTimer(Client->ReconnectTimer, &DueTime, RIL_RECONNECT_PERIOD_MS, 0);
}

static
VOID
RilpHandleDisconnect(
    _In_ PRIL_CLIENT Client)
{
    BOOL ShuttingDown;

    EnterCriticalSection(&Client->DeviceLock);
    RilpCloseDeviceLocked(Client);
    ShuttingDown = Client->ShuttingDown;
    LeaveCriticalSection(&Client->DeviceLock);

    if (ShuttingDown)
        return;

    RilpFailAllPending(Client);
    RilpNotifyPresence(Client, RIL_RADIO_ABSENT);
    RilpStartReconnect(Client);
}

static
VOID
RilpDispatchRecord(
    _In_ PRIL_CLIENT Client,
    _In_reads_bytes_(Length) PBYTE Record,
    _In_ DWORD Length)
{
    PRIL_RESPONSE_RECORD Response;
    PRIL_NOTIFICATION_RECORD Notification;
    DWORD Type;

    if (Length < sizeof(DWORD))
        return;

    Type = *(PDWORD)Record;
    if (Type == RIL_RECORD_RESPONSE && Length >= sizeof(*Response))
    {
        Response = (PRIL_RESPONSE_RECORD)Record;
        if (!RilpTakePending(Client, Response->Context, NULL))
        {
            DPRINT1("Response for unknown context %I64x\n", Response->Context);
            return;
        }

        if (Client->ResultCallback)
        {
            Client->ResultCallback(Client,
                                   Response->ResultType,
                                   Response->Context,
                                   Length > sizeof(*Response) ? Record + sizeof(*Response) : NULL,
                                   Length - sizeof(*Response),
                                   Client->Param);
        }
    }
    else if (Type == RIL_RECORD_NOTIFICATION && Length >= sizeof(*Notification))
    {
        Notification = (PRIL_NOTIFICATION_RECORD)Record;
        if (Client->NotifyCallback)
        {
            Client->NotifyCallback(Client,
                                   Notification->Code,
                                   Length > sizeof(*Notification) ? Record + sizeof(*Notification) : NULL,
                                   Length - sizeof(*Notification),
                                   Client->Param);
        }
    }
    else
    {
        DPRINT1("Dropping record type %lu, length %lu\n", Type, Length);
    }
}

static
VOID
CALLBACK
RilpReadCompleted(
    _Inout_ PTP_CALLBACK_INSTANCE Instance,
    _Inout_opt_ PVOID Context,
    _Inout_ PTP_WAIT Wait,
    _In_ TP_WAIT_RESULT WaitResult)
{
    PRIL_CLIENT Client = Context;
    DWORD Transferred = 0;
    DWORD Error = ERROR_SUCCESS;
    PBYTE NewBuffer;
    BOOL Reissue;

    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(WaitResult);

    if (!GetOverlappedResult(Client->Device, &Client->ReadOverlapped, &Transferred, FALSE))
        Error = GetLastError();
    Client->ReadOutstanding = FALSE;

    if (Client->ShuttingDown)
        return;

    if (Error == ERROR_INSUFFICIENT_BUFFER)
    {
        /* The broker keeps the record queued, retry with a larger buffer */
        NewBuffer = HeapReAlloc(GetProcessHeap(), 0, Client->ReadBuffer, Client->ReadBufferSize * 2);
        if (!NewBuffer)
        {
            RilpHandleDisconnect(Client);
            return;
        }

        Client->ReadBuffer = NewBuffer;
        Client->ReadBufferSize *= 2;
    }
    else if (Error != ERROR_SUCCESS)
    {
        DPRINT("Read failed with %lu, treating as disconnect\n", Error);
        RilpHandleDisconnect(Client);
        return;
    }
    else
    {
        RilpDispatchRecord(Client, Client->ReadBuffer, Transferred);
    }

    EnterCriticalSection(&Client->DeviceLock);
    Reissue = !Client->ShuttingDown && Client->Device != INVALID_HANDLE_VALUE;
    if (Reissue && !RilpIssueRead(Client))
        Reissue = FALSE;
    LeaveCriticalSection(&Client->DeviceLock);

    if (!Reissue && !Client->ShuttingDown)
        RilpHandleDisconnect(Client);
}

HRESULT
RilpSend(
    _In_ HRIL hRil,
    _In_ DWORD Command,
    _In_ ULONGLONG Context,
    _Inout_updates_bytes_(Size) PVOID Block,
    _In_ DWORD Size)
{
    PRIL_CLIENT Client = hRil;
    PRIL_COMMAND_RECORD Header = Block;
    PRIL_PENDING Pending;
    DWORD Written = 0;
    HRESULT hr = S_OK;

    Header->Type = RIL_RECORD_COMMAND;
    Header->Command = Command;
    Header->Context = Context;

    Pending = HeapAlloc(GetProcessHeap(), 0, sizeof(*Pending));
    if (!Pending)
        return E_OUTOFMEMORY;

    Pending->Context = Context;
    Pending->Command = Command;

    EnterCriticalSection(&Client->DeviceLock);
    if (Client->Device == INVALID_HANDLE_VALUE || Client->ShuttingDown)
    {
        LeaveCriticalSection(&Client->DeviceLock);
        HeapFree(GetProcessHeap(), 0, Pending);
        return RIL_E_RADIONOTAVAILABLE;
    }

    /* Track it before writing so a fast response always finds its entry */
    EnterCriticalSection(&Client->PendingLock);
    InsertTailList(&Client->Pending, &Pending->Link);
    LeaveCriticalSection(&Client->PendingLock);

    ZeroMemory(&Client->WriteOverlapped, sizeof(Client->WriteOverlapped));
    Client->WriteOverlapped.hEvent = Client->WriteEvent;

    if (!WriteFile(Client->Device, Block, Size, NULL, &Client->WriteOverlapped) &&
        GetLastError() != ERROR_IO_PENDING)
    {
        hr = RilpHresultFromLastError();
    }
    else if (!GetOverlappedResult(Client->Device, &Client->WriteOverlapped, &Written, TRUE))
    {
        hr = RilpHresultFromLastError();
    }
    else if (Written != Size)
    {
        hr = HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    }
    LeaveCriticalSection(&Client->DeviceLock);

    if (FAILED(hr))
        RilpTakePending(Client, Context, NULL);

    return hr;
}

HRESULT
RilpSendHeaderOnly(
    _In_ HRIL hRil,
    _In_ DWORD Command,
    _In_ ULONGLONG Context)
{
    RIL_COMMAND_RECORD Header;

    return RilpSend(hRil, Command, Context, &Header, sizeof(Header));
}

HRESULT
RilpSendVariable(
    _In_ HRIL hRil,
    _In_ DWORD Command,
    _In_ ULONGLONG Context,
    _In_reads_bytes_opt_(PrefixSize) const VOID *Prefix,
    _In_ DWORD PrefixSize,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize)
{
    DWORD Size;
    PBYTE Block;
    HRESULT hr;

    Size = sizeof(RIL_COMMAND_RECORD) + PrefixSize + DataSize;
    if (Size < PrefixSize || Size > RIL_RECORD_MAX_LENGTH)
        return E_INVALIDARG;

    Block = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
    if (!Block)
        return E_OUTOFMEMORY;

    if (PrefixSize)
        CopyMemory(Block + sizeof(RIL_COMMAND_RECORD), Prefix, PrefixSize);
    if (DataSize)
        CopyMemory(Block + sizeof(RIL_COMMAND_RECORD) + PrefixSize, Data, DataSize);

    hr = RilpSend(hRil, Command, Context, Block, Size);
    HeapFree(GetProcessHeap(), 0, Block);
    return hr;
}

HRESULT
RilpCreateClient(
    _In_ DWORD Index,
    _In_opt_ PRIL_RESULT_CALLBACK ResultCallback,
    _In_opt_ PRIL_NOTIFY_CALLBACK NotifyCallback,
    _In_opt_ PVOID Param,
    _Out_ PRIL_CLIENT *Client)
{
    PRIL_CLIENT NewClient;

    *Client = NULL;

    NewClient = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*NewClient));
    if (!NewClient)
        return E_FAIL;

    NewClient->Index = Index;
    NewClient->ResultCallback = ResultCallback;
    NewClient->NotifyCallback = NotifyCallback;
    NewClient->Param = Param;
    NewClient->Device = INVALID_HANDLE_VALUE;
    InitializeCriticalSection(&NewClient->DeviceLock);
    InitializeCriticalSection(&NewClient->PendingLock);
    InitializeListHead(&NewClient->Pending);

    NewClient->ReadBufferSize = RIL_INITIAL_READ_SIZE;
    NewClient->ReadBuffer = HeapAlloc(GetProcessHeap(), 0, NewClient->ReadBufferSize);
    NewClient->ReadEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    NewClient->WriteEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    NewClient->ReadWait = CreateThreadpoolWait(RilpReadCompleted, NewClient, NULL);
    NewClient->ReconnectTimer = CreateThreadpoolTimer(RilpReconnectTimer, NewClient, NULL);

    if (!NewClient->ReadBuffer || !NewClient->ReadEvent || !NewClient->WriteEvent ||
        !NewClient->ReadWait || !NewClient->ReconnectTimer)
    {
        RilpDestroyClient(NewClient);
        return E_FAIL;
    }

    /* The caller gets the handle first, presence is reported through the notify callback */
    *Client = NewClient;
    if (!RilpTryConnect(NewClient))
    {
        RilpNotifyPresence(NewClient, RIL_RADIO_ABSENT);
        RilpStartReconnect(NewClient);
    }

    return S_OK;
}

VOID
RilpDestroyClient(
    _In_ PRIL_CLIENT Client)
{
    PRIL_PENDING Pending;

    EnterCriticalSection(&Client->DeviceLock);
    Client->ShuttingDown = TRUE;
    LeaveCriticalSection(&Client->DeviceLock);

    if (Client->ReconnectTimer)
    {
        SetThreadpoolTimer(Client->ReconnectTimer, NULL, 0, 0);
        WaitForThreadpoolTimerCallbacks(Client->ReconnectTimer, TRUE);
        CloseThreadpoolTimer(Client->ReconnectTimer);
    }

    EnterCriticalSection(&Client->DeviceLock);
    RilpCloseDeviceLocked(Client);
    LeaveCriticalSection(&Client->DeviceLock);

    /* The cancelled read still owns the buffer until its event fires */
    if (Client->ReadOutstanding && Client->ReadEvent)
        WaitForSingleObject(Client->ReadEvent, INFINITE);

    if (Client->ReadWait)
    {
        SetThreadpoolWait(Client->ReadWait, NULL, NULL);
        WaitForThreadpoolWaitCallbacks(Client->ReadWait, TRUE);
        CloseThreadpoolWait(Client->ReadWait);
    }

    while (!IsListEmpty(&Client->Pending))
    {
        Pending = CONTAINING_RECORD(RemoveHeadList(&Client->Pending), RIL_PENDING, Link);
        HeapFree(GetProcessHeap(), 0, Pending);
    }

    if (Client->ReadEvent)
        CloseHandle(Client->ReadEvent);
    if (Client->WriteEvent)
        CloseHandle(Client->WriteEvent);
    if (Client->ReadBuffer)
        HeapFree(GetProcessHeap(), 0, Client->ReadBuffer);

    DeleteCriticalSection(&Client->PendingLock);
    DeleteCriticalSection(&Client->DeviceLock);
    HeapFree(GetProcessHeap(), 0, Client);
}
