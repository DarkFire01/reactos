/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Secondary interrupt controllers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/**
 * @file
 * @brief
 * The registry of drivers that deliver interrupts of their own.
 *
 * A GPIO or SPB controller has one line and many devices behind it. Its driver
 * takes a slice of the GSIV space, tells the HAL it owns those numbers, and
 * from then on the HAL routes connect, disconnect, mask and unmask for them
 * back to the driver rather than to an interrupt controller. When one of those
 * lines fires, the driver hands the GSIV back and the HAL turns it into the
 * vector the kernel connected an ISR on.
 *
 * Reconstructed from the Win8 x64 HAL: HalpInitializeSecondaryInterruptServices
 * (hal.dll.c:62699), HalpRegisterSecondaryIcInterface (:23269),
 * HalpCreateSecondaryIcEntry (:23391), HalpFindSecondaryIcEntry (:23460),
 * HalpHandleMaskUnmaskSecondaryInterrupt (:23518), HalpInvokeIsrForGsiv
 * (:23254) and the enable, disable and query-primary trio at :54653.
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS *******************************************************************/

#define TAG_HAL_SECONDARY 'lSaH'

/**
 * @brief
 * One line's state, as the HAL sees it.
 *
 * The controller knows how its pin is wired; what the HAL has to remember is
 * the vector the kernel connected on, because that is the only way back from a
 * GSIV the controller reports to the routine that should run.
 */
typedef struct _SECONDARY_INTERRUPT_LINE_STATE
{
    KINTERRUPT_POLARITY Polarity;
    KINTERRUPT_MODE Mode;
    ULONG Vector;
    BOOLEAN Unmasked;
} SECONDARY_INTERRUPT_LINE_STATE, *PSECONDARY_INTERRUPT_LINE_STATE;

/* A line the kernel has released; the entry stays, the vector does not */
#define SECONDARY_VECTOR_DISCONNECTED 0x80000000

/**
 * @brief
 * One registered controller.
 *
 * The line states are allocated with the entry rather than separately, which
 * is what the reference does at :23393 - one block sized to the range means
 * one thing to free and no second failure point.
 */
typedef struct _SECONDARY_IC_LIST_ENTRY
{
    LIST_ENTRY ListEntry;
    ULONG GsivBase;
    ULONG GsivSize;
    SECONDARY_INTERRUPT_PROVIDER_INTERFACE Interface;

    /* GsivSize entries, one per line the controller claimed */
    SECONDARY_INTERRUPT_LINE_STATE State[1];
} SECONDARY_IC_LIST_ENTRY, *PSECONDARY_IC_LIST_ENTRY;

static LIST_ENTRY HalpSecondaryIcList;
static KSPIN_LOCK HalpSecondaryIcListLock;

BOOLEAN HalpSecondaryIcServicesEnabled = FALSE;

/*
 * The slice of GSIV space no interrupt controller can describe, set aside for
 * controllers that are drivers. The reference bases it above the largest GSIV
 * the machine's controllers report (:62714); this tree fixes it, because the
 * ACPI driver already hands these numbers out from a fixed base.
 */
ULONG HalpSecondaryGsivRangeStart = HALP_SECONDARY_GSIV_BASE;
ULONG HalpSecondaryGsivRangeSize =
    HALP_SECONDARY_GSIV_LIMIT - HALP_SECONDARY_GSIV_BASE;

static LONG HalpSecondaryGsivAssignedCount = 0;

/* PRIVATE FUNCTIONS *********************************************************/

/**
 * @brief
 * Finds the controller that claimed a GSIV.
 *
 * @param[in] Gsiv
 * The line.
 *
 * @return
 * Its controller, or NULL if nothing claimed it.
 */
static
PSECONDARY_IC_LIST_ENTRY
HalpFindSecondaryIcEntry(
    _In_ ULONG Gsiv)
{
    PSECONDARY_IC_LIST_ENTRY Found = NULL;
    PLIST_ENTRY NextEntry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&HalpSecondaryIcListLock, &OldIrql);

    for (NextEntry = HalpSecondaryIcList.Flink;
         NextEntry != &HalpSecondaryIcList;
         NextEntry = NextEntry->Flink)
    {
        PSECONDARY_IC_LIST_ENTRY Entry =
            CONTAINING_RECORD(NextEntry, SECONDARY_IC_LIST_ENTRY, ListEntry);

        if ((Gsiv >= Entry->GsivBase) && (Gsiv < Entry->GsivBase + Entry->GsivSize))
        {
            Found = Entry;
            break;
        }
    }

    KeReleaseSpinLock(&HalpSecondaryIcListLock, OldIrql);

    return Found;
}

/**
 * @brief
 * Finds a controller by exactly the range and driver it registered with.
 *
 * Unregistering has to name the same three things registration did, so that a
 * driver can only ever withdraw its own range.
 *
 * @param[in] DriverObject
 * The driver that registered.
 *
 * @param[in] GsivBase
 * The base of its range.
 *
 * @param[in] GsivSize
 * The length of its range.
 *
 * @return
 * The matching controller, or NULL.
 */
static
PSECONDARY_IC_LIST_ENTRY
HalpFindSecondaryIcEntryFromObjectAndRange(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ ULONG GsivBase,
    _In_ ULONG GsivSize)
{
    PSECONDARY_IC_LIST_ENTRY Found = NULL;
    PLIST_ENTRY NextEntry;
    KIRQL OldIrql;

    KeAcquireSpinLock(&HalpSecondaryIcListLock, &OldIrql);

    for (NextEntry = HalpSecondaryIcList.Flink;
         NextEntry != &HalpSecondaryIcList;
         NextEntry = NextEntry->Flink)
    {
        PSECONDARY_IC_LIST_ENTRY Entry =
            CONTAINING_RECORD(NextEntry, SECONDARY_IC_LIST_ENTRY, ListEntry);

        if ((Entry->GsivBase == GsivBase) &&
            (Entry->GsivSize == GsivSize) &&
            (Entry->Interface.DriverObject == DriverObject))
        {
            Found = Entry;
            break;
        }
    }

    KeReleaseSpinLock(&HalpSecondaryIcListLock, OldIrql);

    return Found;
}

/**
 * @brief
 * Says whether an interface block is one the HAL can use.
 *
 * @param[in] Interface
 * What the controller passed.
 *
 * @return
 * TRUE if it is complete and its range is inside the secondary space.
 */
static
BOOLEAN
HalpValidateSecondaryIcInterface(
    _In_ PSECONDARY_INTERRUPT_PROVIDER_INTERFACE Interface)
{
    if (Interface->Version != SECONDARY_INTERRUPT_PROVIDER_INTERFACE_VERSION)
    {
        return FALSE;
    }

    /*
     * Only five of the seven are required. Requesting an interrupt and
     * reporting a line are for waking and for diagnostics, neither of which
     * the HAL needs to route an interrupt.
     */
    if ((Interface->EnableInterrupt == NULL) ||
        (Interface->DisableInterrupt == NULL) ||
        (Interface->MaskInterrupt == NULL) ||
        (Interface->UnmaskInterrupt == NULL) ||
        (Interface->QueryPrimaryInterrupt == NULL) ||
        (Interface->DriverObject == NULL) ||
        (Interface->GsivSize == 0))
    {
        return FALSE;
    }

    if ((Interface->GsivBase < HalpSecondaryGsivRangeStart) ||
        (Interface->GsivBase + Interface->GsivSize >
         HalpSecondaryGsivRangeStart + HalpSecondaryGsivRangeSize))
    {
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief
 * Takes a copy of a validated interface and puts it on the list.
 *
 * @param[in] Interface
 * The controller's interface block.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INSUFFICIENT_RESOURCES, or STATUS_OBJECT_NAME_COLLISION
 * for a range someone already claimed.
 */
static
NTSTATUS
HalpCreateSecondaryIcEntry(
    _In_ PSECONDARY_INTERRUPT_PROVIDER_INTERFACE Interface)
{
    PSECONDARY_IC_LIST_ENTRY Entry;
    SIZE_T Size;
    KIRQL OldIrql;
    ULONG Index;

    Size = FIELD_OFFSET(SECONDARY_IC_LIST_ENTRY, State) +
           Interface->GsivSize * sizeof(SECONDARY_INTERRUPT_LINE_STATE);

    Entry = ExAllocatePoolZero(NonPagedPool, Size, TAG_HAL_SECONDARY);
    if (Entry == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Entry->GsivBase = Interface->GsivBase;
    Entry->GsivSize = Interface->GsivSize;
    Entry->Interface = *Interface;

    /* Nothing is connected yet, so no line has a vector */
    for (Index = 0; Index < Entry->GsivSize; Index++)
    {
        Entry->State[Index].Vector = SECONDARY_VECTOR_DISCONNECTED;
    }

    ObReferenceObject(Interface->DriverObject);

    KeAcquireSpinLock(&HalpSecondaryIcListLock, &OldIrql);

    /*
     * Two controllers claiming one line would make delivery depend on list
     * order, so the overlap is refused rather than resolved.
     */
    {
        PLIST_ENTRY NextEntry;

        for (NextEntry = HalpSecondaryIcList.Flink;
             NextEntry != &HalpSecondaryIcList;
             NextEntry = NextEntry->Flink)
        {
            PSECONDARY_IC_LIST_ENTRY Other =
                CONTAINING_RECORD(NextEntry, SECONDARY_IC_LIST_ENTRY, ListEntry);

            if ((Entry->GsivBase < Other->GsivBase + Other->GsivSize) &&
                (Other->GsivBase < Entry->GsivBase + Entry->GsivSize))
            {
                KeReleaseSpinLock(&HalpSecondaryIcListLock, OldIrql);
                ObDereferenceObject(Interface->DriverObject);
                ExFreePoolWithTag(Entry, TAG_HAL_SECONDARY);
                return STATUS_OBJECT_NAME_COLLISION;
            }
        }
    }

    InsertTailList(&HalpSecondaryIcList, &Entry->ListEntry);

    KeReleaseSpinLock(&HalpSecondaryIcListLock, OldIrql);

    DPRINT1("Secondary interrupt controller registered for GSIV %lu..%lu\n",
            Entry->GsivBase, Entry->GsivBase + Entry->GsivSize - 1);

    return STATUS_SUCCESS;
}

/* PUBLIC FUNCTIONS **********************************************************/

/**
 * @brief
 * Mints a GSIV for an interrupt that a secondary controller demultiplexes.
 *
 * The caller is whoever knows a firmware descriptor names a pin rather than a
 * line: on this system the ACPI driver, on behalf of the resource hub, when it
 * translates a GpioInt.
 *
 * @param[in] OwnerName
 * Name of the descriptor the GSIV is for. Diagnostics only, and optional.
 *
 * @param[in] OwnerNameLength
 * Length of that name in bytes.
 *
 * @param[out] Gsiv
 * Receives the allocated number.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES once the range is spent.
 */
NTSTATUS
NTAPI
HalpAllocateGsivForSecondaryInterrupt(
    _In_reads_bytes_(OwnerNameLength) PCCHAR OwnerName,
    _In_ USHORT OwnerNameLength,
    _Out_ PULONG Gsiv)
{
    NTSTATUS Status;
    LONG Allocated;

    UNREFERENCED_PARAMETER(OwnerName);
    UNREFERENCED_PARAMETER(OwnerNameLength);

    if (!HalpSecondaryIcServicesEnabled)
    {
        return STATUS_NOT_SUPPORTED;
    }

    Allocated = InterlockedIncrement(&HalpSecondaryGsivAssignedCount);
    if ((ULONG)Allocated > HalpSecondaryGsivRangeSize)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /*
     * The kernel's table of secondary vectors is built on the first
     * allocation, not at boot: a machine with no pin-multiplexing controller
     * never needs one. The reference does the same at :23233.
     */
    if (Allocated == 1)
    {
        Status = KeInitializeSecondaryInterruptServices(NULL);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }
    }

    *Gsiv = HalpSecondaryGsivRangeStart + Allocated - 1;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Says whether a GSIV belongs to a secondary controller.
 *
 * @param[in] Type
 * The connection type. Only a controller input can be secondary.
 *
 * @param[in] InputGsiv
 * The line.
 *
 * @return
 * TRUE if the line is in the secondary space.
 */
BOOLEAN
NTAPI
HalpIsInterruptTypeSecondary(
    _In_ ULONG Type,
    _In_ ULONG InputGsiv)
{
    if (Type != InterruptTypeControllerInput)
    {
        return FALSE;
    }

    return (InputGsiv >= HalpSecondaryGsivRangeStart) &&
           (InputGsiv < HalpSecondaryGsivRangeStart + HalpSecondaryGsivRangeSize);
}

/**
 * @brief
 * Runs whatever the kernel connected to one of a controller's lines.
 *
 * Called by the controller's driver once it has worked out which of its pins
 * asserted. The context is the token the HAL handed it when the line was
 * enabled, which is what lets this reach the right controller without another
 * list walk at interrupt time.
 *
 * @param[in] InputGsiv
 * The line that fired.
 *
 * @param[in] ControllerContext
 * The token from the enable callback.
 *
 * @return
 * TRUE if a service routine claimed the interrupt.
 */
BOOLEAN
NTAPI
HalpInvokeIsrForGsiv(
    _In_ ULONG InputGsiv,
    _In_ PVOID ControllerContext)
{
    PSECONDARY_IC_LIST_ENTRY Entry = ControllerContext;
    ULONG Vector;

    if (Entry == NULL)
    {
        return FALSE;
    }

    if ((InputGsiv < Entry->GsivBase) ||
        (InputGsiv >= Entry->GsivBase + Entry->GsivSize))
    {
        return FALSE;
    }

    Vector = Entry->State[InputGsiv - Entry->GsivBase].Vector;
    if (Vector == SECONDARY_VECTOR_DISCONNECTED)
    {
        /* The line fired with nothing connected to it */
        return FALSE;
    }

    return KeDispatchSecondaryInterrupt(Vector, 0);
}

/**
 * @brief
 * Registers a driver as the controller for a range of GSIVs.
 *
 * @param[in] Interface
 * The controller's interface block.
 *
 * @return
 * STATUS_SUCCESS, or the reason the block was refused.
 */
NTSTATUS
NTAPI
HalpRegisterSecondaryIcInterface(
    _In_ PSECONDARY_INTERRUPT_PROVIDER_INTERFACE Interface)
{
    if (!HalpSecondaryIcServicesEnabled)
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (!HalpValidateSecondaryIcInterface(Interface))
    {
        return STATUS_INVALID_PARAMETER;
    }

    return HalpCreateSecondaryIcEntry(Interface);
}

/**
 * @brief
 * Withdraws a registration.
 *
 * @param[in] GsivBase
 * The base of the range that was registered.
 *
 * @param[in] GsivSize
 * Its length.
 *
 * @param[in] DriverObject
 * The driver that registered it.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_OBJECT_NAME_NOT_FOUND.
 */
NTSTATUS
NTAPI
HalpUnregisterSecondaryIcInterface(
    _In_ ULONG GsivBase,
    _In_ ULONG GsivSize,
    _In_ PDRIVER_OBJECT DriverObject)
{
    PSECONDARY_IC_LIST_ENTRY Entry;
    KIRQL OldIrql;

    Entry = HalpFindSecondaryIcEntryFromObjectAndRange(DriverObject,
                                                       GsivBase,
                                                       GsivSize);
    if (Entry == NULL)
    {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    KeAcquireSpinLock(&HalpSecondaryIcListLock, &OldIrql);
    RemoveEntryList(&Entry->ListEntry);
    KeReleaseSpinLock(&HalpSecondaryIcListLock, OldIrql);

    ObDereferenceObject(Entry->Interface.DriverObject);
    ExFreePoolWithTag(Entry, TAG_HAL_SECONDARY);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Masks or unmasks one of a controller's lines.
 *
 * @param[in] InputGsiv
 * The line.
 *
 * @param[in] Flags
 * Passed through to the controller.
 *
 * @param[in] MaskRequest
 * TRUE to mask, FALSE to unmask.
 *
 * @return
 * STATUS_SUCCESS, or the controller's failure.
 */
NTSTATUS
NTAPI
HalpHandleMaskUnmaskSecondaryInterrupt(
    _In_ ULONG InputGsiv,
    _In_ ULONG Flags,
    _In_ BOOLEAN MaskRequest)
{
    PSECONDARY_IC_LIST_ENTRY Entry;
    PSECONDARY_INTERRUPT_LINE_STATE Line;
    NTSTATUS Status;

    if (!HalpSecondaryIcServicesEnabled)
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (!HalpIsInterruptTypeSecondary(InterruptTypeControllerInput, InputGsiv))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Entry = HalpFindSecondaryIcEntry(InputGsiv);
    if (Entry == NULL)
    {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    Line = &Entry->State[InputGsiv - Entry->GsivBase];

    if (MaskRequest)
    {
        Status = Entry->Interface.MaskInterrupt(Entry->Interface.Context,
                                                Flags,
                                                InputGsiv);
        if (NT_SUCCESS(Status))
        {
            Line->Unmasked = FALSE;
        }
    }
    else
    {
        /*
         * The line is recorded as open before the controller is told, so an
         * interrupt that arrives inside the call is not thrown away for
         * belonging to a line the HAL still thinks is masked.
         */
        Line->Unmasked = TRUE;

        Status = Entry->Interface.UnmaskInterrupt(Entry->Interface.Context,
                                                  Flags,
                                                  InputGsiv);
        if (!NT_SUCCESS(Status))
        {
            Line->Unmasked = FALSE;
        }
    }

    return Status;
}

/**
 * @brief
 * Reports the line a secondary interrupt actually arrives on.
 *
 * A device behind a GPIO controller runs at the IRQL of the controller's own
 * interrupt, because that is the interrupt its ISR is called from. The arbiter
 * asks this to work out what IRQL to publish for the device.
 *
 * @param[in] ConnectionData
 * The device's connection data, holding the secondary GSIV.
 *
 * @param[out] PrimaryGsiv
 * Receives the controller's own GSIV.
 *
 * @return
 * STATUS_SUCCESS, or the reason it could not be answered.
 */
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
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (ConnectionData->Count != 1)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Gsiv = ConnectionData->Vectors[0].ControllerInput.Gsiv;
    if (!HalpIsInterruptTypeSecondary(ConnectionData->Vectors[0].Type, Gsiv))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Entry = HalpFindSecondaryIcEntry(Gsiv);
    if (Entry == NULL)
    {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    RtlZeroMemory(&Primary, sizeof(Primary));
    Primary.Size = sizeof(Primary);
    Primary.Version = 1;

    Status = Entry->Interface.QueryPrimaryInterrupt(Entry->Interface.Context,
                                                    Gsiv,
                                                    &Primary);

    /*
     * A controller that has not been given its own line yet answers
     * STATUS_NOT_FOUND. The reference treats that as success (:54673): the
     * caller gets whatever the block already held rather than a failure.
     */
    if (Status == STATUS_NOT_FOUND)
    {
        Status = STATUS_SUCCESS;
    }

    if (NT_SUCCESS(Status))
    {
        *PrimaryGsiv = Primary.PrimaryGsiv;
    }

    return Status;
}

/**
 * @brief
 * Has the controller arm one of its lines, and records what for.
 *
 * @param[in] ConnectionData
 * The line being connected.
 *
 * @return
 * STATUS_SUCCESS, or the controller's failure.
 */
NTSTATUS
NTAPI
HalpEnableSecondaryInterrupt(
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    PSECONDARY_IC_LIST_ENTRY Entry;
    PSECONDARY_INTERRUPT_LINE_STATE Line;
    NTSTATUS Status;
    ULONG Gsiv;

    if (!HalpSecondaryIcServicesEnabled || (ConnectionData->Count != 1))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Gsiv = ConnectionData->Vectors[0].ControllerInput.Gsiv;
    if (!HalpIsInterruptTypeSecondary(ConnectionData->Vectors[0].Type, Gsiv))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Entry = HalpFindSecondaryIcEntry(Gsiv);
    if (Entry == NULL)
    {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    Line = &Entry->State[Gsiv - Entry->GsivBase];

    Line->Mode = ConnectionData->Vectors[0].Mode;
    Line->Polarity = ConnectionData->Vectors[0].Polarity;
    Line->Vector = ConnectionData->Vectors[0].Vector;
    Line->Unmasked = TRUE;

    /*
     * The controller is handed its own list entry: that is what comes back as
     * the context of HalpInvokeIsrForGsiv, so the delivery path costs one
     * indirection rather than a list walk at interrupt time.
     */
    Status = Entry->Interface.EnableInterrupt(Entry->Interface.Context,
                                              Gsiv,
                                              Line->Mode,
                                              Line->Polarity,
                                              Entry);
    if (!NT_SUCCESS(Status))
    {
        Line->Vector = SECONDARY_VECTOR_DISCONNECTED;
        Line->Unmasked = FALSE;
    }

    return Status;
}

/**
 * @brief
 * Has the controller release one of its lines.
 *
 * @param[in] ConnectionData
 * The line being disconnected.
 *
 * @return
 * STATUS_SUCCESS, or the controller's failure.
 */
NTSTATUS
NTAPI
HalpDisableSecondaryInterrupt(
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    PSECONDARY_IC_LIST_ENTRY Entry;
    PSECONDARY_INTERRUPT_LINE_STATE Line;
    NTSTATUS Status;
    ULONG Gsiv;

    if (!HalpSecondaryIcServicesEnabled || (ConnectionData->Count != 1))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Gsiv = ConnectionData->Vectors[0].ControllerInput.Gsiv;
    if (!HalpIsInterruptTypeSecondary(ConnectionData->Vectors[0].Type, Gsiv))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Entry = HalpFindSecondaryIcEntry(Gsiv);
    if (Entry == NULL)
    {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    Status = Entry->Interface.DisableInterrupt(Entry->Interface.Context, Gsiv);
    if (NT_SUCCESS(Status))
    {
        Line = &Entry->State[Gsiv - Entry->GsivBase];
        Line->Vector = SECONDARY_VECTOR_DISCONNECTED;
        Line->Unmasked = FALSE;
    }

    return Status;
}

/**
 * @brief
 * Has a controller raise one of its lines on the HAL's behalf.
 *
 * Used to replay a pin that asserted while the machine was asleep, which is
 * the one case where nothing in the controller's own interrupt path will
 * report it.
 *
 * @param[in] Gsiv
 * The line to raise.
 *
 * @return
 * STATUS_SUCCESS, or the controller's failure.
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
    {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    if ((Entry->Interface.RequestInterrupt == NULL) ||
        !Entry->State[Gsiv - Entry->GsivBase].Unmasked)
    {
        return STATUS_UNSUCCESSFUL;
    }

    Status = Entry->Interface.RequestInterrupt(Entry->Interface.Context,
                                               Gsiv,
                                               &PrimaryGsiv);
    if (!NT_SUCCESS(Status) || (PrimaryGsiv == MAXULONG))
    {
        return Status;
    }

    /*
     * The controller answered with the line it would have raised. Raising it
     * is the HAL's job, and it is the primary controller's business, not the
     * secondary one's.
     */
    if (HalRequestInterrupt == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    HalRequestInterrupt(PrimaryGsiv);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Answers HalQuerySystemInformation for HalSecondaryInterruptInformation.
 *
 * @param[out] Information
 * Receives the GSIV range and the routines a controller drives it with.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_UNSUCCESSFUL when the services are off.
 */
NTSTATUS
NTAPI
HalpQuerySecondaryInterruptInformation(
    _Out_ PHAL_SECONDARY_INTERRUPT_INFORMATION Information)
{
    if (!HalpSecondaryIcServicesEnabled)
    {
        return STATUS_UNSUCCESSFUL;
    }

    RtlZeroMemory(Information, sizeof(*Information));

    Information->Version = HAL_SECONDARY_INTERRUPT_INFORMATION_VERSION;
    Information->GsivRangeStart = HalpSecondaryGsivRangeStart;
    Information->GsivRangeSize = HalpSecondaryGsivRangeSize;

    Information->MaskInterrupt = HalpMaskSecondaryInterrupt;
    Information->UnmaskInterrupt = HalpUnmaskSecondaryInterrupt;
    Information->InvokeIsrForGsiv = HalpInvokeIsrForGsiv;
    Information->UnregisterInterface = HalpUnregisterSecondaryIcInterface;
    Information->RequestInterrupt = HalpRequestSecondaryInterrupt;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Masks one of a secondary controller's lines.
 *
 * This is the HalMaskInterrupt slot of the private dispatch table. Only
 * secondary lines are handled: a real controller input is masked by the
 * interrupt controller code, which has its own path.
 *
 * @param[in] InputGsiv
 * The line.
 *
 * @param[in] Flags
 * Passed through to the controller.
 *
 * @return
 * STATUS_SUCCESS, or the controller's failure.
 */
NTSTATUS
NTAPI
HalpMaskSecondaryInterrupt(
    _In_ ULONG InputGsiv,
    _In_ ULONG Flags)
{
    return HalpHandleMaskUnmaskSecondaryInterrupt(InputGsiv, Flags, TRUE);
}

/**
 * @brief
 * Unmasks one of a secondary controller's lines.
 *
 * @param[in] InputGsiv
 * The line.
 *
 * @param[in] Flags
 * Passed through to the controller.
 *
 * @return
 * STATUS_SUCCESS, or the controller's failure.
 */
NTSTATUS
NTAPI
HalpUnmaskSecondaryInterrupt(
    _In_ ULONG InputGsiv,
    _In_ ULONG Flags)
{
    return HalpHandleMaskUnmaskSecondaryInterrupt(InputGsiv, Flags, FALSE);
}

/**
 * @brief
 * Makes the secondary controller services available.
 *
 * Called once at HAL initialization. Nothing is allocated here: the list is
 * empty until a controller registers, and the kernel's vector table is not
 * built until a GSIV is first handed out.
 */
CODE_SEG("INIT")
VOID
NTAPI
HalpInitializeSecondaryInterruptServices(
    VOID)
{
    InitializeListHead(&HalpSecondaryIcList);
    KeInitializeSpinLock(&HalpSecondaryIcListLock);

    HalpSecondaryIcServicesEnabled = TRUE;
}

/* EOF */
