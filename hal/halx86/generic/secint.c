/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Secondary interrupt controllers, such as GPIO blocks
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/*
 * A secondary controller takes the GSIVs past every I/O APIC input. The ACPI
 * driver gives each one a vector from 256 on, which never reaches the IDT: the
 * controller's own ISR finds the pin that fired and the kernel runs the
 * interrupt objects of its vector from there.
 */

/* GLOBALS *******************************************************************/

static HALP_SECONDARY_PROVIDER HalpSecondaryProvider;
static volatile BOOLEAN HalpSecondaryRegistered;
static KSPIN_LOCK HalpSecondaryLock;
static volatile LONG HalpNextSecondaryGsiv = HALP_SECONDARY_GSIV_BASE;

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

    /* Masking and unmasking come from interrupt service routines, above DISPATCH_LEVEL */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < HIGH_LEVEL)
        KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    KeAcquireSpinLockAtDpcLevel(&HalpSecondaryLock);
    Registered = HalpSecondaryRegistered;
    if (Registered)
        *Provider = HalpSecondaryProvider;
    KeReleaseSpinLockFromDpcLevel(&HalpSecondaryLock);
    if (OldIrql < HIGH_LEVEL)
        KeLowerIrql(OldIrql);

    return Registered;
}

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
 * @brief Runs the service routines of a secondary GSIV, from the controller's ISR.
 *
 * @param[in] ControllerContext
 * The vector the line was enabled with, handed to the controller then.
 *
 * @param[out] Claimed
 * Receives 1 when a service routine took the interrupt, 0 otherwise.
 */
static
NTSTATUS
NTAPI
HalpInvokeSecondaryIsr(
    _In_ ULONG Gsiv,
    _In_opt_ PVOID ControllerContext,
    _Out_ PULONG Claimed)
{
    *Claimed = 0;
    if (!HalpIsSecondaryGsiv(Gsiv) || !ControllerContext)
        return STATUS_INVALID_PARAMETER;

    *Claimed = KeDispatchSecondaryInterrupt((ULONG)(ULONG_PTR)ControllerContext, 0) ? 1 : 0;
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

    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    KeAcquireSpinLockAtDpcLevel(&HalpSecondaryLock);
    if (HalpSecondaryRegistered &&
        HalpSecondaryProvider.GsivBase == GsivBase &&
        HalpSecondaryProvider.GsivSize == GsivSize &&
        HalpSecondaryProvider.DriverObject == DriverObject)
    {
        HalpSecondaryRegistered = FALSE;
        RtlZeroMemory(&HalpSecondaryProvider, sizeof(HalpSecondaryProvider));
        Status = STATUS_SUCCESS;
    }
    KeReleaseSpinLockFromDpcLevel(&HalpSecondaryLock);
    KeLowerIrql(OldIrql);

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
    NTSTATUS Status;
    LONG Next;

    Next = InterlockedIncrement(&HalpNextSecondaryGsiv) - 1;
    if ((ULONG)Next - HALP_SECONDARY_GSIV_BASE >= HALP_SECONDARY_GSIV_COUNT)
    {
        InterlockedDecrement(&HalpNextSecondaryGsiv);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* The kernel needs its secondary vectors before the first line can connect */
    Status = KeInitializeSecondaryInterruptServices(NULL);
    if (!NT_SUCCESS(Status))
    {
        InterlockedDecrement(&HalpNextSecondaryGsiv);
        return Status;
    }

    DPRINT("Secondary GSIV %ld for %.*s\n", Next, OwnerNameLength, OwnerName);
    *Gsiv = (ULONG)Next;
    return STATUS_SUCCESS;
}

/**
 * @brief Reports the line a secondary GSIV is delivered through.
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

    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    KeAcquireSpinLockAtDpcLevel(&HalpSecondaryLock);
    if (HalpSecondaryRegistered)
    {
        Status = STATUS_ALREADY_REGISTERED;
    }
    else
    {
        HalpSecondaryProvider = *Provider;
        HalpSecondaryRegistered = TRUE;
    }
    KeReleaseSpinLockFromDpcLevel(&HalpSecondaryLock);
    KeLowerIrql(OldIrql);

    DPRINT1("Secondary interrupt controller %p owns GSIV %lu, %u lines: 0x%lx\n",
            Provider->DriverObject, Provider->GsivBase, Provider->GsivSize, Status);
    return Status;
}

/**
 * @brief Arms a secondary GSIV at its controller, which hands the vector back on every pin interrupt.
 */
NTSTATUS
NTAPI
HalpEnableSecondaryLine(
    _In_ PINTERRUPT_VECTOR_DATA VectorData)
{
    HALP_SECONDARY_PROVIDER Provider;

    /* The vector is the controller context, so it must be one the kernel dispatches */
    if (VectorData->Vector < 0x100)
        return STATUS_INVALID_PARAMETER;

    if (!HalpGetSecondaryProvider(&Provider))
        return STATUS_DEVICE_NOT_READY;

    return Provider.EnableInterrupt(Provider.Context,
                                    VectorData->ControllerInput.Gsiv,
                                    VectorData->Mode,
                                    VectorData->Polarity,
                                    (PVOID)(ULONG_PTR)VectorData->Vector);
}

NTSTATUS
NTAPI
HalpDisableSecondaryLine(
    _In_ PINTERRUPT_VECTOR_DATA VectorData)
{
    HALP_SECONDARY_PROVIDER Provider;

    if (!HalpGetSecondaryProvider(&Provider))
        return STATUS_DEVICE_NOT_READY;

    return Provider.DisableInterrupt(Provider.Context, VectorData->ControllerInput.Gsiv);
}
