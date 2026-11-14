/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Registration as a secondary interrupt controller
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The HAL reserves a GSIV range it cannot describe itself and the resource hub
 * mints one GSIV per GpioInt from it. Claiming that range here turns connecting
 * such a GSIV into a request to arm a pin.
 */

#define NDEBUG
#include "gpioclxp.h"

typedef struct _GCX_VIRQ
{
    LIST_ENTRY Link;
    ULONG Gsiv;
    PGCX_CONTROLLER Controller;
    BANK_ID BankId;
    PIN_NUMBER PinNumber;
} GCX_VIRQ, *PGCX_VIRQ;

static GCX_HAL_SECONDARY_INFO GcxHalInfo;
static PDRIVER_OBJECT GcxSecondaryOwner;
static FAST_MUTEX GcxSecondaryMutex;

static LIST_ENTRY GcxVirqList;
static KSPIN_LOCK GcxVirqLock;

static LIST_ENTRY GcxControllerList;
static KSPIN_LOCK GcxControllerLock;

_Use_decl_annotations_
VOID
NTAPI
GcxInitializeGlobals(VOID)
{
    InitializeListHead(&GcxVirqList);
    KeInitializeSpinLock(&GcxVirqLock);
    InitializeListHead(&GcxControllerList);
    KeInitializeSpinLock(&GcxControllerLock);
    ExInitializeFastMutex(&GcxSecondaryMutex);
}

_Use_decl_annotations_
VOID
NTAPI
GcxAddController(
    PGCX_CONTROLLER Controller)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&GcxControllerLock, &OldIrql);
    InsertTailList(&GcxControllerList, &Controller->ControllerLink);
    KeReleaseSpinLock(&GcxControllerLock, OldIrql);
}

_Use_decl_annotations_
VOID
NTAPI
GcxRemoveController(
    PGCX_CONTROLLER Controller)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&GcxControllerLock, &OldIrql);
    RemoveEntryList(&Controller->ControllerLink);
    InitializeListHead(&Controller->ControllerLink);
    KeReleaseSpinLock(&GcxControllerLock, OldIrql);
}

/**
 * @brief
 * The controller the firmware knows by a given ACPI path, or NULL.
 */
_Use_decl_annotations_
PGCX_CONTROLLER
NTAPI
GcxFindControllerByBiosName(
    PCUNICODE_STRING BiosName)
{
    PGCX_CONTROLLER Match = NULL;
    PGCX_CONTROLLER Candidate;
    PLIST_ENTRY Entry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&GcxControllerLock, &OldIrql);
    for (Entry = GcxControllerList.Flink; Entry != &GcxControllerList; Entry = Entry->Flink)
    {
        Candidate = CONTAINING_RECORD(Entry, GCX_CONTROLLER, ControllerLink);
        if (RtlEqualUnicodeString(&Candidate->BiosName, BiosName, TRUE))
        {
            Match = Candidate;
            break;
        }
    }
    KeReleaseSpinLock(&GcxControllerLock, OldIrql);

    return Match;
}

/**
 * @brief
 * The mapping for a GSIV. The caller holds GcxVirqLock.
 */
static
PGCX_VIRQ
NTAPI
GcxFindVirqLocked(
    _In_ ULONG Gsiv)
{
    PLIST_ENTRY Entry;
    PGCX_VIRQ Virq;

    for (Entry = GcxVirqList.Flink; Entry != &GcxVirqList; Entry = Entry->Flink)
    {
        Virq = CONTAINING_RECORD(Entry, GCX_VIRQ, Link);
        if (Virq->Gsiv == Gsiv)
            return Virq;
    }

    return NULL;
}

/**
 * @brief
 * Copies out the pin a GSIV was mapped to, if any.
 */
static
BOOLEAN
NTAPI
GcxLookupVirq(
    _In_ ULONG Gsiv,
    _Out_ PGCX_VIRQ Result)
{
    PGCX_VIRQ Virq;
    KIRQL OldIrql;

    KeAcquireSpinLock(&GcxVirqLock, &OldIrql);
    Virq = GcxFindVirqLocked(Gsiv);
    if (Virq != NULL)
        *Result = *Virq;
    KeReleaseSpinLock(&GcxVirqLock, OldIrql);

    return (Virq != NULL);
}

/**
 * @brief
 * Finds the pin behind a GSIV, asking the resource hub the first time.
 *
 * The HAL queries the primary interrupt before it enables a line, so either
 * call may be the first to see a given GSIV.
 */
static
NTSTATUS
NTAPI
GcxGetVirq(
    _In_ ULONG Gsiv,
    _Out_ PGCX_VIRQ Result)
{
    PGCX_VIRQ Virq;
    KIRQL OldIrql;
    NTSTATUS Status;

    if (Gsiv < GcxHalInfo.GsivRangeStart ||
        Gsiv - GcxHalInfo.GsivRangeStart >= GcxHalInfo.GsivRangeSize)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (GcxLookupVirq(Gsiv, Result))
        return STATUS_SUCCESS;

    Virq = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Virq), GCX_POOL_TAG);
    if (Virq == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = GcxResolveGsivToPin(Gsiv, &Virq->Controller, &Virq->BankId, &Virq->PinNumber);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Virq, GCX_POOL_TAG);
        return Status;
    }

    Virq->Gsiv = Gsiv;
    *Result = *Virq;

    /* Two callers may have resolved the same GSIV; the first one in wins */
    KeAcquireSpinLock(&GcxVirqLock, &OldIrql);
    if (GcxFindVirqLocked(Gsiv) == NULL)
    {
        InsertTailList(&GcxVirqList, &Virq->Link);
        Virq = NULL;
    }
    KeReleaseSpinLock(&GcxVirqLock, OldIrql);

    if (Virq != NULL)
        ExFreePoolWithTag(Virq, GCX_POOL_TAG);

    DPRINT("GpioClx: GSIV %lu is bank %u pin %u\n", Gsiv, Result->BankId, Result->PinNumber);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
GcxHubEnableInterrupt(
    _In_ PVOID Context,
    _In_ ULONG Gsiv,
    _In_ KINTERRUPT_MODE Mode,
    _In_ KINTERRUPT_POLARITY Polarity,
    _In_ PVOID ControllerContext)
{
    GCX_VIRQ Virq;
    PGCX_PIN Pin;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Context);

    Status = GcxGetVirq(Gsiv, &Virq);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The token has to be in place before the pin can fire */
    Pin = GcxPin(Virq.Controller, Virq.BankId, Virq.PinNumber);
    if (Pin == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    Pin->Gsiv = Gsiv;
    Pin->TargetContext = ControllerContext;

    Status = GcxEnableInterrupt(Virq.Controller,
                                Virq.BankId,
                                Virq.PinNumber,
                                Mode,
                                Polarity,
                                GPIO_PIN_PULL_CONFIGURATION_DEFAULT);
    if (!NT_SUCCESS(Status))
        Pin->TargetContext = NULL;

    return Status;
}

static
NTSTATUS
NTAPI
GcxHubDisableInterrupt(
    _In_ PVOID Context,
    _In_ ULONG Gsiv)
{
    GCX_VIRQ Virq;

    UNREFERENCED_PARAMETER(Context);

    if (!GcxLookupVirq(Gsiv, &Virq))
        return STATUS_NOT_FOUND;

    return GcxDisableInterrupt(Virq.Controller, Virq.BankId, Virq.PinNumber);
}

static
NTSTATUS
NTAPI
GcxHubMaskInterrupt(
    _In_ PVOID Context,
    _In_ ULONG Flags,
    _In_ ULONG Gsiv)
{
    ULONG64 Failed;
    GCX_VIRQ Virq;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Flags);

    if (!GcxLookupVirq(Gsiv, &Virq))
        return STATUS_NOT_FOUND;

    return GcxMaskInterrupts(Virq.Controller, Virq.BankId, 1ULL << Virq.PinNumber, &Failed);
}

static
NTSTATUS
NTAPI
GcxHubUnmaskInterrupt(
    _In_ PVOID Context,
    _In_ ULONG Flags,
    _In_ ULONG Gsiv)
{
    GCX_VIRQ Virq;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Flags);

    if (!GcxLookupVirq(Gsiv, &Virq))
        return STATUS_NOT_FOUND;

    return GcxUnmaskInterrupt(Virq.Controller, Virq.BankId, Virq.PinNumber);
}

/**
 * @brief
 * Reports the line the controller behind a GSIV sits on.
 *
 * Devices on the controller's pins run their ISRs inside that line's interrupt,
 * which is how the HAL settles their IRQL.
 */
static
NTSTATUS
NTAPI
GcxHubQueryPrimaryInterrupt(
    _In_ PVOID Context,
    _In_ ULONG Gsiv,
    _Out_ PGCX_PRIMARY_INTERRUPT PrimaryInformation)
{
    PGCX_CONTROLLER Controller;
    GCX_VIRQ Virq;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Context);

    Status = GcxGetVirq(Gsiv, &Virq);
    if (!NT_SUCCESS(Status))
        return Status;

    Controller = Virq.Controller;
    if (Controller->PrimaryGsiv == GCX_NO_PRIMARY_GSIV)
        return STATUS_NOT_FOUND;

    PrimaryInformation->PrimaryGsiv = Controller->PrimaryGsiv;
    PrimaryInformation->PrimaryIrql = Controller->PrimaryIrql;
    PrimaryInformation->PrimaryAffinity.Mask = Controller->PrimaryAffinity;
    PrimaryInformation->PrimaryAffinity.Group = 0;
    PrimaryInformation->InterruptMode = Controller->PrimaryMode;
    PrimaryInformation->Polarity = InterruptActiveLow;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
NTAPI
GcxRemoveVirqMappings(
    PGCX_CONTROLLER Controller)
{
    LIST_ENTRY Doomed;
    PLIST_ENTRY Entry;
    PLIST_ENTRY Next;
    PGCX_VIRQ Virq;
    KIRQL OldIrql;

    InitializeListHead(&Doomed);

    KeAcquireSpinLock(&GcxVirqLock, &OldIrql);
    for (Entry = GcxVirqList.Flink; Entry != &GcxVirqList; Entry = Next)
    {
        Next = Entry->Flink;
        Virq = CONTAINING_RECORD(Entry, GCX_VIRQ, Link);
        if (Virq->Controller == Controller)
        {
            RemoveEntryList(Entry);
            InsertTailList(&Doomed, Entry);
        }
    }
    KeReleaseSpinLock(&GcxVirqLock, OldIrql);

    while (!IsListEmpty(&Doomed))
    {
        Entry = RemoveHeadList(&Doomed);
        ExFreePoolWithTag(CONTAINING_RECORD(Entry, GCX_VIRQ, Link), GCX_POOL_TAG);
    }
}

/**
 * @brief
 * Runs whatever the kernel connected to a pin's GSIV.
 *
 * @return
 * TRUE if a service routine claimed the interrupt.
 */
_Use_decl_annotations_
BOOLEAN
NTAPI
GcxDeliverPinInterrupt(
    PGCX_PIN Pin)
{
    if (GcxSecondaryOwner == NULL ||
        GcxHalInfo.InvokeIsrForGsiv == NULL ||
        Pin->TargetContext == NULL)
    {
        return FALSE;
    }

    return GcxHalInfo.InvokeIsrForGsiv(Pin->Gsiv, Pin->TargetContext);
}

/**
 * @brief
 * Claims the HAL's secondary GSIV range, once for the whole driver.
 *
 * @param[in] DriverObject
 * The driver object the HAL records as the range owner.
 *
 * @return
 * STATUS_SUCCESS, or the reason the HAL refused.
 */
_Use_decl_annotations_
NTSTATUS
NTAPI
GcxRegisterSecondaryInterruptController(
    PDRIVER_OBJECT DriverObject)
{
    GCX_SECONDARY_PROVIDER Provider;
    ULONG Returned;
    NTSTATUS Status = STATUS_SUCCESS;

    ExAcquireFastMutex(&GcxSecondaryMutex);

    if (GcxSecondaryOwner != NULL)
        goto Exit;

    RtlZeroMemory(&GcxHalInfo, sizeof(GcxHalInfo));
    Returned = sizeof(GcxHalInfo);
    Status = HalQuerySystemInformation(HalSecondaryInterruptInformation,
                                       sizeof(GcxHalInfo),
                                       &GcxHalInfo,
                                       &Returned);
    if (!NT_SUCCESS(Status))
        goto Exit;

    if (GcxHalInfo.GsivRangeSize == 0)
    {
        Status = STATUS_NOT_SUPPORTED;
        goto Exit;
    }

    RtlZeroMemory(&Provider, sizeof(Provider));
    Provider.Size = sizeof(Provider);
    Provider.Version = GCX_SECONDARY_PROVIDER_VERSION;
    Provider.GsivBase = GcxHalInfo.GsivRangeStart;
    Provider.GsivSize = (USHORT)min(GcxHalInfo.GsivRangeSize, MAXUSHORT);
    Provider.DriverObject = DriverObject;
    Provider.EnableInterrupt = GcxHubEnableInterrupt;
    Provider.DisableInterrupt = GcxHubDisableInterrupt;
    Provider.MaskInterrupt = GcxHubMaskInterrupt;
    Provider.UnmaskInterrupt = GcxHubUnmaskInterrupt;
    Provider.QueryPrimaryInterrupt = GcxHubQueryPrimaryInterrupt;

    Status = HalSetSystemInformation(HalRegisterSecondaryInterruptInterface,
                                     sizeof(Provider),
                                     &Provider);
    if (!NT_SUCCESS(Status))
        goto Exit;

    GcxSecondaryOwner = DriverObject;
    DPRINT("GpioClx: owns GSIV %lu, %u lines\n", Provider.GsivBase, Provider.GsivSize);

Exit:
    ExReleaseFastMutex(&GcxSecondaryMutex);
    return Status;
}

_Use_decl_annotations_
VOID
NTAPI
GcxUnregisterSecondaryInterruptController(VOID)
{
    ExAcquireFastMutex(&GcxSecondaryMutex);

    if (GcxSecondaryOwner != NULL && GcxHalInfo.UnregisterInterface != NULL)
    {
        GcxHalInfo.UnregisterInterface(GcxHalInfo.GsivRangeStart,
                                       GcxHalInfo.GsivRangeSize,
                                       GcxSecondaryOwner);
    }

    GcxSecondaryOwner = NULL;
    ExReleaseFastMutex(&GcxSecondaryMutex);
}
