/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Registration as a secondary interrupt controller
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * This is how a GPIO pin becomes something a driver can connect an ISR to. The
 * HAL sets aside a slice of the GSIV space it cannot describe itself; the
 * resource hub mints one of those numbers per GpioInt in the firmware; this
 * file tells the HAL that those numbers are ours, and from then on connecting
 * one arrives here as a request to arm a pin. When the pin fires, the DPC that
 * demultiplexed it calls back through InvokeIsrForGsiv and the kernel runs
 * whatever was connected.
 *
 * Registration is once for the whole driver, not once per controller: the HAL
 * routes by GSIV, and which controller a GSIV belongs to is this layer's
 * business rather than the HAL's.
 */

#define NDEBUG
#include "gpioclxp.h"

/* The secondary interrupt contract, as the kernel and the HAL declare it */
#include <ndk/haltypes.h>

/* GLOBALS *******************************************************************/

/* What the HAL told us at registration, and whether we are registered */
static HAL_SECONDARY_INTERRUPT_INFORMATION GcxSecondaryInformation;
static BOOLEAN GcxSecondaryRegistered = FALSE;
static PDRIVER_OBJECT GcxSecondaryDriverObject = NULL;

/**
 * @brief
 * One GSIV, and the pin it stands for.
 *
 * The HAL routes by GSIV alone, so turning one back into a pin is this layer's
 * job. The reference does it by lookup on every call - it asks the resource hub
 * which controller the GSIV names and then which of that controller's pins
 * (:25166, :27202) - and caches the answer as a "virq mapping". The mapping is
 * what this list holds.
 */
typedef struct _GCX_VIRQ_MAPPING
{
    LIST_ENTRY ListEntry;
    ULONG Gsiv;
    PGCX_CONTROLLER Controller;
    BANK_ID BankId;
    PIN_NUMBER PinNumber;
} GCX_VIRQ_MAPPING, *PGCX_VIRQ_MAPPING;

static LIST_ENTRY GcxVirqList;
static KSPIN_LOCK GcxVirqLock;
static BOOLEAN GcxVirqListReady = FALSE;

/*
 * Every controller this driver has brought up, so that a firmware name out of a
 * descriptor can be turned back into one of them. The reference keeps the same
 * list and searches it the same way, by name.
 */
static LIST_ENTRY GcxControllerList;
static KSPIN_LOCK GcxControllerLock;
static BOOLEAN GcxControllerListReady = FALSE;

/**
 * @brief
 * Makes sure the two lists exist.
 *
 * Both are built on first use rather than at driver entry: a class extension
 * has no entry point of its own to do it in, and the first controller to come
 * up is the first thing that can need them.
 */
static
VOID
GcxInitializeLists(
    VOID)
{
    if (!GcxControllerListReady)
    {
        InitializeListHead(&GcxControllerList);
        KeInitializeSpinLock(&GcxControllerLock);
        GcxControllerListReady = TRUE;
    }

    if (!GcxVirqListReady)
    {
        InitializeListHead(&GcxVirqList);
        KeInitializeSpinLock(&GcxVirqLock);
        GcxVirqListReady = TRUE;
    }
}

/**
 * @brief
 * Notes a controller that has come up.
 *
 * @param[in] Controller
 * The controller. Its BiosName must already be filled in.
 */
VOID
GcxAddController(
    _In_ PGCX_CONTROLLER Controller)
{
    KIRQL OldIrql;

    GcxInitializeLists();

    KeAcquireSpinLock(&GcxControllerLock, &OldIrql);
    InsertTailList(&GcxControllerList, &Controller->ControllerLink);
    KeReleaseSpinLock(&GcxControllerLock, OldIrql);
}

/**
 * @brief
 * Forgets a controller that is going away.
 *
 * @param[in] Controller
 * The controller.
 */
VOID
GcxRemoveController(
    _In_ PGCX_CONTROLLER Controller)
{
    KIRQL OldIrql;

    if (!GcxControllerListReady)
    {
        return;
    }

    KeAcquireSpinLock(&GcxControllerLock, &OldIrql);
    if (!IsListEmpty(&Controller->ControllerLink))
    {
        RemoveEntryList(&Controller->ControllerLink);
        InitializeListHead(&Controller->ControllerLink);
    }
    KeReleaseSpinLock(&GcxControllerLock, OldIrql);
}

/**
 * @brief
 * The controller the firmware calls by a given name.
 *
 * @param[in] BiosName
 * The name, as it appeared in a descriptor's ResourceSource.
 *
 * @return
 * The controller, or NULL if this driver does not own it.
 */
PGCX_CONTROLLER
GcxFindControllerByBiosName(
    _In_ PCUNICODE_STRING BiosName)
{
    PGCX_CONTROLLER Found = NULL;
    PLIST_ENTRY NextEntry;
    KIRQL OldIrql;

    if (!GcxControllerListReady)
    {
        return NULL;
    }

    KeAcquireSpinLock(&GcxControllerLock, &OldIrql);

    for (NextEntry = GcxControllerList.Flink;
         NextEntry != &GcxControllerList;
         NextEntry = NextEntry->Flink)
    {
        PGCX_CONTROLLER Controller =
            CONTAINING_RECORD(NextEntry, GCX_CONTROLLER, ControllerLink);

        if (RtlEqualUnicodeString(&Controller->BiosName, BiosName, TRUE))
        {
            Found = Controller;
            break;
        }
    }

    KeReleaseSpinLock(&GcxControllerLock, OldIrql);

    return Found;
}

/* PRIVATE FUNCTIONS *********************************************************/

/**
 * @brief
 * The pin a GSIV stands for.
 *
 * @param[in] Gsiv
 * The line.
 *
 * @return
 * Its mapping, or NULL if nothing has claimed the GSIV.
 */
static
PGCX_VIRQ_MAPPING
GcxFindVirqMapping(
    _In_ ULONG Gsiv)
{
    PGCX_VIRQ_MAPPING Found = NULL;
    PLIST_ENTRY NextEntry;
    KIRQL OldIrql;

    if (!GcxVirqListReady)
    {
        return NULL;
    }

    KeAcquireSpinLock(&GcxVirqLock, &OldIrql);

    for (NextEntry = GcxVirqList.Flink;
         NextEntry != &GcxVirqList;
         NextEntry = NextEntry->Flink)
    {
        PGCX_VIRQ_MAPPING Mapping =
            CONTAINING_RECORD(NextEntry, GCX_VIRQ_MAPPING, ListEntry);

        if (Mapping->Gsiv == Gsiv)
        {
            Found = Mapping;
            break;
        }
    }

    KeReleaseSpinLock(&GcxVirqLock, OldIrql);

    return Found;
}

/* SECONDARY CONTROLLER CALLBACKS ********************************************/

/**
 * @brief
 * Arms the pin a GSIV stands for.
 *
 * @param[in] Context
 * Unused; this layer routes by GSIV.
 *
 * @param[in] Gsiv
 * The line.
 *
 * @param[in] Mode
 * Edge or level.
 *
 * @param[in] Polarity
 * Which edge, or which level.
 *
 * @param[in] ControllerContext
 * The HAL's token for this line, to be quoted back when the pin fires.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_OBJECT_NAME_NOT_FOUND for an unclaimed GSIV.
 */
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
    PGCX_VIRQ_MAPPING Mapping;
    PGCX_PIN Pin;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Context);

    Mapping = GcxFindVirqMapping(Gsiv);
    if (Mapping == NULL)
    {
        /*
         * First time this vector has been connected. Ask the resource hub what
         * pin it stands for and remember the answer, which is what the
         * reference does on the same path.
         */
        PGCX_CONTROLLER Controller;
        BANK_ID BankId;
        PIN_NUMBER PinNumber;

        Status = GcxResolveGsivToPin(Gsiv, &Controller, &BankId, &PinNumber);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        Status = GcxAddVirqMapping(Controller, Gsiv, BankId, PinNumber);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        Mapping = GcxFindVirqMapping(Gsiv);
        if (Mapping == NULL)
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }

    Status = GcxEnableInterrupt(Mapping->Controller,
                                Mapping->BankId,
                                Mapping->PinNumber,
                                Mode,
                                Polarity,
                                0);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /*
     * The token is kept on the pin rather than on the mapping: the delivery
     * path already has the pin in hand and would otherwise have to walk the
     * mapping list at interrupt time to find it.
     */
    Pin = GcxPin(Mapping->Controller, Mapping->BankId, Mapping->PinNumber);
    if (Pin != NULL)
    {
        Pin->Gsiv = Gsiv;
        Pin->TargetContext = ControllerContext;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Stops the pin a GSIV stands for being an interrupt source.
 *
 * @param[in] Context
 * Unused.
 *
 * @param[in] Gsiv
 * The line.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_OBJECT_NAME_NOT_FOUND.
 */
static
NTSTATUS
NTAPI
GcxHubDisableInterrupt(
    _In_ PVOID Context,
    _In_ ULONG Gsiv)
{
    PGCX_VIRQ_MAPPING Mapping;

    UNREFERENCED_PARAMETER(Context);

    Mapping = GcxFindVirqMapping(Gsiv);
    if (Mapping == NULL)
    {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    return GcxDisableInterrupt(Mapping->Controller,
                               Mapping->BankId,
                               Mapping->PinNumber);
}

/**
 * @brief
 * Masks the pin a GSIV stands for.
 *
 * @param[in] Context
 * Unused.
 *
 * @param[in] Flags
 * Unused; the HAL passes its own flags through.
 *
 * @param[in] Gsiv
 * The line.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_OBJECT_NAME_NOT_FOUND.
 */
static
NTSTATUS
NTAPI
GcxHubMaskInterrupt(
    _In_ PVOID Context,
    _In_ ULONG Flags,
    _In_ ULONG Gsiv)
{
    PGCX_VIRQ_MAPPING Mapping;
    ULONG64 Failed = 0;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Flags);

    Mapping = GcxFindVirqMapping(Gsiv);
    if (Mapping == NULL)
    {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    return GcxMaskInterrupts(Mapping->Controller,
                             Mapping->BankId,
                             1ULL << Mapping->PinNumber,
                             &Failed);
}

/**
 * @brief
 * Unmasks the pin a GSIV stands for.
 *
 * @param[in] Context
 * Unused.
 *
 * @param[in] Flags
 * Unused.
 *
 * @param[in] Gsiv
 * The line.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_OBJECT_NAME_NOT_FOUND.
 */
static
NTSTATUS
NTAPI
GcxHubUnmaskInterrupt(
    _In_ PVOID Context,
    _In_ ULONG Flags,
    _In_ ULONG Gsiv)
{
    PGCX_VIRQ_MAPPING Mapping;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Flags);

    Mapping = GcxFindVirqMapping(Gsiv);
    if (Mapping == NULL)
    {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    return GcxUnmaskInterrupt(Mapping->Controller,
                              Mapping->BankId,
                              Mapping->PinNumber);
}

/**
 * @brief
 * Reports the line the controller behind a GSIV actually sits on.
 *
 * A device on a GPIO pin runs its ISR from inside the controller's own
 * interrupt, so this is where the IRQL for that device comes from.
 *
 * @param[in] Context
 * Unused.
 *
 * @param[in] Gsiv
 * The line.
 *
 * @param[out] PrimaryInformation
 * Receives the controller's own interrupt.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_NOT_FOUND while the controller has no line yet.
 */
static
NTSTATUS
NTAPI
GcxHubQueryPrimaryInterrupt(
    _In_ PVOID Context,
    _In_ ULONG Gsiv,
    _Out_ PPRIMARY_INTERRUPT_INFORMATION PrimaryInformation)
{
    PGCX_VIRQ_MAPPING Mapping;
    PGCX_CONTROLLER Controller;

    UNREFERENCED_PARAMETER(Context);

    Mapping = GcxFindVirqMapping(Gsiv);
    if (Mapping == NULL)
    {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    Controller = Mapping->Controller;
    if (Controller->PrimaryGsiv == GCX_NO_PRIMARY_GSIV)
    {
        /*
         * The controller has not been given its own interrupt yet. The HAL
         * treats this as an answer rather than a failure, so the arbiter can
         * settle the device's IRQL later without the whole query failing.
         */
        return STATUS_NOT_FOUND;
    }

    PrimaryInformation->PrimaryGsiv = Controller->PrimaryGsiv;
    PrimaryInformation->PrimaryIrql = Controller->PrimaryIrql;
    PrimaryInformation->InterruptMode = LevelSensitive;
    PrimaryInformation->Polarity = InterruptActiveLow;
    PrimaryInformation->PrimaryAffinity.Mask = Controller->PrimaryAffinity;
    PrimaryInformation->PrimaryAffinity.Group = 0;

    return STATUS_SUCCESS;
}

/* PUBLIC FUNCTIONS **********************************************************/

/**
 * @brief
 * Notes that a GSIV stands for one of a controller's pins.
 *
 * Called as a controller learns which of its pins the firmware described as
 * interrupts. The reference builds the same mapping lazily, on the first
 * connect, by asking the resource hub (:25166); building it as the pins become
 * known keeps the lookup off the connect path.
 *
 * @param[in] Controller
 * The controller the pin belongs to.
 *
 * @param[in] Gsiv
 * The line the resource hub minted for it.
 *
 * @param[in] BankId
 * The bank the pin is in.
 *
 * @param[in] PinNumber
 * The pin, as an offset within that bank.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES.
 */
NTSTATUS
GcxAddVirqMapping(
    _In_ PGCX_CONTROLLER Controller,
    _In_ ULONG Gsiv,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    PGCX_VIRQ_MAPPING Mapping;
    KIRQL OldIrql;

    if (GcxFindVirqMapping(Gsiv) != NULL)
    {
        return STATUS_SUCCESS;
    }

    Mapping = ExAllocatePoolZero(NonPagedPool, sizeof(*Mapping), GCX_POOL_TAG);
    if (Mapping == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Mapping->Gsiv = Gsiv;
    Mapping->Controller = Controller;
    Mapping->BankId = BankId;
    Mapping->PinNumber = PinNumber;

    KeAcquireSpinLock(&GcxVirqLock, &OldIrql);
    InsertTailList(&GcxVirqList, &Mapping->ListEntry);
    KeReleaseSpinLock(&GcxVirqLock, OldIrql);

    DPRINT("GpioClx: GSIV %lu is bank %u pin %u\n", Gsiv, BankId, PinNumber);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Drops every mapping that named one controller.
 *
 * @param[in] Controller
 * The controller going away.
 */
VOID
GcxRemoveVirqMappings(
    _In_ PGCX_CONTROLLER Controller)
{
    LIST_ENTRY Dead;
    PLIST_ENTRY NextEntry;
    KIRQL OldIrql;

    if (!GcxVirqListReady)
    {
        return;
    }

    InitializeListHead(&Dead);

    KeAcquireSpinLock(&GcxVirqLock, &OldIrql);

    NextEntry = GcxVirqList.Flink;
    while (NextEntry != &GcxVirqList)
    {
        PGCX_VIRQ_MAPPING Mapping =
            CONTAINING_RECORD(NextEntry, GCX_VIRQ_MAPPING, ListEntry);

        NextEntry = NextEntry->Flink;

        if (Mapping->Controller == Controller)
        {
            RemoveEntryList(&Mapping->ListEntry);
            InsertTailList(&Dead, &Mapping->ListEntry);
        }
    }

    KeReleaseSpinLock(&GcxVirqLock, OldIrql);

    while (!IsListEmpty(&Dead))
    {
        NextEntry = RemoveHeadList(&Dead);
        ExFreePoolWithTag(CONTAINING_RECORD(NextEntry, GCX_VIRQ_MAPPING, ListEntry),
                          GCX_POOL_TAG);
    }
}

/**
 * @brief
 * Hands a fired pin to whoever connected it.
 *
 * @param[in] Pin
 * The pin, carrying the token the HAL gave it when it was armed.
 *
 * @return
 * TRUE if a service routine claimed the interrupt.
 */
BOOLEAN
GcxDeliverPinInterrupt(
    _In_ PGCX_PIN Pin)
{
    if (!GcxSecondaryRegistered ||
        (GcxSecondaryInformation.InvokeIsrForGsiv == NULL) ||
        (Pin->TargetContext == NULL))
    {
        return FALSE;
    }

    return GcxSecondaryInformation.InvokeIsrForGsiv(Pin->Gsiv,
                                                    Pin->TargetContext);
}

/**
 * @brief
 * Tells the HAL that the secondary GSIV space belongs to this driver.
 *
 * Done once, on the first controller to come up. The whole range is claimed
 * rather than a slice per controller, because a GSIV says nothing about which
 * controller minted it - the mapping from one to the other is kept here.
 *
 * @param[in] DriverObject
 * The client driver's object, which is what the HAL keys the registration on.
 *
 * @return
 * STATUS_SUCCESS, or the reason the HAL would not take it.
 */
NTSTATUS
GcxRegisterSecondaryInterruptController(
    _In_ PDRIVER_OBJECT DriverObject)
{
    SECONDARY_INTERRUPT_PROVIDER_INTERFACE Interface;
    ULONG Length = sizeof(GcxSecondaryInformation);
    NTSTATUS Status;

    if (GcxSecondaryRegistered)
    {
        return STATUS_SUCCESS;
    }

    GcxInitializeLists();

    Status = HalQuerySystemInformation(HalSecondaryInterruptInformation,
                                       sizeof(GcxSecondaryInformation),
                                       &GcxSecondaryInformation,
                                       &Length);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: HAL has no secondary interrupt space: 0x%08lX\n", Status);
        return Status;
    }

    if (GcxSecondaryInformation.GsivRangeSize == 0)
    {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(&Interface, sizeof(Interface));
    Interface.Size = sizeof(Interface);
    Interface.Version = SECONDARY_INTERRUPT_PROVIDER_INTERFACE_VERSION;
    Interface.GsivBase = GcxSecondaryInformation.GsivRangeStart;
    Interface.GsivSize = (USHORT)min(GcxSecondaryInformation.GsivRangeSize, MAXUSHORT);
    Interface.DriverObject = DriverObject;

    Interface.EnableInterrupt = GcxHubEnableInterrupt;
    Interface.DisableInterrupt = GcxHubDisableInterrupt;
    Interface.MaskInterrupt = GcxHubMaskInterrupt;
    Interface.UnmaskInterrupt = GcxHubUnmaskInterrupt;
    Interface.QueryPrimaryInterrupt = GcxHubQueryPrimaryInterrupt;

    Status = HalSetSystemInformation(HalRegisterSecondaryInterruptInterface,
                                     sizeof(Interface),
                                     &Interface);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: HAL refused the registration: 0x%08lX\n", Status);
        return Status;
    }

    GcxSecondaryDriverObject = DriverObject;
    GcxSecondaryRegistered = TRUE;

    DPRINT1("GpioClx: owns GSIV %lu..%lu\n",
            Interface.GsivBase,
            Interface.GsivBase + Interface.GsivSize - 1);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Gives the GSIV space back.
 */
VOID
GcxUnregisterSecondaryInterruptController(
    VOID)
{
    if (!GcxSecondaryRegistered)
    {
        return;
    }

    if (GcxSecondaryInformation.UnregisterInterface != NULL)
    {
        GcxSecondaryInformation.UnregisterInterface(
            GcxSecondaryInformation.GsivRangeStart,
            GcxSecondaryInformation.GsivRangeSize,
            GcxSecondaryDriverObject);
    }

    GcxSecondaryRegistered = FALSE;
    GcxSecondaryDriverObject = NULL;
}

/* EOF */
