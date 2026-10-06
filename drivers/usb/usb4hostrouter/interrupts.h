/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     MSI and line interrupts, vector allocation, throttling and cause dispatch
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/** Called from the interrupt DPC at DISPATCH_LEVEL for a cause that fired; the cause is re-enabled after it returns. */
typedef VOID
(NTAPI USB4HR_INTERRUPT_HANDLER)(
    _In_ PVOID Context);
typedef USB4HR_INTERRUPT_HANDLER *PUSB4HR_INTERRUPT_HANDLER;

/** Interrupt causes, messages and their handlers. */
class Usb4HrInterrupts
{
public:
    /** Software state only. Called from EvtDriverDeviceAdd. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Create(
        _In_ Usb4HrHostRouter* HostRouter);

    /** Creates one WDFINTERRUPT per MSI message (up to 16) or a single line interrupt. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Prepare(
        _In_ WDFCMRESLIST Raw,
        _In_ WDFCMRESLIST Translated);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Release();

    /** D0 entry: clears all status, programs throttling and the vector allocation of connected causes. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS ConfigHostInterface();

    /** D0 exit: masks and clears every cause. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS DeconfigHostInterface();

    /** Cause number of transmit ring Ring. */
    ULONG
    TxCause(
        _In_ ULONG Ring) const;

    /** Cause number of receive ring Ring. */
    ULONG
    RxCause(
        _In_ ULONG Ring) const;

    /** Records the handler of a cause and binds it to a message. The cause stays masked. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    Connect(
        _In_ ULONG Cause,
        _In_ PUSB4HR_INTERRUPT_HANDLER Handler,
        _In_ PVOID Context);

    /** Masks the cause and forgets its handler. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    Disconnect(
        _In_ ULONG Cause);

    /** Unmasks a connected cause. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    EnableCause(
        _In_ ULONG Cause);

    /** Masks a cause. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    DisableCause(
        _In_ ULONG Cause);

    BOOLEAN IsMessageSignaled() const;

private:
    /* One bit of the interrupt status register per cause */
    static const ULONG CauseCount = 64;
    static const UCHAR NoMessage = 0xFF;
    static const ULONG MessageCount = USB4HR_INTERRUPT_THROTTLING_COUNT;

    struct CauseSlot
    {
        KSPIN_LOCK HandlerLock;     /**< held while the handler runs */
        PUSB4HR_INTERRUPT_HANDLER Handler;
        PVOID Context;
        UCHAR Message;
        BOOLEAN Enabled;
    };

    static EVT_WDF_INTERRUPT_ISR EvtMessageIsr;
    static EVT_WDF_INTERRUPT_DPC EvtMessageDpc;
    static EVT_WDF_INTERRUPT_ISR EvtLineIsr;
    static EVT_WDF_INTERRUPT_DPC EvtLineDpc;

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    CreateInterrupt(
        _In_ WDFCMRESLIST Raw,
        _In_ WDFCMRESLIST Translated,
        _In_ ULONG Index,
        _In_ ULONG Message);

    /* Caller holds m_VectorLock */
    BOOLEAN
    AssignMessage(
        _In_ ULONG Cause);

    VOID
    ReleaseMessage(
        _In_ ULONG Cause);

    VOID
    ProgramVector(
        _In_ ULONG Cause,
        _In_ UCHAR Message);

    VOID
    ClearVector(
        _In_ ULONG Cause);

    VOID
    WriteCauseRegister(
        _In_ ULONG Offset,
        _In_ ULONG64 Bits);

    ULONG StatusClearRegister() const;

    VOID
    RunHandler(
        _In_ ULONG Cause);

    Usb4HrHostRouter* m_HostRouter;
    Usb4HrHardware* m_Hardware;
    KSPIN_LOCK m_VectorLock;
    CauseSlot m_Causes[CauseCount];

    BOOLEAN m_MessageSignaled;
    ULONG m_InterruptCount;
    WDFINTERRUPT m_Interrupts[MessageCount];
    ULONG m_MessageCause[MessageCount];     /**< MSI: the one cause a message serves */
    USHORT m_FreeMessages;
    volatile LONG64 m_PendingCauses;        /**< line: causes the ISR saw */
};
