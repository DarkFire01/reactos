/*
 * PROJECT:     ReactOS GNSS Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Own port on the Qualcomm IPC router (\Device\IPC)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "gnsssvc.h"
#include <string.h>

#define NDEBUG
#include <debug.h>

/* Router control codes, FILE_DEVICE_NETWORK functions 0x800.. */
#define GS_IOCTL_READ_EVENT         CTL_CODE(FILE_DEVICE_NETWORK, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GS_IOCTL_FIND_SERVERS       CTL_CODE(FILE_DEVICE_NETWORK, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define GS_IOCTL_PORT_OPEN          CTL_CODE(FILE_DEVICE_NETWORK, 0x807, METHOD_NEITHER, FILE_ANY_ACCESS)
#define GS_IOCTL_PORT_CLOSE         CTL_CODE(FILE_DEVICE_NETWORK, 0x808, METHOD_NEITHER, FILE_ANY_ACCESS)

#define GS_ROUTER_DEVICE            L"\\\\.\\IPC"

#define GS_EVENT_RESUME_TX          0
#define GS_EVENT_SERVER_UP          1
#define GS_EVENT_SERVER_DOWN        2

#define GS_BUSY_RETRIES             40
#define GS_BUSY_WAIT_MS             50

typedef struct _GS_ROUTER_EVENT
{
    ULONG Event;
    ULONG ServiceId;
    ULONG Instance;
    GS_ADDR Addr;
} GS_ROUTER_EVENT;

C_ASSERT(sizeof(GS_ROUTER_EVENT) == 20);

typedef struct _GS_FIND_REQUEST
{
    ULONG ServiceId;
    ULONG Instance;
    ULONG InstanceMask;
    ULONG Capacity;
} GS_FIND_REQUEST;

typedef struct _GS_FIND_ENTRY
{
    ULONG ServiceId;
    ULONG Instance;
    GS_ADDR Addr;
} GS_FIND_ENTRY;

typedef struct _GS_FIND_REPLY
{
    ULONG Total;
    ULONG Returned;
    GS_FIND_ENTRY Entry;
} GS_FIND_REPLY;

static HANDLE GsPort = INVALID_HANDLE_VALUE;
static HANDLE GsRxThread;
static HANDLE GsEventThread;
static HANDLE GsTxResume;
static HANDLE GsQuit;
static CRITICAL_SECTION GsSendLock;

/* Waits for one call on the port, giving up once the router threads are told to quit */
static
BOOL
GsWaitIo(
    _In_ BOOL Issued,
    _In_ LPOVERLAPPED Overlapped,
    _Out_ PDWORD Transferred)
{
    HANDLE Waits[2];

    *Transferred = 0;

    if (!Issued && GetLastError() != ERROR_IO_PENDING)
        return FALSE;

    Waits[0] = GsQuit;
    Waits[1] = Overlapped->hEvent;
    if (WaitForMultipleObjects(_countof(Waits), Waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
    {
        CancelIo(GsPort);
        GetOverlappedResult(GsPort, Overlapped, Transferred, TRUE);
        SetLastError(ERROR_OPERATION_ABORTED);
        return FALSE;
    }

    return GetOverlappedResult(GsPort, Overlapped, Transferred, FALSE);
}

static
BOOL
GsIoctl(
    _In_ DWORD Code,
    _In_reads_bytes_opt_(InSize) PVOID In,
    _In_ DWORD InSize,
    _Out_writes_bytes_opt_(OutSize) PVOID Out,
    _In_ DWORD OutSize)
{
    OVERLAPPED Overlapped;
    DWORD Returned;
    BOOL Ok;

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Overlapped.hEvent)
        return FALSE;

    Ok = DeviceIoControl(GsPort, Code, In, InSize, Out, OutSize, NULL, &Overlapped);
    Ok = GsWaitIo(Ok, &Overlapped, &Returned);
    CloseHandle(Overlapped.hEvent);
    return Ok;
}

BOOL
GsRouterOpen(VOID)
{
    /* Opened again after a failed try, the lock and event stay for the process */
    if (!GsTxResume)
    {
        GsTxResume = CreateEventW(NULL, FALSE, FALSE, NULL);
        GsQuit = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!GsTxResume || !GsQuit)
            return FALSE;
        InitializeCriticalSection(&GsSendLock);
    }
    ResetEvent(GsQuit);

    GsPort = CreateFileW(GS_ROUTER_DEVICE,
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                         NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED,
                         NULL);
    if (GsPort == INVALID_HANDLE_VALUE)
        return FALSE;

    /* A fresh handle has no port on the router until it asks for one */
    if (!GsIoctl(GS_IOCTL_PORT_OPEN, NULL, 0, NULL, 0))
    {
        DPRINT1("GNSS: IPC router refused a port (%lu)\n", GetLastError());
        CloseHandle(GsPort);
        GsPort = INVALID_HANDLE_VALUE;
        return FALSE;
    }

    return TRUE;
}

VOID
GsRouterClose(VOID)
{
    OVERLAPPED Overlapped;
    DWORD Returned;
    BOOL Ok;

    if (GsPort != INVALID_HANDLE_VALUE)
    {
        /* The quit event is set by now, so this one call waits on its own */
        ZeroMemory(&Overlapped, sizeof(Overlapped));
        Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (Overlapped.hEvent)
        {
            Ok = DeviceIoControl(GsPort, GS_IOCTL_PORT_CLOSE, NULL, 0, NULL, 0, NULL, &Overlapped);
            if (Ok || GetLastError() == ERROR_IO_PENDING)
                GetOverlappedResult(GsPort, &Overlapped, &Returned, TRUE);
            CloseHandle(Overlapped.hEvent);
        }

        CloseHandle(GsPort);
        GsPort = INVALID_HANDLE_VALUE;
    }
}

_Success_(return != FALSE)
BOOL
GsRouterFindServer(
    _In_ ULONG ServiceId,
    _In_ ULONG Instance,
    _Out_ PGS_ADDR Addr)
{
    GS_FIND_REQUEST Find;
    GS_FIND_REPLY Found;

    Find.ServiceId = ServiceId;
    Find.Instance = Instance;
    Find.InstanceMask = MAXULONG;
    Find.Capacity = 1;

    /* The router sizes the reply by Capacity, one entry here */
    ZeroMemory(&Found, sizeof(Found));
    if (!GsIoctl(GS_IOCTL_FIND_SERVERS, &Find, sizeof(Find), &Found, sizeof(Found)) || Found.Returned == 0)
        return FALSE;

    *Addr = Found.Entry.Addr;
    return TRUE;
}

BOOL
GsRouterSend(
    _In_ const GS_ADDR *Dest,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    OVERLAPPED Overlapped;
    PUCHAR Frame;
    DWORD Written;
    ULONG Try;
    BOOL Ok = FALSE;

    Frame = HeapAlloc(GetProcessHeap(), 0, sizeof(*Dest) + Length);
    if (!Frame)
        return FALSE;

    /* Every write starts with the destination address */
    memcpy(Frame, Dest, sizeof(*Dest));
    memcpy(Frame + sizeof(*Dest), Message, Length);

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (Overlapped.hEvent)
    {
        EnterCriticalSection(&GsSendLock);
        for (Try = 0; Try < GS_BUSY_RETRIES; Try++)
        {
            ResetEvent(Overlapped.hEvent);
            Ok = WriteFile(GsPort, Frame, sizeof(*Dest) + Length, NULL, &Overlapped);
            Ok = GsWaitIo(Ok, &Overlapped, &Written);
            if (Ok || GetLastError() != ERROR_BUSY)
                break;

            /* The router tells us when it takes writes again, poll in case that is missed */
            if (WaitForSingleObject(GsTxResume, GS_BUSY_WAIT_MS) == WAIT_FAILED)
                break;
        }
        LeaveCriticalSection(&GsSendLock);
        CloseHandle(Overlapped.hEvent);
    }

    HeapFree(GetProcessHeap(), 0, Frame);
    return Ok;
}

static
DWORD
WINAPI
GsRxThreadProc(
    _In_ LPVOID Parameter)
{
    OVERLAPPED Overlapped;
    GS_ADDR Source;
    PUCHAR Buffer;
    DWORD Read;
    BOOL Ok;

    UNREFERENCED_PARAMETER(Parameter);

    Buffer = HeapAlloc(GetProcessHeap(), 0, sizeof(Source) + GS_MESSAGE_MAX);
    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Buffer || !Overlapped.hEvent)
        goto Exit;

    for (;;)
    {
        ResetEvent(Overlapped.hEvent);
        Ok = ReadFile(GsPort, Buffer, sizeof(Source) + GS_MESSAGE_MAX, NULL, &Overlapped);
        if (!GsWaitIo(Ok, &Overlapped, &Read))
        {
            if (WaitForSingleObject(GsQuit, 0) != WAIT_OBJECT_0)
                DPRINT1("GNSS: router read failed (%lu), no more messages\n", GetLastError());
            break;
        }

        if (Read < sizeof(Source) + sizeof(QMI_HEADER))
            continue;

        memcpy(&Source, Buffer, sizeof(Source));
        GsLocMessage(&Source, Buffer + sizeof(Source), Read - sizeof(Source));
    }

Exit:
    if (Overlapped.hEvent)
        CloseHandle(Overlapped.hEvent);
    if (Buffer)
        HeapFree(GetProcessHeap(), 0, Buffer);
    return 0;
}

static
DWORD
WINAPI
GsEventThreadProc(
    _In_ LPVOID Parameter)
{
    OVERLAPPED Overlapped;
    GS_ROUTER_EVENT Event;
    DWORD Returned;
    BOOL Ok;

    UNREFERENCED_PARAMETER(Parameter);

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Overlapped.hEvent)
        return 0;

    for (;;)
    {
        ZeroMemory(&Event, sizeof(Event));
        ResetEvent(Overlapped.hEvent);
        Ok = DeviceIoControl(GsPort, GS_IOCTL_READ_EVENT, NULL, 0, &Event, sizeof(Event), NULL, &Overlapped);
        if (!GsWaitIo(Ok, &Overlapped, &Returned))
        {
            if (WaitForSingleObject(GsQuit, 0) != WAIT_OBJECT_0)
                DPRINT1("GNSS: router event read failed (%lu)\n", GetLastError());
            break;
        }

        switch (Event.Event)
        {
            case GS_EVENT_RESUME_TX:
                SetEvent(GsTxResume);
                break;

            case GS_EVENT_SERVER_UP:
            case GS_EVENT_SERVER_DOWN:
                GsLocServerEvent(Event.Event == GS_EVENT_SERVER_UP, Event.ServiceId, Event.Instance, &Event.Addr);
                break;

            default:
                break;
        }
    }

    CloseHandle(Overlapped.hEvent);
    return 0;
}

BOOL
GsRouterStartThreads(VOID)
{
    GsRxThread = CreateThread(NULL, 0, GsRxThreadProc, NULL, 0, NULL);
    GsEventThread = CreateThread(NULL, 0, GsEventThreadProc, NULL, 0, NULL);
    return GsRxThread != NULL && GsEventThread != NULL;
}

VOID
GsRouterStopThreads(VOID)
{
    if (GsQuit)
        SetEvent(GsQuit);

    if (GsRxThread)
    {
        WaitForSingleObject(GsRxThread, 5000);
        CloseHandle(GsRxThread);
        GsRxThread = NULL;
    }

    if (GsEventThread)
    {
        WaitForSingleObject(GsEventThread, 5000);
        CloseHandle(GsEventThread);
        GsEventThread = NULL;
    }
}
