/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Router to router requests: the parent side queues and the child side sender
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/** One entry of the router to router request table: code and smallest buffers. */
struct Usb4DrRequestLimits
{
    ULONG IoControlCode;
    ULONG MinInput;
    ULONG MinOutput;
};

/** Looks Code up in the router to router table; NULL when it is not a router to router request. */
const Usb4DrRequestLimits*
NTAPI
Usb4DrFindRequestLimits(
    _In_ ULONG IoControlCode);

/**
 * Parent side. Every DFP gets a queue that its child PDO forwards to; tunnel requests
 * get this router's half filled in and go on to this router's own lower target.
 */
class Usb4DrForwarder
{
public:
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Create(
        _In_ Usb4DrFdo* Fdo);

    /** Parallel, not power managed, passive level internal IOCTL queue whose context names Port. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    CreatePortQueue(
        _In_ Usb4DrPort* Port,
        _Out_ WDFQUEUE* Queue);

    /** Child PDO side: hands a request to the DFP's queue (send and forget); completes it on failure. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    ForwardFromChild(
        _In_ Usb4DrPort* Port,
        _In_ WDFREQUEST Request);

private:
    static EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtPortIoctl;
    static EVT_WDF_REQUEST_COMPLETION_ROUTINE EvtCreateCompleted;
    static EVT_WDF_REQUEST_COMPLETION_ROUTINE EvtDestroyCompleted;

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    OnCreateTunnel(
        _In_ Usb4DrPort* Port,
        _In_ WDFREQUEST Request);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    OnDestroyTunnel(
        _In_ Usb4DrPort* Port,
        _In_ WDFREQUEST Request);

    /** Sends Request unchanged to this router's lower target, send and forget. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    PassUp(
        _In_ WDFREQUEST Request);

    /** Sends Request to this router's lower target; Reservation comes back as the completion context. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    SendWithCompletion(
        _In_ WDFREQUEST Request,
        _In_ PFN_WDF_REQUEST_COMPLETION_ROUTINE Completion,
        _In_ PVOID Reservation);

    /**
     * Sends a formatted Request to this router's lower target, from a work item when the
     * stack runs low. A failure to send from the work item completes Request and gives
     * Reservation back; a failure returned here leaves both to the caller.
     */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    Send(
        _In_ WDFREQUEST Request,
        _In_ BOOLEAN SendAndForget,
        _In_opt_ PVOID Reservation);

    static EVT_WDF_WORKITEM EvtSendWork;

    /** Forwarder of the router whose queue delivered Request. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    static
    Usb4DrForwarder*
    FromRequest(
        _In_ WDFREQUEST Request);

    Usb4DrFdo* m_Fdo;
};

/** Child side: the tunnel requests a protocol adapter of this router sends to its own PDO. */
class Usb4DrParentLink
{
public:
    /** Reusable request on WdfDeviceGetIoTarget and its 472 and 24 byte buffers. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Create(
        _In_ Usb4DrFdo* Fdo);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Destroy();

    /** CREATE_TUNNEL; waits without a timeout as Windows does, Stop cancels it. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    CreateTunnel(
        _In_ const USB4HR_TUNNEL_BUILD_REQUEST* Input,
        _Out_ PUSB4HR_TUNNEL_BUILD_RESULT Output);

    /** DESTROY_TUNNEL; Usb4Status is the host router's answer. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    DestroyTunnel(
        _In_ USB4HR_HANDLE TunnelHandle,
        _Out_ PUSB4HR_STATUS Usb4Status);

    /** REBUILD_TUNNEL with zero NRD rate and lane count. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    RebuildTunnel(
        _In_ USB4HR_HANDLE TunnelHandle,
        _Out_ PUSB4HR_STATUS Usb4Status);

    /** Cancels a request in flight; the waiting caller sees STATUS_CANCELLED. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID Cancel();

private:
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Send(
        _In_ ULONG IoControlCode,
        _In_ ULONG InputLength,
        _In_ ULONG OutputLength);

    Usb4DrFdo* m_Fdo;
    WDFREQUEST m_Request;
    WDFMEMORY m_InputMemory;
    WDFMEMORY m_OutputMemory;
    PVOID m_Input;
    PVOID m_Output;

    /** One request at a time per router. */
    WDFWAITLOCK m_Lock;

    /** Nonzero while m_Request is on its way to the parent; Cancel only acts then. */
    volatile LONG m_InFlight;
};
