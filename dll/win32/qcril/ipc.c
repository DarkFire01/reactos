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
#define QC_IOCTL_REGISTER_SERVER    CTL_CODE(QC_FILE_DEVICE_NETWORK, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define QC_IOCTL_PORT_OPEN          CTL_CODE(QC_FILE_DEVICE_NETWORK, 0x807, METHOD_NEITHER, FILE_ANY_ACCESS)
#define QC_IOCTL_PORT_CLOSE         CTL_CODE(QC_FILE_DEVICE_NETWORK, 0x808, METHOD_NEITHER, FILE_ANY_ACCESS)

#define QC_IPC_DEVICE               L"\\\\.\\IPC"

/* Router events read through the event control code */
#define QC_ROUTER_EVENT_RESUME_TX   0
#define QC_ROUTER_EVENT_NEW_SERVER  1
#define QC_ROUTER_EVENT_DEL_SERVER  2

typedef struct _QC_ROUTER_EVENT
{
    ULONG Event;
    ULONG ServiceId;
    ULONG Instance;
    QC_ADDR Addr;
} QC_ROUTER_EVENT;

C_ASSERT(sizeof(QC_ROUTER_EVENT) == 20);

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
    QC_SERVICE_WDS,
    QC_SERVICE_PDC,
    QC_SERVICE_IMSS,
    QC_SERVICE_IMSA
};

/* Interface major version each service registers with, part of its router name */
static const UCHAR QcServiceVersions[QcSvcCount] =
{
    1,
    1,
    1,
    2,
    1,
    1,
    1,
    1,
    1
};

static const PCSTR QcServiceNames[QcSvcCount] =
{
    "DMS",
    "NAS",
    "UIM",
    "VOICE",
    "WMS",
    "WDS",
    "PDC",
    "IMSS",
    "IMSA"
};

/* Unused services already logged, one bit per service id */
static UCHAR QcServicesSeen[64];

PCSTR
QcServiceName(
    _In_ QC_SVC Which)
{
    return (Which < QcSvcCount) ? QcServiceNames[Which] : "?";
}

/* QMI error to the RIL result WMRil reports for it */
HRESULT
QcMapQmiError(
    _In_ USHORT Error)
{
    switch (Error)
    {
        case 0x00: return S_OK;
        case 0x02: return E_OUTOFMEMORY;
        case 0x03:
        case 0x04:
        case 0x17: return RIL_ERROR(0x01);
        case 0x05: return RIL_ERROR(0x3B);
        case 0x06: return RIL_ERROR(0x48);
        case 0x0B: return RIL_ERROR(0x6E);
        case 0x0C: return RIL_ERROR(0x0A);
        case 0x0D:
        case 0x35: return RIL_ERROR(0x13);
        case 0x0E: return RIL_ERROR(0x38);
        case 0x18: return RIL_ERROR(0x36);
        case 0x19:
        case 0x47: return RIL_ERROR(0x05);
        case 0x23:
        case 0x6A: return RIL_ERROR(0x6B);
        case 0x24: return RIL_ERROR(0x6C);
        case 0x30: return E_INVALIDARG;
        case 0x31:
        case 0x32: return RIL_ERROR(0x0C);
        case 0x33: return RIL_ERROR(0x29);
        case 0x34:
        case 0x67: return RIL_ERROR(0x47);
        case 0x36: return RIL_ERROR(0x46);
        case 0x39: return RIL_ERROR(0x3E);
        case 0x3A: return RIL_ERROR(0x2E);
        case 0x49: return RIL_ERROR(0x2F);
        case 0x50:
        case 0x71: return RIL_ERROR(0x0D);
        case 0x5A: return RIL_ERROR(0x04);
        case 0x5B:
        case 0x62: return RIL_ERROR(0x62);
        case 0x5C: return RIL_ERROR(0x45);
        case 0x60: return RIL_ERROR(0x66);
        case 0x66: return RIL_ERROR(0x06);
        case 0x68: return RIL_ERROR(0x6A);
        case 0x6E: return RIL_ERROR(0x0F);
        case 0x6F: return RIL_ERROR(0x11);
        case 0x72: return RIL_ERROR(0x0E);
        default: return E_FAIL;
    }
}

USHORT
QcQmiError(
    _In_reads_bytes_(Length) const VOID *Response,
    _In_ ULONG Length)
{
    QMI_RESULT Result;

    if (!QmiGetResult(Response, Length, &Result) || Result.Result == QMI_RESULT_SUCCESS)
        return QC_QMI_ERR_NONE;

    return Result.Error;
}

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

    QcTrace("IPC router port open after %lu attempt(s)\n", Attempt + 1);
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

/* A router port of its own, for a second client of a service the main port already uses */
BOOL
QcIpcOpenPort(
    _Out_ PHANDLE Port)
{
    DWORD Returned;
    HANDLE Handle;

    *Port = INVALID_HANDLE_VALUE;
    Handle = CreateFileW(QC_IPC_DEVICE,
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED,
                         NULL);
    if (Handle == INVALID_HANDLE_VALUE)
        return FALSE;

    if (!QcIoctl(Handle, QC_IOCTL_PORT_OPEN, NULL, 0, NULL, 0, &Returned))
    {
        CloseHandle(Handle);
        return FALSE;
    }

    *Port = Handle;
    return TRUE;
}

VOID
QcIpcClosePort(
    _In_ HANDLE Port)
{
    DWORD Returned;

    if (Port == INVALID_HANDLE_VALUE)
        return;

    QcIoctl(Port, QC_IOCTL_PORT_CLOSE, NULL, 0, NULL, 0, &Returned);
    CloseHandle(Port);
}

/**
 * @brief Announces a service this port serves, the remote processors can then look it up.
 *
 * @param[in] Instance
 * The IDL major version in the low byte, the instance above it.
 */
BOOL
QcIpcRegisterServer(
    _In_ HANDLE Port,
    _In_ ULONG ServiceId,
    _In_ ULONG Instance)
{
    ULONG Server[2];
    DWORD Returned;

    Server[0] = ServiceId;
    Server[1] = Instance;
    return QcIoctl(Port, QC_IOCTL_REGISTER_SERVER, Server, sizeof(Server), NULL, 0, &Returned);
}

/* One framed message on an extra port, a busy router is retried for a short while */
BOOL
QcIpcSendOn(
    _In_ HANDLE Port,
    _In_ const QC_ADDR *Dest,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    OVERLAPPED Overlapped;
    PUCHAR Frame;
    DWORD Written;
    ULONG Attempt;
    BOOL Ok = FALSE;

    Frame = HeapAlloc(GetProcessHeap(), 0, 8 + Length);
    if (!Frame)
        return FALSE;

    ((PULONG)Frame)[0] = Dest->Node;
    ((PULONG)Frame)[1] = Dest->Port;
    memcpy(Frame + 8, Message, Length);

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Overlapped.hEvent)
    {
        for (Attempt = 0; Attempt < 20; Attempt++)
        {
            ResetEvent(Overlapped.hEvent);
            Ok = WriteFile(Port, Frame, 8 + Length, NULL, &Overlapped);
            Ok = QcOverlapped(Port, Ok, &Overlapped, &Written);
            if (Ok || GetLastError() != ERROR_BUSY)
                break;
            Sleep(50);
        }
        CloseHandle(Overlapped.hEvent);
    }

    HeapFree(GetProcessHeap(), 0, Frame);
    return Ok;
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
    Find.Instance = QC_ROUTER_NAME(QC_INSTANCE_DEFAULT, QcServiceVersions[Which]);
    Find.InstanceMask = MAXULONG;
    Find.Capacity = 1;

    /* The router wants the output sized for exactly the entries asked for */
    ZeroMemory(&Found, sizeof(Found));
    if (!QcIoctl(Modem->Ipc, QC_IOCTL_FIND_SERVERS, &Find, sizeof(Find),
                 &Found, sizeof(Found), &Returned) ||
        Found.Returned == 0)
    {
        QcTrace("%s (service %lu) not registered yet (0x%lx)\n",
                QcServiceNames[Which], QcServiceIds[Which], GetLastError());
        return FALSE;
    }

    Addr = Found.Servers[0].Addr;

    EnterCriticalSection(&Modem->ServiceLock);
    Modem->Services[Which].ServiceId = QcServiceIds[Which];
    Modem->Services[Which].Instance = QC_INSTANCE_DEFAULT;
    Modem->Services[Which].Addr = Addr;
    Modem->Services[Which].Ready = TRUE;
    LeaveCriticalSection(&Modem->ServiceLock);

    QcTrace("%s (service %lu) at %lu:%lu\n", QcServiceNames[Which], QcServiceIds[Which], Addr.Node, Addr.Port);
    return TRUE;
}

/* Waits for the event thread to see the router take writes again */
static
BOOL
QcWaitTxResume(
    _In_ PQC_MODEM Modem)
{
    if (WaitForSingleObject(Modem->TxResume, QC_REQUEST_TIMEOUT_MS) != WAIT_OBJECT_0)
    {
        QcTrace("router never resumed transmit\n");
        return FALSE;
    }

    return !Modem->Stopping;
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
        ResetEvent(Modem->TxResume);
        Ok = WriteFile(Modem->Ipc, Frame, 8 + Length, NULL, &Overlapped);
        Ok = QcOverlapped(Modem->Ipc, Ok, &Overlapped, &Written);
        if (Ok || GetLastError() != ERROR_BUSY)
            break;

        QcTrace("router busy, waiting to resume transmit\n");
        if (!QcWaitTxResume(Modem))
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
BOOL
QcCompletePending(
    _In_ PQC_MODEM Modem,
    _In_ const QC_ADDR *Src,
    _In_ USHORT Transaction,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    PLIST_ENTRY Entry;
    BOOL Found = FALSE;

    EnterCriticalSection(&Modem->PendingLock);
    for (Entry = Modem->Pending.Flink; Entry != &Modem->Pending; Entry = Entry->Flink)
    {
        PQC_PENDING Pending = CONTAINING_RECORD(Entry, QC_PENDING, Link);

        if (Pending->Got ||
            Pending->Aborted ||
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
        Found = TRUE;
        break;
    }
    LeaveCriticalSection(&Modem->PendingLock);

    return Found;
}

/* Fails every request still waiting on Server, or on any server when Server is NULL */
static
VOID
QcAbortPending(
    _In_ PQC_MODEM Modem,
    _In_opt_ const QC_ADDR *Server)
{
    PLIST_ENTRY Entry;

    EnterCriticalSection(&Modem->PendingLock);
    for (Entry = Modem->Pending.Flink; Entry != &Modem->Pending; Entry = Entry->Flink)
    {
        PQC_PENDING Pending = CONTAINING_RECORD(Entry, QC_PENDING, Link);

        if (Pending->Got || Pending->Aborted)
            continue;
        if (Server && (Pending->Server.Node != Server->Node || Pending->Server.Port != Server->Port))
            continue;

        QcTrace("aborting msg 0x%04X txn %u, its server went away\n", Pending->MessageId, Pending->Transaction);
        Pending->Aborted = TRUE;
        SetEvent(Pending->Done);
    }
    LeaveCriticalSection(&Modem->PendingLock);
}

/* The modem went away under us, forget every service and every cached state */
static
VOID
QcModemLost(
    _In_ PQC_MODEM Modem)
{
    DWORD Reset = RIL_MODEMRESET_STARTED;
    QC_SVC i;

    QcTrace("ssr: modem services removed, the modem is restarting\n");
    QcNotifyAll(Modem, RIL_NOTIFY_MODEMRESETSTATE, &Reset, sizeof(Reset));

    EnterCriticalSection(&Modem->ServiceLock);
    for (i = 0; i < QcSvcCount; i++)
    {
        Modem->Services[i].Ready = FALSE;
        Modem->Services[i].Configured = FALSE;
    }
    Modem->Pdc.Done = FALSE;
    LeaveCriticalSection(&Modem->ServiceLock);

    QcAbortPending(Modem, NULL);

    /* A carrier configuration step waiting on its indication will not get one */
    EnterCriticalSection(&Modem->StateLock);
    if (Modem->Pdc.WaitId && Modem->Pdc.Event)
    {
        Modem->Pdc.WaitId = 0;
        SetEvent(Modem->Pdc.Event);
    }
    LeaveCriticalSection(&Modem->StateLock);

    QcDmsModemLost(Modem);
    QcNasModemLost(Modem);
    QcUimModemLost(Modem);
    QcVoiceModemLost(Modem);
    QcWdsModemLost(Modem);
    QcImsModemLost(Modem);
    QcImsDataModemLost();
    QcDcmModemLost();

    InterlockedExchange(&Modem->Restarted, 1);
    QcTrace("ssr: state reset, waiting for the services to return\n");
}

static
VOID
QcServerEvent(
    _In_ PQC_MODEM Modem,
    _In_ const QC_ROUTER_EVENT *Event)
{
    QC_SVC i, Which = QcSvcCount;

    if (Event->Event == QC_ROUTER_EVENT_NEW_SERVER)
    {
        for (i = 0; i < QcSvcCount; i++)
        {
            if (QcServiceIds[i] == Event->ServiceId && QcServiceVersions[i] == (Event->Instance & 0xFF))
                Which = i;
        }

        if (Which == QcSvcCount)
        {
            /* Every other server the modem offers, once each, to see what it can do */
            if (Event->ServiceId < 8 * sizeof(QcServicesSeen) &&
                !(QcServicesSeen[Event->ServiceId / 8] & (1 << (Event->ServiceId % 8))))
            {
                QcServicesSeen[Event->ServiceId / 8] |= (UCHAR)(1 << (Event->ServiceId % 8));
                QcTrace("router: unused service 0x%lx instance 0x%lx up at %lu:%lu\n",
                        Event->ServiceId, Event->Instance, Event->Addr.Node, Event->Addr.Port);
            }
            return;
        }

        EnterCriticalSection(&Modem->ServiceLock);
        Modem->Services[Which].Announced = TRUE;
        LeaveCriticalSection(&Modem->ServiceLock);

        QcTrace("router: %s server up at %lu:%lu\n", QcServiceNames[Which], Event->Addr.Node, Event->Addr.Port);
        QcKickSetup(Modem);
        return;
    }

    EnterCriticalSection(&Modem->ServiceLock);
    for (i = 0; i < QcSvcCount; i++)
    {
        if (Modem->Services[i].Ready &&
            Modem->Services[i].ServiceId == Event->ServiceId &&
            Modem->Services[i].Addr.Node == Event->Addr.Node &&
            Modem->Services[i].Addr.Port == Event->Addr.Port)
        {
            Which = i;
            Modem->Services[i].Ready = FALSE;
            Modem->Services[i].Configured = FALSE;
            Modem->Services[i].Announced = FALSE;
        }
    }
    LeaveCriticalSection(&Modem->ServiceLock);

    if (Which == QcSvcCount)
        return;

    QcTrace("router: %s server at %lu:%lu removed\n", QcServiceNames[Which], Event->Addr.Node, Event->Addr.Port);
    QcAbortPending(Modem, &Event->Addr);

    /* Losing a core service means the modem itself restarted */
    if (QC_SVC_IS_CORE(Which) && InterlockedExchange(&Modem->ModemUp, 0) != 0)
        QcModemLost(Modem);
    else if (Which == QcSvcWds)
        QcWdsModemLost(Modem);

    QcKickSetup(Modem);
}

/* Keeps one event read outstanding on the router port for the life of the modem */
static
DWORD
WINAPI
QcEventThread(
    _In_ LPVOID Parameter)
{
    PQC_MODEM Modem = Parameter;
    QC_ROUTER_EVENT Event;
    OVERLAPPED Overlapped;
    DWORD Returned;
    BOOL Ok;

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Overlapped.hEvent)
        return 0;

    while (!Modem->Stopping)
    {
        ZeroMemory(&Event, sizeof(Event));
        ResetEvent(Overlapped.hEvent);
        Ok = DeviceIoControl(Modem->Ipc, QC_IOCTL_TX_EVENT, NULL, 0, &Event, sizeof(Event), NULL, &Overlapped);
        if (!QcOverlapped(Modem->Ipc, Ok, &Overlapped, &Returned))
        {
            if (!Modem->Stopping)
                QcTrace("router event read failed (%lu), event thread exits\n", GetLastError());
            break;
        }

        if (Modem->Stopping)
            break;

        switch (Event.Event)
        {
            case QC_ROUTER_EVENT_RESUME_TX:
                SetEvent(Modem->TxResume);
                break;

            case QC_ROUTER_EVENT_NEW_SERVER:
            case QC_ROUTER_EVENT_DEL_SERVER:
                QcServerEvent(Modem, &Event);
                break;

            default:
                QcTrace("router event %lu ignored\n", Event.Event);
                break;
        }
    }

    /* Nobody waits for transmit any more */
    SetEvent(Modem->TxResume);
    CloseHandle(Overlapped.hEvent);
    return 0;
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
        {
            QcTrace("malformed message from %lu:%lu, %lu bytes, dropped\n", Src.Node, Src.Port, Length);
            continue;
        }

        Header = (const QMI_HEADER *)(Buffer + 8);

        if (Header->Type == QMI_TYPE_RESPONSE)
        {
            if (!QcCompletePending(Modem, &Src, Header->TransactionId, Buffer + 8, Length))
            {
                QcTrace("late response from %lu:%lu msg 0x%04X txn %u dropped\n",
                        Src.Node, Src.Port, Header->MessageId, Header->TransactionId);
            }
            continue;
        }

        if (Header->Type == QMI_TYPE_INDICATION)
        {
            EnterCriticalSection(&Modem->ServiceLock);
            Which = QcServiceFromAddr(Modem, &Src);
            LeaveCriticalSection(&Modem->ServiceLock);

            if (Which == QcSvcCount)
            {
                QcTrace("ind from unknown port %lu:%lu msg 0x%04X dropped\n", Src.Node, Src.Port, Header->MessageId);
                continue;
            }

            QcTrace("ind %s 0x%04X len %u\n", QcServiceNames[Which], Header->MessageId, Header->Length);
            QcIndication(Modem, Which, Header->MessageId, Buffer + 8, Length);
        }
    }

    if (!Modem->Stopping)
        QcTrace("router read failed (%lu), receive thread exits\n", GetLastError());

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
    return QcQmiRequestWait(Modem, Which, Message, MessageLength, Response, ResponseCap,
                            ResponseLen, QC_REQUEST_TIMEOUT_MS);
}

HRESULT
QcQmiRequestWait(
    _In_ PQC_MODEM Modem,
    _In_ QC_SVC Which,
    _Inout_updates_bytes_(MessageLength) PVOID Message,
    _In_ ULONG MessageLength,
    _Out_writes_bytes_to_(ResponseCap, *ResponseLen) PVOID Response,
    _In_ ULONG ResponseCap,
    _Out_ PULONG ResponseLen,
    _In_ DWORD TimeoutMs)
{
    PQMI_HEADER Header = Message;
    QC_PENDING Pending;
    QC_ADDR Server;
    QMI_RESULT Result;
    HRESULT hr;
    DWORD Wait;

    *ResponseLen = 0;

    if (!QcServiceEnsure(Modem, Which))
    {
        QcTrace("-> %s 0x%04X not sent, service not available\n", QcServiceNames[Which], Header->MessageId);
        return RIL_E_RADIONOTPRESENT;
    }

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

    QcTrace("-> %s 0x%04X txn %u len %u\n", QcServiceNames[Which], Pending.MessageId,
            Pending.Transaction, Header->Length);

    if (!QcSendTo(Modem, &Server, Message, MessageLength))
    {
        QcTrace("-> %s 0x%04X txn %u send failed (%lu)\n", QcServiceNames[Which], Pending.MessageId,
                Pending.Transaction, GetLastError());
        EnterCriticalSection(&Modem->PendingLock);
        RemoveEntryList(&Pending.Link);
        LeaveCriticalSection(&Modem->PendingLock);
        CloseHandle(Pending.Done);
        return RIL_E_RADIONOTPRESENT;
    }

    Wait = WaitForSingleObject(Pending.Done, TimeoutMs);

    EnterCriticalSection(&Modem->PendingLock);
    RemoveEntryList(&Pending.Link);
    LeaveCriticalSection(&Modem->PendingLock);
    CloseHandle(Pending.Done);

    if (Pending.Aborted)
    {
        QcTrace("<- %s 0x%04X txn %u aborted, the modem went away\n", QcServiceNames[Which],
                Pending.MessageId, Pending.Transaction);
        return RIL_E_RADIONOTPRESENT;
    }

    if (Wait != WAIT_OBJECT_0 || !Pending.Got)
    {
        QcTrace("<- %s 0x%04X txn %u timed out after %lu ms\n", QcServiceNames[Which],
                Pending.MessageId, Pending.Transaction, TimeoutMs);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    *ResponseLen = Pending.ResponseLen;

    if (!QmiGetResult(Response, Pending.ResponseLen, &Result))
    {
        QcTrace("<- %s 0x%04X txn %u len %lu, no result record\n", QcServiceNames[Which],
                Pending.MessageId, Pending.Transaction, Pending.ResponseLen);
        return S_OK;
    }

    QcTrace("<- %s 0x%04X txn %u result %u error 0x%x\n", QcServiceNames[Which],
            Pending.MessageId, Pending.Transaction, Result.Result, Result.Error);

    if (Result.Result == QMI_RESULT_SUCCESS)
        return S_OK;

    hr = QcMapQmiError(Result.Error);
    return SUCCEEDED(hr) ? E_FAIL : hr;
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
        case QcSvcDms:
            QcDmsIndication(Modem, MessageId, Message, Length);
            break;

        case QcSvcNas:
            QcNasIndication(Modem, MessageId, Message, Length);
            break;

        case QcSvcUim:
            QcUimIndication(Modem, MessageId, Message, Length);
            break;

        case QcSvcVoice:
            QcVoiceIndication(Modem, MessageId, Message, Length);
            break;

        case QcSvcWms:
            QcWmsIndication(Modem, MessageId, Message, Length);
            break;

        case QcSvcWds:
            QcWdsIndication(Modem, MessageId, Message, Length);
            break;

        case QcSvcPdc:
            QcPdcIndication(Modem, MessageId, Message, Length);
            break;

        case QcSvcImsa:
            QcImsaIndication(Modem, MessageId, Message, Length);
            break;

        default:
            QcTrace("ind %s 0x%04X not handled\n", QcServiceNames[Which], MessageId);
            break;
    }
}

BOOL
QcIpcStartReceive(
    _In_ PQC_MODEM Modem)
{
    Modem->RxThread = CreateThread(NULL, 0, QcReceiveThread, Modem, 0, NULL);
    Modem->EventThread = CreateThread(NULL, 0, QcEventThread, Modem, 0, NULL);
    return Modem->RxThread != NULL && Modem->EventThread != NULL;
}
