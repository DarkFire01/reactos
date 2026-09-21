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

/* GLOBALS *******************************************************************/

/*
 * Passive level state of one vector. A level line stays masked from the time
 * its passive routines are queued until the worker has run them.
 */
typedef struct _IOP_PASSIVE_INTERRUPT
{
    LIST_ENTRY ListEntry;
    ULONG Vector;
    ULONG Gsiv;
    KINTERRUPT_MODE InterruptMode;
    KSPIN_LOCK SpinLock;
    BOOLEAN WorkerInProgress;
    BOOLEAN WorkPending;
    WORK_QUEUE_ITEM Worker;
    KDPC Dpc;
    KEVENT DispatcherLock;
    volatile LONG ReferenceCount;
} IOP_PASSIVE_INTERRUPT, *PIOP_PASSIVE_INTERRUPT;

static LIST_ENTRY IopPassiveInterruptList = { &IopPassiveInterruptList, &IopPassiveInterruptList };
static KSPIN_LOCK IopPassiveInterruptListLock;

/* A secondary interrupt connected on every processor of its affinity */
typedef struct _IOP_SECONDARY_INTERRUPT
{
    KSPIN_LOCK SpinLock;
    INTERRUPT_CONNECTION_DATA ConnectionData;
    KEVENT PassiveEvent;
    BOOLEAN Passive;
    UCHAR Count;
    PKINTERRUPT Interrupts[MAXIMUM_PROCESSORS];
    KINTERRUPT Objects[ANYSIZE_ARRAY];
} IOP_SECONDARY_INTERRUPT, *PIOP_SECONDARY_INTERRUPT;

/* PASSIVE AND SECONDARY INTERRUPTS ******************************************/

static
KIRQL
IopAcquirePassiveLock(
    _Inout_ PKSPIN_LOCK SpinLock)
{
    KIRQL OldIrql;

    KeRaiseIrql(KI_HIGHEST_DEVICE_IRQL, &OldIrql);
    KeAcquireSpinLockAtDpcLevel(SpinLock);
    return OldIrql;
}

static
VOID
IopReleasePassiveLock(
    _Inout_ PKSPIN_LOCK SpinLock,
    _In_ KIRQL OldIrql)
{
    KeReleaseSpinLockFromDpcLevel(SpinLock);
    KeLowerIrql(OldIrql);
}

/* Takes a reference on the passive state of a vector. The list lock is held. */
static
PIOP_PASSIVE_INTERRUPT
IopFindPassiveInterruptLocked(
    _In_ ULONG Vector)
{
    PIOP_PASSIVE_INTERRUPT Passive;
    PLIST_ENTRY ListEntry;

    for (ListEntry = IopPassiveInterruptList.Flink;
         ListEntry != &IopPassiveInterruptList;
         ListEntry = ListEntry->Flink)
    {
        Passive = CONTAINING_RECORD(ListEntry, IOP_PASSIVE_INTERRUPT, ListEntry);
        if (Passive->Vector == Vector)
        {
            InterlockedIncrement(&Passive->ReferenceCount);
            return Passive;
        }
    }

    return NULL;
}

static
PIOP_PASSIVE_INTERRUPT
IopFindPassiveInterrupt(
    _In_ ULONG Vector)
{
    PIOP_PASSIVE_INTERRUPT Passive;
    KIRQL OldIrql;

    OldIrql = IopAcquirePassiveLock(&IopPassiveInterruptListLock);
    Passive = IopFindPassiveInterruptLocked(Vector);
    IopReleasePassiveLock(&IopPassiveInterruptListLock, OldIrql);

    return Passive;
}

static
VOID
IopDereferencePassiveInterrupt(
    _Inout_ PIOP_PASSIVE_INTERRUPT Passive)
{
    BOOLEAN Free = FALSE;
    KIRQL ListIrql, OldIrql;

    ListIrql = IopAcquirePassiveLock(&IopPassiveInterruptListLock);
    OldIrql = IopAcquirePassiveLock(&Passive->SpinLock);

    if (InterlockedDecrement(&Passive->ReferenceCount) == 0)
    {
        RemoveEntryList(&Passive->ListEntry);
        Free = TRUE;
    }

    IopReleasePassiveLock(&Passive->SpinLock, OldIrql);
    IopReleasePassiveLock(&IopPassiveInterruptListLock, ListIrql);

    if (Free)
        ExFreePoolWithTag(Passive, TAG_PASSIVE_INTERRUPT);
}

/* Runs the passive routines of a vector until no more work was queued */
static
VOID
NTAPI
IopPassiveInterruptWorker(
    _In_ PVOID Context)
{
    PIOP_PASSIVE_INTERRUPT Passive = Context;
    KIRQL OldIrql;

    KeWaitForSingleObject(&Passive->DispatcherLock, Executive, KernelMode, FALSE, NULL);

    OldIrql = IopAcquirePassiveLock(&Passive->SpinLock);
    while (Passive->WorkPending)
    {
        Passive->WorkPending = FALSE;
        IopReleasePassiveLock(&Passive->SpinLock, OldIrql);

        KiDispatchSecondaryInterrupt(Passive->Vector, TRUE, NULL);

        OldIrql = IopAcquirePassiveLock(&Passive->SpinLock);
    }
    Passive->WorkerInProgress = FALSE;
    IopReleasePassiveLock(&Passive->SpinLock, OldIrql);

    KeSetEvent(&Passive->DispatcherLock, IO_NO_INCREMENT, FALSE);

    if ((Passive->InterruptMode == LevelSensitive) && (HalUnmaskInterrupt != NULL))
        HalUnmaskInterrupt(Passive->Gsiv, HAL_UNMASK_INTERRUPT_PASSIVE);

    IopDereferencePassiveInterrupt(Passive);
}

static
VOID
NTAPI
IopPassiveInterruptDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PIOP_PASSIVE_INTERRUPT Passive = DeferredContext;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    ExQueueWorkItem(&Passive->Worker, CriticalWorkQueue);
}

/**
 * @brief
 * Queues the passive routines of a vector to its worker. Called by the
 * secondary dispatch when it reaches one above DISPATCH_LEVEL.
 */
VOID
NTAPI
IopProcessPassiveInterrupts(
    _In_ ULONG Vector)
{
    PIOP_PASSIVE_INTERRUPT Passive;
    KIRQL OldIrql;

    Passive = IopFindPassiveInterrupt(Vector);
    if (Passive == NULL)
        return;

    if ((Passive->InterruptMode == LevelSensitive) && (HalMaskInterrupt != NULL))
        HalMaskInterrupt(Passive->Gsiv, HAL_MASK_INTERRUPT_PASSIVE);

    OldIrql = IopAcquirePassiveLock(&Passive->SpinLock);

    Passive->WorkPending = TRUE;
    if (!Passive->WorkerInProgress)
    {
        /* The worker drops this reference when it is done */
        Passive->WorkerInProgress = TRUE;
        KeInsertQueueDpc(&Passive->Dpc, NULL, NULL);
    }
    else
    {
        /* The running worker holds a reference, so this cannot be the last */
        InterlockedDecrement(&Passive->ReferenceCount);
    }

    IopReleasePassiveLock(&Passive->SpinLock, OldIrql);
}

/* Gives a passive connection a reference on the passive state of its vector */
static
NTSTATUS
IopReferencePassiveInterrupt(
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    PIOP_PASSIVE_INTERRUPT Passive, Existing;
    KIRQL OldIrql;

    if ((ConnectionData->Count != 1) ||
        (ConnectionData->Vectors[0].Type != InterruptTypeControllerInput))
    {
        return STATUS_INVALID_PARAMETER_1;
    }

    /* Connections sharing a vector share its passive state */
    if (IopFindPassiveInterrupt(ConnectionData->Vectors[0].Vector) != NULL)
        return STATUS_SUCCESS;

    Passive = ExAllocatePoolZero(NonPagedPool, sizeof(*Passive), TAG_PASSIVE_INTERRUPT);
    if (Passive == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Passive->Vector = ConnectionData->Vectors[0].Vector;
    Passive->Gsiv = ConnectionData->Vectors[0].ControllerInput.Gsiv;
    Passive->InterruptMode = ConnectionData->Vectors[0].Mode;
    KeInitializeSpinLock(&Passive->SpinLock);
    ExInitializeWorkItem(&Passive->Worker, IopPassiveInterruptWorker, Passive);
    KeInitializeDpc(&Passive->Dpc, IopPassiveInterruptDpc, Passive);
    KeInitializeEvent(&Passive->DispatcherLock, SynchronizationEvent, TRUE);

    OldIrql = IopAcquirePassiveLock(&IopPassiveInterruptListLock);

    Existing = IopFindPassiveInterruptLocked(Passive->Vector);
    if (Existing == NULL)
    {
        Passive->ReferenceCount = 1;
        InsertTailList(&IopPassiveInterruptList, &Passive->ListEntry);
    }

    IopReleasePassiveLock(&IopPassiveInterruptListLock, OldIrql);

    if (Existing != NULL)
        ExFreePoolWithTag(Passive, TAG_PASSIVE_INTERRUPT);

    return STATUS_SUCCESS;
}

/* Drops the reference a passive connection holds */
static
VOID
IopReleasePassiveInterrupt(
    _In_ ULONG Vector)
{
    PIOP_PASSIVE_INTERRUPT Passive;

    Passive = IopFindPassiveInterrupt(Vector);
    if (Passive == NULL)
        return;

    InterlockedDecrement(&Passive->ReferenceCount);
    IopDereferencePassiveInterrupt(Passive);
}

/**
 * @brief
 * Connects a secondary interrupt with one interrupt object for every active
 * processor it targets. This is the only kind of interrupt that can be
 * connected at passive level.
 *
 * @param[out] InterruptObject
 * Receives the first interrupt object, which IoDisconnectInterrupt takes back.
 */
static
NTSTATUS
IopConnectSecondaryVector(
    _In_ PINTERRUPT_VECTOR_DATA VectorData,
    _In_ PKSERVICE_ROUTINE ServiceRoutine,
    _In_opt_ PVOID ServiceContext,
    _In_opt_ PKSPIN_LOCK SpinLock,
    _In_ KIRQL SynchronizeIrql,
    _In_ BOOLEAN ShareVector,
    _Out_ PKINTERRUPT *InterruptObject)
{
    PIOP_SECONDARY_INTERRUPT Secondary;
    KAFFINITY Affinity;
    NTSTATUS Status;
    UCHAR Count = 0;
    ULONG Number;

    PAGED_CODE();

    *InterruptObject = NULL;

    Affinity = VectorData->TargetProcessors.Mask & KeActiveProcessors;
    if ((VectorData->TargetProcessors.Group != 0) || (Affinity == 0))
        return STATUS_INVALID_PARAMETER;

    for (Number = 0; Number < MAXIMUM_PROCESSORS; Number++)
    {
        if (Affinity & ((KAFFINITY)1 << Number))
            Count++;
    }

    Secondary = ExAllocatePoolZero(NonPagedPool,
                                   FIELD_OFFSET(IOP_SECONDARY_INTERRUPT, Objects) +
                                   Count * sizeof(KINTERRUPT),
                                   TAG_IO_INTERRUPT);
    if (Secondary == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Secondary->ConnectionData.Count = 1;
    Secondary->ConnectionData.Vectors[0] = *VectorData;
    Secondary->Passive = (SynchronizeIrql == PASSIVE_LEVEL);

    if (SpinLock == NULL)
    {
        KeInitializeSpinLock(&Secondary->SpinLock);
        SpinLock = &Secondary->SpinLock;
    }

    for (Number = 0; Secondary->Count < Count; Number++)
    {
        if (!(Affinity & ((KAFFINITY)1 << Number)))
            continue;

        KiInitializeSecondaryInterrupt(&Secondary->Objects[Secondary->Count],
                                       ServiceRoutine,
                                       ServiceContext,
                                       SpinLock,
                                       Secondary->Passive ? &Secondary->PassiveEvent : NULL,
                                       VectorData->Vector,
                                       VectorData->Irql,
                                       SynchronizeIrql,
                                       VectorData->Mode,
                                       ShareVector,
                                       (CHAR)Number);
        Secondary->Interrupts[Secondary->Count] = &Secondary->Objects[Secondary->Count];
        Secondary->Count++;
    }

    if (Secondary->Passive)
    {
        Status = IopReferencePassiveInterrupt(&Secondary->ConnectionData);
        if (!NT_SUCCESS(Status))
            goto Fail;
    }

    Status = KiConnectSecondaryInterrupts(Secondary->Interrupts,
                                          Secondary->Count,
                                          &Secondary->ConnectionData);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Secondary vector 0x%lx (GSIV 0x%lx) did not connect: 0x%lx\n",
                VectorData->Vector, VectorData->ControllerInput.Gsiv, Status);

        if (Secondary->Passive)
            IopReleasePassiveInterrupt(VectorData->Vector);

        goto Fail;
    }

    *InterruptObject = &Secondary->Objects[0];
    return STATUS_SUCCESS;

Fail:
    ExFreePoolWithTag(Secondary, TAG_IO_INTERRUPT);
    return Status;
}

static
VOID
IopDisconnectSecondaryInterrupt(
    _In_ PKINTERRUPT InterruptObject)
{
    PIOP_SECONDARY_INTERRUPT Secondary;
    BOOLEAN InOwnRoutine = FALSE;
    UCHAR Index;

    Secondary = CONTAINING_RECORD(InterruptObject, IOP_SECONDARY_INTERRUPT, Objects);

    KiDisconnectSecondaryInterrupts(Secondary->Interrupts,
                                    Secondary->Count,
                                    &Secondary->ConnectionData);

    if (Secondary->Passive)
    {
        IopReleasePassiveInterrupt(Secondary->ConnectionData.Vectors[0].Vector);

        for (Index = 0; Index < Secondary->Count; Index++)
        {
            if (Secondary->Objects[Index].ServiceThread == KeGetCurrentThread())
                InOwnRoutine = TRUE;
        }

        /* Wait out a passive routine or synchronized call still running */
        if (!InOwnRoutine)
            KeWaitForSingleObject(&Secondary->PassiveEvent, Executive, KernelMode, FALSE, NULL);
    }

    ExFreePoolWithTag(Secondary, TAG_IO_INTERRUPT);
}

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

    if (InterruptObject->Vector >= KI_SECONDARY_VECTOR_BASE)
    {
        IopDisconnectSecondaryInterrupt(InterruptObject);
        return;
    }

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

/* CONNECTION DATA BASED CONNECTS *********************************************/

#ifndef DRIVER_VIOLATION
#define DRIVER_VIOLATION 0x121
#endif

/* Device property with the INTERRUPT_CONNECTION_DATA of the device */
static const DEVPROPKEY IopInterruptConnectionDataKey =
{
    { 0xF0E20F09, 0xD97A, 0x49A9, { 0x80, 0x46, 0xBB, 0x6E, 0x22, 0xE6, 0xBB, 0x2E } },
    2
};

#define IOP_CONNECTION_DATA_SIZE(Count) \
    (FIELD_OFFSET(INTERRUPT_CONNECTION_DATA, Vectors) + (Count) * sizeof(INTERRUPT_VECTOR_DATA))

/* An interrupt connected for one vector of the connection data of a device */
typedef struct _IOP_VECTOR_CONNECTION
{
    PKINTERRUPT Interrupt;
    INTERRUPT_CONNECTION_DATA Vector;
} IOP_VECTOR_CONNECTION, *PIOP_VECTOR_CONNECTION;

/*
 * The line interrupts of a device connected with CONNECT_LINE_BASED. The
 * driver gets a copy of the interrupt object of the first line.
 */
typedef struct _IOP_LINE_INTERRUPTS
{
    KINTERRUPT Interrupt;
    KIRQL UnifiedIrql;
    ULONG Count;
    IOP_VECTOR_CONNECTION Lines[ANYSIZE_ARRAY];
} IOP_LINE_INTERRUPTS, *PIOP_LINE_INTERRUPTS;

/* Context of the interrupt object of one message */
typedef struct _IOP_MESSAGE_INTERRUPT_LINK
{
    PKMESSAGE_SERVICE_ROUTINE ServiceRoutine;
    PVOID ServiceContext;
    ULONG MessageNumber;
    IOP_VECTOR_CONNECTION Connection;
} IOP_MESSAGE_INTERRUPT_LINK, *PIOP_MESSAGE_INTERRUPT_LINK;

/*
 * The messages of a device connected with CONNECT_MESSAGE_BASED. The table
 * returned to the driver is part of this structure, and the links follow the
 * table in the same allocation.
 */
typedef struct _IOP_MESSAGE_INTERRUPTS
{
    PIOP_MESSAGE_INTERRUPT_LINK Links;
    IO_INTERRUPT_MESSAGE_INFO Table;
} IOP_MESSAGE_INTERRUPTS, *PIOP_MESSAGE_INTERRUPTS;

#define IOP_MESSAGE_TABLE_SIZE(Count) \
    (FIELD_OFFSET(IOP_MESSAGE_INTERRUPTS, Table.MessageInfo) + \
     (Count) * sizeof(IO_INTERRUPT_MESSAGE_INFO_ENTRY))

/**
 * @brief
 * Reads the INTERRUPT_CONNECTION_DATA property of a device.
 *
 * @param[out] ConnectionData
 * Receives a copy of the property, or NULL if the property is empty. The
 * caller frees it.
 *
 * @return
 * The status of the property query, or STATUS_DATA_ERROR if the property is
 * shorter than its vector count says.
 */
static
NTSTATUS
IopReadInterruptConnectionData(
    _In_ PDEVICE_OBJECT Pdo,
    _Out_ PINTERRUPT_CONNECTION_DATA *ConnectionData)
{
    PINTERRUPT_CONNECTION_DATA Data;
    DEVPROPTYPE Type;
    ULONG Size = 0;
    NTSTATUS Status;

    PAGED_CODE();

    *ConnectionData = NULL;

    Status = IoGetDevicePropertyData(Pdo, &IopInterruptConnectionDataKey,
                                     0, 0, 0, NULL, &Size, &Type);
    if (Status != STATUS_BUFFER_TOO_SMALL || Size < IOP_CONNECTION_DATA_SIZE(1))
        return Status;

    Data = ExAllocatePoolWithTag(NonPagedPool, Size, TAG_IO_INTERRUPT);
    if (Data == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoGetDevicePropertyData(Pdo, &IopInterruptConnectionDataKey,
                                     0, 0, Size, Data, &Size, &Type);
    if (NT_SUCCESS(Status) && Size < IOP_CONNECTION_DATA_SIZE(Data->Count))
        Status = STATUS_DATA_ERROR;

    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Data, TAG_IO_INTERRUPT);
        return Status;
    }

    *ConnectionData = Data;
    return Status;
}

/* Interrupts are only connected from the connection data of a PnP device */
static
BOOLEAN
IopIsPnpPhysicalDevice(
    _In_opt_ PDEVICE_OBJECT Pdo)
{
    PDEVICE_NODE DeviceNode;

    if (Pdo == NULL)
        return FALSE;

    DeviceNode = IopGetDeviceNode(Pdo);
    return DeviceNode != NULL && !(DeviceNode->Flags & DNF_LEGACY_RESOURCE_DEVICENODE);
}

static
BOOLEAN
IopIsMessageVector(
    _In_ PINTERRUPT_VECTOR_DATA VectorData)
{
    switch (VectorData->Type)
    {
        case InterruptTypeXapicMessage:
        case InterruptTypeHypertransport:
        case InterruptTypeMessageRequest:
            return TRUE;

        default:
            return FALSE;
    }
}

/**
 * @brief
 * Connects an interrupt object for one connection data element on the target
 * processors that are active, then enables the interrupt in the HAL.
 *
 * @param[in] SynchronizeIrql
 * The IRQL the service routine is synchronized at. PASSIVE_LEVEL asks for a
 * passive level interrupt, which only a secondary interrupt can be.
 */
static
NTSTATUS
IopConnectVector(
    _In_ PINTERRUPT_VECTOR_DATA VectorData,
    _In_ PKSERVICE_ROUTINE ServiceRoutine,
    _In_opt_ PVOID ServiceContext,
    _In_opt_ PKSPIN_LOCK SpinLock,
    _In_ KIRQL SynchronizeIrql,
    _In_ BOOLEAN ShareVector,
    _In_ BOOLEAN FloatingSave,
    _Out_ PIOP_VECTOR_CONNECTION Connection)
{
    NTSTATUS Status;

    PAGED_CODE();

    RtlZeroMemory(Connection, sizeof(*Connection));
    Connection->Vector.Count = 1;
    Connection->Vector.Vectors[0] = *VectorData;

    if (KiIsInterruptTypeSecondary(&Connection->Vector))
    {
        return IopConnectSecondaryVector(VectorData,
                                         ServiceRoutine,
                                         ServiceContext,
                                         SpinLock,
                                         SynchronizeIrql,
                                         ShareVector,
                                         &Connection->Interrupt);
    }

    if (SynchronizeIrql == PASSIVE_LEVEL)
    {
        DPRINT1("Passive level interrupts are not supported (vector %lu)\n", VectorData->Vector);
        return STATUS_NOT_SUPPORTED;
    }

    /* The service routine has to be in place before the input is unmasked, or
       a device that is already asserting storms a vector nothing answers */
    Status = IoConnectInterrupt(&Connection->Interrupt,
                                ServiceRoutine,
                                ServiceContext,
                                SpinLock,
                                VectorData->Vector,
                                VectorData->Irql,
                                SynchronizeIrql,
                                VectorData->Mode,
                                ShareVector,
                                VectorData->TargetProcessors.Mask,
                                FloatingSave);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = HalEnableInterrupt(&Connection->Vector);
    if (!NT_SUCCESS(Status))
    {
        IoDisconnectInterrupt(Connection->Interrupt);
        Connection->Interrupt = NULL;
    }

    return Status;
}

static
VOID
IopDisconnectVector(
    _In_ PIOP_VECTOR_CONNECTION Connection)
{
    /* The kernel turns a secondary line off itself once its last object is gone */
    if (KiIsInterruptTypeSecondary(&Connection->Vector))
    {
        IoDisconnectInterrupt(Connection->Interrupt);
        return;
    }

    /* Mask the input first, so that nothing arrives once the object is gone */
    HalDisableInterrupt(&Connection->Vector);
    IoDisconnectInterrupt(Connection->Interrupt);
}

/* Disconnects the line interrupts of a device and frees them */
static
VOID
IopDisconnectLineInterrupts(
    _In_ PIOP_LINE_INTERRUPTS Lines)
{
    ULONG Index;

    for (Index = 0; Index < Lines->Count; Index++)
        IopDisconnectVector(&Lines->Lines[Index]);

    ExFreePoolWithTag(Lines, TAG_IO_INTERRUPT);
}

/**
 * @brief
 * Connects every line interrupt of the connection data of a device, for
 * CONNECT_LINE_BASED. All lines are synchronized at the highest IRQL among
 * them, or at SynchronizeIrql when it is given.
 *
 * @param[out] InterruptObject
 * Receives the interrupt object of the first line.
 */
static
NTSTATUS
IopConnectLineInterrupts(
    _In_opt_ PDEVICE_OBJECT Pdo,
    _Out_ PKINTERRUPT *InterruptObject,
    _In_opt_ PKSERVICE_ROUTINE ServiceRoutine,
    _In_opt_ PVOID ServiceContext,
    _In_opt_ PKSPIN_LOCK SpinLock,
    _In_ KIRQL SynchronizeIrql,
    _In_ BOOLEAN FloatingSave)
{
    PINTERRUPT_CONNECTION_DATA ConnectionData;
    PIOP_LINE_INTERRUPTS Lines = NULL;
    KIRQL HighestIrql = PASSIVE_LEVEL;
    ULONG LineCount = 0;
    ULONG Index;
    NTSTATUS Status;

    PAGED_CODE();

    *InterruptObject = NULL;

    if (!IopIsPnpPhysicalDevice(Pdo) || ServiceRoutine == NULL)
        return STATUS_INVALID_PARAMETER;

    Status = IopReadInterruptConnectionData(Pdo, &ConnectionData);
    if (!NT_SUCCESS(Status) || ConnectionData == NULL)
        return Status;

    for (Index = 0; Index < ConnectionData->Count; Index++)
    {
        if (ConnectionData->Vectors[Index].Type != InterruptTypeControllerInput)
            continue;

        LineCount++;
        HighestIrql = max(HighestIrql, ConnectionData->Vectors[Index].Irql);
    }

    if (LineCount == 0 || (SynchronizeIrql != PASSIVE_LEVEL && SynchronizeIrql < HighestIrql))
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Done;
    }

    Lines = ExAllocatePoolZero(NonPagedPool,
                               FIELD_OFFSET(IOP_LINE_INTERRUPTS, Lines) +
                               LineCount * sizeof(IOP_VECTOR_CONNECTION),
                               TAG_IO_INTERRUPT);
    if (Lines == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    Lines->UnifiedIrql = (SynchronizeIrql != PASSIVE_LEVEL) ? SynchronizeIrql : HighestIrql;

    for (Index = 0; Index < ConnectionData->Count && Lines->Count < LineCount; Index++)
    {
        if (ConnectionData->Vectors[Index].Type != InterruptTypeControllerInput)
            continue;

        Status = IopConnectVector(&ConnectionData->Vectors[Index],
                                  ServiceRoutine,
                                  ServiceContext,
                                  SpinLock,
                                  Lines->UnifiedIrql,
                                  TRUE,
                                  FloatingSave,
                                  &Lines->Lines[Lines->Count]);
        if (!NT_SUCCESS(Status))
            goto Done;

        Lines->Count++;
    }

    /* The copy synchronizes like the interrupt object of the first line */
    RtlCopyMemory(&Lines->Interrupt, Lines->Lines[0].Interrupt, sizeof(KINTERRUPT));
    *InterruptObject = &Lines->Interrupt;

Done:
    ExFreePoolWithTag(ConnectionData, TAG_IO_INTERRUPT);

    if (!NT_SUCCESS(Status) && Lines != NULL)
        IopDisconnectLineInterrupts(Lines);

    return Status;
}

/* Calls the message service routine of the driver with the message number */
static
BOOLEAN
NTAPI
IopMessageInterruptService(
    _In_ PKINTERRUPT Interrupt,
    _In_ PVOID ServiceContext)
{
    PIOP_MESSAGE_INTERRUPT_LINK Link = ServiceContext;

    return Link->ServiceRoutine(Interrupt, Link->ServiceContext, Link->MessageNumber);
}

/* Disconnects the interrupts of a message table and frees it */
static
VOID
IopDisconnectMessageInterrupts(
    _In_ PIO_INTERRUPT_MESSAGE_INFO Table)
{
    PIOP_MESSAGE_INTERRUPTS Messages = CONTAINING_RECORD(Table, IOP_MESSAGE_INTERRUPTS, Table);
    ULONG Index;

    PAGED_CODE();

    for (Index = 0; Index < Table->MessageCount; Index++)
        IopDisconnectVector(&Messages->Links[Index].Connection);

    ExFreePoolWithTag(Messages, TAG_IO_INTERRUPT);
}

/**
 * @brief
 * Connects one message and adds it to the message table. The table entry
 * describes the message as the HAL routes it.
 */
static
NTSTATUS
IopConnectOneMessage(
    _Inout_ PIOP_MESSAGE_INTERRUPTS Messages,
    _In_ PINTERRUPT_VECTOR_DATA VectorData,
    _In_ PKMESSAGE_SERVICE_ROUTINE MessageServiceRoutine,
    _In_opt_ PVOID ServiceContext,
    _In_opt_ PKSPIN_LOCK SpinLock,
    _In_ KIRQL SynchronizeIrql,
    _In_ BOOLEAN FloatingSave)
{
    PIO_INTERRUPT_MESSAGE_INFO Table = &Messages->Table;
    PIO_INTERRUPT_MESSAGE_INFO_ENTRY Entry = &Table->MessageInfo[Table->MessageCount];
    PIOP_MESSAGE_INTERRUPT_LINK Link = &Messages->Links[Table->MessageCount];
    PINTERRUPT_VECTOR_DATA Routed = VectorData;
    INTERRUPT_CONNECTION_DATA RoutingInfo;
    HAL_MESSAGE_TARGET_REQUEST Request;
    NTSTATUS Status;

    Link->ServiceRoutine = MessageServiceRoutine;
    Link->ServiceContext = ServiceContext;
    Link->MessageNumber = Table->MessageCount;

    Status = IopConnectVector(VectorData,
                              IopMessageInterruptService,
                              Link,
                              SpinLock,
                              SynchronizeIrql,
                              TRUE,
                              FloatingSave,
                              &Link->Connection);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The HAL builds the address and data of a message request */
    if (VectorData->Type == InterruptTypeMessageRequest)
    {
        RtlZeroMemory(&Request, sizeof(Request));
        Request.Type = InterruptTargetTypeApic;
        Request.Apic.DestinationMode = VectorData->MessageRequest.DestinationMode;
        Request.Apic.TargetProcessors = VectorData->TargetProcessors;
        Request.Apic.IntRemapInfo = VectorData->IntRemapInfo;
        Request.Apic.Vector = VectorData->Vector;

        Status = HalGetMessageRoutingInfo(&Request, &RoutingInfo);
        if (!NT_SUCCESS(Status))
        {
            IopDisconnectVector(&Link->Connection);
            return Status;
        }

        Routed = &RoutingInfo.Vectors[0];
    }

    Entry->MessageAddress = Routed->XapicMessage.Address;
    Entry->MessageData = Routed->XapicMessage.DataPayload;
    Entry->TargetProcessorSet = Routed->TargetProcessors.Mask;
    Entry->InterruptObject = Link->Connection.Interrupt;
    Entry->Vector = Routed->Vector;
    Entry->Irql = Routed->Irql;
    Entry->Mode = Routed->Mode;
    Entry->Polarity = Routed->Polarity;

    Table->MessageCount++;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Connects all messages of the connection data of a device, for
 * CONNECT_MESSAGE_BASED and CONNECT_MESSAGE_BASED_PASSIVE.
 *
 * @remarks
 * The messages are synchronized at SynchronizeIrql when it is given, at the
 * highest message IRQL when they share a spin lock, and each at its own IRQL
 * otherwise. Passive messages are synchronized at PASSIVE_LEVEL.
 */
static
NTSTATUS
IopConnectMessageInterrupts(
    _In_ ULONG Version,
    _In_opt_ PDEVICE_OBJECT Pdo,
    _Out_ PIO_INTERRUPT_MESSAGE_INFO *MessageTable,
    _In_opt_ PKMESSAGE_SERVICE_ROUTINE MessageServiceRoutine,
    _In_opt_ PVOID ServiceContext,
    _In_opt_ PKSPIN_LOCK SpinLock,
    _In_ KIRQL SynchronizeIrql,
    _In_ BOOLEAN FloatingSave)
{
    PINTERRUPT_CONNECTION_DATA ConnectionData;
    PIOP_MESSAGE_INTERRUPTS Messages = NULL;
    BOOLEAN Passive = (Version == CONNECT_MESSAGE_BASED_PASSIVE);
    KIRQL UnifiedIrql = PASSIVE_LEVEL;
    ULONG MessageCount = 0;
    ULONG VectorCount;
    ULONG Index;
    NTSTATUS Status;

    PAGED_CODE();

    *MessageTable = NULL;

    if (!IopIsPnpPhysicalDevice(Pdo) || MessageServiceRoutine == NULL)
        return STATUS_INVALID_PARAMETER;

    Status = IopReadInterruptConnectionData(Pdo, &ConnectionData);
    if (!NT_SUCCESS(Status))
        return Status;

    VectorCount = (ConnectionData != NULL) ? ConnectionData->Count : 0;

    for (Index = 0; Index < VectorCount; Index++)
    {
        PINTERRUPT_VECTOR_DATA VectorData = &ConnectionData->Vectors[Index];

        if (!IopIsMessageVector(VectorData))
            continue;

        MessageCount++;

        if (Passive)
            continue;

        if (SynchronizeIrql != PASSIVE_LEVEL)
        {
            if (SynchronizeIrql < VectorData->Irql)
            {
                Status = STATUS_INVALID_PARAMETER;
                goto Done;
            }

            UnifiedIrql = SynchronizeIrql;
        }
        else if (SpinLock != NULL)
        {
            UnifiedIrql = max(UnifiedIrql, VectorData->Irql);
        }
    }

    if (MessageCount == 0)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Done;
    }

    Messages = ExAllocatePoolZero(NonPagedPool,
                                  IOP_MESSAGE_TABLE_SIZE(MessageCount) +
                                  MessageCount * sizeof(IOP_MESSAGE_INTERRUPT_LINK),
                                  TAG_IO_INTERRUPT);
    if (Messages == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }

    Messages->Links = (PIOP_MESSAGE_INTERRUPT_LINK)
        ((PUCHAR)Messages + IOP_MESSAGE_TABLE_SIZE(MessageCount));
    Messages->Table.UnifiedIrql = UnifiedIrql;

    for (Index = 0; Index < VectorCount; Index++)
    {
        PINTERRUPT_VECTOR_DATA VectorData = &ConnectionData->Vectors[Index];
        KIRQL ConnectIrql;

        if (!IopIsMessageVector(VectorData))
            continue;

        if (Passive)
            ConnectIrql = PASSIVE_LEVEL;
        else if (UnifiedIrql != PASSIVE_LEVEL)
            ConnectIrql = UnifiedIrql;
        else
            ConnectIrql = VectorData->Irql;

        Status = IopConnectOneMessage(Messages,
                                      VectorData,
                                      MessageServiceRoutine,
                                      ServiceContext,
                                      SpinLock,
                                      ConnectIrql,
                                      FloatingSave);
        if (!NT_SUCCESS(Status))
            goto Done;
    }

    *MessageTable = &Messages->Table;

Done:
    if (ConnectionData != NULL)
        ExFreePoolWithTag(ConnectionData, TAG_IO_INTERRUPT);

    if (!NT_SUCCESS(Status) && Messages != NULL)
        IopDisconnectMessageInterrupts(&Messages->Table);

    return Status;
}

/* Checks if a connection data element is the interrupt a caller specified */
static
BOOLEAN
IopVectorMatchesRequest(
    _In_ PINTERRUPT_VECTOR_DATA VectorData,
    _In_ PIO_CONNECT_INTERRUPT_FULLY_SPECIFIED_PARAMETERS Request,
    _In_ USHORT Group)
{
    /* An IRQL of 0 matches any IRQL */
    if (Request->Irql != PASSIVE_LEVEL && Request->Irql != VectorData->Irql)
        return FALSE;

    return VectorData->Vector == Request->Vector &&
           VectorData->Mode == Request->InterruptMode &&
           VectorData->TargetProcessors.Group == Group &&
           VectorData->TargetProcessors.Mask == Request->ProcessorEnableMask;
}

/**
 * @brief
 * Connects an interrupt for CONNECT_FULLY_SPECIFIED and
 * CONNECT_FULLY_SPECIFIED_GROUP. When the device has connection data, the
 * element that matches the request is connected. Otherwise the interrupt is
 * connected as specified.
 *
 * @param[in] Group
 * The processor group, always 0 for CONNECT_FULLY_SPECIFIED.
 *
 * @remarks
 * Irql and SynchronizeIrql of 0 ask for a passive level interrupt.
 */
static
NTSTATUS
IopConnectFullySpecifiedInterrupt(
    _Inout_ PIO_CONNECT_INTERRUPT_PARAMETERS Parameters,
    _In_ USHORT Group)
{
    PIO_CONNECT_INTERRUPT_FULLY_SPECIFIED_PARAMETERS Request = &Parameters->FullySpecified;
    PINTERRUPT_CONNECTION_DATA ConnectionData;
    IOP_VECTOR_CONNECTION Connection;
    BOOLEAN Passive;
    ULONG VectorCount;
    ULONG Index;
    NTSTATUS Status = STATUS_NOT_SUPPORTED;

    PAGED_CODE();

    if (Request->PhysicalDeviceObject == NULL ||
        Request->ServiceRoutine == NULL ||
        Request->SynchronizeIrql < Request->Irql)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* A passive level interrupt has no spin lock */
    Passive = (Request->Irql == PASSIVE_LEVEL && Request->SynchronizeIrql == PASSIVE_LEVEL);
    if (Passive && Request->SpinLock != NULL)
        return STATUS_INVALID_PARAMETER;

    if (!NT_SUCCESS(IopReadInterruptConnectionData(Request->PhysicalDeviceObject, &ConnectionData)))
    {
        if (Passive)
            return STATUS_NOT_SUPPORTED;

        return IoConnectInterrupt(Request->InterruptObject,
                                  Request->ServiceRoutine,
                                  Request->ServiceContext,
                                  Request->SpinLock,
                                  Request->Vector,
                                  Request->Irql,
                                  Request->SynchronizeIrql,
                                  Request->InterruptMode,
                                  Request->ShareVector,
                                  Request->ProcessorEnableMask,
                                  Request->FloatingSave);
    }

    VectorCount = (ConnectionData != NULL) ? ConnectionData->Count : 0;

    for (Index = 0; Index < VectorCount; Index++)
    {
        PINTERRUPT_VECTOR_DATA Match = &ConnectionData->Vectors[Index];

        if (!IopVectorMatchesRequest(Match, Request, Group))
            continue;

        /* Wake interrupts are always shared */
        if ((Match->Type == InterruptTypeControllerInput) &&
            (Match->ControllerInput.WakeInterrupt != 0))
        {
            Request->ShareVector = TRUE;
        }

        Status = IopConnectVector(Match,
                                  Request->ServiceRoutine,
                                  Request->ServiceContext,
                                  Request->SpinLock,
                                  Request->SynchronizeIrql,
                                  Request->ShareVector,
                                  Request->FloatingSave,
                                  &Connection);
        if (NT_SUCCESS(Status))
            *Request->InterruptObject = Connection.Interrupt;

        break;
    }

    if (Index == VectorCount)
    {
        DPRINT1("IoConnectInterruptEx: PDO %p has no vector %lu at irql %u"
                " mode %u group %u affinity 0x%p\n",
                Request->PhysicalDeviceObject, Request->Vector, Request->Irql,
                Request->InterruptMode, Group, (PVOID)(ULONG_PTR)Request->ProcessorEnableMask);
    }

    if (ConnectionData != NULL)
        ExFreePoolWithTag(ConnectionData, TAG_IO_INTERRUPT);

    return Status;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
IoConnectInterruptEx(
    _Inout_ PIO_CONNECT_INTERRUPT_PARAMETERS Parameters)
{
    PIO_CONNECT_INTERRUPT_MESSAGE_BASED_PARAMETERS MessageBased = &Parameters->MessageBased;
    NTSTATUS Status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        KeBugCheckEx(DRIVER_VIOLATION, 1, KeGetCurrentIrql(), 0, 0);

    DPRINT("IoConnectInterruptEx: version %lu\n", Parameters->Version);

    switch (Parameters->Version)
    {
        case CONNECT_FULLY_SPECIFIED:
            return IopConnectFullySpecifiedInterrupt(Parameters, 0);

        case CONNECT_FULLY_SPECIFIED_GROUP:
            return IopConnectFullySpecifiedInterrupt(Parameters, Parameters->FullySpecified.Group);

        case CONNECT_LINE_BASED:
            return IopConnectLineInterrupts(Parameters->LineBased.PhysicalDeviceObject,
                                            Parameters->LineBased.InterruptObject,
                                            Parameters->LineBased.ServiceRoutine,
                                            Parameters->LineBased.ServiceContext,
                                            Parameters->LineBased.SpinLock,
                                            Parameters->LineBased.SynchronizeIrql,
                                            Parameters->LineBased.FloatingSave);

        case CONNECT_MESSAGE_BASED:
        case CONNECT_MESSAGE_BASED_PASSIVE:
            break;

        default:
            /* Return the highest version that is supported */
            Parameters->Version = CONNECT_MESSAGE_BASED;
            return STATUS_NOT_SUPPORTED;
    }

    Status = IopConnectMessageInterrupts(Parameters->Version,
                                         MessageBased->PhysicalDeviceObject,
                                         MessageBased->ConnectionContext.InterruptMessageTable,
                                         MessageBased->MessageServiceRoutine,
                                         MessageBased->ServiceContext,
                                         MessageBased->SpinLock,
                                         MessageBased->SynchronizeIrql,
                                         MessageBased->FloatingSave);
    if (NT_SUCCESS(Status))
        return Status;

    if (MessageBased->FallBackServiceRoutine == NULL)
        return STATUS_NOT_SUPPORTED;

    /* Tell the caller that line interrupts are connected instead */
    Status = IopConnectLineInterrupts(MessageBased->PhysicalDeviceObject,
                                      MessageBased->ConnectionContext.InterruptObject,
                                      MessageBased->FallBackServiceRoutine,
                                      MessageBased->ServiceContext,
                                      MessageBased->SpinLock,
                                      MessageBased->SynchronizeIrql,
                                      MessageBased->FloatingSave);
    Parameters->Version = CONNECT_LINE_BASED;

    return Status;
}

VOID
NTAPI
IoDisconnectInterruptEx(
    _In_ PIO_DISCONNECT_INTERRUPT_PARAMETERS Parameters)
{
    PAGED_CODE();

    switch (Parameters->Version)
    {
        case CONNECT_FULLY_SPECIFIED:
        case CONNECT_FULLY_SPECIFIED_GROUP:
            IoDisconnectInterrupt(Parameters->ConnectionContext.InterruptObject);
            break;

        case CONNECT_LINE_BASED:
            IopDisconnectLineInterrupts(
                CONTAINING_RECORD(Parameters->ConnectionContext.InterruptObject,
                                  IOP_LINE_INTERRUPTS,
                                  Interrupt));
            break;

        case CONNECT_MESSAGE_BASED:
        case CONNECT_MESSAGE_BASED_PASSIVE:
            IopDisconnectMessageInterrupts(Parameters->ConnectionContext.InterruptMessageTable);
            break;

        default:
            KeBugCheckEx(PNP_DETECTED_FATAL_ERROR, 9, Parameters->Version, 0, 0);
    }
}

/**
 * @brief
 * Returns the processor affinity of an interrupt object. Only processor
 * group 0 is supported.
 */
NTSTATUS
NTAPI
IoGetAffinityInterrupt(
    _In_ PKINTERRUPT InterruptObject,
    _Out_ PGROUP_AFFINITY GroupAffinity)
{
    if (InterruptObject == NULL || GroupAffinity == NULL)
        return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(GroupAffinity, sizeof(*GroupAffinity));
    GroupAffinity->Mask = (KAFFINITY)1 << InterruptObject->Number;

    return STATUS_SUCCESS;
}

/* EOF */
