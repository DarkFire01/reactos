/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     MSI and line interrupts, vector allocation, throttling and cause dispatch
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4hr.h"

#define NDEBUG
#include <debug.h>

#define USB4HR_NO_CAUSE                 0xFFFFFFFF
#define USB4HR_MSI_MESSAGES             16

/* The DPC never dispatches bit 63 */
#define USB4HR_DISPATCHABLE_CAUSES      0x7FFFFFFFFFFFFFFFULL

#define USB4HR_MSI_FLAGS                (CM_RESOURCE_INTERRUPT_LATCHED | CM_RESOURCE_INTERRUPT_MESSAGE)

/** Per WDFINTERRUPT context. */
struct Usb4HrInterruptContext
{
    Usb4HrInterrupts* Interrupts;
    ULONG Message;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Usb4HrInterruptContext, Usb4HrGetInterruptContext);

NTSTATUS
Usb4HrInterrupts::Create(
    _In_ Usb4HrHostRouter* HostRouter)
{
    ULONG Index;

    m_HostRouter = HostRouter;
    m_Hardware = HostRouter->Hardware();
    KeInitializeSpinLock(&m_VectorLock);

    for (Index = 0; Index < CauseCount; Index++)
    {
        KeInitializeSpinLock(&m_Causes[Index].HandlerLock);
        m_Causes[Index].Message = NoMessage;
    }

    for (Index = 0; Index < MessageCount; Index++)
        m_MessageCause[Index] = USB4HR_NO_CAUSE;

    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrInterrupts::CreateInterrupt(
    _In_ WDFCMRESLIST Raw,
    _In_ WDFCMRESLIST Translated,
    _In_ ULONG Index,
    _In_ ULONG Message)
{
    WDF_INTERRUPT_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    Usb4HrInterruptContext* Context;
    WDFINTERRUPT Interrupt;
    NTSTATUS Status;

    if (m_MessageSignaled)
        WDF_INTERRUPT_CONFIG_INIT(&Config, EvtMessageIsr, EvtMessageDpc);
    else
        WDF_INTERRUPT_CONFIG_INIT(&Config, EvtLineIsr, EvtLineDpc);

    Config.InterruptRaw = WdfCmResourceListGetDescriptor(Raw, Index);
    Config.InterruptTranslated = WdfCmResourceListGetDescriptor(Translated, Index);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, Usb4HrInterruptContext);
    Status = WdfInterruptCreate(m_HostRouter->Device(), &Config, &Attributes, &Interrupt);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WdfInterruptCreate for resource %lu failed 0x%lx\n", Index, Status);
        return Status;
    }

    Context = Usb4HrGetInterruptContext(Interrupt);
    Context->Interrupts = this;
    Context->Message = Message;

    m_Interrupts[Message] = Interrupt;
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrInterrupts::Prepare(
    _In_ WDFCMRESLIST Raw,
    _In_ WDFCMRESLIST Translated)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG MessageIndex[USB4HR_MSI_MESSAGES];
    ULONG Count = WdfCmResourceListGetCount(Translated);
    ULONG Messages = 0;
    ULONG LineIndex = USB4HR_NO_CAUSE;
    ULONG Index;
    ULONG Cause;
    NTSTATUS Status;

    for (Index = 0; Index < Count; Index++)
    {
        Descriptor = WdfCmResourceListGetDescriptor(Translated, Index);
        if (!Descriptor || Descriptor->Type != CmResourceTypeInterrupt)
            continue;

        if ((Descriptor->Flags & USB4HR_MSI_FLAGS) == USB4HR_MSI_FLAGS)
        {
            if (Messages < USB4HR_MSI_MESSAGES)
                MessageIndex[Messages] = Index;
            Messages++;
        }
        else if (LineIndex == USB4HR_NO_CAUSE && !(Descriptor->Flags & CM_RESOURCE_INTERRUPT_WAKE_HINT))
        {
            LineIndex = Index;
        }
    }

    m_InterruptCount = 0;
    m_FreeMessages = 0;

    if (Messages == USB4HR_MSI_MESSAGES)
    {
        /* Every cause gets a message of its own */
        m_MessageSignaled = TRUE;
        for (Index = 0; Index < USB4HR_MSI_MESSAGES; Index++)
        {
            Status = CreateInterrupt(Raw, Translated, MessageIndex[Index], Index);
            if (!NT_SUCCESS(Status))
                return Status;
        }
        m_InterruptCount = USB4HR_MSI_MESSAGES;
    }
    else
    {
        /* One message is handled like a line: one ISR reads the whole status register */
        if (Messages == 1)
        {
            LineIndex = MessageIndex[0];
        }
        else if (Messages > 1)
        {
            DPRINT1("Unsupported count of %lu MSI messages\n", Messages);
            return STATUS_INVALID_PARAMETER;
        }

        if (LineIndex == USB4HR_NO_CAUSE)
        {
            DPRINT1("No interrupt resource\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        m_MessageSignaled = FALSE;
        Status = CreateInterrupt(Raw, Translated, LineIndex, 0);
        if (!NT_SUCCESS(Status))
            return Status;
        m_InterruptCount = 1;
    }

    DPRINT("%s interrupts, %lu objects\n", m_MessageSignaled ? "Message" : "Line", m_InterruptCount);

    /* Vector numbers are handed out per cause; line mode still uses 16 of them for throttling */
    {
        Usb4HrSpinLockGuard Guard(&m_VectorLock);

        m_FreeMessages = (USHORT)((1 << MessageCount) - 1);

        for (Cause = 0; Cause < CauseCount; Cause++)
        {
            if (!m_Causes[Cause].Handler)
                continue;

            if (!AssignMessage(Cause))
            {
                DPRINT1("No interrupt message left for cause %lu\n", Cause);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
        }
    }

    return STATUS_SUCCESS;
}

VOID Usb4HrInterrupts::Release()
{
    ULONG Index;

    Usb4HrSpinLockGuard Guard(&m_VectorLock);

    /* KMDF deletes interrupts created in prepare hardware after this returns */
    for (Index = 0; Index < CauseCount; Index++)
        m_Causes[Index].Message = NoMessage;

    for (Index = 0; Index < MessageCount; Index++)
    {
        m_Interrupts[Index] = NULL;
        m_MessageCause[Index] = USB4HR_NO_CAUSE;
    }

    m_InterruptCount = 0;
    m_FreeMessages = 0;
}

ULONG Usb4HrInterrupts::StatusClearRegister() const
{
    if (m_Hardware->HasShimFlag(USB4HR_SHIM_STATUS_CLEAR_ALT))
        return USB4HR_INTERRUPT_STATUS_CLEAR_ALT;

    return USB4HR_INTERRUPT_STATUS_CLEAR;
}

VOID
Usb4HrInterrupts::WriteCauseRegister(
    _In_ ULONG Offset,
    _In_ ULONG64 Bits)
{
    if (m_Hardware->WideInterruptRegisters())
        m_Hardware->Write64(Offset, Bits);
    else
        m_Hardware->Write32(Offset, (ULONG)Bits);
}

VOID
Usb4HrInterrupts::ProgramVector(
    _In_ ULONG Cause,
    _In_ UCHAR Message)
{
    ULONG Offset = USB4HR_INTERRUPT_VECTOR_ALLOCATION + (Cause / 8) * sizeof(ULONG);
    ULONG Shift = (Cause % 8) * USB4HR_INTERRUPT_VECTOR_BITS;
    ULONG Value;

    Value = m_Hardware->Read32(Offset);
    Value &= ~(USB4HR_INTERRUPT_VECTOR_MASK << Shift);
    Value |= ((ULONG)Message & USB4HR_INTERRUPT_VECTOR_MASK) << Shift;
    m_Hardware->Write32(Offset, Value);
}

VOID
Usb4HrInterrupts::ClearVector(
    _In_ ULONG Cause)
{
    ULONG Offset = USB4HR_INTERRUPT_VECTOR_ALLOCATION + (Cause / 8) * sizeof(ULONG);
    ULONG Shift = (Cause % 8) * USB4HR_INTERRUPT_VECTOR_BITS;

    m_Hardware->Write32(Offset, m_Hardware->Read32(Offset) & ~(USB4HR_INTERRUPT_VECTOR_MASK << Shift));
}

BOOLEAN
Usb4HrInterrupts::AssignMessage(
    _In_ ULONG Cause)
{
    UCHAR Message;

    if (!m_FreeMessages)
        return FALSE;

    for (Message = 0; !(m_FreeMessages & (1 << Message)); Message++)
        ;

    m_FreeMessages &= ~(1 << Message);
    m_Causes[Cause].Message = Message;
    if (m_MessageSignaled)
        m_MessageCause[Message] = Cause;

    /* The cause starts masked with nothing pending */
    if (m_Hardware->IsMmioValid())
    {
        WriteCauseRegister(USB4HR_INTERRUPT_MASK_CLEAR, 1ULL << Cause);
        WriteCauseRegister(StatusClearRegister(), 1ULL << Cause);
        ProgramVector(Cause, Message);
    }

    return TRUE;
}

VOID
Usb4HrInterrupts::ReleaseMessage(
    _In_ ULONG Cause)
{
    UCHAR Message = m_Causes[Cause].Message;

    if (Message == NoMessage)
        return;

    if (m_Hardware->IsMmioValid())
    {
        WriteCauseRegister(USB4HR_INTERRUPT_MASK_CLEAR, 1ULL << Cause);
        WriteCauseRegister(StatusClearRegister(), 1ULL << Cause);
        ClearVector(Cause);
    }

    if (m_MessageSignaled)
        m_MessageCause[Message] = USB4HR_NO_CAUSE;

    m_FreeMessages |= (USHORT)(1 << Message);
    m_Causes[Cause].Message = NoMessage;
}

NTSTATUS Usb4HrInterrupts::ConfigHostInterface()
{
    ULONG Cause;

    if (!m_Hardware->IsMmioValid())
    {
        DPRINT1("Interrupt configuration with dead MMIO\n");
        return STATUS_DEVICE_NOT_READY;
    }

    WriteCauseRegister(StatusClearRegister(), ~0ULL);

    /* Status bits must stay set until the driver clears them */
    m_Hardware->Write32(USB4HR_HOST_INTERFACE_CONTROL,
                        m_Hardware->Read32(USB4HR_HOST_INTERFACE_CONTROL) | USB4HR_HOST_CONTROL_NO_AUTO_CLEAR);

    Usb4HrSpinLockGuard Guard(&m_VectorLock);

    for (Cause = 0; Cause < CauseCount; Cause++)
    {
        UCHAR Message = m_Causes[Cause].Message;

        if (Message == NoMessage)
            continue;

        ProgramVector(Cause, Message);
        m_Hardware->Write32(USB4HR_INTERRUPT_THROTTLING + Message * sizeof(ULONG),
                            USB4HR_INTERRUPT_THROTTLING_DEFAULT);
    }

    return STATUS_SUCCESS;
}

NTSTATUS Usb4HrInterrupts::DeconfigHostInterface()
{
    ULONG Cause;

    if (!m_Hardware->IsMmioValid())
    {
        DPRINT1("Interrupt teardown with dead MMIO\n");
        return STATUS_SUCCESS;
    }

    {
        Usb4HrSpinLockGuard Guard(&m_VectorLock);

        if (m_MessageSignaled)
        {
            for (Cause = 0; Cause < CauseCount; Cause++)
            {
                if (m_Causes[Cause].Message == NoMessage)
                    continue;

                WriteCauseRegister(USB4HR_INTERRUPT_MASK_CLEAR, 1ULL << Cause);
                WriteCauseRegister(StatusClearRegister(), 1ULL << Cause);
                ClearVector(Cause);
            }
        }
        else
        {
            WriteCauseRegister(USB4HR_INTERRUPT_MASK_CLEAR, m_Hardware->WideInterruptRegisters() ?
                                                            USB4HR_DISPATCHABLE_CAUSES : 0xFFFFFFFFULL);
        }
    }

    WriteCauseRegister(StatusClearRegister(), ~0ULL);
    return STATUS_SUCCESS;
}

ULONG
Usb4HrInterrupts::TxCause(
    _In_ ULONG Ring) const
{
    return Ring;
}

ULONG
Usb4HrInterrupts::RxCause(
    _In_ ULONG Ring) const
{
    return m_Hardware->PathCount() + Ring;
}

NTSTATUS
Usb4HrInterrupts::Connect(
    _In_ ULONG Cause,
    _In_ PUSB4HR_INTERRUPT_HANDLER Handler,
    _In_ PVOID Context)
{
    if (Cause >= 2 * USB4HR_MAX_PATHS || !Handler)
    {
        DPRINT1("Connect of invalid cause %lu\n", Cause);
        return STATUS_INVALID_PARAMETER;
    }

    Usb4HrSpinLockGuard Guard(&m_VectorLock);

    if (m_Causes[Cause].Handler)
    {
        DPRINT1("Cause %lu already connected\n", Cause);
        return STATUS_UNSUCCESSFUL;
    }

    m_Causes[Cause].Context = Context;
    m_Causes[Cause].Enabled = FALSE;
    m_Causes[Cause].Handler = Handler;

    /* Before prepare hardware the message is picked when the interrupts exist */
    if (m_InterruptCount && !AssignMessage(Cause))
    {
        DPRINT1("No interrupt message left for cause %lu\n", Cause);
        m_Causes[Cause].Handler = NULL;
        m_Causes[Cause].Context = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

VOID
Usb4HrInterrupts::Disconnect(
    _In_ ULONG Cause)
{
    KIRQL OldIrql;

    if (Cause >= CauseCount)
        return;

    {
        Usb4HrSpinLockGuard Guard(&m_VectorLock);

        m_Causes[Cause].Enabled = FALSE;
        ReleaseMessage(Cause);
    }

    /* Waits for a handler that is running right now */
    KeAcquireSpinLock(&m_Causes[Cause].HandlerLock, &OldIrql);
    m_Causes[Cause].Handler = NULL;
    m_Causes[Cause].Context = NULL;
    KeReleaseSpinLock(&m_Causes[Cause].HandlerLock, OldIrql);
}

VOID
Usb4HrInterrupts::EnableCause(
    _In_ ULONG Cause)
{
    if (Cause >= CauseCount)
        return;

    Usb4HrSpinLockGuard Guard(&m_VectorLock);

    if (!m_Causes[Cause].Handler)
    {
        DPRINT1("Enable of unconnected cause %lu\n", Cause);
        return;
    }

    m_Causes[Cause].Enabled = TRUE;
    WriteCauseRegister(USB4HR_INTERRUPT_MASK_SET, 1ULL << Cause);
}

VOID
Usb4HrInterrupts::DisableCause(
    _In_ ULONG Cause)
{
    if (Cause >= CauseCount)
        return;

    Usb4HrSpinLockGuard Guard(&m_VectorLock);

    m_Causes[Cause].Enabled = FALSE;
    WriteCauseRegister(USB4HR_INTERRUPT_MASK_CLEAR, 1ULL << Cause);
}

BOOLEAN Usb4HrInterrupts::IsMessageSignaled() const
{
    return m_MessageSignaled;
}

VOID
Usb4HrInterrupts::RunHandler(
    _In_ ULONG Cause)
{
    CauseSlot* Slot = &m_Causes[Cause];
    BOOLEAN Handled = FALSE;

    KeAcquireSpinLockAtDpcLevel(&Slot->HandlerLock);
    if (Slot->Handler)
    {
        Slot->Handler(Slot->Context);
        Handled = TRUE;
    }
    KeReleaseSpinLockFromDpcLevel(&Slot->HandlerLock);

    if (!Handled)
    {
        DPRINT1("Interrupt cause %lu has no handler, left masked\n", Cause);
        return;
    }

    /* Unmask unless the owner disabled the cause meanwhile */
    KeAcquireSpinLockAtDpcLevel(&m_VectorLock);
    if (Slot->Enabled)
        WriteCauseRegister(USB4HR_INTERRUPT_MASK_SET, 1ULL << Cause);
    KeReleaseSpinLockFromDpcLevel(&m_VectorLock);
}

BOOLEAN
NTAPI
Usb4HrInterrupts::EvtMessageIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID)
{
    Usb4HrInterruptContext* Context = Usb4HrGetInterruptContext(Interrupt);
    Usb4HrInterrupts* Self = Context->Interrupts;
    ULONG Cause = Self->m_MessageCause[Context->Message];

    UNREFERENCED_PARAMETER(MessageID);

    if (Cause == USB4HR_NO_CAUSE)
        return TRUE;

    Self->WriteCauseRegister(USB4HR_INTERRUPT_MASK_CLEAR, 1ULL << Cause);
    Self->WriteCauseRegister(Self->StatusClearRegister(), 1ULL << Cause);
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

VOID
NTAPI
Usb4HrInterrupts::EvtMessageDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject)
{
    Usb4HrInterruptContext* Context = Usb4HrGetInterruptContext(Interrupt);
    Usb4HrInterrupts* Self = Context->Interrupts;
    ULONG Cause = Self->m_MessageCause[Context->Message];

    UNREFERENCED_PARAMETER(AssociatedObject);

    if (Cause != USB4HR_NO_CAUSE)
        Self->RunHandler(Cause);
}

BOOLEAN
NTAPI
Usb4HrInterrupts::EvtLineIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID)
{
    Usb4HrInterrupts* Self = Usb4HrGetInterruptContext(Interrupt)->Interrupts;
    Usb4HrHardware* Hardware = Self->m_Hardware;
    ULONG64 Status;

    UNREFERENCED_PARAMETER(MessageID);

    if (Hardware->WideInterruptRegisters())
        Status = Hardware->Read64(USB4HR_INTERRUPT_STATUS);
    else
        Status = Hardware->Read32(USB4HR_INTERRUPT_STATUS);

    if (!Status)
        return FALSE;

    InterlockedOr64(&Self->m_PendingCauses, (LONG64)Status);
    Self->WriteCauseRegister(USB4HR_INTERRUPT_MASK_CLEAR, Status);
    Self->WriteCauseRegister(Self->StatusClearRegister(), Status);
    WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

VOID
NTAPI
Usb4HrInterrupts::EvtLineDpc(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject)
{
    Usb4HrInterrupts* Self = Usb4HrGetInterruptContext(Interrupt)->Interrupts;
    ULONG64 Pending;
    ULONG Cause;

    UNREFERENCED_PARAMETER(AssociatedObject);

    Pending = (ULONG64)InterlockedExchange64(&Self->m_PendingCauses, 0) & USB4HR_DISPATCHABLE_CAUSES;

    for (Cause = 0; Pending != 0; Cause++)
    {
        if (!(Pending & (1ULL << Cause)))
            continue;

        Pending &= ~(1ULL << Cause);
        Self->RunHandler(Cause);
    }
}
