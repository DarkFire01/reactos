/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/io/iomgr/irq.c
 * PURPOSE:         I/O Wrappers (called Completion Ports) for Kernel Queues
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

/*
 * @implemented
 */
NTSTATUS
NTAPI
IoConnectInterrupt(OUT PKINTERRUPT *InterruptObject,
                   IN PKSERVICE_ROUTINE ServiceRoutine,
                   IN PVOID ServiceContext,
                   IN PKSPIN_LOCK SpinLock,
                   IN ULONG Vector,
                   IN KIRQL Irql,
                   IN KIRQL SynchronizeIrql,
                   IN KINTERRUPT_MODE InterruptMode,
                   IN BOOLEAN ShareVector,
                   IN KAFFINITY ProcessorEnableMask,
                   IN BOOLEAN FloatingSave)
{
    PKINTERRUPT Interrupt;
    PKINTERRUPT InterruptUsed;
    PIO_INTERRUPT IoInterrupt;
    BOOLEAN FirstRun;
    CCHAR Count = 0;
    KAFFINITY Affinity;

    PAGED_CODE();

    /* Assume failure */
    *InterruptObject = NULL;

    /* Get the affinity */
    Affinity = ProcessorEnableMask & KeActiveProcessors;
    while (Affinity)
    {
        /* Increase count */
        if (Affinity & 1) Count++;
        Affinity >>= 1;
    }

    /* Make sure we have a valid CPU count */
    if (!Count) return STATUS_INVALID_PARAMETER;

    /* Allocate the array of I/O interrupts */
    IoInterrupt = ExAllocatePoolZero(NonPagedPool,
                                     (Count - 1) * sizeof(KINTERRUPT) +
                                     sizeof(IO_INTERRUPT),
                                     TAG_IO_INTERRUPT);
    if (!IoInterrupt) return STATUS_INSUFFICIENT_RESOURCES;

    /* Use the structure's spinlock, if none was provided */
    if (!SpinLock)
    {
        SpinLock = &IoInterrupt->SpinLock;
        KeInitializeSpinLock(SpinLock);
    }

    /* We first start with a built-in interrupt inside the I/O structure */
    Interrupt = (PKINTERRUPT)(IoInterrupt + 1);
    FirstRun = TRUE;

    /* Now create all the interrupts */
    Affinity = ProcessorEnableMask & KeActiveProcessors;
    for (Count = 0; Affinity; Count++, Affinity >>= 1)
    {
        /* Check if it's enabled for this CPU */
        if (!(Affinity & 1))
            continue;

        /* Check which one we will use */
        InterruptUsed = FirstRun ? &IoInterrupt->FirstInterrupt : Interrupt;

        /* Initialize it */
        KeInitializeInterrupt(InterruptUsed,
                              ServiceRoutine,
                              ServiceContext,
                              SpinLock,
                              Vector,
                              Irql,
                              SynchronizeIrql,
                              InterruptMode,
                              ShareVector,
                              Count,
                              FloatingSave);

        /* Connect it */
        if (!KeConnectInterrupt(InterruptUsed))
        {
            /* Check how far we got */
            if (FirstRun)
            {
                /* We failed early so just free this */
                ExFreePoolWithTag(IoInterrupt, TAG_IO_INTERRUPT);
            }
            else
            {
                /* Far enough, so disconnect everything */
                IoDisconnectInterrupt(&IoInterrupt->FirstInterrupt);
            }

            /* And fail */
            return STATUS_INVALID_PARAMETER;
        }

        /* Now we've used up our First Run */
        if (FirstRun)
        {
            FirstRun = FALSE;
        }
        else
        {
            /* Move on to the next one */
            IoInterrupt->Interrupt[(UCHAR)Count] = Interrupt++;
        }
    }

    /* Return success */
    *InterruptObject = &IoInterrupt->FirstInterrupt;
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
VOID
NTAPI
IoDisconnectInterrupt(PKINTERRUPT InterruptObject)
{
    PIO_INTERRUPT IoInterrupt;
    ULONG i;

    PAGED_CODE();

    /* Get the I/O interrupt */
    IoInterrupt = CONTAINING_RECORD(InterruptObject,
                                    IO_INTERRUPT,
                                    FirstInterrupt);

    /* Disconnect the first one */
    KeDisconnectInterrupt(&IoInterrupt->FirstInterrupt);

    /* Now disconnect the others */
    for (i = 0; i < KeNumberProcessors; i++)
    {
        /* Make sure one was registered */
        if (!IoInterrupt->Interrupt[i])
            continue;

        /* Disconnect it */
        KeDisconnectInterrupt(IoInterrupt->Interrupt[i]);
    }

    /* Free the I/O interrupt */
    ExFreePoolWithTag(IoInterrupt, TAG_IO_INTERRUPT);
}

static PCM_PARTIAL_RESOURCE_DESCRIPTOR
IopFindMessageInterruptDescriptor(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject);
static PCM_PARTIAL_RESOURCE_DESCRIPTOR
IopFindMessageInterruptDescriptorEx(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_ BOOLEAN Translated);

NTSTATUS
IopConnectInterruptExFullySpecific(
    _Inout_ PIO_CONNECT_INTERRUPT_PARAMETERS Parameters)
{
    NTSTATUS Status;
    ULONG Vector = Parameters->FullySpecified.Vector;
    KIRQL Irql = Parameters->FullySpecified.Irql;
    KIRQL SynchronizeIrql = Parameters->FullySpecified.SynchronizeIrql;
    KINTERRUPT_MODE Mode = Parameters->FullySpecified.InterruptMode;
    KAFFINITY Affinity = Parameters->FullySpecified.ProcessorEnableMask;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR RawDesc;

    PAGED_CODE();

    /*
     * If the device was granted a message-signalled interrupt (MSI/MSI-X), the
     * caller passes the *translated* system vector, but the LAPIC actually
     * delivers the *raw* message vector that the bus driver programmed into the
     * device's MSI/MSI-X capability. IoConnectInterrupt() arms the IDT at the
     * vector it is given, so without remapping the IDT entry would never match
     * the delivered vector and the ISR would never run. Remap to the raw vector
     * (with its message IRQL and edge-triggered mode) before connecting.
     *
     * Win8's IoConnectInterruptEx resolves the same thing via the device's
     * interrupt connection data; we derive it from the raw/translated resource
     * lists the PnP manager assigned.
     */
    RawDesc = IopFindMessageInterruptDescriptor(Parameters->FullySpecified.PhysicalDeviceObject);
    if (RawDesc != NULL)
    {
        ULONG RawBase = RawDesc->u.MessageInterrupt.Raw.Vector;
        ULONG Count = RawDesc->u.MessageInterrupt.Raw.MessageCount;
        BOOLEAN Remap = FALSE;

        if (Count == 0)
            Count = 1;

        if (Count == 1)
        {
            /*
             * Single message: the device delivers exactly one vector - the raw
             * message vector the bus driver programmed into the MSI/MSI-X
             * capability. The caller's translated vector belongs to a different
             * HAL domain and would arm the wrong IDT entry, so use the raw one.
             */
            Vector = RawBase;
            Remap = TRUE;
        }
        else
        {
            /*
             * Multiple messages: map the caller's translated vector to the raw
             * message vector by its offset within the translated block.
             */
            PCM_PARTIAL_RESOURCE_DESCRIPTOR TransDesc =
                IopFindMessageInterruptDescriptorEx(Parameters->FullySpecified.PhysicalDeviceObject, TRUE);
            if (TransDesc != NULL)
            {
                ULONG TransBase = TransDesc->u.MessageInterrupt.Translated.Vector;
                if (Vector >= TransBase && Vector < TransBase + Count)
                {
                    Vector = RawBase + (Vector - TransBase);
                    Remap = TRUE;
                }
            }
        }

        if (Remap)
        {
            Irql = HalGetMessageVectorIrql((UCHAR)Vector);
            Mode = Latched;       /* MSI/MSI-X are edge-triggered */
            Affinity = RawDesc->u.MessageInterrupt.Raw.Affinity;
        }
    }

    /* KeInitializeInterrupt requires SynchronizeIrql >= the interrupt IRQL */
    if (SynchronizeIrql < Irql)
        SynchronizeIrql = Irql;

    Status = IoConnectInterrupt(Parameters->FullySpecified.InterruptObject,
                                Parameters->FullySpecified.ServiceRoutine,
                                Parameters->FullySpecified.ServiceContext,
                                Parameters->FullySpecified.SpinLock,
                                Vector,
                                Irql,
                                SynchronizeIrql,
                                Mode,
                                Parameters->FullySpecified.ShareVector,
                                Affinity,
                                Parameters->FullySpecified.FloatingSave);
    if (!NT_SUCCESS(Status))
        DPRINT1("IopConnectInterruptExFullySpecific() failed: 0x%lx\n", Status);
    return Status;
}

/*
 * Trampoline installed as the ServiceRoutine of a message-signalled KINTERRUPT.
 * The generic interrupt dispatcher calls ServiceRoutine(Interrupt, Context); we
 * forward to the driver's KMESSAGE_SERVICE_ROUTINE, supplying the message index
 * that distinguishes which MSI/MSI-X message fired.
 */
BOOLEAN
NTAPI
KiMessageInterruptDispatch(
    _In_ PKINTERRUPT Interrupt,
    _In_ PVOID ServiceContext)
{
    PKMESSAGE_SERVICE_ROUTINE MessageRoutine;

    MessageRoutine = (PKMESSAGE_SERVICE_ROUTINE)Interrupt->MessageServiceRoutine;
    return MessageRoutine(Interrupt, ServiceContext, Interrupt->MessageIndex);
}

/*
 * Tear down the KINTERRUPT objects backing a message-based connection.
 *
 * The message vectors themselves are owned by the device's resource list (they
 * were allocated by the PnP manager when the CM_RESOURCE_INTERRUPT_MESSAGE
 * descriptor was assigned and live until the device is stopped/removed), so we
 * disconnect the interrupt objects here but deliberately do NOT free the vectors.
 */
static
VOID
IopFreeMsiInterrupts(
    _In_ PIO_INTERRUPT_MESSAGE_INFO MessageInfo,
    _In_ ULONG MessageCount)
{
    ULONG i;

    for (i = 0; i < MessageCount; i++)
    {
        PIO_INTERRUPT_MESSAGE_INFO_ENTRY Entry = &MessageInfo->MessageInfo[i];

        if (Entry->InterruptObject)
        {
            KeDisconnectInterrupt(Entry->InterruptObject);
            ExFreePoolWithTag(Entry->InterruptObject, TAG_IO_INTERRUPT);
            Entry->InterruptObject = NULL;
        }
    }
}

/*
 * Locate the message-signalled interrupt descriptor a device was assigned. The
 * bus driver (pci.sys) reports a CM_RESOURCE_INTERRUPT_MESSAGE requirement and
 * the PnP manager fills the raw MessageInterrupt form with the granted base
 * vector and message count.
 */
static
PCM_PARTIAL_RESOURCE_DESCRIPTOR
IopFindMessageInterruptDescriptorEx(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_ BOOLEAN Translated)
{
    PDEVICE_NODE DeviceNode;
    PCM_RESOURCE_LIST ResourceList;
    PCM_FULL_RESOURCE_DESCRIPTOR FullDescriptor;
    ULONG i, j;

    DeviceNode = IopGetDeviceNode(PhysicalDeviceObject);
    if (!DeviceNode)
        return NULL;

    /*
     * The raw list carries the MessageInterrupt.Raw form (base vector + count);
     * the translated list carries MessageInterrupt.Translated (system vector).
     */
    ResourceList = Translated ? DeviceNode->ResourceListTranslated : DeviceNode->ResourceList;
    if (!ResourceList)
        return NULL;

    FullDescriptor = &ResourceList->List[0];

    for (i = 0; i < ResourceList->Count; i++)
    {
        PCM_PARTIAL_RESOURCE_LIST PartialList = &FullDescriptor->PartialResourceList;

        for (j = 0; j < PartialList->Count; j++)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor = &PartialList->PartialDescriptors[j];

            if (Descriptor->Type == CmResourceTypeInterrupt &&
                (Descriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE))
            {
                return Descriptor;
            }
        }

        FullDescriptor = CmiGetNextResourceDescriptor(FullDescriptor);
    }

    return NULL;
}

static
PCM_PARTIAL_RESOURCE_DESCRIPTOR
IopFindMessageInterruptDescriptor(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    return IopFindMessageInterruptDescriptorEx(PhysicalDeviceObject, FALSE);
}

NTSTATUS
IopConnectInterruptExMessageBased(
    _Inout_ PIO_CONNECT_INTERRUPT_PARAMETERS Parameters)
{
    PIO_CONNECT_INTERRUPT_MESSAGE_BASED_PARAMETERS Message = &Parameters->MessageBased;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    PIO_INTERRUPT_MESSAGE_INFO MessageInfo = NULL;
    NTSTATUS Status;
    ULONG MessageCount;
    ULONG BaseVector;
    KAFFINITY Affinity;
    ULONG i = 0;
    KIRQL UnifiedIrql = PASSIVE_LEVEL;
    SIZE_T Size;

    PAGED_CODE();

    /*
     * The device's interrupt was assigned through the resource list. Find the
     * message-signalled descriptor the PnP manager granted; the bus driver has
     * already programmed the MSI/MSI-X capability at START, so all we do here is
     * stand up a KINTERRUPT for each already-allocated message vector.
     */
    Descriptor = IopFindMessageInterruptDescriptor(Message->PhysicalDeviceObject);
    if (!Descriptor)
    {
        DPRINT1("IoConnectInterruptEx: no message interrupt resource assigned\n");
        return STATUS_NOT_SUPPORTED;
    }

    MessageCount = Descriptor->u.MessageInterrupt.Raw.MessageCount;
    BaseVector = Descriptor->u.MessageInterrupt.Raw.Vector;
    Affinity = Descriptor->u.MessageInterrupt.Raw.Affinity;
    if (MessageCount == 0)
        MessageCount = 1;

    /* Allocate the variable-length message table handed back to the caller */
    Size = FIELD_OFFSET(IO_INTERRUPT_MESSAGE_INFO, MessageInfo) +
           MessageCount * sizeof(IO_INTERRUPT_MESSAGE_INFO_ENTRY);
    MessageInfo = ExAllocatePoolZero(NonPagedPool, Size, TAG_IO_INTERRUPT);
    if (!MessageInfo)
        return STATUS_INSUFFICIENT_RESOURCES;

    MessageInfo->MessageCount = MessageCount;

    /* Stand up a KINTERRUPT for each assigned (consecutive) message vector */
    for (i = 0; i < MessageCount; i++)
    {
        PIO_INTERRUPT_MESSAGE_INFO_ENTRY Entry = &MessageInfo->MessageInfo[i];
        PKINTERRUPT Interrupt;
        PHYSICAL_ADDRESS Address;
        ULONG Data;
        UCHAR Vector = (UCHAR)(BaseVector + i);
        KIRQL Irql;
        KIRQL SynchronizeIrql;

        Irql = HalGetMessageVectorIrql(Vector);

        /* MSI is edge-triggered (latched); recover the message address/data */
        HalGetMessageVectorMessage(Vector, Latched, &Address, &Data);

        Interrupt = ExAllocatePoolZero(NonPagedPool, sizeof(KINTERRUPT), TAG_IO_INTERRUPT);
        if (!Interrupt)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto FailConnection;
        }

        /* Default the synchronize IRQL to the message IRQL when too low */
        SynchronizeIrql = Message->SynchronizeIrql;
        if (SynchronizeIrql < Irql)
            SynchronizeIrql = Irql;

        KeInitializeInterrupt(Interrupt,
                              (PKSERVICE_ROUTINE)KiMessageInterruptDispatch,
                              Message->ServiceContext,
                              Message->SpinLock,
                              Vector,
                              Irql,
                              SynchronizeIrql,
                              Latched,
                              FALSE,
                              0,
                              Message->FloatingSave);

        /* Route this object through the message trampoline */
        Interrupt->MessageServiceRoutine = (PKSERVICE_ROUTINE)Message->MessageServiceRoutine;
        Interrupt->MessageIndex = i;

        /* Record the entry before connecting so cleanup can find it */
        Entry->MessageAddress = Address;
        Entry->TargetProcessorSet = Affinity;
        Entry->InterruptObject = Interrupt;
        Entry->MessageData = Data;
        Entry->Vector = Vector;
        Entry->Irql = Irql;
        Entry->Mode = Latched;
        Entry->Polarity = InterruptActiveHigh;

        if (!KeConnectInterrupt(Interrupt))
        {
            ExFreePoolWithTag(Interrupt, TAG_IO_INTERRUPT);
            Entry->InterruptObject = NULL;
            Status = STATUS_INVALID_PARAMETER;
            goto FailConnection;
        }

        if (Irql > UnifiedIrql)
            UnifiedIrql = Irql;
    }

    MessageInfo->UnifiedIrql = UnifiedIrql;

    /* Success - hand the message table back to the caller */
    *Message->ConnectionContext.InterruptMessageTable = MessageInfo;
    return STATUS_SUCCESS;

FailConnection:
    IopFreeMsiInterrupts(MessageInfo, i);
    ExFreePoolWithTag(MessageInfo, TAG_IO_INTERRUPT);
    return Status;
}

NTSTATUS
NTAPI
IoConnectInterruptEx(
    _Inout_ PIO_CONNECT_INTERRUPT_PARAMETERS Parameters)
{
    NTSTATUS Status;

    PAGED_CODE();

    switch (Parameters->Version)
    {
        case CONNECT_FULLY_SPECIFIED:
            return IopConnectInterruptExFullySpecific(Parameters);
        case CONNECT_FULLY_SPECIFIED_GROUP:
            //TODO: We don't do anything for the group type
            return IopConnectInterruptExFullySpecific(Parameters);
        case CONNECT_MESSAGE_BASED:
            Status = IopConnectInterruptExMessageBased(Parameters);
            if (NT_SUCCESS(Status))
                return Status;

            /*
             * Message-based connection failed. If the caller supplied a fallback
             * line-based ISR, report the failure so it can retry with a
             * line-based connection (CONNECT_LINE_BASED remains a FIXME below).
             */
            DPRINT1("IoConnectInterruptEx: message-based connect failed (0x%lx)\n", Status);
            return Status;
        case CONNECT_LINE_BASED:
            DPRINT1("FIXME: Line based interrupts are UNIMPLEMENTED\n");
            return STATUS_NOT_SUPPORTED;
    }

    return STATUS_INVALID_PARAMETER;
}

VOID
NTAPI
IoDisconnectInterruptEx(
    _In_ PIO_DISCONNECT_INTERRUPT_PARAMETERS Parameters)
{
    PAGED_CODE();

    switch (Parameters->Version)
    {
        case CONNECT_MESSAGE_BASED:
        {
            PIO_INTERRUPT_MESSAGE_INFO MessageInfo;

            MessageInfo = Parameters->ConnectionContext.InterruptMessageTable;
            if (!MessageInfo)
                break;

            /*
             * Disconnect the message interrupt objects. The MSI/MSI-X capability
             * is disabled by the bus driver when the device is stopped/removed,
             * and the message vectors are owned by the resource list, so there is
             * nothing else to release here.
             */
            IopFreeMsiInterrupts(MessageInfo, MessageInfo->MessageCount);
            ExFreePoolWithTag(MessageInfo, TAG_IO_INTERRUPT);
            break;
        }

        case CONNECT_FULLY_SPECIFIED:
        case CONNECT_FULLY_SPECIFIED_GROUP:
        case CONNECT_LINE_BASED:
        default:
            if (Parameters->ConnectionContext.InterruptObject)
                IoDisconnectInterrupt(Parameters->ConnectionContext.InterruptObject);
            break;
    }
}

/* EOF */
