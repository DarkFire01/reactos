/*
 * PROJECT:     ReactOS ARM64 HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     IRQL and software interrupt management
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/*
 * The IRQL lives in the PCR and the GIC priority mask follows it, so only
 * interrupts above the current IRQL reach the processor. The software
 * interrupts are SGIs numbered like their IRQL.
 */

/* GLOBALS *******************************************************************/

/* Software interrupts each processor requested for itself and has not taken yet, one bit per IRQL */
static volatile LONG HalpPendingSoftwareInterrupts[MAXIMUM_PROCESSORS];

/* FUNCTIONS *****************************************************************/

/*
 * Below DISPATCH_LEVEL a thread can move to another processor between any two
 * instructions, and x18 then names the new one. The PCR is only read with
 * interrupts off, and the barrier keeps the compiler from reading x18 earlier.
 */
FORCEINLINE
PKPCR
HalpLockPcr(
    _Out_ PBOOLEAN Enabled)
{
    *Enabled = KeDisableInterrupts();
    _ReadWriteBarrier();
    return KeGetPcr();
}

FORCEINLINE
VOID
HalpUnlockPcr(
    _In_ BOOLEAN Enabled)
{
    _ReadWriteBarrier();
    KeRestoreInterrupts(Enabled);
}

static
VOID
HalpWaitForSoftwareInterrupts(
    _In_ KIRQL Irql)
{
    BOOLEAN Enabled;
    ULONG Spins;
    LONG Pending;

    /* Nothing can be taken with interrupts off, they run once they are on again */
    Enabled = KeDisableInterrupts();
    KeRestoreInterrupts(Enabled);
    if (!Enabled)
        return;

    /* Bounded, a lost SGI must not hang the processor. A thread that moved has nothing left to wait for */
    for (Spins = 0; Spins < 1000000; Spins++)
    {
        HalpLockPcr(&Enabled);
        Pending = HalpPendingSoftwareInterrupts[KeGetCurrentProcessorNumber()];
        HalpUnlockPcr(Enabled);

        if (!(Pending >> (Irql + 1)))
            break;
        YieldProcessor();
    }
}

VOID
NTAPI
HalpSetPriorityMask(
    _In_ KIRQL Irql)
{
    HalpGicSetPriorityMask(HalpIrqlToPriority(Irql));
}

#undef KeGetCurrentIrql
KIRQL
NTAPI
KeGetCurrentIrql(VOID)
{
    return KeGetPcr()->CurrentIrql;
}

/* The PCR and the mask change together, on one processor, so an interrupt never sees them disagree */
KIRQL
FASTCALL
KfRaiseIrql(
    _In_ KIRQL NewIrql)
{
    BOOLEAN Enabled;
    KIRQL OldIrql;
    PKPCR Pcr;

    Pcr = HalpLockPcr(&Enabled);
    OldIrql = Pcr->CurrentIrql;
    if (NewIrql < OldIrql)
    {
        HalpUnlockPcr(Enabled);
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL, OldIrql, NewIrql, 0, 9);
    }

    Pcr->CurrentIrql = NewIrql;
    HalpSetPriorityMask(NewIrql);
    HalpUnlockPcr(Enabled);
    return OldIrql;
}

VOID
FASTCALL
KfLowerIrql(
    _In_ KIRQL NewIrql)
{
    BOOLEAN Enabled;
    KIRQL OldIrql;
    PKPCR Pcr;

    Pcr = HalpLockPcr(&Enabled);
    OldIrql = Pcr->CurrentIrql;
    if (NewIrql > OldIrql)
    {
        HalpUnlockPcr(Enabled);
        KeBugCheckEx(IRQL_NOT_LESS_OR_EQUAL, NewIrql, OldIrql, 0, 9);
    }

    Pcr->CurrentIrql = NewIrql;
    HalpSetPriorityMask(NewIrql);
    HalpUnlockPcr(Enabled);

    HalpWaitForSoftwareInterrupts(NewIrql);
}

VOID
FASTCALL
HalRequestSoftwareInterrupt(
    _In_ KIRQL Irql)
{
    BOOLEAN Enabled;
    KIRQL CurrentIrql;
    ULONG Number;
    PKPCR Pcr;

    /* The request belongs to the processor the caller runs on when it is made */
    Pcr = HalpLockPcr(&Enabled);
    Number = KeGetCurrentProcessorNumber();
    CurrentIrql = Pcr->CurrentIrql;
    InterlockedOr(&HalpPendingSoftwareInterrupts[Number], 1L << Irql);
    HalpGicSendSgi(Irql, AFFINITY_MASK(Number));
    HalpUnlockPcr(Enabled);

    if (Irql > CurrentIrql)
        HalpWaitForSoftwareInterrupts(CurrentIrql);
}

VOID
FASTCALL
HalClearSoftwareInterrupt(
    _In_ KIRQL Irql)
{
    /* A requested SGI is always delivered, the handlers cope with an empty queue */
    UNREFERENCED_PARAMETER(Irql);
}

VOID
NTAPI
HalRequestIpi(
    _In_ KAFFINITY TargetProcessors)
{
    HalpGicSendSgi(IPI_LEVEL, TargetProcessors);
}

/**
 * @brief Acknowledges the highest priority pending interrupt.
 *
 * @return The INTID, or GIC_SPURIOUS_INTID when there is none.
 */
ULONG
FASTCALL
HalGetInterruptSource(VOID)
{
    return HalpGicAcknowledge();
}

/**
 * @brief Raises to the IRQL of the interrupt and ends it at the GIC.
 *
 * The early end of interrupt only drops the running priority, the priority
 * mask keeps the line quiet until the IRQL goes back down.
 */
BOOLEAN
NTAPI
HalBeginSystemInterrupt(
    _In_ KIRQL Irql,
    _In_ ULONG Vector,
    _Out_ PKIRQL OldIrql)
{
    KIRQL CurrentIrql;

    CurrentIrql = KeGetPcr()->CurrentIrql;
    *OldIrql = CurrentIrql;

    if (Irql > CurrentIrql)
    {
        KeGetPcr()->CurrentIrql = Irql;
        HalpSetPriorityMask(Irql);
    }

    HalpGicEndOfInterrupt(Vector);

    if (Irql > CurrentIrql)
    {
        if (Vector < GIC_SGI_COUNT)
            InterlockedAnd(&HalpPendingSoftwareInterrupts[KeGetCurrentProcessorNumber()], ~(1L << Vector));
        return TRUE;
    }

    /* It slipped in before the mask went up, send software interrupts again */
    if (Vector < GIC_SGI_COUNT)
        HalpGicSendSgi(Vector, AFFINITY_MASK(KeGetCurrentProcessorNumber()));

    return FALSE;
}

VOID
NTAPI
HalEndSystemInterrupt(
    _In_ KIRQL OldIrql,
    _In_ PKTRAP_FRAME TrapFrame)
{
    UNREFERENCED_PARAMETER(TrapFrame);

    KeGetPcr()->CurrentIrql = OldIrql;
    HalpSetPriorityMask(OldIrql);
}

BOOLEAN
NTAPI
HalEnableSystemInterrupt(
    _In_ ULONG Vector,
    _In_ KIRQL Irql,
    _In_ KINTERRUPT_MODE InterruptMode)
{
    /* A secondary controller arms its own lines once the ISR is connected */
    if (HalpIsSecondaryGsiv(Vector))
        return TRUE;

    if ((Vector < GIC_SGI_COUNT) || (Vector > GIC_MAX_INTID))
        return FALSE;

    HalpGicSetPriority(Vector, HalpIrqlToPriority(Irql));
    return HalpGicEnableInterrupt(Vector, InterruptMode);
}

VOID
NTAPI
HalDisableSystemInterrupt(
    _In_ ULONG Vector,
    _In_ KIRQL Irql)
{
    UNREFERENCED_PARAMETER(Irql);

    if ((Vector < GIC_SGI_COUNT) || (Vector > GIC_MAX_INTID))
        return;

    HalpGicDisableInterrupt(Vector);
}

/**
 * @brief Translates a bus interrupt into a system vector and IRQL.
 *
 * Firmware describes interrupts as GSIVs, which are the GIC INTIDs, so the
 * vector is the level itself. Device interrupts run between DISPATCH_LEVEL
 * and CLOCK_LEVEL.
 */
ULONG
NTAPI
HalGetInterruptVector(
    _In_ INTERFACE_TYPE InterfaceType,
    _In_ ULONG BusNumber,
    _In_ ULONG BusInterruptLevel,
    _In_ ULONG BusInterruptVector,
    _Out_ PKIRQL Irql,
    _Out_ PKAFFINITY Affinity)
{
    UNREFERENCED_PARAMETER(InterfaceType);
    UNREFERENCED_PARAMETER(BusNumber);
    UNREFERENCED_PARAMETER(BusInterruptLevel);

    if (HalpGetSecondaryVector(BusInterruptVector, Irql, Affinity))
        return BusInterruptVector;

    if ((BusInterruptVector < GIC_PPI_FIRST) || (BusInterruptVector > GIC_MAX_INTID))
    {
        *Irql = 0;
        *Affinity = 0;
        return 0;
    }

    *Irql = HalConvertDeviceIdtToIrql(BusInterruptVector);
    *Affinity = 1;
    return BusInterruptVector;
}

/* Device vectors are the GIC interrupt IDs, spread over the device IRQLs */
KIRQL
NTAPI
HalConvertDeviceIdtToIrql(
    _In_ ULONG IdtEntry)
{
    return (KIRQL)(DISPATCH_LEVEL + 1 + (IdtEntry % (CLOCK_LEVEL - DISPATCH_LEVEL - 1)));
}

BOOLEAN
FASTCALL
HalSystemVectorDispatchEntry(
    _In_ ULONG Vector,
    _Out_ PKINTERRUPT_ROUTINE **FlatDispatch,
    _Out_ PKINTERRUPT_ROUTINE *NoConnection)
{
    UNREFERENCED_PARAMETER(Vector);
    UNREFERENCED_PARAMETER(FlatDispatch);
    UNREFERENCED_PARAMETER(NoConnection);
    return FALSE;
}

/* Line interrupts reach the GIC as their GSIV, which is also the vector */
static
PINTERRUPT_VECTOR_DATA
HalpGetLineVector(
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    PINTERRUPT_VECTOR_DATA VectorData;

    if ((ConnectionData == NULL) || (ConnectionData->Count != 1))
        return NULL;

    VectorData = &ConnectionData->Vectors[0];
    if ((VectorData->Type != InterruptTypeControllerInput) ||
        (VectorData->Vector != VectorData->ControllerInput.Gsiv))
    {
        return NULL;
    }

    return VectorData;
}

NTSTATUS
NTAPI
HalEnableInterrupt(
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    PINTERRUPT_VECTOR_DATA VectorData = HalpGetLineVector(ConnectionData);

    if (!VectorData)
        return STATUS_INVALID_PARAMETER;

    if (HalpIsSecondaryGsiv(VectorData->Vector))
        return HalpEnableSecondaryLine(VectorData->Vector, VectorData->Mode, VectorData->Polarity);

    if (!HalEnableSystemInterrupt(VectorData->Vector, VectorData->Irql, VectorData->Mode))
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HalDisableInterrupt(
    _In_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    PINTERRUPT_VECTOR_DATA VectorData = HalpGetLineVector(ConnectionData);

    if (!VectorData)
        return STATUS_INVALID_PARAMETER;

    if (HalpIsSecondaryGsiv(VectorData->Vector))
        return HalpDisableSecondaryLine(VectorData->Vector);

    HalDisableSystemInterrupt(VectorData->Vector, VectorData->Irql);
    return STATUS_SUCCESS;
}

/* Message signaled interrupts need the GIC ITS */
NTSTATUS
NTAPI
HalGetMessageRoutingInfo(
    _In_ PHAL_MESSAGE_SIGNAL_TARGET_REQUEST Request,
    _Out_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(ConnectionData);
    return STATUS_NOT_SUPPORTED;
}
