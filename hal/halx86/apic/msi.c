/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Message-signaled interrupt support for the APIC HAL
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Layouts shared with the kernel and the bus drivers */
#ifdef _M_IX86
C_ASSERT(sizeof(INTERRUPT_VECTOR_DATA) == 80);
C_ASSERT(FIELD_OFFSET(INTERRUPT_CONNECTION_DATA, Vectors) == 8);
C_ASSERT(sizeof(HAL_INTERRUPT_TARGET_INFORMATION) == 24);
C_ASSERT(FIELD_OFFSET(HAL_MESSAGE_TARGET_REQUEST, Apic.DestinationMode) == 0x18);
#endif

/* Defined in acpi/madt.c */
extern const PPROCESSOR_IDENTITY HalpProcessorIdentity;

/* Defined in generic/misc.c */
extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

/* Resolved once at phase 0, see HalpInitializeMessageInterrupts */
static BOOLEAN HalpMessageInterruptsEnabled;

static KSPIN_LOCK HalpMessageVectorLock;

/* A block of message vectors has to fit in a single priority row */
#define HALP_MAX_MESSAGE_VECTORS 16

/* PRIVATE FUNCTIONS **********************************************************/

/* Only the processors that route the local APIC address range to themselves */
static
BOOLEAN
HalpCpuSupportsMessageInterrupts(VOID)
{
    PKPRCB Prcb = KeGetCurrentPrcb();
    BOOLEAN IsIntel, IsAmd;

    IsIntel = (strncmp((PCSTR)Prcb->VendorString, "GenuineIntel", 12) == 0);
    IsAmd = (strncmp((PCSTR)Prcb->VendorString, "AuthenticAMD", 12) == 0);
    if (!IsIntel && !IsAmd)
    {
        return FALSE;
    }

    if (Prcb->CpuType >= 15)
    {
        return TRUE;
    }

    /* Pentium M and the family 6 models that came after it */
    return IsIntel && (Prcb->CpuType == 6) && ((Prcb->CpuStep >> 8) >= 0x0D);
}

/* Flat logical mode holds one bit per processor, so it only covers 8 of them */
static
HAL_APIC_DESTINATION_MODE
HalpGetApicDestinationMode(VOID)
{
    ULONG Count = max(HalpApicInfoTable.ProcessorCount, (ULONG)KeNumberProcessors);

    return (Count > 8) ? ApicDestinationModePhysical : ApicDestinationModeLogicalFlat;
}

static
NTSTATUS
HalpGetLocalApicIdForProcessor(
    _In_ ULONG ProcessorNumber,
    _Out_ PULONG ApicId)
{
    if (ProcessorNumber == KeGetCurrentProcessorNumber())
    {
        *ApicId = ApicRead(APIC_ID) >> 24;
        return STATUS_SUCCESS;
    }

    if ((ProcessorNumber >= HalpApicInfoTable.ProcessorCount) ||
        (ProcessorNumber >= MAXIMUM_PROCESSORS))
    {
        return STATUS_NOT_FOUND;
    }

    *ApicId = HalpProcessorIdentity[ProcessorNumber].LapicId;
    return STATUS_SUCCESS;
}

static
NTSTATUS
HalpGetProcessorForLocalApicId(
    _In_ ULONG ApicId,
    _Out_ PULONG ProcessorNumber)
{
    ULONG Index;

    if (ApicId == (ApicRead(APIC_ID) >> 24))
    {
        *ProcessorNumber = KeGetCurrentProcessorNumber();
        return STATUS_SUCCESS;
    }

    for (Index = 0; Index < HalpApicInfoTable.ProcessorCount; Index++)
    {
        if (HalpProcessorIdentity[Index].LapicId == ApicId)
        {
            *ProcessorNumber = Index;
            return STATUS_SUCCESS;
        }
    }

    return STATUS_NOT_FOUND;
}

/**
 * @brief
 * Turns a processor set into an APIC destination. A lone processor is named by
 * its physical APIC ID, a set of them by the flat logical IDs of its members.
 *
 * @param[in] TargetProcessors
 * Processors the interrupt should reach.
 *
 * @param[out] Logical
 * Receives TRUE when the destination holds logical IDs.
 *
 * @param[out] Destination
 * Receives the destination field of a redirection entry or message address.
 */
NTSTATUS
NTAPI
HalpBuildInterruptDestination(
    _In_ KAFFINITY TargetProcessors,
    _Out_ PBOOLEAN Logical,
    _Out_ PUCHAR Destination)
{
    ULONG Processor, ApicId;
    NTSTATUS Status;

    if (TargetProcessors == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((TargetProcessors & (TargetProcessors - 1)) == 0)
    {
        BitScanForwardAffinity(&Processor, TargetProcessors);
        Status = HalpGetLocalApicIdForProcessor(Processor, &ApicId);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        *Logical = FALSE;
        *Destination = (UCHAR)ApicId;
        return STATUS_SUCCESS;
    }

    /* Bit N of a flat logical ID belongs to processor N */
    if ((HalpGetApicDestinationMode() != ApicDestinationModeLogicalFlat) ||
        (TargetProcessors > 0xFF))
    {
        return STATUS_INVALID_PARAMETER;
    }

    *Logical = TRUE;
    *Destination = (UCHAR)TargetProcessors;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reserves a run of vectors for message-signaled interrupts. The run starts on
 * a multiple of its own size, so a device can add the message number to the
 * base vector and land on one of them.
 *
 * @param[in] Count
 * Number of vectors wanted.
 *
 * @param[out] BaseVector
 * Receives the first vector of the run.
 */
static
NTSTATUS
HalpAllocateMessageVectors(
    _In_ ULONG Count,
    _Out_ PUCHAR BaseVector)
{
    KIRQL OldIrql, Irql;
    ULONG Alignment, Row, LastRow, Base, Index;

    if ((Count == 0) || (Count > HALP_MAX_MESSAGE_VECTORS))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Round the run up to a power of two */
    for (Alignment = 1; Alignment < Count; Alignment <<= 1)
    {
        NOTHING;
    }

    KeAcquireSpinLock(&HalpMessageVectorLock, &OldIrql);

    LastRow = 0;
    for (Irql = CLOCK_LEVEL - 1; Irql >= CMCI_LEVEL; Irql--)
    {
        /* Several IRQLs can share a row, so walk each row only once */
        Row = IrqlToTpr(Irql) & 0xF0;
        if (Row == LastRow)
        {
            continue;
        }
        LastRow = Row;

        for (Base = Row; Base + Count <= Row + 16; Base += Alignment)
        {
            for (Index = 0; Index < Count; Index++)
            {
                if (HalpVectorToIndex[Base + Index] != APIC_FREE_VECTOR)
                {
                    break;
                }
            }

            if (Index != Count)
            {
                continue;
            }

            for (Index = 0; Index < Count; Index++)
            {
                HalpVectorToIndex[Base + Index] = APIC_MSI_VECTOR;
            }

            KeReleaseSpinLock(&HalpMessageVectorLock, OldIrql);
            *BaseVector = (UCHAR)Base;
            return STATUS_SUCCESS;
        }
    }

    KeReleaseSpinLock(&HalpMessageVectorLock, OldIrql);
    DPRINT1("No room for %lu message vector(s)\n", Count);
    return STATUS_INSUFFICIENT_RESOURCES;
}

static
VOID
HalpFreeMessageVectors(
    _In_ UCHAR BaseVector,
    _In_ ULONG Count)
{
    KIRQL OldIrql;
    ULONG Vector;

    KeAcquireSpinLock(&HalpMessageVectorLock, &OldIrql);

    for (Vector = BaseVector; (Vector < BaseVector + Count) && (Vector <= 0xFF); Vector++)
    {
        if (HalpVectorToIndex[Vector] == APIC_MSI_VECTOR)
        {
            HalpVectorToIndex[Vector] = APIC_FREE_VECTOR;
        }
    }

    KeReleaseSpinLock(&HalpMessageVectorLock, OldIrql);
}

/**
 * @brief
 * Private dispatch routine handing a device its message vectors. A vector is
 * good on every processor, so the processor set is only tested for members.
 */
static
NTSTATUS
NTAPI
HalpAllocateMessageTarget(
    _In_ PDEVICE_OBJECT Owner,
    _In_ KAFFINITY ProcessorSet,
    _In_ ULONG NumberOfIdtEntries,
    _In_ KINTERRUPT_MODE Mode,
    _In_ BOOLEAN ShareVector,
    _Out_ PULONG Vector,
    _Out_ PKIRQL Irql,
    _Out_ PULONG IdtEntry)
{
    UCHAR Base;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Owner);
    UNREFERENCED_PARAMETER(Mode);
    UNREFERENCED_PARAMETER(ShareVector);

    if ((ProcessorSet == 0) || (NumberOfIdtEntries == 0))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!HalpMessageInterruptsEnabled)
    {
        return STATUS_NOT_SUPPORTED;
    }

    Status = HalpAllocateMessageVectors(NumberOfIdtEntries, &Base);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    *Vector = Base;
    *IdtEntry = Base;
    *Irql = HalpVectorToIrql(Base);
    return STATUS_SUCCESS;
}

/* Private dispatch routine giving one message vector back */
static
VOID
NTAPI
HalpFreeMessageTarget(
    _In_ PDEVICE_OBJECT Owner,
    _In_ ULONG Vector,
    _In_ KAFFINITY ProcessorSet)
{
    UNREFERENCED_PARAMETER(Owner);
    UNREFERENCED_PARAMETER(ProcessorSet);

    if (Vector <= 0xFF)
    {
        HalpFreeMessageVectors((UCHAR)Vector, 1);
    }
}

VOID
NTAPI
HalpInitializeMessageInterrupts(VOID)
{
    KeInitializeSpinLock(&HalpMessageVectorLock);

    /* The boot options win over what the processor is known to do */
    if (HalpMessageInterruptPolicy & HALP_MESSAGE_INTERRUPTS_FORCE_OFF)
    {
        HalpMessageInterruptsEnabled = FALSE;
    }
    else if (HalpMessageInterruptPolicy & HALP_MESSAGE_INTERRUPTS_FORCE_ON)
    {
        HalpMessageInterruptsEnabled = TRUE;
    }
    else
    {
        HalpMessageInterruptsEnabled = HalpCpuSupportsMessageInterrupts();
    }

    /* Leave the routines the ACPI driver installed alone */
    if (HalAllocateMessageTargetOverride == NULL)
    {
        HalAllocateMessageTargetOverride = HalpAllocateMessageTarget;
        HalFreeMessageTargetOverride = HalpFreeMessageTarget;
    }
}

/* PUBLIC FUNCTIONS ***********************************************************/

NTSTATUS
NTAPI
HalGetProcessorIdByNtNumber(
    _In_ ULONG ProcessorNumber,
    _Out_ PULONG ProcessorId)
{
    if (ProcessorNumber >= (ULONG)KeNumberProcessors)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return HalpGetLocalApicIdForProcessor(ProcessorNumber, ProcessorId);
}

/**
 * @brief
 * Reports how interrupts can be aimed, either for the machine as a whole or
 * for the processor owning a local APIC ID.
 *
 * @param[in] Type
 * InterruptTargetTypeGlobal or InterruptTargetTypeApic.
 *
 * @param[in] Id
 * Local APIC ID of the processor, read for InterruptTargetTypeApic only.
 *
 * @param[out] Information
 * Receives the targeting information.
 */
NTSTATUS
NTAPI
HalGetInterruptTargetInformation(
    _In_ HAL_INTERRUPT_TARGET_TYPE Type,
    _In_ ULONG Id,
    _Out_ PHAL_INTERRUPT_TARGET_INFORMATION Information)
{
    HAL_APIC_DESTINATION_MODE Mode;
    ULONG Processor;
    NTSTATUS Status;

    if ((Type != InterruptTargetTypeGlobal) && (Type != InterruptTargetTypeApic))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Mode = HalpGetApicDestinationMode();

    RtlZeroMemory(Information, sizeof(*Information));
    Information->Type = Type;
    Information->Apic.DestinationMode = Mode;

    /* Processors never leave, so a destination stays good once it is built */
    Information->Flags = HAL_INTERRUPT_TARGET_STATIC_DESTINATIONS;
    if (HalpMessageInterruptsEnabled)
    {
        Information->Flags |= HAL_INTERRUPT_TARGET_MSI_SUPPORTED;
    }

    if (Type == InterruptTargetTypeApic)
    {
        Status = HalpGetProcessorForLocalApicId(Id, &Processor);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        Information->ProcessorNumber.Number = (UCHAR)Processor;

        if ((Mode == ApicDestinationModeLogicalFlat) && (Processor < 8))
        {
            Information->Flags |= HAL_INTERRUPT_TARGET_LOGICAL_ID_VALID;
            Information->Apic.LogicalApicId = ApicLogicalId(Processor);
        }
        else
        {
            Information->Apic.DestinationMode = ApicDestinationModePhysical;
        }
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Builds the address and data a device writes to raise a vector on a processor
 * set. InterruptTargetTypeApicRequest only checks the request over and hands it
 * back for the caller to finish later.
 *
 * @param[in] Request
 * The vector, the processor set and the destination mode wanted.
 *
 * @param[out] ConnectionData
 * Receives a connection data block holding the single vector.
 */
NTSTATUS
NTAPI
HalGetMessageRoutingInfo(
    _In_ PHAL_MESSAGE_TARGET_REQUEST Request,
    _Out_ PINTERRUPT_CONNECTION_DATA ConnectionData)
{
    PINTERRUPT_VECTOR_DATA VectorData;
    HAL_APIC_DESTINATION_MODE Mode;
    KAFFINITY Targets;
    BOOLEAN Logical, MultiTarget;
    UCHAR Destination;
    ULONG Address, Data;
    NTSTATUS Status;

    if ((Request->Type != InterruptTargetTypeApic) &&
        (Request->Type != InterruptTargetTypeApicRequest))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((Request->Apic.Vector > 0xFF) ||
        (Request->Apic.TargetProcessors.Group != 0) ||
        (Request->Apic.TargetProcessors.Mask == 0))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Targets = Request->Apic.TargetProcessors.Mask;
    MultiTarget = ((Targets & (Targets - 1)) != 0);
    Mode = Request->Apic.DestinationMode;

    if (Mode == ApicDestinationModeLogicalFlat)
    {
        /* Flat mode names even a lone processor by its logical ID */
        if ((HalpGetApicDestinationMode() != ApicDestinationModeLogicalFlat) ||
            (Targets > 0xFF))
        {
            return STATUS_INVALID_PARAMETER;
        }

        Logical = TRUE;
        Destination = (UCHAR)Targets;
    }
    else if (Mode == ApicDestinationModePhysical)
    {
        if (MultiTarget)
        {
            return STATUS_INVALID_PARAMETER;
        }

        Status = HalpBuildInterruptDestination(Targets, &Logical, &Destination);
        if (!NT_SUCCESS(Status))
        {
            return Status;
        }
    }
    else
    {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(ConnectionData, FIELD_OFFSET(INTERRUPT_CONNECTION_DATA, Vectors[1]));
    ConnectionData->Count = 1;

    VectorData = &ConnectionData->Vectors[0];
    VectorData->Vector = Request->Apic.Vector;
    VectorData->Irql = HalpVectorToIrql((UCHAR)Request->Apic.Vector);
    VectorData->Polarity = InterruptActiveHigh;
    VectorData->Mode = Latched;
    VectorData->TargetProcessors = Request->Apic.TargetProcessors;

    if (Request->Type == InterruptTargetTypeApicRequest)
    {
        VectorData->Type = InterruptTypeMessageRequest;
        VectorData->MessageRequest.DestinationMode = Mode;
        return STATUS_SUCCESS;
    }

    Address = APIC_MSI_ADDRESS_BASE | ((ULONG)Destination << 12);
    Data = Request->Apic.Vector | APIC_MSI_DATA_ASSERT;
    if (Logical)
    {
        Address |= APIC_MSI_ADDRESS_LOGICAL;
        Data |= APIC_MSI_DATA_LOGICAL;

        /* Let the hardware pick the least busy processor of the set */
        if (MultiTarget)
        {
            Address |= APIC_MSI_ADDRESS_REDIRHINT;
            Data |= APIC_MSI_DATA_LOWEST_PRIORITY;
        }
    }

    VectorData->Type = InterruptTypeXapicMessage;
    VectorData->XapicMessage.Address.QuadPart = Address;
    VectorData->XapicMessage.DataPayload = Data;
    VectorData->IntRemapInfo.FlagTranslated = 1;
    VectorData->IntRemapInfo.u.Msi.MessageAddressLow = Address;
    VectorData->IntRemapInfo.u.Msi.MessageData = (USHORT)Data;

    return STATUS_SUCCESS;
}

/* EOF */
