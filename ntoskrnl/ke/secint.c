/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Secondary interrupt connection and dispatch
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

/* KeDispatchSecondaryInterrupt flag: only run the passive level routines */
#define KE_SECONDARY_DISPATCH_PASSIVE_ONLY  0x00100000

/* Seconds a passive level ISR may run before the debugger is entered */
ULONG KiPassiveWatchdogTimeout = 300;

typedef struct _KSECONDARY_IDT_ENTRY
{
    KSPIN_LOCK SpinLock;
    KEVENT ConnectLock;
    BOOLEAN LineMasked;
    PKINTERRUPT InterruptList;
} KSECONDARY_IDT_ENTRY, *PKSECONDARY_IDT_ENTRY;

/* Where a disconnect waiting on a running ISR gets its result */
typedef struct _KI_DISCONNECT_DATA
{
    PKEVENT Event;
    PINTERRUPT_CONNECTION_DATA ConnectionData;
    NTSTATUS Status;
} KI_DISCONNECT_DATA, *PKI_DISCONNECT_DATA;

typedef struct _KI_PASSIVE_WATCHDOG
{
    KTIMER Timer;
    KDPC Dpc;
    KEVENT Event;
    PKINTERRUPT Interrupt;
} KI_PASSIVE_WATCHDOG, *PKI_PASSIVE_WATCHDOG;

BOOLEAN KiSecondaryInterruptServicesEnabled;
static PKSECONDARY_IDT_ENTRY KiGlobalSecondaryIdt;

/* Disconnects finished above DISPATCH_LEVEL, signaled from a DPC */
static LIST_ENTRY KiSecondarySignalList;
static KSPIN_LOCK KiSecondarySignalListLock;
static KDPC KiSecondarySignalDpc;
static BOOLEAN KiSecondarySignalDpcQueued;

/* PRIVATE FUNCTIONS *********************************************************/

static
PKSECONDARY_IDT_ENTRY
KiSecondaryIdtEntry(
    _In_ ULONG Vector)
{
    ULONG Index = Vector - KI_SECONDARY_VECTOR_BASE;

    if (!KiSecondaryInterruptServicesEnabled || (Index >= KI_SECONDARY_VECTOR_COUNT))
        return NULL;

    return &KiGlobalSecondaryIdt[Index];
}

static
KIRQL
KiAcquireSecondaryIdtLock(
    _Inout_ PKSECONDARY_IDT_ENTRY Entry)
{
    KIRQL OldIrql;

    KeRaiseIrql(KI_HIGHEST_DEVICE_IRQL, &OldIrql);
    KeAcquireSpinLockAtDpcLevel(&Entry->SpinLock);
    return OldIrql;
}

static
VOID
KiReleaseSecondaryIdtLock(
    _Inout_ PKSECONDARY_IDT_ENTRY Entry,
    _In_ KIRQL OldIrql)
{
    KeReleaseSpinLockFromDpcLevel(&Entry->SpinLock);
    KeLowerIrql(OldIrql);
}

/* Serializes connect and disconnect on one vector */
static
VOID
KiAcquireSecondaryConnectLock(
    _Inout_ PKSECONDARY_IDT_ENTRY Entry)
{
    KeEnterCriticalRegion();
    KeWaitForSingleObject(&Entry->ConnectLock, Executive, KernelMode, FALSE, NULL);
}

static
VOID
KiReleaseSecondaryConnectLock(
    _Inout_ PKSECONDARY_IDT_ENTRY Entry)
{
    KeSetEvent(&Entry->ConnectLock, IO_NO_INCREMENT, FALSE);
    KeLeaveCriticalRegion();
}

FORCEINLINE
PKINTERRUPT
KiNextInterrupt(
    _In_ PKINTERRUPT Interrupt)
{
    return CONTAINING_RECORD(Interrupt->InterruptListEntry.Flink, KINTERRUPT, InterruptListEntry);
}

static
PKINTERRUPT
KiFindFirstPassiveInterrupt(
    _In_ PKINTERRUPT Head)
{
    PKINTERRUPT Interrupt = Head;

    do
    {
        if (Interrupt->SynchronizeIrql == PASSIVE_LEVEL)
            return Interrupt;

        Interrupt = KiNextInterrupt(Interrupt);
    } while (Interrupt != Head);

    return NULL;
}

/**
 * @brief
 * Links an interrupt into a shared list, keeping the passive level routines
 * behind every other one.
 */
static
VOID
KiInsertInterruptOrdered(
    _Inout_ PKINTERRUPT Head,
    _Inout_ PKINTERRUPT Interrupt)
{
    PKINTERRUPT Tail, Before = Head;

    if ((Interrupt->SynchronizeIrql != PASSIVE_LEVEL) && (Head->SynchronizeIrql != PASSIVE_LEVEL))
    {
        Tail = CONTAINING_RECORD(Head->InterruptListEntry.Blink, KINTERRUPT, InterruptListEntry);
        if (Tail->SynchronizeIrql == PASSIVE_LEVEL)
            Before = KiFindFirstPassiveInterrupt(Head);
    }

    InsertTailList(&Before->InterruptListEntry, &Interrupt->InterruptListEntry);
}

/**
 * @brief
 * Takes an interrupt off its vector. The caller holds the IDT entry lock.
 *
 * @return
 * STATUS_INTERRUPT_STILL_CONNECTED when other interrupts remain on the vector.
 */
static
NTSTATUS
KiUnlinkSecondaryInterrupt(
    _Inout_ PKINTERRUPT Interrupt)
{
    PKSECONDARY_IDT_ENTRY Entry = &KiGlobalSecondaryIdt[Interrupt->Vector - KI_SECONDARY_VECTOR_BASE];

    if (!Interrupt->Connected)
        return STATUS_INVALID_PARAMETER_1;

    if (Entry->InterruptList == Interrupt)
    {
        if (IsListEmpty(&Interrupt->InterruptListEntry))
            Entry->InterruptList = NULL;
        else
            Entry->InterruptList = KiNextInterrupt(Interrupt);
    }

    RemoveEntryList(&Interrupt->InterruptListEntry);
    Interrupt->Connected = FALSE;

    return (Entry->InterruptList != NULL) ? STATUS_INTERRUPT_STILL_CONNECTED : STATUS_SUCCESS;
}

/**
 * @brief
 * Finishes a disconnect that had to wait for the interrupt's routine to
 * return. The caller holds the IDT entry lock.
 */
static
VOID
KiProcessPendingDisconnect(
    _Inout_ PKINTERRUPT Interrupt,
    _Inout_ PLIST_ENTRY DisconnectList)
{
    PKI_DISCONNECT_DATA DisconnectData;
    NTSTATUS Status;

    if (!(Interrupt->InternalState & KI_INTERRUPT_DISCONNECT_PENDING) || (Interrupt->ActiveCount != 0))
        return;

    Status = KiUnlinkSecondaryInterrupt(Interrupt);

    DisconnectData = Interrupt->DisconnectData;
    if (DisconnectData != NULL)
        DisconnectData->Status = Status;

    /* Unlinked, so the list entry is free to carry it to its waiter */
    InsertTailList(DisconnectList, &Interrupt->InterruptListEntry);
}

/* Wakes the disconnects waiting on each interrupt in the list */
static
VOID
KiProcessDisconnectList(
    _Inout_ PLIST_ENTRY DisconnectList)
{
    PKI_DISCONNECT_DATA DisconnectData;
    PKINTERRUPT Interrupt;
    PKEVENT Event;

    while (!IsListEmpty(DisconnectList))
    {
        Interrupt = CONTAINING_RECORD(RemoveHeadList(DisconnectList), KINTERRUPT, InterruptListEntry);

        /* The waiter may free the interrupt as soon as it is woken */
        DisconnectData = Interrupt->DisconnectData;
        Event = (DisconnectData != NULL) ? DisconnectData->Event : NULL;
        if (Event != NULL)
            KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
    }
}

static
VOID
KiMoveListTail(
    _Inout_ PLIST_ENTRY Destination,
    _Inout_ PLIST_ENTRY Source)
{
    PLIST_ENTRY First;

    if (IsListEmpty(Source))
        return;

    First = Source->Flink;
    RemoveEntryList(Source);
    AppendTailList(Destination, First);
    InitializeListHead(Source);
}

static
VOID
NTAPI
KiSecondarySignalDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    LIST_ENTRY DisconnectList;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    InitializeListHead(&DisconnectList);

    KeRaiseIrql(KI_HIGHEST_DEVICE_IRQL, &OldIrql);
    KeAcquireSpinLockAtDpcLevel(&KiSecondarySignalListLock);

    KiMoveListTail(&DisconnectList, &KiSecondarySignalList);
    KiSecondarySignalDpcQueued = FALSE;

    KeReleaseSpinLockFromDpcLevel(&KiSecondarySignalListLock);
    KeLowerIrql(OldIrql);

    KiProcessDisconnectList(&DisconnectList);
}

static
VOID
KiQueueSecondarySignals(
    _Inout_ PLIST_ENTRY DisconnectList)
{
    KIRQL OldIrql;

    if (IsListEmpty(DisconnectList))
        return;

    KeRaiseIrql(KI_HIGHEST_DEVICE_IRQL, &OldIrql);
    KeAcquireSpinLockAtDpcLevel(&KiSecondarySignalListLock);

    KiMoveListTail(&KiSecondarySignalList, DisconnectList);
    if (!KiSecondarySignalDpcQueued)
    {
        KiSecondarySignalDpcQueued = TRUE;
        KeInsertQueueDpc(&KiSecondarySignalDpc, NULL, NULL);
    }

    KeReleaseSpinLockFromDpcLevel(&KiSecondarySignalListLock);
    KeLowerIrql(OldIrql);
}

static
VOID
NTAPI
KiPassiveIsrWatchdogDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PKI_PASSIVE_WATCHDOG Watchdog = DeferredContext;
    PKINTERRUPT Interrupt = Watchdog->Interrupt;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    KeSetEvent(&Watchdog->Event, IO_NO_INCREMENT, FALSE);

    DbgPrintEx(DPFLTR_DEFAULT_ID,
               DPFLTR_ERROR_LEVEL,
               "\nPassive-level ISR watchdog timeout! Interrupt: %p\n",
               Interrupt);
    DbgBreakPoint();
}

/**
 * @brief
 * Calls one interrupt's service routine, at its synchronize IRQL under its
 * lock, or at PASSIVE_LEVEL holding its passive event.
 *
 * @param[in] InterruptIrql
 * The IRQL the dispatch was entered at.
 */
static
BOOLEAN
KiInvokeInterruptServiceRoutine(
    _Inout_ PKINTERRUPT Interrupt,
    _In_ KIRQL InterruptIrql)
{
    KI_PASSIVE_WATCHDOG Watchdog;
    BOOLEAN WatchdogArmed = FALSE;
    BOOLEAN Raised = FALSE;
    LARGE_INTEGER DueTime;
    BOOLEAN Handled;
    KIRQL OldIrql;

    if (Interrupt->SynchronizeIrql != PASSIVE_LEVEL)
    {
        if (Interrupt->SynchronizeIrql > InterruptIrql)
        {
            KeRaiseIrql(Interrupt->SynchronizeIrql, &OldIrql);
            Raised = TRUE;
        }

        KeAcquireSpinLockAtDpcLevel(Interrupt->ActualLock);
    }
    else
    {
        KeEnterCriticalRegion();
        KeWaitForSingleObject(Interrupt->PassiveEvent, Executive, KernelMode, FALSE, NULL);

        if (KdDebuggerEnabled && (KiPassiveWatchdogTimeout != 0))
        {
            Watchdog.Interrupt = Interrupt;
            KeInitializeEvent(&Watchdog.Event, SynchronizationEvent, FALSE);
            KeInitializeTimerEx(&Watchdog.Timer, NotificationTimer);
            KeInitializeDpc(&Watchdog.Dpc, KiPassiveIsrWatchdogDpc, &Watchdog);

            DueTime.QuadPart = -10000000LL * KiPassiveWatchdogTimeout;
            KeSetTimer(&Watchdog.Timer, DueTime, &Watchdog.Dpc);
            WatchdogArmed = TRUE;
        }
    }

    Interrupt->ServiceThread = KeGetCurrentThread();
    Handled = Interrupt->ServiceRoutine(Interrupt, Interrupt->ServiceContext);

    /* A watchdog that already fired has to finish with our stack first */
    if (WatchdogArmed && !KeCancelTimer(&Watchdog.Timer))
        KeWaitForSingleObject(&Watchdog.Event, Executive, KernelMode, FALSE, NULL);

    Interrupt->ServiceThread = NULL;

    if (Interrupt->SynchronizeIrql != PASSIVE_LEVEL)
    {
        KeReleaseSpinLockFromDpcLevel(Interrupt->ActualLock);
        if (Raised)
            KeLowerIrql(OldIrql);
    }
    else
    {
        KeSetEvent(Interrupt->PassiveEvent, IO_NO_INCREMENT, FALSE);
        KeLeaveCriticalRegion();
    }

    return Handled;
}

/**
 * @brief
 * Puts an interrupt on its secondary vector.
 *
 * @return
 * STATUS_SUCCESS for the first interrupt on the vector,
 * STATUS_INTERRUPT_VECTOR_ALREADY_CONNECTED when it joined others.
 */
static
NTSTATUS
KiConnectSecondaryInterrupt(
    _Inout_ PKINTERRUPT Interrupt)
{
    PKSECONDARY_IDT_ENTRY Entry;
    NTSTATUS Status = STATUS_INVALID_PARAMETER_1;
    PKINTERRUPT Head;
    KIRQL OldIrql;

    if (!KiSecondaryInterruptServicesEnabled)
        return STATUS_UNSUCCESSFUL;

    Entry = KiSecondaryIdtEntry(Interrupt->Vector);
    if ((Entry == NULL) ||
        (Interrupt->Irql > KI_HIGHEST_DEVICE_IRQL) ||
        (Interrupt->Number >= KeNumberProcessors) ||
        ((Interrupt->SynchronizeIrql < Interrupt->Irql) && (Interrupt->SynchronizeIrql != PASSIVE_LEVEL)))
    {
        return STATUS_INVALID_PARAMETER_1;
    }

    KiAcquireSecondaryConnectLock(Entry);
    OldIrql = KiAcquireSecondaryIdtLock(Entry);

    if (!Interrupt->Connected)
    {
        Head = Entry->InterruptList;
        if (Head == NULL)
        {
            InitializeListHead(&Interrupt->InterruptListEntry);
            Entry->InterruptList = Interrupt;
            Entry->LineMasked = FALSE;
            Interrupt->Connected = TRUE;
            Status = STATUS_SUCCESS;
        }
        else if (Interrupt->ShareVector && Head->ShareVector && (Head->Mode == Interrupt->Mode))
        {
            KiInsertInterruptOrdered(Head, Interrupt);

            /* Nothing passive may run ahead of a routine that is not */
            if ((Head->SynchronizeIrql == PASSIVE_LEVEL) && (Interrupt->SynchronizeIrql != PASSIVE_LEVEL))
                Entry->InterruptList = Interrupt;

            Interrupt->Connected = TRUE;
            Status = STATUS_INTERRUPT_VECTOR_ALREADY_CONNECTED;
        }
    }

    KiReleaseSecondaryIdtLock(Entry, OldIrql);
    KiReleaseSecondaryConnectLock(Entry);

    return Status;
}

/**
 * @brief
 * Takes an interrupt off its vector, waiting for its routine when it is
 * running on another thread.
 */
static
NTSTATUS
KiDisconnectSecondaryInterruptCommon(
    _Inout_ PKINTERRUPT Interrupt,
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    PKSECONDARY_IDT_ENTRY Entry = &KiGlobalSecondaryIdt[Interrupt->Vector - KI_SECONDARY_VECTOR_BASE];
    NTSTATUS Status = STATUS_INVALID_PARAMETER_1;
    KI_DISCONNECT_DATA DisconnectData;
    BOOLEAN Pending = FALSE;
    KEVENT Event;
    KIRQL OldIrql;

    DisconnectData.Event = NULL;
    DisconnectData.ConnectionData = ConnectionData;
    DisconnectData.Status = STATUS_SUCCESS;

    /* A routine disconnecting itself cannot wait for itself to return */
    if (Interrupt->ServiceThread != KeGetCurrentThread())
    {
        KeInitializeEvent(&Event, SynchronizationEvent, FALSE);
        DisconnectData.Event = &Event;
    }

    OldIrql = KiAcquireSecondaryIdtLock(Entry);

    if (Interrupt->Connected && !(Interrupt->InternalState & KI_INTERRUPT_DISCONNECT_PENDING))
    {
        if (Interrupt->ActiveCount != 0)
        {
            Interrupt->DisconnectData = (DisconnectData.Event != NULL) ? &DisconnectData : NULL;
            InterlockedOr(&Interrupt->InternalState, KI_INTERRUPT_DISCONNECT_PENDING);
            Pending = TRUE;
        }
        else
        {
            Status = KiUnlinkSecondaryInterrupt(Interrupt);
        }
    }

    KiReleaseSecondaryIdtLock(Entry, OldIrql);

    if (Pending && (DisconnectData.Event != NULL))
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = DisconnectData.Status;
    }

    return Status;
}

static
NTSTATUS
KiDisconnectSecondaryInterrupt(
    _Inout_ PKINTERRUPT Interrupt,
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    PKSECONDARY_IDT_ENTRY Entry;
    BOOLEAN LastOnVector;
    NTSTATUS Status;
    KIRQL OldIrql;

    Entry = KiSecondaryIdtEntry(Interrupt->Vector);
    if (Entry == NULL)
        return STATUS_INVALID_PARAMETER_1;

    KiAcquireSecondaryConnectLock(Entry);

    OldIrql = KiAcquireSecondaryIdtLock(Entry);
    LastOnVector = Interrupt->Connected &&
                   (Entry->InterruptList == Interrupt) &&
                   IsListEmpty(&Interrupt->InterruptListEntry);
    KiReleaseSecondaryIdtLock(Entry, OldIrql);

    /* The controller stops raising the line before its last routine goes */
    if (LastOnVector)
        HalDisableInterrupt(ConnectionData);

    Status = KiDisconnectSecondaryInterruptCommon(Interrupt, ConnectionData);

    KiReleaseSecondaryConnectLock(Entry);
    return Status;
}

/**
 * @brief
 * Masks the line once every interrupt on the vector is disabled.
 *
 * @return
 * STATUS_INTERRUPT_STILL_CONNECTED when an enabled interrupt keeps it open.
 */
static
NTSTATUS
KiMaskSecondaryInterruptLine(
    _In_ ULONG Vector,
    _In_ ULONG Gsiv)
{
    PKSECONDARY_IDT_ENTRY Entry;
    PKINTERRUPT Head, Interrupt;
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN MaskLine = FALSE;
    KIRQL OldIrql;

    Entry = KiSecondaryIdtEntry(Vector);
    if (Entry == NULL)
        return STATUS_INVALID_PARAMETER;

    OldIrql = KiAcquireSecondaryIdtLock(Entry);

    Head = Entry->InterruptList;
    if (!Entry->LineMasked && (Head != NULL))
    {
        Interrupt = Head;
        do
        {
            if (!(Interrupt->InternalState & KINTERRUPT_STATE_DISABLED))
            {
                Status = STATUS_INTERRUPT_STILL_CONNECTED;
                break;
            }

            Interrupt = KiNextInterrupt(Interrupt);
        } while (Interrupt != Head);

        if (Status == STATUS_SUCCESS)
        {
            Entry->LineMasked = TRUE;
            MaskLine = TRUE;
        }
    }

    KiReleaseSecondaryIdtLock(Entry, OldIrql);

    if (MaskLine && (HalMaskInterrupt != NULL))
        HalMaskInterrupt(Gsiv, 0);

    return Status;
}

/**
 * @brief
 * Unmasks the line as soon as one interrupt on the vector is enabled again.
 *
 * @return
 * STATUS_INTERRUPT_STILL_CONNECTED when the line was not masked.
 */
static
NTSTATUS
KiUnmaskSecondaryInterruptLine(
    _In_ ULONG Vector,
    _In_ ULONG Gsiv)
{
    PKSECONDARY_IDT_ENTRY Entry;
    PKINTERRUPT Head, Interrupt;
    BOOLEAN UnmaskLine = FALSE;
    KIRQL OldIrql;

    Entry = KiSecondaryIdtEntry(Vector);
    if (Entry == NULL)
        return STATUS_INVALID_PARAMETER;

    OldIrql = KiAcquireSecondaryIdtLock(Entry);

    if (!Entry->LineMasked)
    {
        KiReleaseSecondaryIdtLock(Entry, OldIrql);
        return STATUS_INTERRUPT_STILL_CONNECTED;
    }

    Head = Entry->InterruptList;
    if (Head != NULL)
    {
        Interrupt = Head;
        do
        {
            if (!(Interrupt->InternalState & KINTERRUPT_STATE_DISABLED))
            {
                Entry->LineMasked = FALSE;
                UnmaskLine = TRUE;
                break;
            }

            Interrupt = KiNextInterrupt(Interrupt);
        } while (Interrupt != Head);
    }

    KiReleaseSecondaryIdtLock(Entry, OldIrql);

    if (!UnmaskLine)
        return STATUS_SUCCESS;

    if (HalUnmaskInterrupt == NULL)
        return STATUS_NOT_SUPPORTED;

    return HalUnmaskInterrupt(Gsiv, 0);
}

/* Disables a set of interrupts and masks the line if nothing else needs it */
static
NTSTATUS
KiMaskSecondaryInterrupts(
    _In_reads_(Count) PKINTERRUPT *Interrupts,
    _In_ UCHAR Count,
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    NTSTATUS Status;
    UCHAR Index;

    for (Index = 0; Index < Count; Index++)
    {
        if (InterlockedOr(&Interrupts[Index]->InternalState, KINTERRUPT_STATE_DISABLED) &
            KINTERRUPT_STATE_DISABLED)
        {
            return STATUS_ALREADY_DISCONNECTED;
        }
    }

    Status = KiMaskSecondaryInterruptLine(Interrupts[0]->Vector,
                                          ConnectionData->Vectors[0].ControllerInput.Gsiv);
    if (Status == STATUS_INTERRUPT_STILL_CONNECTED)
        Status = STATUS_SUCCESS;

    return Status;
}

/* PUBLIC FUNCTIONS **********************************************************/

/**
 * @brief
 * Builds the secondary IDT. The HAL calls this when it hands out the first
 * secondary GSIV.
 *
 * @param[in] HalExports
 * Unused.
 */
NTSTATUS
NTAPI
KeInitializeSecondaryInterruptServices(
    _In_opt_ PVOID HalExports)
{
    PKSECONDARY_IDT_ENTRY Table;
    ULONG Index;

    UNREFERENCED_PARAMETER(HalExports);

    if (KiSecondaryInterruptServicesEnabled)
        return STATUS_SUCCESS;

    Table = ExAllocatePoolZero(NonPagedPool,
                               KI_SECONDARY_VECTOR_COUNT * sizeof(*Table),
                               TAG_SECONDARY_IDT);
    if (Table == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (Index = 0; Index < KI_SECONDARY_VECTOR_COUNT; Index++)
    {
        KeInitializeSpinLock(&Table[Index].SpinLock);
        KeInitializeEvent(&Table[Index].ConnectLock, SynchronizationEvent, TRUE);
    }

    InitializeListHead(&KiSecondarySignalList);
    KeInitializeSpinLock(&KiSecondarySignalListLock);
    KeInitializeDpc(&KiSecondarySignalDpc, KiSecondarySignalDpcRoutine, NULL);
    KiSecondarySignalDpcQueued = FALSE;

    KiGlobalSecondaryIdt = Table;
    KiSecondaryInterruptServicesEnabled = TRUE;

    return STATUS_SUCCESS;
}

BOOLEAN
NTAPI
KiIsInterruptTypeSecondary(
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    if (!KiSecondaryInterruptServicesEnabled ||
        (ConnectionData->Count != 1) ||
        (HalIsInterruptTypeSecondary == NULL))
    {
        return FALSE;
    }

    return HalIsInterruptTypeSecondary(ConnectionData->Vectors[0].Type,
                                       ConnectionData->Vectors[0].ControllerInput.Gsiv);
}

/**
 * @brief
 * Initializes an interrupt object for a secondary vector.
 *
 * @param[in] PassiveEvent
 * Shared by every object of one connection. Required when SynchronizeIrql is
 * PASSIVE_LEVEL, which makes the whole object passive.
 */
VOID
NTAPI
KiInitializeSecondaryInterrupt(
    _Out_ PKINTERRUPT Interrupt,
    _In_ PKSERVICE_ROUTINE ServiceRoutine,
    _In_opt_ PVOID ServiceContext,
    _In_ PKSPIN_LOCK SpinLock,
    _In_opt_ PKEVENT PassiveEvent,
    _In_ ULONG Vector,
    _In_ KIRQL Irql,
    _In_ KIRQL SynchronizeIrql,
    _In_ KINTERRUPT_MODE InterruptMode,
    _In_ BOOLEAN ShareVector,
    _In_ CHAR ProcessorNumber)
{
    KeInitializeInterrupt(Interrupt,
                          ServiceRoutine,
                          ServiceContext,
                          SpinLock,
                          Vector,
                          (SynchronizeIrql != PASSIVE_LEVEL) ? Irql : PASSIVE_LEVEL,
                          SynchronizeIrql,
                          InterruptMode,
                          ShareVector,
                          ProcessorNumber,
                          FALSE);

    if (SynchronizeIrql == PASSIVE_LEVEL)
    {
        ASSERT(PassiveEvent != NULL);
        KeInitializeEvent(PassiveEvent, SynchronizationEvent, TRUE);
        Interrupt->PassiveEvent = PassiveEvent;
    }
}

/**
 * @brief
 * Connects the per-processor objects of one secondary connection. The line is
 * enabled for the first connection on the vector and unmasked for a shared one.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INTERRUPT_VECTOR_ALREADY_CONNECTED for a shared
 * vector, or the failure after every object was disconnected again.
 */
NTSTATUS
NTAPI
KiConnectSecondaryInterrupts(
    _In_reads_(Count) PKINTERRUPT *Interrupts,
    _In_ UCHAR Count,
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    BOOLEAN Shared = FALSE;
    NTSTATUS Status;
    UCHAR Index;

    for (Index = 0; Index < Count; Index++)
    {
        InterlockedAnd(&Interrupts[Index]->InternalState, ~KINTERRUPT_STATE_DISABLED);

        Status = KiConnectSecondaryInterrupt(Interrupts[Index]);
        if (!NT_SUCCESS(Status))
            goto Undo;

        if (Status == STATUS_INTERRUPT_VECTOR_ALREADY_CONNECTED)
            Shared = TRUE;
    }

    if (Shared)
    {
        Status = KiUnmaskSecondaryInterruptLine(Interrupts[0]->Vector,
                                                ConnectionData->Vectors[0].ControllerInput.Gsiv);
        if (NT_SUCCESS(Status))
            return STATUS_INTERRUPT_VECTOR_ALREADY_CONNECTED;
    }
    else
    {
        Status = HalEnableInterrupt(ConnectionData);
        if (NT_SUCCESS(Status))
            return STATUS_SUCCESS;
    }

Undo:
    while (Index != 0)
    {
        Index--;
        KiDisconnectSecondaryInterrupts(&Interrupts[Index], 1, ConnectionData);
    }

    return Status;
}

/**
 * @brief
 * Disables and disconnects the per-processor objects of one secondary
 * connection.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INTERRUPT_STILL_CONNECTED when other connections
 * remain on the vector, or the last failure.
 */
NTSTATUS
NTAPI
KiDisconnectSecondaryInterrupts(
    _In_reads_(Count) PKINTERRUPT *Interrupts,
    _In_ UCHAR Count,
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    NTSTATUS Result = STATUS_SUCCESS;
    NTSTATUS Status;
    UCHAR Index;

    KiMaskSecondaryInterrupts(Interrupts, Count, ConnectionData);

    for (Index = 0; Index < Count; Index++)
    {
        Status = KiDisconnectSecondaryInterrupt(Interrupts[Index], ConnectionData);
        InterlockedOr(&Interrupts[Index]->InternalState, KINTERRUPT_STATE_DISABLED);

        if (!NT_SUCCESS(Status))
            Result = Status;
        else if (Status == STATUS_INTERRUPT_STILL_CONNECTED)
            Result = STATUS_INTERRUPT_STILL_CONNECTED;
    }

    return Result;
}

/**
 * @brief
 * Runs the routines connected to a secondary vector. Level lines stop at the
 * first routine that claims the interrupt, edge lines are rescanned until a
 * whole pass claims nothing. A passive routine reached above DISPATCH_LEVEL
 * is handed to the passive worker for the vector.
 *
 * @param[in] PassiveOnly
 * Start at the first passive routine, as the passive worker does.
 *
 * @param[in,out] DisconnectList
 * Receives disconnects finished here that still have to be signaled. When
 * NULL they are signaled before returning, if the IRQL allows it.
 */
BOOLEAN
NTAPI
KiDispatchSecondaryInterrupt(
    _In_ ULONG Vector,
    _In_ BOOLEAN PassiveOnly,
    _Inout_opt_ PLIST_ENTRY DisconnectList)
{
    PKSECONDARY_IDT_ENTRY Entry;
    PKINTERRUPT Head, Interrupt, Next;
    LIST_ENTRY LocalDisconnectList;
    BOOLEAN Handled = FALSE, HandledThisPass = FALSE, DeferPassive = FALSE;
    KINTERRUPT_MODE Mode;
    ULONG PassiveVector = 0;
    ULONG Calls = 0;
    KIRQL EntryIrql, OldIrql;

    EntryIrql = KeGetCurrentIrql();

    if (DisconnectList == NULL)
        DisconnectList = &LocalDisconnectList;

    InitializeListHead(DisconnectList);

    Entry = KiSecondaryIdtEntry(Vector);
    if (Entry == NULL)
        return FALSE;

    OldIrql = KiAcquireSecondaryIdtLock(Entry);

    Head = Entry->InterruptList;
    if (Head != NULL)
    {
        Head->ActiveCount++;
        Mode = Head->Mode;
        Interrupt = PassiveOnly ? KiFindFirstPassiveInterrupt(Head) : Head;

        while (Interrupt != NULL)
        {
            if ((EntryIrql > DISPATCH_LEVEL) && (Interrupt->SynchronizeIrql == PASSIVE_LEVEL))
            {
                PassiveVector = Interrupt->Vector;
                DeferPassive = TRUE;
                break;
            }

            if (Interrupt->InternalState & KINTERRUPT_STATE_DISABLED)
            {
                Next = KiNextInterrupt(Interrupt);
            }
            else
            {
                Interrupt->ActiveCount++;
                Calls++;

                KiReleaseSecondaryIdtLock(Entry, OldIrql);
                Handled = KiInvokeInterruptServiceRoutine(Interrupt, OldIrql);
                OldIrql = KiAcquireSecondaryIdtLock(Entry);

                Next = KiNextInterrupt(Interrupt);
                Interrupt->ActiveCount--;
                KiProcessPendingDisconnect(Interrupt, DisconnectList);
            }

            if (Mode == Latched)
            {
                if (Handled)
                    HandledThisPass = TRUE;

                if (Next == Head)
                {
                    if (!HandledThisPass || (Calls <= 1))
                    {
                        Handled = TRUE;
                        break;
                    }

                    Calls = 0;
                    HandledThisPass = FALSE;
                    Handled = FALSE;
                }
            }
            else if (Handled || (Next == Head))
            {
                break;
            }

            Interrupt = Next;
        }

        Head->ActiveCount--;
        KiProcessPendingDisconnect(Head, DisconnectList);
    }

    KiReleaseSecondaryIdtLock(Entry, OldIrql);

    if (DeferPassive)
        IopProcessPassiveInterrupts(PassiveVector);

    if (EntryIrql < DISPATCH_LEVEL)
        KiProcessDisconnectList(DisconnectList);

    return Handled;
}

/**
 * @brief
 * Runs the routines connected to a secondary vector. Called by the HAL for
 * the controller that demultiplexed the interrupt.
 *
 * @param[in] Flags
 * KE_SECONDARY_DISPATCH_PASSIVE_ONLY to run only the passive routines.
 *
 * @return
 * TRUE if a routine claimed the interrupt.
 */
BOOLEAN
NTAPI
KeDispatchSecondaryInterrupt(
    _In_ ULONG Vector,
    _In_ ULONG Flags)
{
    LIST_ENTRY DisconnectList;
    BOOLEAN PassiveOnly;
    BOOLEAN Handled;

    PassiveOnly = (Flags & KE_SECONDARY_DISPATCH_PASSIVE_ONLY) != 0;

    if (KeGetCurrentIrql() == PASSIVE_LEVEL)
        return KiDispatchSecondaryInterrupt(Vector, PassiveOnly, NULL);

    Handled = KiDispatchSecondaryInterrupt(Vector, PassiveOnly, &DisconnectList);
    KiQueueSecondarySignals(&DisconnectList);

    return Handled;
}

BOOLEAN
NTAPI
KiSynchronizePassiveInterruptExecution(
    _In_ PKINTERRUPT Interrupt,
    _In_ PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
    _In_opt_ PVOID SynchronizeContext)
{
    BOOLEAN Result;

    KeEnterCriticalRegion();
    KeWaitForSingleObject(Interrupt->PassiveEvent, Executive, KernelMode, FALSE, NULL);

    Result = SynchronizeRoutine(SynchronizeContext);

    KeSetEvent(Interrupt->PassiveEvent, IO_NO_INCREMENT, FALSE);
    KeLeaveCriticalRegion();

    return Result;
}

/* EOF */
