/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Secondary interrupt controller support
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

#define TAG_HAL_SECONDARY_IC        'SlaH'

/* Secondary GSIVs start past every primary input, never below this */
#define HALP_SECONDARY_GSIV_FLOOR   1024
#define HALP_SECONDARY_GSIV_COUNT   512

/* Set in a line's vector once the controller has released it */
#define HALP_SECONDARY_LINE_DISCONNECTED    0x80000000

typedef struct _SECONDARY_INTERRUPT_LINE_STATE
{
    KINTERRUPT_POLARITY Polarity;
    KINTERRUPT_MODE Mode;
    ULONG Vector;
    BOOLEAN Unmasked;
} SECONDARY_INTERRUPT_LINE_STATE, *PSECONDARY_INTERRUPT_LINE_STATE;

typedef struct _SECONDARY_IC_LIST_ENTRY
{
    LIST_ENTRY ListEntry;
    ULONG GsivBase;
    ULONG GsivSize;
    SECONDARY_INTERRUPT_PROVIDER_INTERFACE Interface;
    volatile LONG BusyCount;
    volatile LONG ExclusiveWaiterCount;
    KEVENT NotificationEvent;
    LIST_ENTRY SignalListEntry;
    SECONDARY_INTERRUPT_LINE_STATE State[ANYSIZE_ARRAY];
} SECONDARY_IC_LIST_ENTRY, *PSECONDARY_IC_LIST_ENTRY;

BOOLEAN HalpSecondaryIcServicesEnabled;
ULONG HalpSecondaryGsivRangeStart;
ULONG HalpSecondaryGsivRangeSize;

static LIST_ENTRY HalpSecondaryIcList;
static KSPIN_LOCK HalpSecondaryIcListLock;
static volatile LONG HalpSecondaryGsivAssignedCount;

/* Entries whose exclusive waiter has to be woken from a DPC */
static LIST_ENTRY HalpSecondarySignalList;
static KSPIN_LOCK HalpSecondarySignalListLock;
static KDPC HalpSecondarySignalDpc;
static BOOLEAN HalpSecondarySignalDpcQueued;

/* PRIVATE FUNCTIONS *********************************************************/

static
KIRQL
HalpAcquireSecondaryLock(
    _Inout_ PKSPIN_LOCK SpinLock)
{
    KIRQL OldIrql;

    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    KeAcquireSpinLockAtDpcLevel(SpinLock);
    return OldIrql;
}

static
VOID
HalpReleaseSecondaryLock(
    _Inout_ PKSPIN_LOCK SpinLock,
    _In_ KIRQL OldIrql)
{
    KeReleaseSpinLockFromDpcLevel(SpinLock);
    KeLowerIrql(OldIrql);
}

/**
 * @brief
 * Wakes the exclusive waiters of every entry queued on the signal list.
 */
static
VOID
NTAPI
HalpSecondarySignalDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PSECONDARY_IC_LIST_ENTRY Entry;
    PLIST_ENTRY ListEntry;
    KIRQL OldIrql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    OldIrql = HalpAcquireSecondaryLock(&HalpSecondarySignalListLock);

    while (!IsListEmpty(&HalpSecondarySignalList))
    {
        ListEntry = RemoveHeadList(&HalpSecondarySignalList);
        HalpReleaseSecondaryLock(&HalpSecondarySignalListLock, OldIrql);

        Entry = CONTAINING_RECORD(ListEntry, SECONDARY_IC_LIST_ENTRY, SignalListEntry);
        KeSetEvent(&Entry->NotificationEvent, IO_NO_INCREMENT, FALSE);

        OldIrql = HalpAcquireSecondaryLock(&HalpSecondarySignalListLock);
    }

    HalpSecondarySignalDpcQueued = FALSE;
    HalpReleaseSecondaryLock(&HalpSecondarySignalListLock, OldIrql);
}

static
VOID
HalpQueueSecondarySignal(
    _In_ PSECONDARY_IC_LIST_ENTRY Entry)
{
    KIRQL OldIrql;

    OldIrql = HalpAcquireSecondaryLock(&HalpSecondarySignalListLock);

    InsertTailList(&HalpSecondarySignalList, &Entry->SignalListEntry);
    if (!HalpSecondarySignalDpcQueued)
    {
        HalpSecondarySignalDpcQueued = TRUE;
        KeInsertQueueDpc(&HalpSecondarySignalDpc, NULL, NULL);
    }

    HalpReleaseSecondaryLock(&HalpSecondarySignalListLock, OldIrql);
}

/**
 * @brief
 * Drops a reference taken by one of the lookups below.
 *
 * @param[in] SignalEvent
 * Wake an exclusive waiter once only it is left holding the entry.
 */
static
VOID
HalpReleaseSecondaryIcEntryShared(
    _In_ PSECONDARY_IC_LIST_ENTRY Entry,
    _In_ BOOLEAN SignalEvent)
{
    LONG Remaining;

    Remaining = InterlockedDecrement(&Entry->BusyCount);
    if (!SignalEvent || (Remaining != 1) ||
        (InterlockedCompareExchange(&Entry->ExclusiveWaiterCount, 0, 0) == 0))
    {
        return;
    }

    if (KeGetCurrentIrql() <= DISPATCH_LEVEL)
        KeSetEvent(&Entry->NotificationEvent, IO_NO_INCREMENT, FALSE);
    else
        HalpQueueSecondarySignal(Entry);
}

/**
 * @brief
 * Waits until the caller holds the only reference, and returns with the list
 * lock held so that nothing can look the entry up again.
 */
static
VOID
HalpAcquireSecondaryIcEntryExclusive(
    _In_ PSECONDARY_IC_LIST_ENTRY Entry,
    _Out_ PKIRQL OldIrql)
{
    InterlockedIncrement(&Entry->ExclusiveWaiterCount);

    for (;;)
    {
        KeWaitForSingleObject(&Entry->NotificationEvent, Executive, KernelMode, FALSE, NULL);

        *OldIrql = HalpAcquireSecondaryLock(&HalpSecondaryIcListLock);
        if (InterlockedCompareExchange(&Entry->BusyCount, 2, 1) == 1)
            break;

        HalpReleaseSecondaryLock(&HalpSecondaryIcListLock, *OldIrql);
    }

    InterlockedDecrement(&Entry->ExclusiveWaiterCount);
}

/**
 * @brief
 * Finds the controller that owns a GSIV and takes a reference on it.
 */
static
PSECONDARY_IC_LIST_ENTRY
HalpFindSecondaryIcEntry(
    _In_ ULONG InputGsiv)
{
    PSECONDARY_IC_LIST_ENTRY Entry, Found = NULL;
    PLIST_ENTRY ListEntry;
    KIRQL OldIrql;

    OldIrql = HalpAcquireSecondaryLock(&HalpSecondaryIcListLock);

    for (ListEntry = HalpSecondaryIcList.Flink;
         ListEntry != &HalpSecondaryIcList;
         ListEntry = ListEntry->Flink)
    {
        Entry = CONTAINING_RECORD(ListEntry, SECONDARY_IC_LIST_ENTRY, ListEntry);
        if ((InputGsiv - Entry->GsivBase) < Entry->GsivSize)
        {
            InterlockedIncrement(&Entry->BusyCount);
            Found = Entry;
            break;
        }
    }

    HalpReleaseSecondaryLock(&HalpSecondaryIcListLock, OldIrql);
    return Found;
}

/**
 * @brief
 * Finds the registration a driver made for exactly this range and takes a
 * reference on it.
 */
static
PSECONDARY_IC_LIST_ENTRY
HalpFindSecondaryIcEntryByRange(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ ULONG GsivBase,
    _In_ ULONG GsivSize)
{
    PSECONDARY_IC_LIST_ENTRY Entry, Found = NULL;
    PLIST_ENTRY ListEntry;
    KIRQL OldIrql;

    OldIrql = HalpAcquireSecondaryLock(&HalpSecondaryIcListLock);

    for (ListEntry = HalpSecondaryIcList.Flink;
         ListEntry != &HalpSecondaryIcList;
         ListEntry = ListEntry->Flink)
    {
        Entry = CONTAINING_RECORD(ListEntry, SECONDARY_IC_LIST_ENTRY, ListEntry);
        if ((Entry->GsivBase == GsivBase) &&
            (Entry->GsivSize == GsivSize) &&
            (Entry->Interface.DriverObject == DriverObject))
        {
            InterlockedIncrement(&Entry->BusyCount);
            Found = Entry;
            break;
        }
    }

    HalpReleaseSecondaryLock(&HalpSecondaryIcListLock, OldIrql);
    return Found;
}

static
BOOLEAN
HalpIsSecondaryIcInterfaceValid(
    _In_ PSECONDARY_INTERRUPT_PROVIDER_INTERFACE Interface)
{
    if ((Interface->Version != SECONDARY_INTERRUPT_PROVIDER_INTERFACE_VERSION) ||
        (Interface->EnableInterrupt == NULL) ||
        (Interface->DisableInterrupt == NULL) ||
        (Interface->MaskInterrupt == NULL) ||
        (Interface->UnmaskInterrupt == NULL) ||
        (Interface->QueryPrimaryInterrupt == NULL) ||
        (Interface->DriverObject == NULL) ||
        (Interface->GsivSize == 0))
    {
        return FALSE;
    }

    return (Interface->GsivBase >= HalpSecondaryGsivRangeStart) &&
           (Interface->GsivBase + Interface->GsivSize <=
            HalpSecondaryGsivRangeStart + HalpSecondaryGsivRangeSize);
}

static
NTSTATUS
HalpCreateSecondaryIcEntry(
    _In_ PSECONDARY_INTERRUPT_PROVIDER_INTERFACE Interface)
{
    PSECONDARY_IC_LIST_ENTRY Entry;
    SIZE_T Size;
    KIRQL OldIrql;

    Size = FIELD_OFFSET(SECONDARY_IC_LIST_ENTRY, State) +
           Interface->GsivSize * sizeof(SECONDARY_INTERRUPT_LINE_STATE);

    Entry = ExAllocatePoolZero(NonPagedPool, Size, TAG_HAL_SECONDARY_IC);
    if (Entry == NULL)
        return STATUS_NO_MEMORY;

    Entry->GsivBase = Interface->GsivBase;
    Entry->GsivSize = Interface->GsivSize;
    RtlCopyMemory(&Entry->Interface, Interface, sizeof(Entry->Interface));

    ObReferenceObject(Entry->Interface.DriverObject);
    KeInitializeEvent(&Entry->NotificationEvent, SynchronizationEvent, TRUE);

    OldIrql = HalpAcquireSecondaryLock(&HalpSecondaryIcListLock);
    InsertTailList(&HalpSecondaryIcList, &Entry->ListEntry);
    HalpReleaseSecondaryLock(&HalpSecondaryIcListLock, OldIrql);

    DPRINT("Secondary controller owns GSIV 0x%lx to 0x%lx\n",
           Entry->GsivBase, Entry->GsivBase + Entry->GsivSize - 1);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Unlinks and frees an entry the caller holds one reference on.
 */
static
NTSTATUS
HalpDeleteSecondaryIcEntry(
    _In_ PSECONDARY_IC_LIST_ENTRY Entry)
{
    KIRQL OldIrql;

    if (InterlockedCompareExchange(&Entry->ExclusiveWaiterCount, 0, 0) > 0)
        return STATUS_UNSUCCESSFUL;

    HalpAcquireSecondaryIcEntryExclusive(Entry, &OldIrql);

    RemoveEntryList(&Entry->ListEntry);
    InterlockedDecrement(&Entry->BusyCount);

    HalpReleaseSecondaryLock(&HalpSecondaryIcListLock, OldIrql);

    KeSetEvent(&Entry->NotificationEvent, IO_NO_INCREMENT, FALSE);
    ObDereferenceObject(Entry->Interface.DriverObject);
    ExFreePoolWithTag(Entry, TAG_HAL_SECONDARY_IC);
    return STATUS_SUCCESS;
}

static
BOOLEAN
HalpIsSecondaryConnection(
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    if (ConnectionData->Count != 1)
        return FALSE;

    return HalpIsInterruptTypeSecondary(ConnectionData->Vectors[0].Type,
                                        ConnectionData->Vectors[0].ControllerInput.Gsiv);
}

/* Publishes the range under Control\HAL\SecondaryInterrupts */
static
NTSTATUS
HalpRecordSecondaryGsivRange(VOID)
{
    UNICODE_STRING HalKeyName = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\HAL");
    UNICODE_STRING SubKeyName = RTL_CONSTANT_STRING(L"SecondaryInterrupts");
    UNICODE_STRING BaseName = RTL_CONSTANT_STRING(L"SecondaryGsivBase");
    UNICODE_STRING SizeName = RTL_CONSTANT_STRING(L"SecondaryGsivSize");
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE HalKey = NULL, SubKey = NULL;
    NTSTATUS Status;

    InitializeObjectAttributes(&ObjectAttributes,
                               &HalKeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    Status = ZwOpenKey(&HalKey, KEY_READ, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return Status;

    InitializeObjectAttributes(&ObjectAttributes,
                               &SubKeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               HalKey,
                               NULL);
    Status = ZwOpenKey(&SubKey, KEY_SET_VALUE, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
    {
        Status = ZwCreateKey(&SubKey,
                             KEY_SET_VALUE,
                             &ObjectAttributes,
                             0,
                             NULL,
                             REG_OPTION_VOLATILE,
                             NULL);
    }

    if (NT_SUCCESS(Status))
    {
        Status = ZwSetValueKey(SubKey,
                               &BaseName,
                               0,
                               REG_DWORD,
                               &HalpSecondaryGsivRangeStart,
                               sizeof(HalpSecondaryGsivRangeStart));
        if (NT_SUCCESS(Status))
        {
            Status = ZwSetValueKey(SubKey,
                                   &SizeName,
                                   0,
                                   REG_DWORD,
                                   &HalpSecondaryGsivRangeSize,
                                   sizeof(HalpSecondaryGsivRangeSize));
        }
    }

    if (SubKey != NULL)
        ZwClose(SubKey);

    ZwClose(HalKey);
    return Status;
}

/* PUBLIC FUNCTIONS **********************************************************/

NTSTATUS
NTAPI
HalpAllocateGsivForSecondaryInterrupt(
    _In_reads_bytes_(OwnerNameLength) PCCHAR OwnerName,
    _In_ USHORT OwnerNameLength,
    _Out_ PULONG Gsiv)
{
    NTSTATUS Status;
    ULONG Assigned;

    UNREFERENCED_PARAMETER(OwnerName);
    UNREFERENCED_PARAMETER(OwnerNameLength);

    if ((ULONG)HalpSecondaryGsivAssignedCount >= HalpSecondaryGsivRangeSize)
        return STATUS_INSUFFICIENT_RESOURCES;

    Assigned = (ULONG)InterlockedIncrement(&HalpSecondaryGsivAssignedCount);
    if (Assigned >= HalpSecondaryGsivRangeSize)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* The kernel builds its secondary IDT on the first GSIV handed out */
    if (Assigned == 1)
    {
        Status = KeInitializeSecondaryInterruptServices(NULL);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    *Gsiv = HalpSecondaryGsivRangeStart + Assigned - 1;
    return STATUS_SUCCESS;
}

BOOLEAN
NTAPI
HalpIsInterruptTypeSecondary(
    _In_ ULONG Type,
    _In_ ULONG InputGsiv)
{
    if (Type != InterruptTypeControllerInput)
        return FALSE;

    return (InputGsiv >= HalpSecondaryGsivRangeStart) &&
           (InputGsiv < HalpSecondaryGsivRangeStart + HalpSecondaryGsivRangeSize);
}

/* A controller input that no primary controller serves but a secondary one may */
BOOLEAN
NTAPI
HalpIsSecondaryControllerInput(
    _In_ PINTERRUPT_VECTOR_DATA VectorData)
{
    if ((VectorData->Type != InterruptTypeControllerInput) ||
        HalpIsInterruptInputValid(VectorData->ControllerInput.Gsiv))
    {
        return FALSE;
    }

    return HalpIsInterruptTypeSecondary(VectorData->Type, VectorData->ControllerInput.Gsiv);
}

/**
 * @brief
 * Runs the service routines connected to a secondary GSIV. Called by the
 * controller once it knows which of its lines fired.
 *
 * @param[in] ControllerContext
 * The context the enable callback was given for this line.
 */
BOOLEAN
NTAPI
HalpInvokeIsrForGsiv(
    _In_ ULONG InputGsiv,
    _In_ PVOID ControllerContext)
{
    PSECONDARY_IC_LIST_ENTRY Entry = ControllerContext;
    ULONG Index;

    Index = InputGsiv - Entry->GsivBase;
    if (Index >= Entry->GsivSize)
        return FALSE;

    return KeDispatchSecondaryInterrupt(Entry->State[Index].Vector, 0);
}

NTSTATUS
NTAPI
HalpRegisterSecondaryIcInterface(
    _In_ PSECONDARY_INTERRUPT_PROVIDER_INTERFACE Interface)
{
    if (!HalpIsSecondaryIcInterfaceValid(Interface))
        return STATUS_INVALID_PARAMETER;

    return HalpCreateSecondaryIcEntry(Interface);
}

NTSTATUS
NTAPI
HalpUnregisterSecondaryIcInterface(
    _In_ ULONG GsivBase,
    _In_ ULONG GsivSize,
    _In_ PDRIVER_OBJECT DriverObject)
{
    PSECONDARY_IC_LIST_ENTRY Entry;
    NTSTATUS Status;

    Entry = HalpFindSecondaryIcEntryByRange(DriverObject, GsivBase, GsivSize);
    if (Entry == NULL)
        return STATUS_SECONDARY_IC_PROVIDER_NOT_REGISTERED;

    Status = HalpDeleteSecondaryIcEntry(Entry);
    if (!NT_SUCCESS(Status))
        HalpReleaseSecondaryIcEntryShared(Entry, TRUE);

    return Status;
}

NTSTATUS
NTAPI
HalpHandleMaskUnmaskSecondaryInterrupt(
    _In_ ULONG InputGsiv,
    _In_ ULONG Flags,
    _In_ BOOLEAN MaskRequest)
{
    PSECONDARY_INTERRUPT_LINE_STATE Line;
    PSECONDARY_IC_LIST_ENTRY Entry;
    NTSTATUS Status;

    if (!HalpSecondaryIcServicesEnabled)
        return STATUS_NOT_SUPPORTED;

    if (!HalpIsInterruptTypeSecondary(InterruptTypeControllerInput, InputGsiv))
        return STATUS_INVALID_PARAMETER;

    Entry = HalpFindSecondaryIcEntry(InputGsiv);
    if (Entry == NULL)
        return STATUS_SECONDARY_IC_PROVIDER_NOT_REGISTERED;

    Line = &Entry->State[InputGsiv - Entry->GsivBase];

    if (MaskRequest)
    {
        Status = Entry->Interface.MaskInterrupt(Entry->Interface.Context, Flags, InputGsiv);
        if (NT_SUCCESS(Status))
            Line->Unmasked = FALSE;
    }
    else
    {
        /* Marked first, the line can fire before the callback returns */
        Line->Unmasked = TRUE;
        Status = Entry->Interface.UnmaskInterrupt(Entry->Interface.Context, Flags, InputGsiv);
        if (!NT_SUCCESS(Status))
            Line->Unmasked = FALSE;
    }

    HalpReleaseSecondaryIcEntryShared(Entry, TRUE);
    return Status;
}

NTSTATUS
NTAPI
HalpSecondaryInterruptQueryPrimaryInformation(
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData,
    _Out_ PULONG PrimaryGsiv)
{
    PRIMARY_INTERRUPT_INFORMATION Primary;
    PSECONDARY_IC_LIST_ENTRY Entry;
    NTSTATUS Status;
    ULONG Gsiv;

    if (!HalpSecondaryIcServicesEnabled)
        return STATUS_NOT_SUPPORTED;

    if (!HalpIsSecondaryConnection(ConnectionData))
        return STATUS_INVALID_PARAMETER;

    Gsiv = ConnectionData->Vectors[0].ControllerInput.Gsiv;
    Entry = HalpFindSecondaryIcEntry(Gsiv);
    if (Entry == NULL)
        return STATUS_SECONDARY_IC_PROVIDER_NOT_REGISTERED;

    RtlZeroMemory(&Primary, sizeof(Primary));
    Status = Entry->Interface.QueryPrimaryInterrupt(Entry->Interface.Context, Gsiv, &Primary);

    /* A controller that has not been started yet cannot name its line */
    if (Status == STATUS_MORE_PROCESSING_REQUIRED)
        Status = STATUS_SUCCESS;

    HalpReleaseSecondaryIcEntryShared(Entry, TRUE);

    if (NT_SUCCESS(Status))
        *PrimaryGsiv = Primary.PrimaryGsiv;

    return Status;
}

NTSTATUS
NTAPI
HalpEnableSecondaryInterrupt(
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    PINTERRUPT_VECTOR_DATA VectorData = &ConnectionData->Vectors[0];
    PSECONDARY_INTERRUPT_LINE_STATE Line;
    PSECONDARY_IC_LIST_ENTRY Entry;
    NTSTATUS Status;
    ULONG Gsiv;

    if (!HalpSecondaryIcServicesEnabled)
        return STATUS_NOT_SUPPORTED;

    if (!HalpIsSecondaryConnection(ConnectionData))
        return STATUS_INVALID_PARAMETER;

    Gsiv = VectorData->ControllerInput.Gsiv;
    Entry = HalpFindSecondaryIcEntry(Gsiv);
    if (Entry == NULL)
        return STATUS_SECONDARY_IC_PROVIDER_NOT_REGISTERED;

    Line = &Entry->State[Gsiv - Entry->GsivBase];
    Line->Mode = VectorData->Mode;
    Line->Polarity = VectorData->Polarity;
    Line->Vector = VectorData->Vector;
    Line->Unmasked = TRUE;

    /* The entry comes back as the context of HalpInvokeIsrForGsiv */
    Status = Entry->Interface.EnableInterrupt(Entry->Interface.Context,
                                              Gsiv,
                                              VectorData->Mode,
                                              VectorData->Polarity,
                                              Entry);
    if (!NT_SUCCESS(Status))
    {
        Line->Vector |= HALP_SECONDARY_LINE_DISCONNECTED;
        Line->Unmasked = FALSE;
    }

    HalpReleaseSecondaryIcEntryShared(Entry, TRUE);
    return Status;
}

NTSTATUS
NTAPI
HalpDisableSecondaryInterrupt(
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    PSECONDARY_INTERRUPT_LINE_STATE Line;
    PSECONDARY_IC_LIST_ENTRY Entry;
    NTSTATUS Status;
    ULONG Gsiv;

    if (!HalpSecondaryIcServicesEnabled)
        return STATUS_NOT_SUPPORTED;

    if (!HalpIsSecondaryConnection(ConnectionData))
        return STATUS_INVALID_PARAMETER;

    Gsiv = ConnectionData->Vectors[0].ControllerInput.Gsiv;
    Entry = HalpFindSecondaryIcEntry(Gsiv);
    if (Entry == NULL)
        return STATUS_SECONDARY_IC_PROVIDER_NOT_REGISTERED;

    Status = Entry->Interface.DisableInterrupt(Entry->Interface.Context, Gsiv);
    if (NT_SUCCESS(Status))
    {
        Line = &Entry->State[Gsiv - Entry->GsivBase];
        Line->Vector |= HALP_SECONDARY_LINE_DISCONNECTED;
        Line->Unmasked = FALSE;
    }

    HalpReleaseSecondaryIcEntryShared(Entry, TRUE);
    return Status;
}

/**
 * @brief
 * Asks the controller which primary line would carry a secondary GSIV, then
 * raises that line. Only lines that are unmasked can be requested.
 */
NTSTATUS
NTAPI
HalpRequestSecondaryInterrupt(
    _In_ ULONG Gsiv)
{
    PSECONDARY_IC_LIST_ENTRY Entry;
    ULONG PrimaryGsiv = MAXULONG;
    NTSTATUS Status;

    if (!HalpSecondaryIcServicesEnabled ||
        !HalpIsInterruptTypeSecondary(InterruptTypeControllerInput, Gsiv))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Entry = HalpFindSecondaryIcEntry(Gsiv);
    if (Entry == NULL)
        return STATUS_SECONDARY_IC_PROVIDER_NOT_REGISTERED;

    if (Entry->State[Gsiv - Entry->GsivBase].Unmasked &&
        (Entry->Interface.RequestInterrupt != NULL))
    {
        Status = Entry->Interface.RequestInterrupt(Entry->Interface.Context, Gsiv, &PrimaryGsiv);
    }
    else
    {
        Status = STATUS_UNSUCCESSFUL;
    }

    HalpReleaseSecondaryIcEntryShared(Entry, TRUE);

    if (!NT_SUCCESS(Status) || (PrimaryGsiv == MAXULONG))
        return Status;

    return HalRequestInterrupt(PrimaryGsiv);
}

NTSTATUS
NTAPI
HalpMaskInterrupt(
    _In_ ULONG InputGsiv,
    _In_ ULONG Flags)
{
    if (HalpIsInterruptInputValid(InputGsiv))
        return HalpSetInterruptInputMask(InputGsiv, Flags, TRUE);

    if (HalpIsInterruptTypeSecondary(InterruptTypeControllerInput, InputGsiv))
        return HalpHandleMaskUnmaskSecondaryInterrupt(InputGsiv, Flags, TRUE);

    return STATUS_INVALID_PARAMETER;
}

NTSTATUS
NTAPI
HalpUnmaskInterrupt(
    _In_ ULONG InputGsiv,
    _In_ ULONG Flags)
{
    if (HalpIsInterruptInputValid(InputGsiv))
        return HalpSetInterruptInputMask(InputGsiv, Flags, FALSE);

    if (HalpIsInterruptTypeSecondary(InterruptTypeControllerInput, InputGsiv))
        return HalpHandleMaskUnmaskSecondaryInterrupt(InputGsiv, Flags, FALSE);

    return STATUS_INVALID_PARAMETER;
}

NTSTATUS
NTAPI
HalpRequestInterrupt(
    _In_ ULONG Gsiv)
{
    if (HalpIsInterruptInputValid(Gsiv))
        return HalpRequestInterruptInput(Gsiv);

    if (HalpIsInterruptTypeSecondary(InterruptTypeControllerInput, Gsiv))
        return HalpRequestSecondaryInterrupt(Gsiv);

    return STATUS_INVALID_PARAMETER;
}

/**
 * @brief
 * Fills in the HalSecondaryInterruptInformation block.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_UNSUCCESSFUL with a zeroed block when the
 * services are off.
 */
NTSTATUS
NTAPI
HalpQuerySecondaryInterruptInformation(
    _Out_ PHAL_SECONDARY_INTERRUPT_INFORMATION Information)
{
    RtlZeroMemory(Information, sizeof(*Information));

    if (!HalpSecondaryIcServicesEnabled)
        return STATUS_UNSUCCESSFUL;

    Information->Version = HAL_SECONDARY_INTERRUPT_INFORMATION_VERSION;
    Information->GsivRangeStart = HalpSecondaryGsivRangeStart;
    Information->GsivRangeSize = HalpSecondaryGsivRangeSize;
    Information->MaskInterrupt = HalpMaskInterrupt;
    Information->UnmaskInterrupt = HalpUnmaskInterrupt;
    Information->InvokeIsrForGsiv = HalpInvokeIsrForGsiv;
    Information->UnregisterInterface = HalpUnregisterSecondaryIcInterface;
    Information->RequestInterrupt = HalpRequestInterrupt;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Places the secondary GSIV range above every primary input and turns the
 * services on. Needs the registry, so it runs from the HAL's AddDevice.
 */
NTSTATUS
NTAPI
HalpInitializeSecondaryInterruptServices(VOID)
{
    ULONG MaximumGsiv;
    NTSTATUS Status;

    InitializeListHead(&HalpSecondaryIcList);
    KeInitializeSpinLock(&HalpSecondaryIcListLock);

    InitializeListHead(&HalpSecondarySignalList);
    KeInitializeSpinLock(&HalpSecondarySignalListLock);
    KeInitializeDpc(&HalpSecondarySignalDpc, HalpSecondarySignalDpcRoutine, NULL);
    HalpSecondarySignalDpcQueued = FALSE;

    Status = HalpQueryMaximumGsiv(&MaximumGsiv);
    if (!NT_SUCCESS(Status))
        return Status;

    if (MaximumGsiv + HALP_SECONDARY_GSIV_COUNT + 1 < MaximumGsiv)
        return STATUS_UNSUCCESSFUL;

    HalpSecondaryGsivRangeStart = max(MaximumGsiv + 1, HALP_SECONDARY_GSIV_FLOOR);
    HalpSecondaryGsivRangeSize = HALP_SECONDARY_GSIV_COUNT;
    HalpSecondaryGsivAssignedCount = 0;

    HalpRecordSecondaryGsivRange();

    HalpSecondaryIcServicesEnabled = TRUE;
    return STATUS_SUCCESS;
}

/* EOF */
