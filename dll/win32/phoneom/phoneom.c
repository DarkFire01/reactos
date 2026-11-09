/*
 * PROJECT:     ReactOS Phone Object Model Client
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client side of the phone service pipe protocol
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <ndk/rtlfuncs.h>
#include <stdlib.h>
#include <strsafe.h>

#include <telephony/phoneom.h>
#include <telephony/phonepipe.h>

#define PHONEOM_CONNECT_TIMEOUT_MS  5000
#define PHONEOM_REPLY_TIMEOUT_MS    75000

typedef struct _PHONEOM_QUEUED_EVENT
{
    LIST_ENTRY Link;
    DWORD Event;
    DWORD DataSize;
    BYTE Data[ANYSIZE_ARRAY];
} PHONEOM_QUEUED_EVENT, *PPHONEOM_QUEUED_EVENT;

typedef struct _PHONEOM_SESSION
{
    HANDLE Pipe;
    HANDLE StopEvent;
    HANDLE ReaderThread;
    HANDLE DispatchThread;
    PPHONEOM_EVENT_CALLBACK Callback;
    PVOID Context;
    volatile LONG Disconnected;

    /* One request is in flight at a time */
    CRITICAL_SECTION RequestLock;
    DWORD Sequence;
    HANDLE WriteEvent;

    /* Filled by the reader for the request that is waiting */
    CRITICAL_SECTION ReplyLock;
    HANDLE ReplyEvent;
    DWORD WaitingSequence;
    HRESULT ReplyStatus;
    DWORD ReplySize;
    PBYTE ReplyBuffer;

    /* Events go through their own thread so callbacks may issue requests */
    CRITICAL_SECTION EventLock;
    LIST_ENTRY Events;
    HANDLE EventReady;
} PHONEOM_SESSION, *PPHONEOM_SESSION;

static
VOID
PhOmQueueEvent(
    _In_ PPHONEOM_SESSION Session,
    _In_ DWORD Event,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize)
{
    PPHONEOM_QUEUED_EVENT Queued;

    if (!Session->Callback)
        return;

    Queued = HeapAlloc(GetProcessHeap(), 0, FIELD_OFFSET(PHONEOM_QUEUED_EVENT, Data) + DataSize);
    if (!Queued)
        return;

    Queued->Event = Event;
    Queued->DataSize = DataSize;
    if (Data && DataSize)
        CopyMemory(Queued->Data, Data, DataSize);

    EnterCriticalSection(&Session->EventLock);
    InsertTailList(&Session->Events, &Queued->Link);
    LeaveCriticalSection(&Session->EventLock);

    SetEvent(Session->EventReady);
}

static
DWORD
WINAPI
PhOmDispatchThread(
    _In_ LPVOID Parameter)
{
    PPHONEOM_SESSION Session = Parameter;
    PPHONEOM_QUEUED_EVENT Queued;
    HANDLE Waits[2];
    PLIST_ENTRY Entry;

    Waits[0] = Session->StopEvent;
    Waits[1] = Session->EventReady;

    while (WaitForMultipleObjects(_countof(Waits), Waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
    {
        for (;;)
        {
            EnterCriticalSection(&Session->EventLock);
            Entry = IsListEmpty(&Session->Events) ? NULL : RemoveHeadList(&Session->Events);
            LeaveCriticalSection(&Session->EventLock);

            if (!Entry)
                break;

            Queued = CONTAINING_RECORD(Entry, PHONEOM_QUEUED_EVENT, Link);
            Session->Callback(Session, Queued->Event, Queued->DataSize ? Queued->Data : NULL, Queued->DataSize, Session->Context);
            HeapFree(GetProcessHeap(), 0, Queued);
        }
    }

    return 0;
}

static
VOID
PhOmMarkDisconnected(
    _In_ PPHONEOM_SESSION Session)
{
    if (InterlockedExchange(&Session->Disconnected, TRUE))
        return;

    /* Whoever waits for a reply gets the failure now */
    EnterCriticalSection(&Session->ReplyLock);
    if (Session->WaitingSequence)
    {
        Session->ReplyStatus = HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED);
        Session->ReplySize = 0;
        Session->WaitingSequence = 0;
        SetEvent(Session->ReplyEvent);
    }
    LeaveCriticalSection(&Session->ReplyLock);

    PhOmQueueEvent(Session, PhoneEventDisconnected, NULL, 0);
}

static
DWORD
WINAPI
PhOmReaderThread(
    _In_ LPVOID Parameter)
{
    PPHONEOM_SESSION Session = Parameter;
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

    Waits[0] = Session->StopEvent;
    Waits[1] = Overlapped.hEvent;

    for (;;)
    {
        ResetEvent(Overlapped.hEvent);
        Ok = ReadFile(Session->Pipe, Header, PHONE_PIPE_MAX_MESSAGE, &Read, &Overlapped);
        if (!Ok && GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForMultipleObjects(_countof(Waits), Waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
            {
                CancelIo(Session->Pipe);
                GetOverlappedResult(Session->Pipe, &Overlapped, &Read, TRUE);
                break;
            }
            Ok = GetOverlappedResult(Session->Pipe, &Overlapped, &Read, FALSE);
        }

        if (!Ok || Read < sizeof(*Header) || Header->DataSize > Read - sizeof(*Header))
            break;

        if (Header->Kind == PhonePipeEvent)
        {
            PhOmQueueEvent(Session, Header->Code, Header + 1, Header->DataSize);
            continue;
        }

        if (Header->Kind != PhonePipeReply)
            continue;

        EnterCriticalSection(&Session->ReplyLock);
        if (Session->WaitingSequence && Header->Sequence == Session->WaitingSequence)
        {
            Session->ReplyStatus = Header->Status;
            Session->ReplySize = Header->DataSize;
            CopyMemory(Session->ReplyBuffer, Header + 1, Header->DataSize);
            Session->WaitingSequence = 0;
            SetEvent(Session->ReplyEvent);
        }
        LeaveCriticalSection(&Session->ReplyLock);
    }

Exit:
    if (WaitForSingleObject(Session->StopEvent, 0) != WAIT_OBJECT_0)
        PhOmMarkDisconnected(Session);

    if (Overlapped.hEvent)
        CloseHandle(Overlapped.hEvent);
    if (Header)
        HeapFree(GetProcessHeap(), 0, Header);
    return 0;
}

/* Sends one request and copies up to OutSize bytes of the reply payload */
static
HRESULT
PhOmTransact(
    _In_ HPHONEOM Session,
    _In_ DWORD Code,
    _In_reads_bytes_opt_(InSize) const VOID *In,
    _In_ DWORD InSize,
    _Out_writes_bytes_to_opt_(OutSize, *Returned) PVOID Out,
    _In_ DWORD OutSize,
    _Out_opt_ PDWORD Returned)
{
    PPHONE_PIPE_HEADER Header;
    OVERLAPPED Overlapped;
    DWORD Total, Written;
    HRESULT hr;
    BOOL Ok;

    if (Returned)
        *Returned = 0;

    if (!Session)
        return E_HANDLE;

    if (Session->Disconnected)
        return HRESULT_FROM_WIN32(ERROR_PIPE_NOT_CONNECTED);

    if (InSize > PHONE_PIPE_MAX_MESSAGE - sizeof(*Header))
        return E_INVALIDARG;

    Total = sizeof(*Header) + InSize;
    Header = HeapAlloc(GetProcessHeap(), 0, Total);
    if (!Header)
        return E_OUTOFMEMORY;

    EnterCriticalSection(&Session->RequestLock);

    if (++Session->Sequence == 0)
        Session->Sequence = 1;

    Header->Kind = PhonePipeRequest;
    Header->Code = Code;
    Header->Sequence = Session->Sequence;
    Header->Status = S_OK;
    Header->DataSize = InSize;
    if (In && InSize)
        CopyMemory(Header + 1, In, InSize);

    EnterCriticalSection(&Session->ReplyLock);
    ResetEvent(Session->ReplyEvent);
    Session->WaitingSequence = Session->Sequence;
    LeaveCriticalSection(&Session->ReplyLock);

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = Session->WriteEvent;
    Ok = WriteFile(Session->Pipe, Header, Total, &Written, &Overlapped);
    if (!Ok && GetLastError() == ERROR_IO_PENDING)
        Ok = GetOverlappedResult(Session->Pipe, &Overlapped, &Written, TRUE);

    if (!Ok || Written != Total)
    {
        hr = HRESULT_FROM_WIN32(Ok ? ERROR_WRITE_FAULT : GetLastError());
    }
    else if (WaitForSingleObject(Session->ReplyEvent, PHONEOM_REPLY_TIMEOUT_MS) != WAIT_OBJECT_0)
    {
        hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    else
    {
        hr = Session->ReplyStatus;
        if (SUCCEEDED(hr) && Out)
        {
            Total = min(Session->ReplySize, OutSize);
            CopyMemory(Out, Session->ReplyBuffer, Total);
            if (Returned)
                *Returned = Total;
        }
    }

    EnterCriticalSection(&Session->ReplyLock);
    Session->WaitingSequence = 0;
    LeaveCriticalSection(&Session->ReplyLock);

    LeaveCriticalSection(&Session->RequestLock);

    HeapFree(GetProcessHeap(), 0, Header);
    return hr;
}

static
HRESULT
PhOmTransactDword(
    _In_ HPHONEOM Session,
    _In_ DWORD Code,
    _In_ DWORD Value)
{
    return PhOmTransact(Session, Code, &Value, sizeof(Value), NULL, 0, NULL);
}

static
VOID
PhOmFreeSession(
    _In_ PPHONEOM_SESSION Session)
{
    PLIST_ENTRY Entry;

    if (Session->StopEvent)
        SetEvent(Session->StopEvent);

    if (Session->ReaderThread)
    {
        WaitForSingleObject(Session->ReaderThread, INFINITE);
        CloseHandle(Session->ReaderThread);
    }

    if (Session->DispatchThread)
    {
        WaitForSingleObject(Session->DispatchThread, INFINITE);
        CloseHandle(Session->DispatchThread);
    }

    while (!IsListEmpty(&Session->Events))
    {
        Entry = RemoveHeadList(&Session->Events);
        HeapFree(GetProcessHeap(), 0, CONTAINING_RECORD(Entry, PHONEOM_QUEUED_EVENT, Link));
    }

    if (Session->Pipe != INVALID_HANDLE_VALUE)
        CloseHandle(Session->Pipe);
    if (Session->StopEvent)
        CloseHandle(Session->StopEvent);
    if (Session->WriteEvent)
        CloseHandle(Session->WriteEvent);
    if (Session->ReplyEvent)
        CloseHandle(Session->ReplyEvent);
    if (Session->EventReady)
        CloseHandle(Session->EventReady);
    if (Session->ReplyBuffer)
        HeapFree(GetProcessHeap(), 0, Session->ReplyBuffer);

    DeleteCriticalSection(&Session->RequestLock);
    DeleteCriticalSection(&Session->ReplyLock);
    DeleteCriticalSection(&Session->EventLock);
    HeapFree(GetProcessHeap(), 0, Session);
}

HRESULT
WINAPI
PhoneOmOpen(
    _In_opt_ PPHONEOM_EVENT_CALLBACK Callback,
    _In_opt_ PVOID Context,
    _Out_ HPHONEOM *Phone)
{
    PPHONEOM_SESSION Session;
    DWORD Mode = PIPE_READMODE_MESSAGE;
    HRESULT hr;

    if (!Phone)
        return E_POINTER;
    *Phone = NULL;

    Session = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Session));
    if (!Session)
        return E_OUTOFMEMORY;

    Session->Pipe = INVALID_HANDLE_VALUE;
    Session->Callback = Callback;
    Session->Context = Context;
    InitializeCriticalSection(&Session->RequestLock);
    InitializeCriticalSection(&Session->ReplyLock);
    InitializeCriticalSection(&Session->EventLock);
    InitializeListHead(&Session->Events);

    Session->StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Session->WriteEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Session->ReplyEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Session->EventReady = CreateEventW(NULL, FALSE, FALSE, NULL);
    Session->ReplyBuffer = HeapAlloc(GetProcessHeap(), 0, PHONE_PIPE_MAX_MESSAGE);
    if (!Session->StopEvent || !Session->WriteEvent || !Session->ReplyEvent ||
        !Session->EventReady || !Session->ReplyBuffer)
    {
        hr = E_OUTOFMEMORY;
        goto Fail;
    }

    for (;;)
    {
        Session->Pipe = CreateFileW(PHONE_PIPE_NAME,
                                    GENERIC_READ | GENERIC_WRITE,
                                    0,
                                    NULL,
                                    OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                    NULL);
        if (Session->Pipe != INVALID_HANDLE_VALUE)
            break;

        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(PHONE_PIPE_NAME, PHONEOM_CONNECT_TIMEOUT_MS))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            goto Fail;
        }
    }

    if (!SetNamedPipeHandleState(Session->Pipe, &Mode, NULL, NULL))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto Fail;
    }

    if (Callback)
    {
        Session->DispatchThread = CreateThread(NULL, 0, PhOmDispatchThread, Session, 0, NULL);
        if (!Session->DispatchThread)
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            goto Fail;
        }
    }

    Session->ReaderThread = CreateThread(NULL, 0, PhOmReaderThread, Session, 0, NULL);
    if (!Session->ReaderThread)
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto Fail;
    }

    *Phone = Session;
    return S_OK;

Fail:
    PhOmFreeSession(Session);
    return hr;
}

VOID
WINAPI
PhoneOmClose(
    _In_ HPHONEOM Phone)
{
    if (Phone)
        PhOmFreeSession(Phone);
}

HRESULT
WINAPI
PhoneOmGetRadio(
    _In_ HPHONEOM Phone,
    _Out_ PPHONEOM_RADIO Radio)
{
    DWORD Returned;
    HRESULT hr;

    if (!Radio)
        return E_POINTER;

    ZeroMemory(Radio, sizeof(*Radio));
    hr = PhOmTransact(Phone, PhoneRequestGetRadio, NULL, 0, Radio, sizeof(*Radio), &Returned);
    if (SUCCEEDED(hr) && Returned != sizeof(*Radio))
        hr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    return hr;
}

HRESULT
WINAPI
PhoneOmSetRadioPower(
    _In_ HPHONEOM Phone,
    _In_ BOOL On)
{
    return PhOmTransactDword(Phone, PhoneRequestSetRadioPower, On ? 1 : 0);
}

HRESULT
WINAPI
PhoneOmDial(
    _In_ HPHONEOM Phone,
    _In_ LPCWSTR Number,
    _Out_opt_ PDWORD CallId)
{
    PHONE_REQUEST_DIAL Request;
    DWORD Id = 0;
    HRESULT hr;

    if (CallId)
        *CallId = 0;

    if (!Number)
        return E_POINTER;

    ZeroMemory(&Request, sizeof(Request));
    hr = StringCchCopyW(Request.Number, _countof(Request.Number), Number);
    if (FAILED(hr))
        return hr;

    hr = PhOmTransact(Phone, PhoneRequestDial, &Request, sizeof(Request), &Id, sizeof(Id), NULL);
    if (SUCCEEDED(hr) && CallId)
        *CallId = Id;
    return hr;
}

HRESULT
WINAPI
PhoneOmAnswer(
    _In_ HPHONEOM Phone,
    _In_ DWORD CallId)
{
    return PhOmTransactDword(Phone, PhoneRequestAnswer, CallId);
}

HRESULT
WINAPI
PhoneOmHangUp(
    _In_ HPHONEOM Phone,
    _In_ DWORD CallId)
{
    return PhOmTransactDword(Phone, PhoneRequestHangUp, CallId);
}

HRESULT
WINAPI
PhoneOmSwap(
    _In_ HPHONEOM Phone)
{
    return PhOmTransact(Phone, PhoneRequestSwap, NULL, 0, NULL, 0, NULL);
}

HRESULT
WINAPI
PhoneOmSendDtmf(
    _In_ HPHONEOM Phone,
    _In_ WCHAR Digit)
{
    return PhOmTransactDword(Phone, PhoneRequestDtmf, Digit);
}

HRESULT
WINAPI
PhoneOmGetCalls(
    _In_ HPHONEOM Phone,
    _Out_writes_to_(MaxCalls, *Count) PPHONEOM_CALL Calls,
    _In_ DWORD MaxCalls,
    _Out_ PDWORD Count)
{
    DWORD Returned;
    HRESULT hr;

    if (!Count || (!Calls && MaxCalls))
        return E_POINTER;
    *Count = 0;

    hr = PhOmTransact(Phone, PhoneRequestGetCalls, NULL, 0, Calls, MaxCalls * sizeof(*Calls), &Returned);
    if (SUCCEEDED(hr))
        *Count = Returned / sizeof(*Calls);
    return hr;
}

HRESULT
WINAPI
PhoneOmSetAudioRoute(
    _In_ HPHONEOM Phone,
    _In_ DWORD Route)
{
    return PhOmTransactDword(Phone, PhoneRequestSetAudioRoute, Route);
}

HRESULT
WINAPI
PhoneOmSetMute(
    _In_ HPHONEOM Phone,
    _In_ BOOL Mute)
{
    return PhOmTransactDword(Phone, PhoneRequestSetMute, Mute ? 1 : 0);
}

HRESULT
WINAPI
PhoneOmSendMessage(
    _In_ HPHONEOM Phone,
    _In_ LPCWSTR Address,
    _In_ LPCWSTR Text,
    _Out_opt_ PDWORD MessageId)
{
    PHONE_REQUEST_MESSAGE *Request;
    DWORD Id = 0;
    HRESULT hr;

    if (MessageId)
        *MessageId = 0;

    if (!Address || !Text)
        return E_POINTER;

    Request = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Request));
    if (!Request)
        return E_OUTOFMEMORY;

    hr = StringCchCopyW(Request->Address, _countof(Request->Address), Address);
    if (SUCCEEDED(hr))
        hr = StringCchCopyW(Request->Text, _countof(Request->Text), Text);

    if (SUCCEEDED(hr))
        hr = PhOmTransact(Phone, PhoneRequestSendMessage, Request, sizeof(*Request), &Id, sizeof(Id), NULL);

    HeapFree(GetProcessHeap(), 0, Request);

    if (SUCCEEDED(hr) && MessageId)
        *MessageId = Id;
    return hr;
}

HRESULT
WINAPI
PhoneOmGetMessages(
    _In_ HPHONEOM Phone,
    _In_ DWORD FirstId,
    _Out_writes_to_(MaxMessages, *Count) PPHONEOM_MESSAGE Messages,
    _In_ DWORD MaxMessages,
    _Out_ PDWORD Count)
{
    PHONE_REQUEST_RANGE Range;
    DWORD Returned;
    HRESULT hr;

    if (!Count || (!Messages && MaxMessages))
        return E_POINTER;
    *Count = 0;

    Range.FirstId = FirstId;
    Range.MaxCount = MaxMessages;

    hr = PhOmTransact(Phone,
                      PhoneRequestGetMessages,
                      &Range,
                      sizeof(Range),
                      Messages,
                      MaxMessages * sizeof(*Messages),
                      &Returned);
    if (SUCCEEDED(hr))
        *Count = Returned / sizeof(*Messages);
    return hr;
}

HRESULT
WINAPI
PhoneOmMarkMessageRead(
    _In_ HPHONEOM Phone,
    _In_ DWORD MessageId)
{
    return PhOmTransactDword(Phone, PhoneRequestMarkRead, MessageId);
}

HRESULT
WINAPI
PhoneOmDeleteMessage(
    _In_ HPHONEOM Phone,
    _In_ DWORD MessageId)
{
    return PhOmTransactDword(Phone, PhoneRequestDeleteMessage, MessageId);
}
