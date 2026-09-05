/*
 * PROJECT:         ReactOS Kernel
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            ntoskrnl/io/iomgr/irq.c
 * PURPOSE:         I/O Wrappers (called Completion Ports) for Kernel Queues
 * PROGRAMMERS:     Alex Ionescu (alex.ionescu@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include <ntoskrnl.h>
#include <devpropdef.h>

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

    /*
     * An interrupt on a secondary vector was never placed in the IDT, so the
     * ordinary disconnect would go looking for a dispatch entry that does not
     * exist. It also only ever has the one object: delivery is a call from the
     * controller's driver rather than a hardware route, so there is nothing
     * per-processor about it.
     */
    if (IoInterrupt->FirstInterrupt.Vector >= SECONDARY_VECTOR_BASE)
    {
        KiDisconnectSecondaryInterrupt(&IoInterrupt->FirstInterrupt);
        ExFreePoolWithTag(IoInterrupt, TAG_IO_INTERRUPT);
        return;
    }

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

/* CONNECTION-DATA BASED CONNECTS *********************************************/

/*
 * The bus driver (or the arbiter that placed the device's interrupts)
 * publishes what to connect as the INTERRUPT_CONNECTION_DATA property on the
 * PDO: one element per line or message. Line-based and message-based connects
 * consume that property; the fully-specified form keeps taking the vector
 * from the caller.
 */
static const DEVPROPKEY IopInterruptConnectionDataKey =
{
    { 0xF0E20F09, 0xD97A, 0x49A9, { 0x80, 0x46, 0xBB, 0x6E, 0x22, 0xE6, 0xBB, 0x2E } },
    1
};

/* Bookkeeping behind one message of a message-based connect */
typedef struct _IOP_MESSAGE_INTERRUPT_LINK
{
    PKMESSAGE_SERVICE_ROUTINE ServiceRoutine;
    PVOID ServiceContext;
    ULONG MessageIndex;
} IOP_MESSAGE_INTERRUPT_LINK, *PIOP_MESSAGE_INTERRUPT_LINK;

/* The message table handed to the driver, with what is needed to take it
   down again in front of it */
typedef struct _IOP_MESSAGE_INTERRUPTS
{
    PIOP_MESSAGE_INTERRUPT_LINK Links;
    PINTERRUPT_CONNECTION_DATA ConnectionData;
    IO_INTERRUPT_MESSAGE_INFO Table;
} IOP_MESSAGE_INTERRUPTS, *PIOP_MESSAGE_INTERRUPTS;

#define IOP_MESSAGE_TABLE_SIZE(Count) \
    (FIELD_OFFSET(IOP_MESSAGE_INTERRUPTS, Table.MessageInfo) + \
     (Count) * sizeof(IO_INTERRUPT_MESSAGE_INFO_ENTRY))

/**
 * @brief
 * Fetches a device's interrupt connection data property.
 *
 * @param[in] Pdo
 * The physical device object carrying the property.
 *
 * @param[out] ConnectionData
 * Receives a pool copy of the property; the caller frees it.
 *
 * @return
 * STATUS_NOT_FOUND when the device has no such property, or a
 * failure when it is malformed.
 */
NTSTATUS
IopReadInterruptConnectionData(
    _In_ PDEVICE_OBJECT Pdo,
    _Out_ PINTERRUPT_CONNECTION_DATA *ConnectionData)
{
    PINTERRUPT_CONNECTION_DATA Data;
    ULONG Size = 0, Type = 0;
    NTSTATUS Status;

    PAGED_CODE();

    *ConnectionData = NULL;

    Status = IoGetDevicePropertyData(Pdo,
                                     &IopInterruptConnectionDataKey,
                                     0,
                                     0,
                                     0,
                                     NULL,
                                     &Size,
                                     &Type);
    if (Status != STATUS_BUFFER_TOO_SMALL)
    {
        return NT_SUCCESS(Status) ? STATUS_NOT_FOUND : Status;
    }
    if (Size < FIELD_OFFSET(INTERRUPT_CONNECTION_DATA, Vectors[1]))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Data = ExAllocatePoolWithTag(NonPagedPool, Size, TAG_IO_INTERRUPT);
    if (Data == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = IoGetDevicePropertyData(Pdo,
                                     &IopInterruptConnectionDataKey,
                                     0,
                                     0,
                                     Size,
                                     Data,
                                     &Size,
                                     &Type);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Data, TAG_IO_INTERRUPT);
        return Status;
    }

    /* Every element the count announces must be there */
    if ((Data->Count == 0) ||
        (Size < FIELD_OFFSET(INTERRUPT_CONNECTION_DATA, Vectors[Data->Count])))
    {
        ExFreePoolWithTag(Data, TAG_IO_INTERRUPT);
        return STATUS_INVALID_PARAMETER;
    }

    *ConnectionData = Data;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Tells whether a connection data element describes a message.
 */
static
BOOLEAN
IopIsMessageVector(
    _In_ PINTERRUPT_VECTOR_DATA VectorData)
{
    return (VectorData->Type == InterruptTypeXapicMessage) ||
           (VectorData->Type == InterruptTypeHypertransport) ||
           (VectorData->Type == InterruptTypeMessageRequest);
}

/**
 * @brief
 * Connects one interrupt described by a connection data element: the HAL
 * takes ownership of the vector (and routes the input for a line), then
 * the interrupt object is connected on the element's processors.
 */
/**
 * @brief
 * Connects a service routine to an interrupt another driver delivers.
 *
 * The interrupt object is built exactly as it would be for a line, because
 * from the driver's side nothing about it differs: it still has an IRQL, a
 * lock and a mode, and KeSynchronizeExecution still has to mean something. All
 * that changes is where it is registered, and that only one is needed - a
 * secondary interrupt is delivered by a call on whichever processor the
 * controller's own interrupt landed on, so there is nothing to place per
 * processor.
 *
 * @param[in] VectorData
 * The line, already carrying the secondary vector it was given.
 *
 * @param[in] ServiceRoutine
 * The driver's ISR.
 *
 * @param[in] ServiceContext
 * What to hand it.
 *
 * @param[in] SpinLock
 * The driver's interrupt lock, or NULL to use the one in the block.
 *
 * @param[in] SynchronizeIrql
 * The IRQL to synchronize to, or zero for the interrupt's own.
 *
 * @param[in] ShareVector
 * Whether the driver will share the pin.
 *
 * @param[in] Affinity
 * Recorded on the object, for the sake of anything that reads it back.
 *
 * @param[out] InterruptObject
 * Receives the interrupt object.
 *
 * @return
 * STATUS_SUCCESS, or the failure that stopped the connect.
 */
static
NTSTATUS
IopConnectSecondaryInterrupt(
    _In_ PINTERRUPT_VECTOR_DATA VectorData,
    _In_ PKSERVICE_ROUTINE ServiceRoutine,
    _In_opt_ PVOID ServiceContext,
    _In_opt_ PKSPIN_LOCK SpinLock,
    _In_ KIRQL SynchronizeIrql,
    _In_ BOOLEAN ShareVector,
    _In_ KAFFINITY Affinity,
    _Out_ PKINTERRUPT *InterruptObject)
{
    PIO_INTERRUPT IoInterrupt;
    NTSTATUS Status;
    CCHAR Number;

    PAGED_CODE();

    *InterruptObject = NULL;

    IoInterrupt = ExAllocatePoolZero(NonPagedPool,
                                     sizeof(IO_INTERRUPT),
                                     TAG_IO_INTERRUPT);
    if (IoInterrupt == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (SpinLock == NULL)
    {
        SpinLock = &IoInterrupt->SpinLock;
        KeInitializeSpinLock(SpinLock);
    }

    /* The processor the object records is the first one the line may target */
    Number = (CCHAR)(Affinity ? (CCHAR)RtlFindLeastSignificantBit(Affinity) : 0);

    KeInitializeInterrupt(&IoInterrupt->FirstInterrupt,
                          ServiceRoutine,
                          ServiceContext,
                          SpinLock,
                          VectorData->Vector,
                          VectorData->Irql,
                          SynchronizeIrql ? SynchronizeIrql : VectorData->Irql,
                          VectorData->Mode,
                          ShareVector,
                          Number,
                          FALSE);

    Status = KiConnectSecondaryInterrupt(&IoInterrupt->FirstInterrupt);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Secondary vector %lu would not take another interrupt: 0x%08lx\n",
                VectorData->Vector, Status);
        ExFreePoolWithTag(IoInterrupt, TAG_IO_INTERRUPT);
        return Status;
    }

    /*
     * A pin is armed masked so that nothing is delivered between the
     * controller enabling it and the ISR being on the vector; now that it is,
     * the line can be let through.
     */
    Status = KiUnmaskSecondaryInterrupt(VectorData->Vector,
                                        VectorData->ControllerInput.Gsiv);
    if (!NT_SUCCESS(Status) && (Status != STATUS_NOT_SUPPORTED))
    {
        DPRINT1("GSIV %lu would not unmask: 0x%08lx\n",
                VectorData->ControllerInput.Gsiv, Status);
        KiDisconnectSecondaryInterrupt(&IoInterrupt->FirstInterrupt);
        ExFreePoolWithTag(IoInterrupt, TAG_IO_INTERRUPT);
        return Status;
    }

    *InterruptObject = &IoInterrupt->FirstInterrupt;
    return STATUS_SUCCESS;
}

static
NTSTATUS
IopConnectVectorData(
    _In_ PINTERRUPT_VECTOR_DATA VectorData,
    _In_ PKSERVICE_ROUTINE ServiceRoutine,
    _In_ PVOID ServiceContext,
    _In_opt_ PKSPIN_LOCK SpinLock,
    _In_ KIRQL SynchronizeIrql,
    _In_ BOOLEAN ShareVector,
    _In_ BOOLEAN FloatingSave,
    _Out_ PKINTERRUPT *InterruptObject)
{
    INTERRUPT_CONNECTION_DATA Single;
    KAFFINITY Affinity;
    NTSTATUS Status;

    PAGED_CODE();

    *InterruptObject = NULL;

    RtlZeroMemory(&Single, sizeof(Single));
    Single.Count = 1;
    Single.Vectors[0] = *VectorData;

    Affinity = VectorData->TargetProcessors.Mask & KeActiveProcessors;
    if (Affinity == 0)
    {
        Affinity = KeActiveProcessors;
    }

    /*
     * An interrupt another driver demultiplexes has no processor vector of its
     * own. It is named by one of the kernel's secondary vectors instead, and
     * that name has to be settled before the HAL is told to enable the line,
     * because what the HAL records is what the controller quotes back when the
     * line fires.
     */
    if (KiIsInterruptTypeSecondary(&Single))
    {
        Status = KeAllocateSecondaryVector(VectorData->ControllerInput.Gsiv,
                                           &Single.Vectors[0].Vector);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("No secondary vector for GSIV %lu: 0x%08lx\n",
                    VectorData->ControllerInput.Gsiv, Status);
            return Status;
        }

        Status = HalEnableInterrupt(&Single);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        Status = IopConnectSecondaryInterrupt(&Single.Vectors[0],
                                              ServiceRoutine,
                                              ServiceContext,
                                              SpinLock,
                                              SynchronizeIrql,
                                              ShareVector,
                                              Affinity,
                                              InterruptObject);
        if (!NT_SUCCESS(Status))
        {
            HalDisableInterrupt(&Single);
        }

        return Status;
    }

    Status = HalEnableInterrupt(&Single);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = IoConnectInterrupt(InterruptObject,
                                ServiceRoutine,
                                ServiceContext,
                                SpinLock,
                                VectorData->Vector,
                                VectorData->Irql,
                                SynchronizeIrql ? SynchronizeIrql : VectorData->Irql,
                                VectorData->Mode,
                                ShareVector,
                                Affinity,
                                FloatingSave);
    if (!NT_SUCCESS(Status))
    {
        HalDisableInterrupt(&Single);
    }

    return Status;
}

/**
 * @brief
 * The service routine behind every message interrupt: forwards to the
 * driver's message routine with the message number.
 */
static
BOOLEAN
NTAPI
IopMessageInterruptService(
    _In_ PKINTERRUPT Interrupt,
    _In_ PVOID ServiceContext)
{
    PIOP_MESSAGE_INTERRUPT_LINK Link = ServiceContext;

    return Link->ServiceRoutine(Interrupt, Link->ServiceContext, Link->MessageIndex);
}

/**
 * @brief
 * Line-based connect (CONNECT_LINE_BASED): connects the device's controller
 * input as published in its connection data. A device without the property
 * is connected from its translated interrupt resource instead.
 */
static
NTSTATUS
IopConnectLineInterrupt(
    _In_ PDEVICE_OBJECT Pdo,
    _Out_ PKINTERRUPT *InterruptObject,
    _In_ PKSERVICE_ROUTINE ServiceRoutine,
    _In_opt_ PVOID ServiceContext,
    _In_opt_ PKSPIN_LOCK SpinLock,
    _In_ KIRQL SynchronizeIrql,
    _In_ BOOLEAN FloatingSave)
{
    PINTERRUPT_CONNECTION_DATA ConnectionData;
    PINTERRUPT_VECTOR_DATA VectorData = NULL;
    NTSTATUS Status;
    ULONG i;

    PAGED_CODE();

    *InterruptObject = NULL;
    if ((Pdo == NULL) || (ServiceRoutine == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status = IopReadInterruptConnectionData(Pdo, &ConnectionData);
    if (NT_SUCCESS(Status))
    {
        for (i = 0; i < ConnectionData->Count; i++)
        {
            if (ConnectionData->Vectors[i].Type == InterruptTypeControllerInput)
            {
                VectorData = &ConnectionData->Vectors[i];
                break;
            }
        }
        if (VectorData == NULL)
        {
            ExFreePoolWithTag(ConnectionData, TAG_IO_INTERRUPT);
            return STATUS_NOT_FOUND;
        }
        if ((SynchronizeIrql != 0) && (SynchronizeIrql < VectorData->Irql))
        {
            ExFreePoolWithTag(ConnectionData, TAG_IO_INTERRUPT);
            return STATUS_INVALID_PARAMETER;
        }

        Status = IopConnectVectorData(VectorData,
                                      ServiceRoutine,
                                      ServiceContext,
                                      SpinLock,
                                      SynchronizeIrql,
                                      TRUE,
                                      FloatingSave,
                                      InterruptObject);
        ExFreePoolWithTag(ConnectionData, TAG_IO_INTERRUPT);
        return Status;
    }
    if (Status != STATUS_NOT_FOUND)
    {
        return Status;
    }

    /* No property: the translated resources of the device node say what the
       kernel assigned to it */
    {
        PDEVICE_NODE DeviceNode = IopGetDeviceNode(Pdo);
        PCM_RESOURCE_LIST List;
        PCM_FULL_RESOURCE_DESCRIPTOR Full;
        ULONG j;

        if ((DeviceNode == NULL) || (DeviceNode->ResourceListTranslated == NULL))
        {
            return STATUS_NOT_FOUND;
        }

        List = DeviceNode->ResourceListTranslated;
        Full = &List->List[0];
        for (i = 0; i < List->Count; i++)
        {
            for (j = 0; j < Full->PartialResourceList.Count; j++)
            {
                PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;

                Descriptor = &Full->PartialResourceList.PartialDescriptors[j];
                if ((Descriptor->Type == CmResourceTypeInterrupt) &&
                    !(Descriptor->Flags & CM_RESOURCE_INTERRUPT_MESSAGE))
                {
                    KIRQL Irql = (KIRQL)Descriptor->u.Interrupt.Level;
                    KAFFINITY Affinity = Descriptor->u.Interrupt.Affinity;

                    if ((SynchronizeIrql != 0) && (SynchronizeIrql < Irql))
                    {
                        return STATUS_INVALID_PARAMETER;
                    }
                    if ((Affinity & KeActiveProcessors) == 0)
                    {
                        Affinity = KeActiveProcessors;
                    }

                    return IoConnectInterrupt(InterruptObject,
                                              ServiceRoutine,
                                              ServiceContext,
                                              SpinLock,
                                              Descriptor->u.Interrupt.Vector,
                                              Irql,
                                              SynchronizeIrql ? SynchronizeIrql : Irql,
                                              (Descriptor->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ?
                                                  Latched : LevelSensitive,
                                              TRUE,
                                              Affinity,
                                              FloatingSave);
                }
            }
            Full = CmiGetNextResourceDescriptor(Full);
        }
    }

    return STATUS_NOT_FOUND;
}

/**
 * @brief
 * Tears down a message-based connect: every message interrupt is
 * disconnected and the table freed.
 */
static
VOID
IopDisconnectMessageInterrupts(
    _In_ PIO_INTERRUPT_MESSAGE_INFO Table)
{
    PIOP_MESSAGE_INTERRUPTS Messages;
    INTERRUPT_CONNECTION_DATA Single;
    ULONG i;

    PAGED_CODE();

    Messages = CONTAINING_RECORD(Table, IOP_MESSAGE_INTERRUPTS, Table);

    for (i = 0; i < Table->MessageCount; i++)
    {
        if (Table->MessageInfo[i].InterruptObject != NULL)
        {
            IoDisconnectInterrupt(Table->MessageInfo[i].InterruptObject);
        }
        if (Messages->ConnectionData != NULL)
        {
            RtlZeroMemory(&Single, sizeof(Single));
            Single.Count = 1;
            Single.Vectors[0] = Messages->ConnectionData->Vectors[Messages->Links[i].MessageIndex];
            HalDisableInterrupt(&Single);
        }
    }

    if (Messages->ConnectionData != NULL)
    {
        ExFreePoolWithTag(Messages->ConnectionData, TAG_IO_INTERRUPT);
    }
    if (Messages->Links != NULL)
    {
        ExFreePoolWithTag(Messages->Links, TAG_IO_INTERRUPT);
    }
    ExFreePoolWithTag(Messages, TAG_IO_INTERRUPT);
}

/**
 * @brief
 * Message-based connect (CONNECT_MESSAGE_BASED): connects every message
 * the device was assigned and returns the table the driver programs into
 * the device.
 */
static
NTSTATUS
IopConnectMessageInterrupts(
    _In_ PDEVICE_OBJECT Pdo,
    _Out_ PIO_INTERRUPT_MESSAGE_INFO *MessageTable,
    _In_ PKMESSAGE_SERVICE_ROUTINE MessageServiceRoutine,
    _In_opt_ PVOID ServiceContext,
    _In_opt_ PKSPIN_LOCK SpinLock,
    _In_ KIRQL SynchronizeIrql,
    _In_ BOOLEAN FloatingSave)
{
    PINTERRUPT_CONNECTION_DATA ConnectionData;
    PIOP_MESSAGE_INTERRUPTS Messages = NULL;
    PIO_INTERRUPT_MESSAGE_INFO Table;
    ULONG Count = 0, i;
    KIRQL UnifiedIrql = 0;
    NTSTATUS Status;

    PAGED_CODE();

    *MessageTable = NULL;
    if ((Pdo == NULL) || (MessageServiceRoutine == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status = IopReadInterruptConnectionData(Pdo, &ConnectionData);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Count the messages and settle the IRQL they all run at: the caller's
       synchronization IRQL if given, else the highest message IRQL when the
       messages share a spin lock, else each message's own */
    for (i = 0; i < ConnectionData->Count; i++)
    {
        PINTERRUPT_VECTOR_DATA VectorData = &ConnectionData->Vectors[i];

        if (!IopIsMessageVector(VectorData))
        {
            continue;
        }
        Count++;

        if (SynchronizeIrql != 0)
        {
            if (SynchronizeIrql < VectorData->Irql)
            {
                Status = STATUS_INVALID_PARAMETER;
                goto Cleanup;
            }
        }
        else if ((SpinLock != NULL) && (VectorData->Irql > UnifiedIrql))
        {
            UnifiedIrql = VectorData->Irql;
        }
    }
    if (Count == 0)
    {
        Status = STATUS_NOT_FOUND;
        goto Cleanup;
    }
    if (SynchronizeIrql != 0)
    {
        UnifiedIrql = SynchronizeIrql;
    }

    Messages = ExAllocatePoolZero(NonPagedPool, IOP_MESSAGE_TABLE_SIZE(Count), TAG_IO_INTERRUPT);
    if (Messages == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    Messages->Links = ExAllocatePoolZero(NonPagedPool,
                                         Count * sizeof(IOP_MESSAGE_INTERRUPT_LINK),
                                         TAG_IO_INTERRUPT);
    if (Messages->Links == NULL)
    {
        ExFreePoolWithTag(Messages, TAG_IO_INTERRUPT);
        Messages = NULL;
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    Messages->ConnectionData = ConnectionData;
    Table = &Messages->Table;
    Table->UnifiedIrql = UnifiedIrql;

    for (i = 0; i < ConnectionData->Count; i++)
    {
        PINTERRUPT_VECTOR_DATA VectorData = &ConnectionData->Vectors[i];
        PINTERRUPT_VECTOR_DATA Resolved = VectorData;
        PIO_INTERRUPT_MESSAGE_INFO_ENTRY Entry;
        PIOP_MESSAGE_INTERRUPT_LINK Link;
        INTERRUPT_CONNECTION_DATA Routed;
        HAL_MESSAGE_TARGET_REQUEST Request;

        if (!IopIsMessageVector(VectorData))
        {
            continue;
        }

        Link = &Messages->Links[Table->MessageCount];
        Link->ServiceRoutine = MessageServiceRoutine;
        Link->ServiceContext = ServiceContext;
        Link->MessageIndex = i;

        Entry = &Table->MessageInfo[Table->MessageCount];
        Status = IopConnectVectorData(VectorData,
                                      IopMessageInterruptService,
                                      Link,
                                      SpinLock,
                                      UnifiedIrql,
                                      FALSE,
                                      FloatingSave,
                                      &Entry->InterruptObject);
        if (!NT_SUCCESS(Status))
        {
            goto Cleanup;
        }
        Table->MessageCount++;

        /* A bare request still needs the HAL to form the message */
        if (VectorData->Type == InterruptTypeMessageRequest)
        {
            RtlZeroMemory(&Request, sizeof(Request));
            Request.Type = InterruptTargetTypeApic;
            Request.Apic.Vector = VectorData->Vector;
            Request.Apic.TargetProcessors = VectorData->TargetProcessors;
            Request.Apic.DestinationMode = VectorData->MessageRequest.DestinationMode;
            Request.Apic.IntRemapInfo = VectorData->IntRemapInfo;

            Status = HalGetMessageRoutingInfo(&Request, &Routed);
            if (!NT_SUCCESS(Status))
            {
                goto Cleanup;
            }
            Resolved = &Routed.Vectors[0];
        }

        Entry->MessageAddress = Resolved->XapicMessage.Address;
        Entry->MessageData = Resolved->XapicMessage.DataPayload;
        Entry->TargetProcessorSet = VectorData->TargetProcessors.Mask;
        Entry->Vector = VectorData->Vector;
        Entry->Irql = VectorData->Irql;
        Entry->Mode = VectorData->Mode;
        Entry->Polarity = VectorData->Polarity;
    }

    *MessageTable = Table;
    return STATUS_SUCCESS;

Cleanup:
    if (Messages != NULL)
    {
        /* Also frees the connection data */
        IopDisconnectMessageInterrupts(&Messages->Table);
    }
    else if (ConnectionData != NULL)
    {
        ExFreePoolWithTag(ConnectionData, TAG_IO_INTERRUPT);
    }
    return Status;
}

/**
 * @brief
 * Fully specified connect (CONNECT_FULLY_SPECIFIED and
 * CONNECT_FULLY_SPECIFIED_GROUP): connects the element of the device's
 * connection data that matches the vector the caller names.
 *
 * @param[in,out] Parameters
 * The caller's connect parameters. ShareVector is updated when the matched
 * element demands it.
 *
 * @param[in] Group
 * The processor group the caller asked for. Only CONNECT_FULLY_SPECIFIED_GROUP
 * names one; plain CONNECT_FULLY_SPECIFIED always means group 0.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_PARAMETER for a malformed request, or
 * STATUS_NOT_SUPPORTED when nothing in the connection data matches.
 *
 * @remarks
 * An Irql of PASSIVE_LEVEL is a wildcard, not a request to connect at
 * PASSIVE_LEVEL: it means the caller is content with whatever IRQL the
 * arbiter assigned, and the IRQL is then taken from the connection data.
 * Drivers that let the arbiter place their interrupt (sdbus.sys among them)
 * pass zero here, and forwarding that zero straight to KeInitializeInterrupt
 * as a real IRQL is what used to take the machine down.
 */
static
NTSTATUS
IopConnectInterruptExFullySpecific(
    _Inout_ PIO_CONNECT_INTERRUPT_PARAMETERS Parameters,
    _In_ USHORT Group)
{
    PINTERRUPT_CONNECTION_DATA ConnectionData;
    PINTERRUPT_VECTOR_DATA VectorData = NULL;
    BOOLEAN IrqlIsWildcard;
    NTSTATUS Status;
    ULONG i;

    PAGED_CODE();

    if ((Parameters->FullySpecified.PhysicalDeviceObject == NULL) ||
        (Parameters->FullySpecified.ServiceRoutine == NULL) ||
        (Parameters->FullySpecified.InterruptObject == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* The interrupt cannot be synchronized below the IRQL it runs at */
    if (Parameters->FullySpecified.SynchronizeIrql < Parameters->FullySpecified.Irql)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * Both IRQLs left at zero says "place this wherever the arbiter put it".
     * A caller doing that has no IRQL to raise a spin lock to, so it must not
     * have supplied one.
     */
    IrqlIsWildcard = (Parameters->FullySpecified.Irql == PASSIVE_LEVEL) &&
                     (Parameters->FullySpecified.SynchronizeIrql == PASSIVE_LEVEL);
    if (IrqlIsWildcard && (Parameters->FullySpecified.SpinLock != NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    *Parameters->FullySpecified.InterruptObject = NULL;

    Status = IopReadInterruptConnectionData(Parameters->FullySpecified.PhysicalDeviceObject,
                                           &ConnectionData);
    if (!NT_SUCCESS(Status))
    {
        /*
         * Nothing published for this device. Without it there is no IRQL to
         * discover, so a wildcard request cannot be honoured; anything else
         * is connected exactly as the caller described it.
         */
        if (IrqlIsWildcard)
        {
            DPRINT1("IoConnectInterruptEx: no connection data for PDO %p, and the "
                    "caller left the IRQL unspecified\n",
                    Parameters->FullySpecified.PhysicalDeviceObject);
            return STATUS_NOT_SUPPORTED;
        }

        return IoConnectInterrupt(Parameters->FullySpecified.InterruptObject,
                                  Parameters->FullySpecified.ServiceRoutine,
                                  Parameters->FullySpecified.ServiceContext,
                                  Parameters->FullySpecified.SpinLock,
                                  Parameters->FullySpecified.Vector,
                                  Parameters->FullySpecified.Irql,
                                  Parameters->FullySpecified.SynchronizeIrql,
                                  Parameters->FullySpecified.InterruptMode,
                                  Parameters->FullySpecified.ShareVector,
                                  Parameters->FullySpecified.ProcessorEnableMask,
                                  Parameters->FullySpecified.FloatingSave);
    }

    for (i = 0; i < ConnectionData->Count; i++)
    {
        PINTERRUPT_VECTOR_DATA Candidate = &ConnectionData->Vectors[i];

        if (Candidate->Vector != Parameters->FullySpecified.Vector)
            continue;

        /* Zero matches any IRQL; anything else has to be the one assigned */
        if ((Parameters->FullySpecified.Irql != PASSIVE_LEVEL) &&
            (Candidate->Irql != Parameters->FullySpecified.Irql))
            continue;

        if (Candidate->Mode != Parameters->FullySpecified.InterruptMode)
            continue;

        if (Candidate->TargetProcessors.Group != Group)
            continue;

        if (Candidate->TargetProcessors.Mask !=
            Parameters->FullySpecified.ProcessorEnableMask)
            continue;

        VectorData = Candidate;
        break;
    }

    if (VectorData == NULL)
    {
        /*
         * The arbiter published this device's interrupts and the caller is
         * asking for one that is not among them. The vector a driver passes
         * here is the one it read out of its own translated resource, so the
         * two disagreeing means the resource and the property were produced
         * by different code - see IopTranslateResourceListEntry, which takes
         * the translated form from this same property for exactly that
         * reason.
         */
        DPRINT1("IoConnectInterruptEx: PDO %p publishes no vector %lu at irql %u "
                "mode %u group %u affinity 0x%p\n",
                Parameters->FullySpecified.PhysicalDeviceObject,
                Parameters->FullySpecified.Vector,
                Parameters->FullySpecified.Irql,
                Parameters->FullySpecified.InterruptMode,
                Group,
                (PVOID)(ULONG_PTR)Parameters->FullySpecified.ProcessorEnableMask);
        ExFreePoolWithTag(ConnectionData, TAG_IO_INTERRUPT);
        return STATUS_NOT_SUPPORTED;
    }

    /* A wake-capable controller input is shared with the wake path */
    if ((VectorData->Type == InterruptTypeControllerInput) &&
        (VectorData->ControllerInput.WakeInterrupt != 0))
    {
        Parameters->FullySpecified.ShareVector = TRUE;
    }

    Status = IopConnectVectorData(VectorData,
                                  Parameters->FullySpecified.ServiceRoutine,
                                  Parameters->FullySpecified.ServiceContext,
                                  Parameters->FullySpecified.SpinLock,
                                  Parameters->FullySpecified.SynchronizeIrql,
                                  Parameters->FullySpecified.ShareVector,
                                  Parameters->FullySpecified.FloatingSave,
                                  Parameters->FullySpecified.InterruptObject);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IopConnectInterruptExFullySpecific() failed: 0x%lx\n", Status);
    }

    ExFreePoolWithTag(ConnectionData, TAG_IO_INTERRUPT);
    return Status;
}

static
NTSTATUS
NTAPI
IopConnectInterruptExWorker(
    _Inout_ PIO_CONNECT_INTERRUPT_PARAMETERS Parameters)
{
    NTSTATUS Status;

    PAGED_CODE();


    switch (Parameters->Version)
    {
        case CONNECT_FULLY_SPECIFIED:
            return IopConnectInterruptExFullySpecific(Parameters, 0);

        case CONNECT_FULLY_SPECIFIED_GROUP:
            return IopConnectInterruptExFullySpecific(Parameters,
                                                      Parameters->FullySpecified.Group);

        case CONNECT_LINE_BASED:
            return IopConnectLineInterrupt(Parameters->LineBased.PhysicalDeviceObject,
                                           Parameters->LineBased.InterruptObject,
                                           Parameters->LineBased.ServiceRoutine,
                                           Parameters->LineBased.ServiceContext,
                                           Parameters->LineBased.SpinLock,
                                           Parameters->LineBased.SynchronizeIrql,
                                           Parameters->LineBased.FloatingSave);

        case CONNECT_MESSAGE_BASED:
        {
            PIO_INTERRUPT_MESSAGE_INFO *MessageTable =
                Parameters->MessageBased.ConnectionContext.InterruptMessageTable;

            Status = IopConnectMessageInterrupts(Parameters->MessageBased.PhysicalDeviceObject,
                                                 MessageTable,
                                                 Parameters->MessageBased.MessageServiceRoutine,
                                                 Parameters->MessageBased.ServiceContext,
                                                 Parameters->MessageBased.SpinLock,
                                                 Parameters->MessageBased.SynchronizeIrql,
                                                 Parameters->MessageBased.FloatingSave);
            if (NT_SUCCESS(Status))
            {
                return Status;
            }

            /* No messages for this device: connect its line instead, if the
               caller supplied a routine for that, and say so */
            if (Parameters->MessageBased.FallBackServiceRoutine != NULL)
            {
                PKINTERRUPT *InterruptObject =
                    Parameters->MessageBased.ConnectionContext.InterruptObject;

                Status = IopConnectLineInterrupt(Parameters->MessageBased.PhysicalDeviceObject,
                                                 InterruptObject,
                                                 Parameters->MessageBased.FallBackServiceRoutine,
                                                 Parameters->MessageBased.ServiceContext,
                                                 Parameters->MessageBased.SpinLock,
                                                 Parameters->MessageBased.SynchronizeIrql,
                                                 Parameters->MessageBased.FloatingSave);
                if (NT_SUCCESS(Status))
                {
                    Parameters->Version = CONNECT_LINE_BASED;
                }
                return Status;
            }
            return Status;
        }

        default:
            /* Tell the caller the newest form this kernel speaks */
            Parameters->Version = CONNECT_MESSAGE_BASED;
            return STATUS_NOT_SUPPORTED;
    }
}

/**
 * @brief
 * Connects a device's interrupt, in whichever of the four forms the caller
 * describes it.
 *
 * @param[in,out] Parameters
 * The connect parameters. On an unsupported version the Version field is
 * set to the newest form this kernel speaks.
 *
 * @return
 * STATUS_SUCCESS, or a failure from the form-specific connect.
 */
NTSTATUS
NTAPI
IoConnectInterruptEx(
    _Inout_ PIO_CONNECT_INTERRUPT_PARAMETERS Parameters)
{
    NTSTATUS Status;

    PAGED_CODE();

    /*
     * Trace what the caller asked for and what it got. A request we cannot
     * honour otherwise disappears into the form-specific connect with
     * nothing on the debug port to say what was wanted.
     */
    DPRINT1("IoConnectInterruptEx: version %lu\n", Parameters->Version);
    if ((Parameters->Version == CONNECT_FULLY_SPECIFIED) ||
        (Parameters->Version == CONNECT_FULLY_SPECIFIED_GROUP))
    {
        DPRINT1("  fully specified: pdo %p vector %lu irql %u syncirql %u\n",
                Parameters->FullySpecified.PhysicalDeviceObject,
                Parameters->FullySpecified.Vector,
                Parameters->FullySpecified.Irql,
                Parameters->FullySpecified.SynchronizeIrql);
        DPRINT1("  mode %u share %u floatsave %u affinity 0x%p isr %p\n",
                Parameters->FullySpecified.InterruptMode,
                Parameters->FullySpecified.ShareVector,
                Parameters->FullySpecified.FloatingSave,
                (PVOID)(ULONG_PTR)Parameters->FullySpecified.ProcessorEnableMask,
                Parameters->FullySpecified.ServiceRoutine);
    }
    else if (Parameters->Version == CONNECT_MESSAGE_BASED)
    {
        DPRINT1("  message based: pdo %p syncirql %u floatsave %u isr %p fallback %p\n",
                Parameters->MessageBased.PhysicalDeviceObject,
                Parameters->MessageBased.SynchronizeIrql,
                Parameters->MessageBased.FloatingSave,
                Parameters->MessageBased.MessageServiceRoutine,
                Parameters->MessageBased.FallBackServiceRoutine);
    }

    Status = IopConnectInterruptExWorker(Parameters);

    DPRINT1("IoConnectInterruptEx: -> 0x%08lx (version now %lu)\n",
            Status, Parameters->Version);
    return Status;
}

VOID
NTAPI
IoDisconnectInterruptEx(
    _In_ PIO_DISCONNECT_INTERRUPT_PARAMETERS Parameters)
{
    PAGED_CODE();

    if (Parameters->ConnectionContext.Generic == NULL)
    {
        return;
    }

    if (Parameters->Version == CONNECT_MESSAGE_BASED)
    {
        IopDisconnectMessageInterrupts(Parameters->ConnectionContext.InterruptMessageTable);
    }
    else
    {
        IoDisconnectInterrupt(Parameters->ConnectionContext.InterruptObject);
    }
}

/**
 * @brief
 * Returns the processor affinity an interrupt object is bound to.
 *
 * @param[in] InterruptObject
 * The connected interrupt object to query.
 *
 * @param[out] GroupAffinity
 * Receives the group and processor mask the interrupt is serviced on.
 *
 * @return
 * STATUS_SUCCESS on success, or STATUS_INVALID_PARAMETER when either
 * argument is NULL.
 *
 * @remarks
 * ReactOS models a single processor group, so the group is always 0 and the
 * mask is derived from the processor the interrupt object was connected on.
 */
NTSTATUS
NTAPI
IoGetAffinityInterrupt(
    _In_ PKINTERRUPT InterruptObject,
    _Out_ PGROUP_AFFINITY GroupAffinity)
{
    if (InterruptObject == NULL || GroupAffinity == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    GroupAffinity->Group = 0;
    GroupAffinity->Reserved[0] = 0;
    GroupAffinity->Reserved[1] = 0;
    GroupAffinity->Reserved[2] = 0;
    GroupAffinity->Mask = (KAFFINITY)1 << InterruptObject->Number;

    return STATUS_SUCCESS;
}

/* EOF */
