/*
 * PROJECT:     ReactOS ARM64 HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Secondary interrupt controllers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/*
 * A secondary controller, such as a GPIO block, takes the GSIVs above the GIC.
 * Their interrupt objects sit in the PCR like any other vector, but the GIC
 * never raises them: the controller's own ISR finds the pin that fired and
 * hands its GSIV back here, and the objects run inside that ISR.
 */

/* GLOBALS *******************************************************************/

static HALP_SECONDARY_PROVIDER HalpSecondaryProvider;
static volatile BOOLEAN HalpSecondaryRegistered;
static KSPIN_LOCK HalpSecondaryLock;
static volatile LONG HalpNextSecondaryGsiv;

/* FUNCTIONS *****************************************************************/

/**
 * @brief Copies the registered provider, or returns FALSE when there is none.
 */
static
BOOLEAN
HalpGetSecondaryProvider(
    _Out_ PHALP_SECONDARY_PROVIDER Provider)
{
    BOOLEAN Registered;
    KIRQL OldIrql;

    KeAcquireSpinLock(&HalpSecondaryLock, &OldIrql);
    Registered = HalpSecondaryRegistered;
    if (Registered)
        *Provider = HalpSecondaryProvider;
    KeReleaseSpinLock(&HalpSecondaryLock, OldIrql);

    return Registered;
}

/**
 * @brief Masks a secondary GSIV at its controller.
 */
static
NTSTATUS
NTAPI
HalpMaskSecondaryInterrupt(
    _In_ ULONG Gsiv,
    _In_ ULONG Flags)
{
    HALP_SECONDARY_PROVIDER Provider;

    if (!HalpIsSecondaryGsiv(Gsiv))
        return STATUS_INVALID_PARAMETER;
    if (!HalpGetSecondaryProvider(&Provider) || !Provider.MaskInterrupt)
        return STATUS_NOT_FOUND;

    return Provider.MaskInterrupt(Provider.Context, Flags, Gsiv);
}

static
NTSTATUS
NTAPI
HalpUnmaskSecondaryInterrupt(
    _In_ ULONG Gsiv,
    _In_ ULONG Flags)
{
    HALP_SECONDARY_PROVIDER Provider;

    if (!HalpIsSecondaryGsiv(Gsiv))
        return STATUS_INVALID_PARAMETER;
    if (!HalpGetSecondaryProvider(&Provider) || !Provider.UnmaskInterrupt)
        return STATUS_NOT_FOUND;

    return Provider.UnmaskInterrupt(Provider.Context, Flags, Gsiv);
}

/**
 * @brief Runs the service routines connected to a secondary GSIV.
 *
 * @param[out] Claimed
 * Receives 1 when a service routine took the interrupt, 0 otherwise.
 *
 * @remarks Called from the controller's ISR, so at its IRQL.
 */
static
NTSTATUS
NTAPI
HalpInvokeSecondaryIsr(
    _In_ ULONG Gsiv,
    _In_opt_ PVOID ControllerContext,
    _Out_ PULONG Claimed)
{
    PKINTERRUPT Head, Interrupt;
    PLIST_ENTRY Entry;
    BOOLEAN Handled = FALSE;

    UNREFERENCED_PARAMETER(ControllerContext);

    *Claimed = 0;
    if (!HalpIsSecondaryGsiv(Gsiv))
        return STATUS_INVALID_PARAMETER;

    Head = *KiInterruptObjectSlot((PKIPCR)KeGetPcr(), Gsiv);
    if (!Head)
        return STATUS_NOT_FOUND;

    /* The first object heads the list of the ones sharing the GSIV */
    Entry = &Head->InterruptListEntry;
    do
    {
        Interrupt = CONTAINING_RECORD(Entry, KINTERRUPT, InterruptListEntry);
        KeAcquireSpinLockAtDpcLevel(Interrupt->ActualLock);
        if (Interrupt->ServiceRoutine(Interrupt, Interrupt->ServiceContext))
            Handled = TRUE;
        KeReleaseSpinLockFromDpcLevel(Interrupt->ActualLock);
        Entry = Entry->Flink;
    } while (Entry != &Head->InterruptListEntry);

    *Claimed = Handled ? 1 : 0;
    return STATUS_SUCCESS;
}

/**
 * @brief Drops the provider that owns a GSIV range.
 */
static
NTSTATUS
NTAPI
HalpUnregisterSecondaryProvider(
    _In_ ULONG GsivBase,
    _In_ ULONG GsivSize,
    _In_ PDRIVER_OBJECT DriverObject)
{
    NTSTATUS Status = STATUS_NOT_FOUND;
    KIRQL OldIrql;

    KeAcquireSpinLock(&HalpSecondaryLock, &OldIrql);
    if (HalpSecondaryRegistered &&
        HalpSecondaryProvider.GsivBase == GsivBase &&
        HalpSecondaryProvider.GsivSize == GsivSize &&
        HalpSecondaryProvider.DriverObject == DriverObject)
    {
        HalpSecondaryRegistered = FALSE;
        RtlZeroMemory(&HalpSecondaryProvider, sizeof(HalpSecondaryProvider));
        Status = STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&HalpSecondaryLock, OldIrql);

    return Status;
}

static
NTSTATUS
NTAPI
HalpRequestSecondaryInterrupt(
    _In_ ULONG Gsiv)
{
    UNREFERENCED_PARAMETER(Gsiv);

    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief Tells whether an interrupt reaches the system through a secondary controller.
 */
static
BOOLEAN
NTAPI
HalpIsInterruptTypeSecondary(
    _In_ ULONG Type,
    _In_ ULONG InputGsiv)
{
    return (Type == InterruptTypeControllerInput) && HalpIsSecondaryGsiv(InputGsiv);
}

/**
 * @brief Hands out the next free GSIV of the secondary range.
 *
 * @param[in] OwnerName
 * The controller the GSIV is minted for, kept only for tracing.
 */
static
NTSTATUS
NTAPI
HalpAllocateGsivForSecondaryInterrupt(
    _In_reads_bytes_(OwnerNameLength) PCCHAR OwnerName,
    _In_ USHORT OwnerNameLength,
    _Out_ PULONG Gsiv)
{
    LONG Next;

    Next = InterlockedIncrement(&HalpNextSecondaryGsiv) - 1;
    if ((ULONG)Next - HALP_SECONDARY_GSIV_BASE >= HALP_SECONDARY_GSIV_COUNT)
    {
        InterlockedDecrement(&HalpNextSecondaryGsiv);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    DPRINT("Secondary GSIV %ld for %.*s\n", Next, OwnerNameLength, OwnerName);
    *Gsiv = (ULONG)Next;
    return STATUS_SUCCESS;
}

/**
 * @brief Reports the GIC line a secondary GSIV is delivered through.
 */
static
NTSTATUS
NTAPI
HalpSecondaryInterruptQueryPrimaryInformation(
    _In_ PINTERRUPT_VECTOR_DATA VectorData,
    _Out_ PULONG PrimaryGsiv)
{
    HALP_SECONDARY_PROVIDER Provider;
    HALP_PRIMARY_LINE Primary;
    NTSTATUS Status;

    if (VectorData->Type != InterruptTypeControllerInput ||
        !HalpIsSecondaryGsiv(VectorData->ControllerInput.Gsiv))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (!HalpGetSecondaryProvider(&Provider) || !Provider.QueryPrimaryInterrupt)
        return STATUS_NOT_FOUND;

    RtlZeroMemory(&Primary, sizeof(Primary));
    Status = Provider.QueryPrimaryInterrupt(Provider.Context, VectorData->ControllerInput.Gsiv, &Primary);
    if (NT_SUCCESS(Status))
        *PrimaryGsiv = Primary.PrimaryGsiv;

    return Status;
}

VOID
NTAPI
HalpInitializeSecondaryInterrupts(VOID)
{
    KeInitializeSpinLock(&HalpSecondaryLock);
    HalpNextSecondaryGsiv = HALP_SECONDARY_GSIV_BASE;

    HalSecondaryInterruptQueryPrimaryInformation = HalpSecondaryInterruptQueryPrimaryInformation;
    HalMaskInterrupt = HalpMaskSecondaryInterrupt;
    HalUnmaskInterrupt = HalpUnmaskSecondaryInterrupt;
    HalIsInterruptTypeSecondary = HalpIsInterruptTypeSecondary;
    HalAllocateGsivForSecondaryInterrupt = HalpAllocateGsivForSecondaryInterrupt;
}

/**
 * @brief Answers HalSecondaryInterruptInformation with the range and the routines a controller calls.
 */
NTSTATUS
NTAPI
HalpQuerySecondaryInformation(
    _Out_writes_bytes_(BufferSize) PVOID Buffer,
    _In_ ULONG BufferSize,
    _Out_ PULONG ReturnedLength)
{
    PHALP_SECONDARY_INFORMATION Information = Buffer;

    *ReturnedLength = sizeof(*Information);
    if (BufferSize < sizeof(*Information))
        return STATUS_INFO_LENGTH_MISMATCH;

    RtlZeroMemory(Information, sizeof(*Information));
    Information->Version = HALP_SECONDARY_INFORMATION_VERSION;
    Information->GsivRangeStart = HALP_SECONDARY_GSIV_BASE;
    Information->GsivRangeSize = HALP_SECONDARY_GSIV_COUNT;
    Information->MaskInterrupt = (PVOID)HalpMaskSecondaryInterrupt;
    Information->UnmaskInterrupt = (PVOID)HalpUnmaskSecondaryInterrupt;
    Information->InvokeIsrForGsiv = (PVOID)HalpInvokeSecondaryIsr;
    Information->UnregisterInterface = (PVOID)HalpUnregisterSecondaryProvider;
    Information->RequestInterrupt = (PVOID)HalpRequestSecondaryInterrupt;
    return STATUS_SUCCESS;
}

/**
 * @brief Takes a controller as the owner of the secondary GSIV range.
 *
 * @return
 * STATUS_INVALID_PARAMETER for a short or foreign range, STATUS_ALREADY_REGISTERED
 * when another controller owns it.
 */
NTSTATUS
NTAPI
HalpRegisterSecondaryProvider(
    _In_reads_bytes_(BufferSize) PVOID Buffer,
    _In_ ULONG BufferSize)
{
    PHALP_SECONDARY_PROVIDER Provider = Buffer;
    NTSTATUS Status = STATUS_SUCCESS;
    KIRQL OldIrql;

    if (BufferSize < sizeof(*Provider) || Provider->Size < sizeof(*Provider) ||
        Provider->Version != HALP_SECONDARY_PROVIDER_VERSION ||
        !Provider->EnableInterrupt || !Provider->DisableInterrupt || !Provider->QueryPrimaryInterrupt)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Provider->GsivBase < HALP_SECONDARY_GSIV_BASE ||
        Provider->GsivBase - HALP_SECONDARY_GSIV_BASE + Provider->GsivSize > HALP_SECONDARY_GSIV_COUNT)
    {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&HalpSecondaryLock, &OldIrql);
    if (HalpSecondaryRegistered)
    {
        Status = STATUS_ALREADY_REGISTERED;
    }
    else
    {
        HalpSecondaryProvider = *Provider;
        HalpSecondaryRegistered = TRUE;
    }
    KeReleaseSpinLock(&HalpSecondaryLock, OldIrql);

    DPRINT1("Secondary interrupt controller %p owns GSIV %lu, %u lines: 0x%lx\n",
            Provider->DriverObject, Provider->GsivBase, Provider->GsivSize, Status);
    return Status;
}

/**
 * @brief Picks the IRQL and processors for a secondary GSIV.
 *
 * Its service routines run inside the controller's ISR, so they take the IRQL of
 * that line. Before the controller registers, the highest device IRQL keeps
 * KeSynchronizeExecution above wherever the line ends up.
 */
BOOLEAN
NTAPI
HalpGetSecondaryVector(
    _In_ ULONG Gsiv,
    _Out_ PKIRQL Irql,
    _Out_ PKAFFINITY Affinity)
{
    HALP_SECONDARY_PROVIDER Provider;
    HALP_PRIMARY_LINE Primary;

    if (!HalpIsSecondaryGsiv(Gsiv))
        return FALSE;

    *Irql = CLOCK_LEVEL - 1;
    *Affinity = 1;

    if (!HalpGetSecondaryProvider(&Provider))
        return TRUE;

    RtlZeroMemory(&Primary, sizeof(Primary));
    if (NT_SUCCESS(Provider.QueryPrimaryInterrupt(Provider.Context, Gsiv, &Primary)))
    {
        *Irql = Primary.PrimaryIrql;
        *Affinity = Primary.PrimaryAffinity.Mask ? Primary.PrimaryAffinity.Mask : 1;
    }

    return TRUE;
}

/**
 * @brief Arms a secondary GSIV at its controller, at PASSIVE_LEVEL.
 */
NTSTATUS
NTAPI
HalpEnableSecondaryLine(
    _In_ ULONG Gsiv,
    _In_ KINTERRUPT_MODE Mode,
    _In_ KINTERRUPT_POLARITY Polarity)
{
    HALP_SECONDARY_PROVIDER Provider;

    if (!HalpGetSecondaryProvider(&Provider))
        return STATUS_DEVICE_NOT_READY;

    return Provider.EnableInterrupt(Provider.Context, Gsiv, Mode, Polarity, (PVOID)(ULONG_PTR)Gsiv);
}

NTSTATUS
NTAPI
HalpDisableSecondaryLine(
    _In_ ULONG Gsiv)
{
    HALP_SECONDARY_PROVIDER Provider;

    if (!HalpGetSecondaryProvider(&Provider))
        return STATUS_DEVICE_NOT_READY;

    return Provider.DisableInterrupt(Provider.Context, Gsiv);
}
