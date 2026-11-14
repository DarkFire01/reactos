/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     QMI transport over the Qualcomm IPC router (\Device\IPC)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcril.h"
#include <winioctl.h>
#include <string.h>

/* The Qualcomm IPC router control interface, FILE_DEVICE_NETWORK function codes 0x800.. */
#define QC_FILE_DEVICE_NETWORK      0x00000012
#define QC_IOCTL_TX_EVENT           CTL_CODE(QC_FILE_DEVICE_NETWORK, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define QC_IOCTL_FIND_SERVERS       CTL_CODE(QC_FILE_DEVICE_NETWORK, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define QC_IOCTL_PORT_OPEN          CTL_CODE(QC_FILE_DEVICE_NETWORK, 0x807, METHOD_NEITHER, FILE_ANY_ACCESS)
#define QC_IOCTL_PORT_CLOSE         CTL_CODE(QC_FILE_DEVICE_NETWORK, 0x808, METHOD_NEITHER, FILE_ANY_ACCESS)

#define QC_IPC_DEVICE               L"\\\\.\\IPC"

/* A send refused with ERROR_BUSY may go again once the router reports this event */
#define QC_TX_EVENT_RESUME          0
#define QC_TX_EVENT_SIZE            20

typedef struct _QC_FIND_SERVERS
{
    ULONG ServiceId;
    ULONG Instance;
    ULONG InstanceMask;
    ULONG Capacity;
} QC_FIND_SERVERS;

typedef struct _QC_SERVER_ENTRY
{
    ULONG ServiceId;
    ULONG Instance;
    QC_ADDR Addr;
} QC_SERVER_ENTRY;

typedef struct _QC_FOUND_SERVERS
{
    ULONG Total;
    ULONG Returned;
    QC_SERVER_ENTRY Servers[1];
} QC_FOUND_SERVERS;

/* The wire service ids, indexed by QC_SVC */
static const ULONG QcServiceIds[QcSvcCount] =
{
    QC_SERVICE_DMS,
    QC_SERVICE_NAS,
    QC_SERVICE_UIM,
    QC_SERVICE_VOICE,
    QC_SERVICE_WMS,
    QC_SERVICE_WDS
};

static
BOOL
QcOverlapped(
    _In_ HANDLE Handle,
    _In_ BOOL Ok,
    _In_ LPOVERLAPPED Overlapped,
    _Out_ PDWORD Transferred)
{
    *Transferred = 0;

    if (!Ok && GetLastError() != ERROR_IO_PENDING)
        return FALSE;

    return GetOverlappedResult(Handle, Overlapped, Transferred, TRUE);
}

static
BOOL
QcIoctl(
    _In_ HANDLE Handle,
    _In_ DWORD Code,
    _In_opt_ PVOID In,
    _In_ DWORD InSize,
    _Out_opt_ PVOID Out,
    _In_ DWORD OutSize,
    _Out_ PDWORD Returned)
{
    OVERLAPPED Overlapped;
    BOOL Ok;

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Overlapped.hEvent)
        return FALSE;

    Ok = DeviceIoControl(Handle, Code, In, InSize, Out, OutSize, NULL, &Overlapped);
    Ok = QcOverlapped(Handle, Ok, &Overlapped, Returned);
    CloseHandle(Overlapped.hEvent);
    return Ok;
}

BOOL
QcIpcOpen(
    _In_ PQC_MODEM Modem)
{
    ULONG Attempt;
    DWORD Returned;

    for (Attempt = 0; Attempt < QC_RESOLVE_RETRY; Attempt++)
    {
        Modem->Ipc = CreateFileW(QC_IPC_DEVICE,
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL,
                                 OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED,
                                 NULL);
        if (Modem->Ipc != INVALID_HANDLE_VALUE)
            break;

        Sleep(QC_RESOLVE_DELAY_MS);
    }

    if (Modem->Ipc == INVALID_HANDLE_VALUE)
    {
        QcTrace("cannot open the IPC router (%lu)\n", GetLastError());
        return FALSE;
    }

    /* The handle has no router port until it asks for one */
    if (!QcIoctl(Modem->Ipc, QC_IOCTL_PORT_OPEN, NULL, 0, NULL, 0, &Returned))
    {
        QcTrace("IPC router refused a port (%lu)\n", GetLastError());
        CloseHandle(Modem->Ipc);
        Modem->Ipc = INVALID_HANDLE_VALUE;
        return FALSE;
    }

    QcTrace("IPC router port open\n");
    return TRUE;
}

VOID
QcIpcClose(
    _In_ PQC_MODEM Modem)
{
    DWORD Returned;

    if (Modem->Ipc != INVALID_HANDLE_VALUE)
    {
        QcIoctl(Modem->Ipc, QC_IOCTL_PORT_CLOSE, NULL, 0, NULL, 0, &Returned);
        CloseHandle(Modem->Ipc);
        Modem->Ipc = INVALID_HANDLE_VALUE;
    }
}

BOOL
QcServiceEnsure(
    _In_ PQC_MODEM Modem,
    _In_ QC_SVC Which)
{
    QC_FIND_SERVERS Find;
    QC_FOUND_SERVERS Found;
    QC_ADDR Addr;
    DWORD Returned;
    BOOL Ready;

    EnterCriticalSection(&Modem->ServiceLock);
    Ready = Modem->Services[Which].Ready;
    LeaveCriticalSection(&Modem->ServiceLock);
    if (Ready)
        return TRUE;

    Find.ServiceId = QcServiceIds[Which];
    Find.Instance = QC_ROUTER_NAME(QC_INSTANCE_DEFAULT, QC_VERSION_DEFAULT);
    Find.InstanceMask = MAXULONG;
    Find.Capacity = 1;

    /* The router wants the output sized for exactly the entries asked for */
    ZeroMemory(&Found, sizeof(Found));
    if (!QcIoctl(Modem->Ipc, QC_IOCTL_FIND_SERVERS, &Find, sizeof(Find),
                 &Found, sizeof(Found), &Returned) ||
        Found.Returned == 0)
    {
        QcTrace("service %lu not registered yet (0x%lx)\n", QcServiceIds[Which], GetLastError());
        return FALSE;
    }

    Addr = Found.Servers[0].Addr;

    EnterCriticalSection(&Modem->ServiceLock);
    Modem->Services[Which].ServiceId = QcServiceIds[Which];
    Modem->Services[Which].Instance = QC_INSTANCE_DEFAULT;
    Modem->Services[Which].Addr = Addr;
    Modem->Services[Which].Ready = TRUE;
    LeaveCriticalSection(&Modem->ServiceLock);

    QcTrace("service %lu at %lu:%lu\n", QcServiceIds[Which], Addr.Node, Addr.Port);
    return TRUE;
}

/* Waits for the router to take writes again after one was refused as busy */
static
BOOL
QcWaitTxResume(
    _In_ PQC_MODEM Modem)
{
    ULONG Event[QC_TX_EVENT_SIZE / sizeof(ULONG)];
    DWORD Returned;

    for (;;)
    {
        ZeroMemory(Event, sizeof(Event));
        if (!QcIoctl(Modem->Ipc, QC_IOCTL_TX_EVENT, NULL, 0, Event, sizeof(Event), &Returned))
            return FALSE;

        if (Event[0] == QC_TX_EVENT_RESUME)
            return TRUE;
    }
}

/* Sends one framed message to a destination port */
static
BOOL
QcSendTo(
    _In_ PQC_MODEM Modem,
    _In_ const QC_ADDR *Dest,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    OVERLAPPED Overlapped;
    PUCHAR Frame;
    DWORD Written;
    BOOL Ok;

    Frame = HeapAlloc(GetProcessHeap(), 0, 8 + Length);
    if (!Frame)
        return FALSE;

    ((PULONG)Frame)[0] = Dest->Node;
    ((PULONG)Frame)[1] = Dest->Port;
    memcpy(Frame + 8, Message, Length);

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Overlapped.hEvent)
    {
        HeapFree(GetProcessHeap(), 0, Frame);
        return FALSE;
    }

    EnterCriticalSection(&Modem->SendLock);
    for (;;)
    {
        ResetEvent(Overlapped.hEvent);
        Ok = WriteFile(Modem->Ipc, Frame, 8 + Length, NULL, &Overlapped);
        Ok = QcOverlapped(Modem->Ipc, Ok, &Overlapped, &Written);
        if (Ok || GetLastError() != ERROR_BUSY || !QcWaitTxResume(Modem))
            break;
    }
    LeaveCriticalSection(&Modem->SendLock);

    CloseHandle(Overlapped.hEvent);
    HeapFree(GetProcessHeap(), 0, Frame);
    return Ok;
}

static
QC_SVC
QcServiceFromAddr(
    _In_ PQC_MODEM Modem,
    _In_ const QC_ADDR *Addr)
{
    QC_SVC i;

    for (i = 0; i < QcSvcCount; i++)
    {
        if (Modem->Services[i].Ready &&
            Modem->Services[i].Addr.Node == Addr->Node &&
            Modem->Services[i].Addr.Port == Addr->Port)
        {
            return i;
        }
    }

    return QcSvcCount;
}

/* Hands a response to the waiting request that matches server and transaction */
static
VOID
QcCompletePending(
    _In_ PQC_MODEM Modem,
    _In_ const QC_ADDR *Src,
    _In_ USHORT Transaction,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    PLIST_ENTRY Entry;

    EnterCriticalSection(&Modem->PendingLock);
    for (Entry = Modem->Pending.Flink; Entry != &Modem->Pending; Entry = Entry->Flink)
    {
        PQC_PENDING Pending = CONTAINING_RECORD(Entry, QC_PENDING, Link);

        if (Pending->Got ||
            Pending->Transaction != Transaction ||
            Pending->Server.Node != Src->Node ||
            Pending->Server.Port != Src->Port)
        {
            continue;
        }

        Pending->ResponseLen = min(Length, Pending->ResponseCap);
        if (Pending->ResponseLen)
            memcpy(Pending->Response, Message, Pending->ResponseLen);
        Pending->Got = TRUE;
        SetEvent(Pending->Done);
        break;
    }
    LeaveCriticalSection(&Modem->PendingLock);
}

static
DWORD
WINAPI
QcReceiveThread(
    _In_ LPVOID Parameter)
{
    PQC_MODEM Modem = Parameter;
    OVERLAPPED Overlapped;
    HANDLE Event;
    PUCHAR Buffer;
    QC_ADDR Src;
    const QMI_HEADER *Header;
    DWORD Read;
    ULONG Length;
    QC_SVC Which;

    Buffer = HeapAlloc(GetProcessHeap(), 0, 8 + QC_MESSAGE_MAX);
    Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Buffer || !Event)
        goto Exit;

    while (!Modem->Stopping)
    {
        BOOL Ok;

        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Overlapped.hEvent = Event;

        Ok = ReadFile(Modem->Ipc, Buffer, 8 + QC_MESSAGE_MAX, NULL, &Overlapped);
        if (!Ok && GetLastError() != ERROR_IO_PENDING)
            break;

        if (!GetOverlappedResult(Modem->Ipc, &Overlapped, &Read, TRUE))
            break;

        if (Modem->Stopping || Read < 8 + sizeof(QMI_HEADER))
            continue;

        Src.Node = ((PULONG)Buffer)[0];
        Src.Port = ((PULONG)Buffer)[1];
        Length = Read - 8;

        if (!QmiMessageValid(Buffer + 8, Length))
            continue;

        Header = (const QMI_HEADER *)(Buffer + 8);

        if (Header->Type == QMI_TYPE_RESPONSE)
        {
            QcCompletePending(Modem, &Src, Header->TransactionId, Buffer + 8, Length);
            continue;
        }

        if (Header->Type == QMI_TYPE_INDICATION)
        {
            EnterCriticalSection(&Modem->ServiceLock);
            Which = QcServiceFromAddr(Modem, &Src);
            LeaveCriticalSection(&Modem->ServiceLock);

            if (Which != QcSvcCount)
                QcIndication(Modem, Which, Header->MessageId, Buffer + 8, Length);
        }
    }

Exit:
    if (Event)
        CloseHandle(Event);
    if (Buffer)
        HeapFree(GetProcessHeap(), 0, Buffer);
    return 0;
}

HRESULT
QcQmiRequest(
    _In_ PQC_MODEM Modem,
    _In_ QC_SVC Which,
    _Inout_updates_bytes_(MessageLength) PVOID Message,
    _In_ ULONG MessageLength,
    _Out_writes_bytes_to_(ResponseCap, *ResponseLen) PVOID Response,
    _In_ ULONG ResponseCap,
    _Out_ PULONG ResponseLen)
{
    PQMI_HEADER Header = Message;
    QC_PENDING Pending;
    QC_ADDR Server;
    QMI_RESULT Result;
    DWORD Wait;

    *ResponseLen = 0;

    if (!QcServiceEnsure(Modem, Which))
        return RIL_E_RADIONOTAVAILABLE;

    EnterCriticalSection(&Modem->ServiceLock);
    Server = Modem->Services[Which].Addr;
    LeaveCriticalSection(&Modem->ServiceLock);

    ZeroMemory(&Pending, sizeof(Pending));
    Pending.Server = Server;
    Pending.MessageId = Header->MessageId;
    Pending.Response = Response;
    Pending.ResponseCap = ResponseCap;
    Pending.Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Pending.Done)
        return E_OUTOFMEMORY;

    EnterCriticalSection(&Modem->PendingLock);
    Pending.Transaction = ++Modem->NextTransaction;
    if (Pending.Transaction == 0)
        Pending.Transaction = ++Modem->NextTransaction;
    Header->Type = QMI_TYPE_REQUEST;
    Header->TransactionId = Pending.Transaction;
    InsertTailList(&Modem->Pending, &Pending.Link);
    LeaveCriticalSection(&Modem->PendingLock);

    if (!QcSendTo(Modem, &Server, Message, MessageLength))
    {
        EnterCriticalSection(&Modem->PendingLock);
        RemoveEntryList(&Pending.Link);
        LeaveCriticalSection(&Modem->PendingLock);
        CloseHandle(Pending.Done);
        return RIL_E_RADIONOTAVAILABLE;
    }

    Wait = WaitForSingleObject(Pending.Done, QC_REQUEST_TIMEOUT_MS);

    EnterCriticalSection(&Modem->PendingLock);
    RemoveEntryList(&Pending.Link);
    LeaveCriticalSection(&Modem->PendingLock);
    CloseHandle(Pending.Done);

    if (Wait != WAIT_OBJECT_0 || !Pending.Got)
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);

    *ResponseLen = Pending.ResponseLen;

    if (QmiGetResult(Response, Pending.ResponseLen, &Result) && Result.Result != QMI_RESULT_SUCCESS)
        return RIL_ERROR(0x80 | (Result.Error & 0x7F));

    return S_OK;
}

HRESULT
QcQmiCommand(
    _In_ PQC_MODEM Modem,
    _In_ QC_SVC Which,
    _Inout_updates_bytes_(MessageLength) PVOID Message,
    _In_ ULONG MessageLength)
{
    BYTE Response[QC_MESSAGE_MAX];
    ULONG ResponseLen;

    return QcQmiRequest(Modem, Which, Message, MessageLength, Response, sizeof(Response), &ResponseLen);
}

VOID
QcIndication(
    _In_ PQC_MODEM Modem,
    _In_ QC_SVC Which,
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    switch (Which)
    {
        case QcSvcVoice:
            QcVoiceIndication(Modem, MessageId, Message, Length);
            break;

        case QcSvcWms:
            QcWmsIndication(Modem, MessageId, Message, Length);
            break;

        default:
            break;
    }
}

BOOL
QcIpcStartReceive(
    _In_ PQC_MODEM Modem)
{
    Modem->RxThread = CreateThread(NULL, 0, QcReceiveThread, Modem, 0, NULL);
    return Modem->RxThread != NULL;
}
