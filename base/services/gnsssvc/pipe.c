/*
 * PROJECT:     ReactOS GNSS Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client pipe, requests and fix subscriptions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "gnsssvc.h"

#define NDEBUG
#include <debug.h>

/* System and administrators own it, interactive and service users may talk to it */
#define GS_PIPE_SDDL        L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12019b;;;IU)(A;;0x12019b;;;SU)"
#define GS_WRITE_WAIT_MS    1000

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS  0x00000008
#endif

typedef struct _GS_CLIENT
{
    LIST_ENTRY Link;
    HANDLE Pipe;
    HANDLE Thread;
    HANDLE WriteEvent;
    CRITICAL_SECTION WriteLock;
    BOOL Wants;                 /* keeps a session running */
    BOOL Subscribed;            /* gets every event, sends nothing more */
    DWORD IntervalMs;
} GS_CLIENT, *PGS_CLIENT;

static PSECURITY_DESCRIPTOR GsPipeSecurity;
static CRITICAL_SECTION GsClientLock;
static LIST_ENTRY GsClients;
static HANDLE GsListenThread;
static volatile LONG GsClientThreads;
static HANDLE GsClientsGone;

/* A client that does not read in time loses the message */
static
BOOL
GsPipeWrite(
    _In_ PGS_CLIENT Client,
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ DWORD Size)
{
    OVERLAPPED Overlapped;
    DWORD Written = 0;
    BOOL Ok;

    EnterCriticalSection(&Client->WriteLock);
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = Client->WriteEvent;
    ResetEvent(Client->WriteEvent);

    Ok = WriteFile(Client->Pipe, Data, Size, NULL, &Overlapped);
    if (Ok || GetLastError() == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject(Client->WriteEvent, GS_WRITE_WAIT_MS) != WAIT_OBJECT_0)
            CancelIo(Client->Pipe);
        Ok = GetOverlappedResult(Client->Pipe, &Overlapped, &Written, TRUE);
    }
    LeaveCriticalSection(&Client->WriteLock);

    return Ok && Written == Size;
}

VOID
GsPipeBroadcast(
    _In_ DWORD Code,
    _In_ const NATIVEGNSS_STATUS *Status,
    _In_ const NATIVEGNSS_FIX *Fix)
{
    NATIVEGNSS_EVENT Event;
    PLIST_ENTRY Entry;
    PGS_CLIENT Client;

    Event.Code = Code;
    Event.Status = *Status;
    Event.Fix = *Fix;

    EnterCriticalSection(&GsClientLock);
    for (Entry = GsClients.Flink; Entry != &GsClients; Entry = Entry->Flink)
    {
        Client = CONTAINING_RECORD(Entry, GS_CLIENT, Link);
        if (Client->Subscribed)
            GsPipeWrite(Client, &Event, sizeof(Event));
    }
    LeaveCriticalSection(&GsClientLock);
}

DWORD
GsPipeDemand(
    _Out_ PDWORD Clients)
{
    PLIST_ENTRY Entry;
    PGS_CLIENT Client;
    DWORD Interval = 0;

    *Clients = 0;

    EnterCriticalSection(&GsClientLock);
    for (Entry = GsClients.Flink; Entry != &GsClients; Entry = Entry->Flink)
    {
        Client = CONTAINING_RECORD(Entry, GS_CLIENT, Link);
        if (!Client->Wants)
            continue;

        (*Clients)++;
        if (Interval == 0 || Client->IntervalMs < Interval)
            Interval = Client->IntervalMs;
    }
    LeaveCriticalSection(&GsClientLock);

    return Interval;
}

static
VOID
GsSetWants(
    _In_ PGS_CLIENT Client,
    _In_ BOOL Wants,
    _In_ BOOL Subscribe,
    _In_ DWORD IntervalMs)
{
    if (IntervalMs == 0)
        IntervalMs = NATIVEGNSS_DEFAULT_INTERVAL_MS;
    else if (IntervalMs < NATIVEGNSS_MIN_INTERVAL_MS)
        IntervalMs = NATIVEGNSS_MIN_INTERVAL_MS;

    EnterCriticalSection(&GsClientLock);
    Client->Wants = Wants;
    Client->IntervalMs = IntervalMs;
    if (Subscribe)
        Client->Subscribed = TRUE;
    LeaveCriticalSection(&GsClientLock);

    GsLocDemandChanged();
}

static
VOID
GsDispatch(
    _In_ PGS_CLIENT Client,
    _In_ const NATIVEGNSS_REQUEST *Request,
    _Out_ PNATIVEGNSS_REPLY Reply)
{
    ZeroMemory(Reply, sizeof(*Reply));

    switch (Request->Command)
    {
        case NativeGnssGetFix:
            break;

        case NativeGnssStart:
            GsSetWants(Client, TRUE, FALSE, Request->IntervalMs);
            break;

        case NativeGnssStop:
            GsSetWants(Client, FALSE, FALSE, 0);
            break;

        case NativeGnssSubscribe:
            GsSetWants(Client, TRUE, TRUE, Request->IntervalMs);
            break;

        case NativeGnssGetStatus:
            break;

        default:
            Reply->Error = ERROR_INVALID_FUNCTION;
            return;
    }

    GsLocGetStatus(&Reply->Status);
    GsLocGetFix(&Reply->Fix);
}

static
VOID
GsFreeClient(
    _In_ PGS_CLIENT Client)
{
    BOOL Wanted;

    EnterCriticalSection(&GsClientLock);
    RemoveEntryList(&Client->Link);
    Wanted = Client->Wants;
    LeaveCriticalSection(&GsClientLock);

    /* The last client leaving lets the engine go idle */
    if (Wanted)
        GsLocDemandChanged();

    DisconnectNamedPipe(Client->Pipe);
    CloseHandle(Client->Pipe);
    CloseHandle(Client->WriteEvent);
    if (Client->Thread)
        CloseHandle(Client->Thread);
    DeleteCriticalSection(&Client->WriteLock);
    HeapFree(GetProcessHeap(), 0, Client);
}

static
DWORD
WINAPI
GsClientThread(
    _In_ LPVOID Parameter)
{
    PGS_CLIENT Client = Parameter;
    NATIVEGNSS_REQUEST Request;
    PNATIVEGNSS_REPLY Reply;
    OVERLAPPED Overlapped;
    HANDLE Waits[2];
    DWORD Read;
    BOOL Subscribed, Ok;

    Reply = HeapAlloc(GetProcessHeap(), 0, sizeof(*Reply));
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Reply || !Overlapped.hEvent)
        goto Exit;

    Waits[0] = GsStopEvent;
    Waits[1] = Overlapped.hEvent;

    for (;;)
    {
        ResetEvent(Overlapped.hEvent);
        Ok = ReadFile(Client->Pipe, &Request, sizeof(Request), NULL, &Overlapped);
        if (!Ok && GetLastError() != ERROR_IO_PENDING)
            break;

        if (WaitForMultipleObjects(_countof(Waits), Waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
        {
            CancelIo(Client->Pipe);
            GetOverlappedResult(Client->Pipe, &Overlapped, &Read, TRUE);
            break;
        }

        if (!GetOverlappedResult(Client->Pipe, &Overlapped, &Read, FALSE))
            break;

        EnterCriticalSection(&GsClientLock);
        Subscribed = Client->Subscribed;
        LeaveCriticalSection(&GsClientLock);

        /* A subscription only carries events, a read just notices the client leaving */
        if (Subscribed)
            continue;

        if (Read != sizeof(Request))
        {
            ZeroMemory(Reply, sizeof(*Reply));
            Reply->Error = ERROR_INVALID_DATA;
        }
        else
        {
            GsDispatch(Client, &Request, Reply);
            if (Request.Command == NativeGnssSubscribe)
                continue;
        }

        if (!GsPipeWrite(Client, Reply, sizeof(*Reply)))
            break;
    }

Exit:
    if (Overlapped.hEvent)
        CloseHandle(Overlapped.hEvent);
    if (Reply)
        HeapFree(GetProcessHeap(), 0, Reply);

    GsFreeClient(Client);

    if (InterlockedDecrement(&GsClientThreads) == 0)
        SetEvent(GsClientsGone);
    return 0;
}

static
HANDLE
GsCreatePipeInstance(
    _In_ BOOL First)
{
    SECURITY_ATTRIBUTES Attributes;

    Attributes.nLength = sizeof(Attributes);
    Attributes.lpSecurityDescriptor = GsPipeSecurity;
    Attributes.bInheritHandle = FALSE;

    return CreateNamedPipeW(NATIVEGNSS_PIPE_NAME,
                            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (First ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES,
                            sizeof(NATIVEGNSS_EVENT),
                            sizeof(NATIVEGNSS_REQUEST),
                            0,
                            &Attributes);
}

static
VOID
GsAcceptClient(
    _In_ HANDLE Pipe)
{
    PGS_CLIENT Client;

    Client = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Client));
    if (!Client)
        goto Fail;

    Client->Pipe = Pipe;
    Client->WriteEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Client->WriteEvent)
        goto Fail;

    InitializeCriticalSection(&Client->WriteLock);

    EnterCriticalSection(&GsClientLock);
    InsertTailList(&GsClients, &Client->Link);
    LeaveCriticalSection(&GsClientLock);

    if (InterlockedIncrement(&GsClientThreads) == 1)
        ResetEvent(GsClientsGone);

    Client->Thread = CreateThread(NULL, 0, GsClientThread, Client, 0, NULL);
    if (!Client->Thread)
    {
        GsFreeClient(Client);
        if (InterlockedDecrement(&GsClientThreads) == 0)
            SetEvent(GsClientsGone);
    }
    return;

Fail:
    if (Client)
        HeapFree(GetProcessHeap(), 0, Client);
    DisconnectNamedPipe(Pipe);
    CloseHandle(Pipe);
}

static
DWORD
WINAPI
GsListenThreadProc(
    _In_ LPVOID Parameter)
{
    OVERLAPPED Overlapped;
    HANDLE Waits[2];
    HANDLE Pipe;
    BOOL First = TRUE, Connected;
    DWORD Bytes;

    UNREFERENCED_PARAMETER(Parameter);

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Overlapped.hEvent)
        return GetLastError();

    Waits[0] = GsStopEvent;
    Waits[1] = Overlapped.hEvent;

    for (;;)
    {
        Pipe = GsCreatePipeInstance(First);
        if (Pipe == INVALID_HANDLE_VALUE)
        {
            DPRINT1("GNSS: CreateNamedPipe failed %lu\n", GetLastError());
            if (WaitForSingleObject(GsStopEvent, 1000) == WAIT_OBJECT_0)
                break;
            continue;
        }
        First = FALSE;

        ResetEvent(Overlapped.hEvent);
        Connected = ConnectNamedPipe(Pipe, &Overlapped);
        if (!Connected)
        {
            switch (GetLastError())
            {
                case ERROR_PIPE_CONNECTED:
                    Connected = TRUE;
                    break;

                case ERROR_IO_PENDING:
                    if (WaitForMultipleObjects(_countof(Waits), Waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
                    {
                        CancelIo(Pipe);
                        GetOverlappedResult(Pipe, &Overlapped, &Bytes, TRUE);
                        CloseHandle(Pipe);
                        goto Exit;
                    }
                    Connected = GetOverlappedResult(Pipe, &Overlapped, &Bytes, FALSE);
                    break;

                default:
                    break;
            }
        }

        if (Connected)
            GsAcceptClient(Pipe);
        else
            CloseHandle(Pipe);
    }

Exit:
    CloseHandle(Overlapped.hEvent);
    return 0;
}

BOOL
GsPipeStart(VOID)
{
    InitializeCriticalSection(&GsClientLock);
    InitializeListHead(&GsClients);

    GsClientsGone = CreateEventW(NULL, TRUE, TRUE, NULL);
    if (!GsClientsGone)
        return FALSE;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(GS_PIPE_SDDL, SDDL_REVISION_1, &GsPipeSecurity, NULL))
    {
        DPRINT1("GNSS: pipe security descriptor failed %lu\n", GetLastError());
        return FALSE;
    }

    GsListenThread = CreateThread(NULL, 0, GsListenThreadProc, NULL, 0, NULL);
    return GsListenThread != NULL;
}

/* GsStopEvent must already be set */
VOID
GsPipeStop(VOID)
{
    if (GsListenThread)
    {
        WaitForSingleObject(GsListenThread, INFINITE);
        CloseHandle(GsListenThread);
        GsListenThread = NULL;
    }

    if (GsClientsGone)
        WaitForSingleObject(GsClientsGone, 5000);

    if (GsPipeSecurity)
    {
        LocalFree(GsPipeSecurity);
        GsPipeSecurity = NULL;
    }
}
