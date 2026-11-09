/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Named pipe server for phoneom.dll clients
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "phonesvc.h"
#include <sddl.h>

#define NDEBUG
#include <debug.h>

#define PH_PIPE_WRITE_TIMEOUT_MS    3000

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS  0x00000008
#endif

/* SYSTEM and administrators get full access, interactive and service users may connect but not create instances */
#define PH_PIPE_SDDL L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x12019b;;;IU)(A;;0x12019b;;;SU)"

typedef struct _PH_CLIENT
{
    LIST_ENTRY Link;
    HANDLE Pipe;
    HANDLE Thread;
    CRITICAL_SECTION WriteLock;
    HANDLE WriteEvent;
    BOOL Broken;
} PH_CLIENT, *PPH_CLIENT;

static CRITICAL_SECTION PhClientLock;
static LIST_ENTRY PhClients;
static HANDLE PhListenThread;
static PSECURITY_DESCRIPTOR PhPipeSecurity;
static LONG PhClientThreads;
static HANDLE PhClientsGone;

static
BOOL
PhPipeWrite(
    _In_ PPH_CLIENT Client,
    _In_ DWORD Kind,
    _In_ DWORD Code,
    _In_ DWORD Sequence,
    _In_ HRESULT Status,
    _In_reads_bytes_opt_(Size) const VOID *Data,
    _In_ DWORD Size)
{
    PPHONE_PIPE_HEADER Header;
    OVERLAPPED Overlapped;
    DWORD Total, Written = 0;
    BOOL Ok = FALSE;

    if (Size > PHONE_PIPE_MAX_MESSAGE - sizeof(*Header))
        return FALSE;

    Total = sizeof(*Header) + Size;
    Header = HeapAlloc(GetProcessHeap(), 0, Total);
    if (!Header)
        return FALSE;

    Header->Kind = Kind;
    Header->Code = Code;
    Header->Sequence = Sequence;
    Header->Status = Status;
    Header->DataSize = Size;
    if (Data && Size)
        CopyMemory(Header + 1, Data, Size);

    EnterCriticalSection(&Client->WriteLock);

    if (!Client->Broken)
    {
        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Overlapped.hEvent = Client->WriteEvent;

        if (WriteFile(Client->Pipe, Header, Total, &Written, &Overlapped))
        {
            Ok = (Written == Total);
        }
        else if (GetLastError() == ERROR_IO_PENDING)
        {
            /* A client that stops reading must not stall the service */
            if (WaitForSingleObject(Overlapped.hEvent, PH_PIPE_WRITE_TIMEOUT_MS) != WAIT_OBJECT_0)
            {
                CancelIo(Client->Pipe);
                GetOverlappedResult(Client->Pipe, &Overlapped, &Written, TRUE);
            }
            else
            {
                Ok = GetOverlappedResult(Client->Pipe, &Overlapped, &Written, FALSE) && Written == Total;
            }
        }

        if (!Ok)
            Client->Broken = TRUE;
    }

    LeaveCriticalSection(&Client->WriteLock);

    HeapFree(GetProcessHeap(), 0, Header);
    return Ok;
}

VOID
PhBroadcast(
    _In_ DWORD Event,
    _In_reads_bytes_opt_(Size) const VOID *Data,
    _In_ DWORD Size)
{
    PLIST_ENTRY Entry;
    PPH_CLIENT Client;

    EnterCriticalSection(&PhClientLock);
    for (Entry = PhClients.Flink; Entry != &PhClients; Entry = Entry->Flink)
    {
        Client = CONTAINING_RECORD(Entry, PH_CLIENT, Link);
        PhPipeWrite(Client, PhonePipeEvent, Event, 0, S_OK, Data, Size);
    }
    LeaveCriticalSection(&PhClientLock);
}

static
BOOL
PhPayloadDword(
    _In_ const PHONE_PIPE_HEADER *Header,
    _Out_ PDWORD Value)
{
    if (Header->DataSize < sizeof(DWORD))
        return FALSE;

    CopyMemory(Value, Header + 1, sizeof(*Value));
    return TRUE;
}

static
VOID
PhDispatch(
    _In_ PPH_CLIENT Client,
    _In_ const PHONE_PIPE_HEADER *Header)
{
    const VOID *Payload = Header + 1;
    PHONEOM_RADIO Radio;
    PHONE_REQUEST_DIAL *Dial;
    PHONE_REQUEST_MESSAGE *Message;
    PHONE_REQUEST_RANGE Range;
    PVOID Reply = NULL;
    DWORD ReplySize = 0;
    DWORD Value, Result = 0;
    HRESULT hr = S_OK;

    switch (Header->Code)
    {
        case PhoneRequestGetRadio:
            PhGetRadio(&Radio);
            Reply = &Radio;
            ReplySize = sizeof(Radio);
            break;

        case PhoneRequestSetRadioPower:
            hr = PhPayloadDword(Header, &Value) ? PhSetRadioPower(Value != 0) : E_INVALIDARG;
            break;

        case PhoneRequestDial:
            if (Header->DataSize < sizeof(*Dial))
            {
                hr = E_INVALIDARG;
                break;
            }

            Dial = (PHONE_REQUEST_DIAL *)Payload;
            Dial->Number[_countof(Dial->Number) - 1] = UNICODE_NULL;
            hr = PhDial(Dial->Number, &Result);
            Reply = &Result;
            ReplySize = sizeof(Result);
            break;

        case PhoneRequestAnswer:
            hr = PhPayloadDword(Header, &Value) ? PhManageCall(RIL_CALLACTION_ANSWER, Value) : E_INVALIDARG;
            break;

        case PhoneRequestHangUp:
            if (!PhPayloadDword(Header, &Value))
                hr = E_INVALIDARG;
            else if (Value == 0)
                hr = PhManageCall(RIL_CALLACTION_RELEASE_ALL, 0);
            else
                hr = PhManageCall(RIL_CALLACTION_RELEASE_CALL, Value);
            break;

        case PhoneRequestSwap:
            hr = PhManageCall(RIL_CALLACTION_SWAP, 0);
            break;

        case PhoneRequestDtmf:
            hr = PhPayloadDword(Header, &Value) ? PhSendDtmf((WCHAR)Value) : E_INVALIDARG;
            break;

        case PhoneRequestGetCalls:
            Reply = HeapAlloc(GetProcessHeap(), 0, PHONEOM_MAX_CALLS * sizeof(PHONEOM_CALL));
            if (!Reply)
            {
                hr = E_OUTOFMEMORY;
                break;
            }
            ReplySize = PhGetCalls(Reply, PHONEOM_MAX_CALLS) * sizeof(PHONEOM_CALL);
            break;

        case PhoneRequestSetAudioRoute:
            hr = PhPayloadDword(Header, &Value) ? PhAudioSetRoute(Value) : E_INVALIDARG;
            break;

        case PhoneRequestSetMute:
            hr = PhPayloadDword(Header, &Value) ? PhAudioSetMute(Value != 0) : E_INVALIDARG;
            break;

        case PhoneRequestSendMessage:
            if (Header->DataSize < sizeof(*Message))
            {
                hr = E_INVALIDARG;
                break;
            }

            Message = (PHONE_REQUEST_MESSAGE *)Payload;
            Message->Address[_countof(Message->Address) - 1] = UNICODE_NULL;
            Message->Text[_countof(Message->Text) - 1] = UNICODE_NULL;
            hr = PhSmsSend(Message->Address, Message->Text, &Result);
            Reply = &Result;
            ReplySize = sizeof(Result);
            break;

        case PhoneRequestGetMessages:
            if (Header->DataSize < sizeof(Range))
            {
                hr = E_INVALIDARG;
                break;
            }

            CopyMemory(&Range, Payload, sizeof(Range));
            Range.MaxCount = min(Range.MaxCount,
                                 (PHONE_PIPE_MAX_MESSAGE - sizeof(PHONE_PIPE_HEADER)) / sizeof(PHONEOM_MESSAGE));

            Reply = HeapAlloc(GetProcessHeap(), 0, max(Range.MaxCount, 1) * sizeof(PHONEOM_MESSAGE));
            if (!Reply)
            {
                hr = E_OUTOFMEMORY;
                break;
            }
            ReplySize = PhStoreEnumerate(Range.FirstId, Reply, Range.MaxCount) * sizeof(PHONEOM_MESSAGE);
            break;

        case PhoneRequestMarkRead:
            if (!PhPayloadDword(Header, &Value))
                hr = E_INVALIDARG;
            else if (!PhStoreUpdateFlags(Value, PHONEOM_MESSAGE_READ, 0, NULL))
                hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            break;

        case PhoneRequestDeleteMessage:
            if (!PhPayloadDword(Header, &Value))
                hr = E_INVALIDARG;
            else if (!PhStoreDelete(Value))
                hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            break;

        default:
            hr = E_NOTIMPL;
            break;
    }

    PhPipeWrite(Client, PhonePipeReply, Header->Code, Header->Sequence, hr, FAILED(hr) ? NULL : Reply, FAILED(hr) ? 0 : ReplySize);

    if (Reply && Reply != &Radio && Reply != &Result)
        HeapFree(GetProcessHeap(), 0, Reply);
}

static
VOID
PhFreeClient(
    _In_ PPH_CLIENT Client)
{
    EnterCriticalSection(&PhClientLock);
    RemoveEntryList(&Client->Link);
    LeaveCriticalSection(&PhClientLock);

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
PhClientThread(
    _In_ LPVOID Parameter)
{
    PPH_CLIENT Client = Parameter;
    PPHONE_PIPE_HEADER Header;
    OVERLAPPED Overlapped;
    HANDLE Waits[2];
    DWORD Read;
    BOOL Ok;

    Header = HeapAlloc(GetProcessHeap(), 0, PHONE_PIPE_MAX_MESSAGE);
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (!Header || !Overlapped.hEvent)
        goto Exit;

    Waits[0] = PhStopEvent;
    Waits[1] = Overlapped.hEvent;

    while (!Client->Broken)
    {
        ResetEvent(Overlapped.hEvent);
        Ok = ReadFile(Client->Pipe, Header, PHONE_PIPE_MAX_MESSAGE, &Read, &Overlapped);
        if (!Ok && GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForMultipleObjects(_countof(Waits), Waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
            {
                CancelIo(Client->Pipe);
                GetOverlappedResult(Client->Pipe, &Overlapped, &Read, TRUE);
                break;
            }
            Ok = GetOverlappedResult(Client->Pipe, &Overlapped, &Read, FALSE);
        }

        /* A message larger than the protocol allows ends the session */
        if (!Ok)
            break;

        if (Read < sizeof(*Header) ||
            Header->Kind != PhonePipeRequest ||
            Header->DataSize > Read - sizeof(*Header))
        {
            break;
        }

        PhDispatch(Client, Header);
    }

Exit:
    if (Overlapped.hEvent)
        CloseHandle(Overlapped.hEvent);
    if (Header)
        HeapFree(GetProcessHeap(), 0, Header);

    PhFreeClient(Client);

    if (InterlockedDecrement(&PhClientThreads) == 0)
        SetEvent(PhClientsGone);
    return 0;
}

static
HANDLE
PhCreatePipeInstance(
    _In_ BOOL First)
{
    SECURITY_ATTRIBUTES Attributes;

    Attributes.nLength = sizeof(Attributes);
    Attributes.lpSecurityDescriptor = PhPipeSecurity;
    Attributes.bInheritHandle = FALSE;

    return CreateNamedPipeW(PHONE_PIPE_NAME,
                            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (First ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES,
                            PHONE_PIPE_MAX_MESSAGE,
                            PHONE_PIPE_MAX_MESSAGE,
                            0,
                            &Attributes);
}

static
VOID
PhAcceptClient(
    _In_ HANDLE Pipe)
{
    PPH_CLIENT Client;

    Client = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Client));
    if (!Client)
        goto Fail;

    Client->Pipe = Pipe;
    Client->WriteEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Client->WriteEvent)
        goto Fail;

    InitializeCriticalSection(&Client->WriteLock);

    EnterCriticalSection(&PhClientLock);
    InsertTailList(&PhClients, &Client->Link);
    LeaveCriticalSection(&PhClientLock);

    if (InterlockedIncrement(&PhClientThreads) == 1)
        ResetEvent(PhClientsGone);

    Client->Thread = CreateThread(NULL, 0, PhClientThread, Client, 0, NULL);
    if (!Client->Thread)
    {
        PhFreeClient(Client);
        if (InterlockedDecrement(&PhClientThreads) == 0)
            SetEvent(PhClientsGone);
    }
    return;

Fail:
    if (Client)
    {
        if (Client->WriteEvent)
            CloseHandle(Client->WriteEvent);
        HeapFree(GetProcessHeap(), 0, Client);
    }
    DisconnectNamedPipe(Pipe);
    CloseHandle(Pipe);
}

static
DWORD
WINAPI
PhListenThreadProc(
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

    Waits[0] = PhStopEvent;
    Waits[1] = Overlapped.hEvent;

    for (;;)
    {
        Pipe = PhCreatePipeInstance(First);
        if (Pipe == INVALID_HANDLE_VALUE)
        {
            DPRINT1("CreateNamedPipe failed (%lu)\n", GetLastError());
            if (WaitForSingleObject(PhStopEvent, 1000) == WAIT_OBJECT_0)
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
            PhAcceptClient(Pipe);
        else
            CloseHandle(Pipe);
    }

Exit:
    CloseHandle(Overlapped.hEvent);
    return 0;
}

BOOL
PhPipeStart(VOID)
{
    InitializeCriticalSection(&PhClientLock);
    InitializeListHead(&PhClients);

    PhClientsGone = CreateEventW(NULL, TRUE, TRUE, NULL);
    if (!PhClientsGone)
        return FALSE;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(PH_PIPE_SDDL, SDDL_REVISION_1, &PhPipeSecurity, NULL))
    {
        DPRINT1("Pipe security descriptor failed (%lu)\n", GetLastError());
        return FALSE;
    }

    PhListenThread = CreateThread(NULL, 0, PhListenThreadProc, NULL, 0, NULL);
    return PhListenThread != NULL;
}

/* PhStopEvent must already be set */
VOID
PhPipeStop(VOID)
{
    if (PhListenThread)
    {
        WaitForSingleObject(PhListenThread, INFINITE);
        CloseHandle(PhListenThread);
        PhListenThread = NULL;
    }

    /* Client threads may be inside a RIL request, give them the request timeout */
    if (PhClientsGone)
        WaitForSingleObject(PhClientsGone, PHONE_RIL_TIMEOUT_MS);

    if (PhPipeSecurity)
    {
        LocalFree(PhPipeSecurity);
        PhPipeSecurity = NULL;
    }
}
