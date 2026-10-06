/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Configuration space accessor: one outstanding read or write request at a time
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#define USB4HR_CONFIG_TIMEOUT_MS        32
#define USB4HR_CONFIG_ATTEMPTS          3
#define USB4HR_CONFIG_DRAIN_MS          5000
#define USB4HR_CONFIG_DRAIN_STEP_MS     10

/** Which configuration IOCTL a request completes. */
enum class Usb4HrConfigKind : ULONG
{
    Read,
    ReadEx,
    Write
};

/** Where an access goes. The sequence number is the node's, 0 to 3. */
struct Usb4HrConfigTarget
{
    Usb4HrRoute Route;
    UCHAR Adapter;
    UCHAR Sequence;
    ULONG Space;    /**< USB4HR_SPACE_* */
};

struct Usb4HrConfigRequest;

/** Serializes configuration reads and writes on ring 0. */
class Usb4HrConfigAccessor
{
public:
    /** Timer, DPC and lists; registers for read, write and notification packets. Called from EvtDriverDeviceAdd. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Create(
        _In_ Usb4HrHostRouter* HostRouter);

    /** Starts executing queued requests. After the rings started. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID Resume();

    /** Stops issuing, waits up to USB4HR_CONFIG_DRAIN_MS for the outstanding request. Before the rings stop. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Pause();

    /**
     * Queues a configuration IOCTL. The request is completed by the accessor,
     * with STATUS_SUCCESS and the USB4 status in the output buffer once it ran;
     * it is cancelable while queued.
     */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    SubmitIoctl(
        _In_ WDFREQUEST Request,
        _In_ Usb4HrConfigKind Kind,
        _In_ const Usb4HrConfigTarget* Target,
        _In_ const USB4HR_CONFIG_INPUT* Input);

    /** Synchronous read; Status is the USB4 status when the return value is a success. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Read(
        _In_ const Usb4HrConfigTarget* Target,
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _Out_writes_(DwordCount) PULONG Buffer,
        _Out_ PUSB4HR_STATUS Status);

    /** Synchronous write; Status is the USB4 status when the return value is a success. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Write(
        _In_ const Usb4HrConfigTarget* Target,
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _In_reads_(DwordCount) const ULONG* Buffer,
        _Out_ PUSB4HR_STATUS Status);

private:
    static USB4HR_RX_HANDLER OnPacket;
    static EVT_WDF_TIMER OnResponseTimeout;
    static EVT_WDF_DPC OnCompletionDpc;
    static EVT_WDF_REQUEST_CANCEL OnCancel;

    /** Queues a request, or fails it right away when the accessor is paused. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    BOOLEAN
    Enqueue(
        _Inout_ struct Usb4HrConfigRequest* Entry);

    /** Sends queued requests until one is on the wire. Lock held. */
    _IRQL_requires_(DISPATCH_LEVEL)
    VOID StartNext();

    /** Puts the request on the wire and arms the response timer. Lock held. */
    _IRQL_requires_(DISPATCH_LEVEL)
    NTSTATUS
    Transmit(
        _Inout_ struct Usb4HrConfigRequest* Entry);

    /** Hands a request to the completion DPC with its USB4 status. Lock held. */
    _IRQL_requires_(DISPATCH_LEVEL)
    VOID
    Finish(
        _Inout_ struct Usb4HrConfigRequest* Entry,
        _In_ USB4HR_STATUS Status);

    /** Removes the cancel routine of a queued IOCTL; FALSE when cancellation won. Lock held. */
    _IRQL_requires_(DISPATCH_LEVEL)
    BOOLEAN
    ClaimFromCancel(
        _Inout_ struct Usb4HrConfigRequest* Entry);

    _IRQL_requires_(DISPATCH_LEVEL)
    BOOLEAN
    MatchResponse(
        _In_ const struct Usb4HrConfigRequest* Entry,
        _In_ const Usb4HrRxPacket* Packet);

    _IRQL_requires_(DISPATCH_LEVEL)
    BOOLEAN
    ShouldFailOnNotification(
        _In_ const struct Usb4HrConfigRequest* Entry,
        _In_ const Usb4HrRxPacket* Packet);

    _IRQL_requires_(DISPATCH_LEVEL)
    VOID
    ProcessPacket(
        _In_ const Usb4HrRxPacket* Packet);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID ProcessTimeout();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID CompleteFinished();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    CancelIoctl(
        _In_ WDFREQUEST Request);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    RunSynchronous(
        _Inout_ struct Usb4HrConfigRequest* Entry,
        _Out_ PUSB4HR_STATUS Status);

    Usb4HrHostRouter* m_HostRouter;
    KSPIN_LOCK m_Lock;
    LIST_ENTRY m_Pending;       /**< waiting for the wire */
    LIST_ENTRY m_Canceling;    /**< cancel routine owns the completion */
    LIST_ENTRY m_Finished;      /**< waiting for the completion DPC */
    struct Usb4HrConfigRequest* m_Current;
    BOOLEAN m_Running;
    ULONGLONG m_Deadline;       /**< interrupt time the current attempt expires */
    volatile LONG m_Owned;      /**< requests not completed back to their owner */
    WDFTIMER m_Timer;
    WDFDPC m_Dpc;
};
