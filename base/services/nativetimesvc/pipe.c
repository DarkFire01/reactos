/*
 * PROJECT:     ReactOS Native Time Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client pipe, requests and event subscriptions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "nativetimesvc.h"

#define NDEBUG
#include <debug.h>

/* System and administrators own it, interactive and service users may talk to it */
#define NT_PIPE_SDDL        L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12019b;;;IU)(A;;0x12019b;;;SU)"
#define NT_WRITE_WAIT_MS    1000

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS  0x00000008
#endif

typedef struct _NT_CLIENT
{
    LIST_ENTRY Link;
    HANDLE Pipe;
    HANDLE Thread;
    HANDLE WriteEvent;
    CRITICAL_SECTION WriteLock;
    BOOL Subscribed;
} NT_CLIENT, *PNT_CLIENT;

static PSECURITY_DESCRIPTOR NtPipeSecurity;
static CRITICAL_SECTION NtClientLock;
static LIST_ENTRY NtClients;
static HANDLE NtListenThread;
static volatile LONG NtClientThreads;
static HANDLE NtClientsGone;

/* One message to one client, it gives up on a client that does not read */
static
BOOL
NtPipeWrite(
    _In_ PNT_CLIENT Client,
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
    if (!Ok && GetLastError() == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject(Client->WriteEvent, NT_WRITE_WAIT_MS) != WAIT_OBJECT_0)
            CancelIo(Client->Pipe);
        Ok = GetOverlappedResult(Client->Pipe, &Overlapped, &Written, TRUE);
    }
    else if (Ok)
    {
        Ok = GetOverlappedResult(Client->Pipe, &Overlapped, &Written, TRUE);
    }
    LeaveCriticalSection(&Client->WriteLock);

    return Ok && Written == Size;
}

VOID
NtPipeBroadcast(
    _In_ DWORD Code,
    _In_opt_ const NATIVETIME_TRIGGER *Trigger)
{
    NATIVETIME_EVENT Event;
    PLIST_ENTRY Entry;
    PNT_CLIENT Client;

    ZeroMemory(&Event, sizeof(Event));
    Event.Code = Code;
    if (Trigger)
        Event.Trigger = *Trigger;

    EnterCriticalSection(&NtClientLock);
    for (Entry = NtClients.Flink; Entry != &NtClients; Entry = Entry->Flink)
    {
        Client = CONTAINING_RECORD(Entry, NT_CLIENT, Link);
        if (Client->Subscribed)
            NtPipeWrite(Client, &Event, sizeof(Event));
    }
    LeaveCriticalSection(&NtClientLock);
}

BOOL
NtPipeHasSubscribers(VOID)
{
    PLIST_ENTRY Entry;
    BOOL Found = FALSE;

    EnterCriticalSection(&NtClientLock);
    for (Entry = NtClients.Flink; Entry != &NtClients && !Found; Entry = Entry->Flink)
        Found = CONTAINING_RECORD(Entry, NT_CLIENT, Link)->Subscribed;
    LeaveCriticalSection(&NtClientLock);

    return Found;
}

static
VOID
NtDispatch(
    _In_ PNT_CLIENT Client,
    _In_ const NATIVETIME_REQUEST *Request,
    _Out_ PNATIVETIME_REPLY Reply)
{
    ZeroMemory(Reply, sizeof(*Reply));

    switch (Request->Command)
    {
        case NativeTimeList:
            Reply->Count = NtTriggersList(Reply->Triggers);
            break;

        case NativeTimeSet:
            Reply->Error = NtTriggersSet(&Request->Trigger, &Reply->Id);
            break;

        case NativeTimeDelete:
            Reply->Error = NtTriggersDelete(Request->Id);
            break;

        case NativeTimeSnooze:
            Reply->Error = NtTriggersSnooze(Request->Id, Request->SnoozeMinutes);
            break;

        case NativeTimeDismiss:
            Reply->Error = NtTriggersDismiss(Request->Id);
            break;

        case NativeTimeSubscribe:
            EnterCriticalSection(&NtClientLock);
            Client->Subscribed = TRUE;
            LeaveCriticalSection(&NtClientLock);
            break;

        case NativeTimeStatus:
            NtClockGetStatus(&Reply->Status);
            break;

        default:
            Reply->Error = ERROR_INVALID_FUNCTION;
            break;
    }
}

static
VOID
NtFreeClient(
    _In_ PNT_CLIENT Client)
{
    EnterCriticalSection(&NtClientLock);
    RemoveEntryList(&Client->Link);
    LeaveCriticalSection(&NtClientLock);

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
NtClientThread(
    _In_ LPVOID Parameter)
{
    PNT_CLIENT Client = Parameter;
    NATIVETIME_REQUEST Request;
    PNATIVETIME_REPLY Reply;
    OVERLAPPED Overlapped;
    HANDLE Waits[2];
    DWORD Read;
    BOOL Subscribed, Ok;

    Reply = HeapAlloc(GetProcessHeap(), 0, sizeof(*Reply));
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Reply || !Overlapped.hEvent)
        goto Exit;

    Waits[0] = NtStopEvent;
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

        EnterCriticalSection(&NtClientLock);
        Subscribed = Client->Subscribed;
        LeaveCriticalSection(&NtClientLock);

        /* A subscription only carries events, anything read on it just keeps it open */
        if (Subscribed)
            continue;

        if (Read != sizeof(Request))
        {
            ZeroMemory(Reply, sizeof(*Reply));
            Reply->Error = ERROR_INVALID_DATA;
        }
        else
        {
            NtDispatch(Client, &Request, Reply);

            /* From here on the connection only carries events, there is no reply */
            if (Request.Command == NativeTimeSubscribe)
                continue;
        }

        if (!NtPipeWrite(Client, Reply, sizeof(*Reply)))
            break;
    }

Exit:
    if (Overlapped.hEvent)
        CloseHandle(Overlapped.hEvent);
    if (Reply)
        HeapFree(GetProcessHeap(), 0, Reply);

    NtFreeClient(Client);

    if (InterlockedDecrement(&NtClientThreads) == 0)
        SetEvent(NtClientsGone);
    return 0;
}

static
HANDLE
NtCreatePipeInstance(
    _In_ BOOL First)
{
    SECURITY_ATTRIBUTES Attributes;

    Attributes.nLength = sizeof(Attributes);
    Attributes.lpSecurityDescriptor = NtPipeSecurity;
    Attributes.bInheritHandle = FALSE;

    return CreateNamedPipeW(NATIVETIME_PIPE_NAME,
                            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (First ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES,
                            sizeof(NATIVETIME_REPLY),
                            sizeof(NATIVETIME_REQUEST),
                            0,
                            &Attributes);
}

static
VOID
NtAcceptClient(
    _In_ HANDLE Pipe)
{
    PNT_CLIENT Client;

    Client = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Client));
    if (!Client)
        goto Fail;

    Client->Pipe = Pipe;
    Client->WriteEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Client->WriteEvent)
        goto Fail;

    InitializeCriticalSection(&Client->WriteLock);

    EnterCriticalSection(&NtClientLock);
    InsertTailList(&NtClients, &Client->Link);
    LeaveCriticalSection(&NtClientLock);

    if (InterlockedIncrement(&NtClientThreads) == 1)
        ResetEvent(NtClientsGone);

    Client->Thread = CreateThread(NULL, 0, NtClientThread, Client, 0, NULL);
    if (!Client->Thread)
    {
        NtFreeClient(Client);
        if (InterlockedDecrement(&NtClientThreads) == 0)
            SetEvent(NtClientsGone);
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
NtListenThreadProc(
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

    Waits[0] = NtStopEvent;
    Waits[1] = Overlapped.hEvent;

    for (;;)
    {
        Pipe = NtCreatePipeInstance(First);
        if (Pipe == INVALID_HANDLE_VALUE)
        {
            DPRINT1("CreateNamedPipe failed %lu\n", GetLastError());
            if (WaitForSingleObject(NtStopEvent, 1000) == WAIT_OBJECT_0)
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
            NtAcceptClient(Pipe);
        else
            CloseHandle(Pipe);
    }

Exit:
    CloseHandle(Overlapped.hEvent);
    return 0;
}

BOOL
NtPipeStart(VOID)
{
    InitializeCriticalSection(&NtClientLock);
    InitializeListHead(&NtClients);

    NtClientsGone = CreateEventW(NULL, TRUE, TRUE, NULL);
    if (!NtClientsGone)
        return FALSE;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(NT_PIPE_SDDL, SDDL_REVISION_1, &NtPipeSecurity, NULL))
    {
        DPRINT1("Pipe security descriptor failed %lu\n", GetLastError());
        return FALSE;
    }

    NtListenThread = CreateThread(NULL, 0, NtListenThreadProc, NULL, 0, NULL);
    return NtListenThread != NULL;
}

/* NtStopEvent must already be set */
VOID
NtPipeStop(VOID)
{
    if (NtListenThread)
    {
        WaitForSingleObject(NtListenThread, INFINITE);
        CloseHandle(NtListenThread);
        NtListenThread = NULL;
    }

    if (NtClientsGone)
        WaitForSingleObject(NtClientsGone, 5000);

    if (NtPipeSecurity)
    {
        LocalFree(NtPipeSecurity);
        NtPipeSecurity = NULL;
    }
}
