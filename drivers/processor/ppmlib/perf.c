/*
 * PROJECT:     ReactOS Processor Power Management Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ACPI P-states and throttling, perf domains and registration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ppmlib.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Duty cycle enable bit of P_CNT */
#define PPMLIB_THT_EN                   0x10

/* Features that read _PSD */
#define PPMLIB_PSD_USERS                (PPMLIB_CPPC | PPMLIB_PSTATE_MASK | PPMLIB_PCC)

#define PPMLIB_LOGICAL_PROCESSOR_CLASS  107

NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformationEx(
    _In_ ULONG SystemInformationClass,
    _In_reads_bytes_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength);

/* VALIDATION *****************************************************************/

/* Control must be I/O or FFH, Status absent or in the same space */
static
NTSTATUS
NTAPI
PpmLibValidateControlPair(
    _In_ PPPMLIB_CONTROL_PAIR Pair)
{
    if (Pair->Status.Address != 0 && Pair->Status.SpaceId != Pair->Control.SpaceId)
        return STATUS_NOT_SUPPORTED;

    if (Pair->Control.SpaceId != PPMLIB_SPACE_IO && Pair->Control.SpaceId != PPMLIB_SPACE_FFH)
        return STATUS_NOT_SUPPORTED;

    return STATUS_SUCCESS;
}

/* Frequencies strictly falling and nonzero, power never rising */
static
NTSTATUS
NTAPI
PpmLibValidatePssOrder(
    _In_ PPPMLIB_PSS Pss)
{
    ULONG Index;

    if (Pss->State[0].Frequency == 0)
        return STATUS_INVALID_PARAMETER;

    for (Index = 1; Index < Pss->Count; Index++)
    {
        if (Pss->State[Index].Frequency == 0 ||
            Pss->State[Index].Frequency >= Pss->State[Index - 1].Frequency ||
            Pss->State[Index].Power > Pss->State[Index - 1].Power)
        {
            return STATUS_UNSUCCESSFUL;
        }
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
PpmLibValidatePStates(
    _In_ PPPMLIB_CONTROL_PAIR Pct,
    _In_ PPPMLIB_PSS Pss,
    _Inout_ PULONG Errors)
{
    NTSTATUS Status;

    Status = PpmLibValidatePssOrder(Pss);
    if (!NT_SUCCESS(Status))
    {
        *Errors |= PPMLIB_ERROR_PSS;
        return Status;
    }

    Status = PpmLibValidateControlPair(Pct);
    if (!NT_SUCCESS(Status))
    {
        *Errors |= PPMLIB_ERROR_PCT;
        return Status;
    }

    if (PpmLib.Vendor.ValidatePStates)
    {
        Status = PpmLib.Vendor.ValidatePStates(Pct, Pss, Errors);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return STATUS_SUCCESS;
}

/* Percentages strictly falling from 100, power never rising */
static
NTSTATUS
NTAPI
PpmLibValidateTStates(
    _In_ PPPMLIB_CONTROL_PAIR Ptc,
    _In_opt_ PPPMLIB_TSS Tss,
    _Out_ PULONG Errors)
{
    NTSTATUS Status;
    ULONG Index;

    *Errors = 0;
    if (Tss == NULL)
        return STATUS_UNSUCCESSFUL;

    for (Index = 1; Index < Tss->Count; Index++)
    {
        if (Tss->State[Index].Percent >= Tss->State[Index - 1].Percent ||
            Tss->State[Index].Power > Tss->State[Index - 1].Power)
        {
            *Errors = PPMLIB_ERROR_TSS;
            return STATUS_UNSUCCESSFUL;
        }
    }

    if (Tss->State[0].Percent != 100)
    {
        *Errors = PPMLIB_ERROR_TSS;
        return STATUS_UNSUCCESSFUL;
    }

    Status = PpmLibValidateControlPair(Ptc);
    if (!NT_SUCCESS(Status))
        *Errors = PPMLIB_ERROR_PTC;

    return Status;
}

/* Valid only as a single package naming 1 to 32 processors */
static
NTSTATUS
NTAPI
PpmLibValidateDependency(
    _In_ PPPMLIB_DEPENDENCY_LIST List)
{
    PPPMLIB_DEPENDENCY Entry = &List->Entry[0];

    if (List->Count != 1 || Entry->Entries != 5)
        return STATUS_UNSUCCESSFUL;

    if (Entry->Coordination != PPM_COORDINATION_HW_ALL &&
        Entry->Coordination != PPM_COORDINATION_SW_ALL &&
        Entry->Coordination != PPM_COORDINATION_SW_ANY &&
        Entry->Coordination != PPM_COORDINATION_PACKAGE)
    {
        return STATUS_UNSUCCESSFUL;
    }

    if (Entry->Processors == 0 || Entry->Processors > 32)
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
PpmLibInitDependency(
    _Inout_ PPPMLIB_DEVICE Device,
    _In_ BOOLEAN Throttle)
{
    PPPMLIB_DEPENDENCY_LIST List;
    NTSTATUS Status;

    Status = PpmLibEvaluateDependency(Device, Throttle, &List);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = PpmLibValidateDependency(List);
    if (!NT_SUCCESS(Status))
    {
        Device->Invalidated |= Throttle ? PPMLIB_ERROR_TSD : PPMLIB_ERROR_PSD;
        ExFreePoolWithTag(List, PPMLIB_TAG);
        return Status;
    }

    if (Throttle)
        Device->Tsd = List;
    else
        Device->Psd = List;

    return STATUS_SUCCESS;
}

/* ACPI 1.0 duty cycle throttling through P_BLK */
static
NTSTATUS
NTAPI
PpmLibInitFadtThrottle(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_FADT_INFO Fadt = &PpmLib.Fadt;
    PPPMLIB_TSS Tss;
    ULONG Steps;
    ULONG Index;

    if (PpmLib.ProcessorCount > 1 && !(Device->Enabled & PPMLIB_ACPI1_THROTTLE_MP))
        return STATUS_UNSUCCESSFUL;

    if (!(Device->Enabled & PPMLIB_ACPI1_THROTTLE))
        return STATUS_UNSUCCESSFUL;

    if (!PpmLibIsValidRegister(&Device->PBlk) ||
        Fadt->DutyWidth == 0 ||
        Fadt->DutyWidth > 4 ||
        (ULONG)Fadt->DutyWidth + Fadt->DutyOffset > 32)
    {
        return STATUS_UNSUCCESSFUL;
    }

    Steps = 1UL << Fadt->DutyWidth;
    Tss = ExAllocatePoolWithTag(NonPagedPoolNx, FIELD_OFFSET(PPMLIB_TSS, State[Steps]), PPMLIB_TAG);
    if (Tss == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Tss, FIELD_OFFSET(PPMLIB_TSS, State[Steps]));
    Tss->Count = Steps;

    /* Step i keeps the clock running for (Steps - i) of every Steps periods */
    for (Index = 1; Index < Steps; Index++)
    {
        Tss->State[Index].Percent = 100 * (Steps - Index) / Steps;
        Tss->State[Index].Control = ((Steps - Index) << Fadt->DutyOffset) | PPMLIB_THT_EN;
    }

    Tss->State[0].Percent = 100;
    Tss->State[0].Control = Tss->State[1].Control & ~PPMLIB_THT_EN;

    if (Device->Tss)
        ExFreePoolWithTag(Device->Tss, PPMLIB_TAG);
    Device->Tss = Tss;

    Device->Ptc.Control = Device->PBlk;
    RtlZeroMemory(&Device->Ptc.Status, sizeof(Device->Ptc.Status));
    Device->Tpc = 0;
    return STATUS_SUCCESS;
}

/**
 * @brief Reads and checks _PCT and _PSS. Failing features are cleared.
 */
NTSTATUS
NTAPI
PpmLibInitPerfStates(
    _Inout_ PPPMLIB_DEVICE Device)
{
    NTSTATUS ControlStatus;
    NTSTATUS Status;
    ULONG Errors;

    PpmLibAcquireAcpi(Device);

    ControlStatus = PpmLibEvaluateControlPair(Device, FALSE, &Device->Pct);
    Status = ControlStatus;

    if (Device->Enabled & PPMLIB_PSS_MASK)
    {
        Status = PpmLibEvaluatePss(Device, &Device->Pss);
        if (!NT_SUCCESS(Status))
            Device->Enabled &= ~PPMLIB_PSS_MASK;
    }

    if (NT_SUCCESS(ControlStatus))
    {
        if (NT_SUCCESS(Status) && (Device->Enabled & PPMLIB_PSS_MASK))
        {
            Errors = 0;
            Status = PpmLibValidatePStates(&Device->Pct, Device->Pss, &Errors);
            if (!NT_SUCCESS(Status))
            {
                Device->Invalidated |= Errors;
                Device->Enabled &= ~PPMLIB_PSS_MASK;
            }
        }
    }
    else
    {
        Status = ControlStatus;
    }

    PpmLibReleaseAcpi(Device);
    return Status;
}

/**
 * @brief Sets up _PTC/_TSS throttling, or ACPI 1.0 duty cycle throttling.
 */
NTSTATUS
NTAPI
PpmLibInitThrottleStates(
    _Inout_ PPPMLIB_DEVICE Device)
{
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    NTSTATUS TssStatus;
    ULONG Errors;

    PpmLibAcquireAcpi(Device);

    if (Device->Enabled & PPMLIB_TSS_MASK)
    {
        Status = PpmLibEvaluateControlPair(Device, TRUE, &Device->Ptc);
        TssStatus = PpmLibEvaluateTss(Device, &Device->Tss);
        if (!NT_SUCCESS(TssStatus))
            Status = TssStatus;

        if (NT_SUCCESS(Status))
            goto Validate;

        Device->Enabled &= ~PPMLIB_TSS_MASK;
    }

    if (!(Device->Enabled & PPMLIB_ACPI1_THROTTLE_MASK))
        goto Exit;

    Status = PpmLibInitFadtThrottle(Device);
    if (!NT_SUCCESS(Status))
    {
        Device->Enabled &= ~PPMLIB_ACPI1_THROTTLE_MASK;
        goto Exit;
    }

Validate:
    Status = PpmLibValidateTStates(&Device->Ptc, Device->Tss, &Errors);
    if (!NT_SUCCESS(Status))
        Device->Invalidated |= Errors;

Exit:
    PpmLibReleaseAcpi(Device);
    return Status;
}

/**
 * @brief Reads _CSD, _PSD and _TSD and picks the dependency that governs
 * performance coordination.
 */
VOID
NTAPI
PpmLibInitDomains(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PpmLibAcquireAcpi(Device);

    if ((Device->Enabled & PPMLIB_CST_IDLE) &&
        Device->Cst != NULL &&
        !NT_SUCCESS(PpmLibInitIdleDomain(Device, Device->Cst, &Device->Csd)))
    {
        Device->Csd = NULL;
    }

    if ((Device->Enabled & PPMLIB_PSD_USERS) &&
        NT_SUCCESS(PpmLibInitDependency(Device, FALSE)) &&
        !(Device->Enabled & PPMLIB_PEP_PERF))
    {
        Device->Xsd = Device->Psd;
    }

    if ((Device->Enabled & PPMLIB_TSS_MASK) &&
        NT_SUCCESS(PpmLibInitDependency(Device, TRUE)) &&
        !(Device->Enabled & (PPMLIB_PEP_PERF | PPMLIB_PSD_USERS)))
    {
        Device->Xsd = Device->Tsd;
    }

    PpmLibReleaseAcpi(Device);
}

/* DOMAINS ********************************************************************/

/**
 * @brief Lists the processors that share a perf domain with Device. Global lock held.
 *
 * @return The member count, the device itself when it stands alone.
 */
ULONG
NTAPI
PpmLibGetDomainMembers(
    _In_ PPPMLIB_DEVICE Device,
    _Out_writes_(PPMLIB_MAX_PROCESSORS) PPPMLIB_DEVICE *Members,
    _Out_ PUCHAR Coordination)
{
    PLIST_ENTRY Head;
    PLIST_ENTRY Entry;
    ULONG Count = 0;

    if (Device->PerfDomain)
    {
        *Coordination = (UCHAR)Device->Xsd->Entry[0].Coordination;
        Head = &Device->PerfDomain->Members;
        for (Entry = Head->Flink; Entry != Head && Count < PPMLIB_MAX_PROCESSORS; Entry = Entry->Flink)
            Members[Count++] = CONTAINING_RECORD(Entry, PPMLIB_DEVICE, DomainLink);
        return Count;
    }

    if (Device->Package && Device->Package->Total > 1)
    {
        *Coordination = PPM_COORDINATION_PACKAGE;
        Head = &Device->Package->Members;
        for (Entry = Head->Flink; Entry != Head && Count < PPMLIB_MAX_PROCESSORS; Entry = Entry->Flink)
            Members[Count++] = CONTAINING_RECORD(Entry, PPMLIB_DEVICE, PackageLink);
        return Count;
    }

    *Coordination = PPM_COORDINATION_NONE;
    Members[0] = Device;
    return 1;
}

/* Joins the _PSD/_TSD domain, TRUE once the last member has arrived */
static
BOOLEAN
NTAPI
PpmLibJoinPerfDomain(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_DEPENDENCY Dependency = &Device->Xsd->Entry[0];
    PPPMLIB_PERF_DOMAIN Domain = NULL;
    PLIST_ENTRY Entry;
    BOOLEAN Complete;

    PpmLibAcquireGlobal();

    for (Entry = PpmLib.PerfDomainList.Flink; Entry != &PpmLib.PerfDomainList; Entry = Entry->Flink)
    {
        Domain = CONTAINING_RECORD(Entry, PPMLIB_PERF_DOMAIN, Link);
        if (Domain->Domain == Dependency->Domain)
            break;
        Domain = NULL;
    }

    if (Domain == NULL)
    {
        Domain = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*Domain), PPMLIB_TAG);
        if (Domain == NULL)
        {
            PpmLibReleaseGlobal();
            return FALSE;
        }

        RtlZeroMemory(Domain, sizeof(*Domain));
        InitializeListHead(&Domain->Members);
        Domain->Domain = Dependency->Domain;
        Domain->Coordination = Dependency->Coordination;
        Domain->Total = Dependency->Processors;
        InsertTailList(&PpmLib.PerfDomainList, &Domain->Link);
    }

    Device->PerfDomain = Domain;
    InsertTailList(&Domain->Members, &Device->DomainLink);
    Complete = (++Domain->Count == Domain->Total);

    PpmLibReleaseGlobal();
    return Complete;
}

/* Builds the package that holds the device's processor from the topology */
static
PPPMLIB_PACKAGE
NTAPI
PpmLibCreatePackage(
    _In_ PPPMLIB_DEVICE Device)
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Buffer;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Record;
    LOGICAL_PROCESSOR_RELATIONSHIP Relation = RelationProcessorPackage;
    PPPMLIB_PACKAGE Package = NULL;
    KAFFINITY Mask = 0;
    ULONG Offset;
    ULONG Size = 0;
    ULONG Index;
    NTSTATUS Status;

    PAGED_CODE();

    Status = ZwQuerySystemInformationEx(PPMLIB_LOGICAL_PROCESSOR_CLASS,
                                        &Relation,
                                        sizeof(Relation),
                                        NULL,
                                        0,
                                        &Size);
    if (Status != STATUS_INFO_LENGTH_MISMATCH || Size == 0)
        return NULL;

    Buffer = ExAllocatePoolWithTag(PagedPool, Size, PPMLIB_TAG);
    if (Buffer == NULL)
        return NULL;

    Status = ZwQuerySystemInformationEx(PPMLIB_LOGICAL_PROCESSOR_CLASS,
                                        &Relation,
                                        sizeof(Relation),
                                        Buffer,
                                        Size,
                                        &Size);
    if (NT_SUCCESS(Status) && Size != 0)
    {
        /* A processor found in no package leaves the mask empty */
        for (Offset = 0; Offset < Size; Offset += Record->Size)
        {
            Record = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)((PUCHAR)Buffer + Offset);
            if (Record->Size == 0)
                break;

            Mask = 0;
            for (Index = 0; Index < Record->Processor.GroupCount; Index++)
            {
                if (Record->Processor.GroupMask[Index].Group == 0)
                    Mask |= Record->Processor.GroupMask[Index].Mask;
            }

            if (Device->NtNumber < PPMLIB_MAX_PROCESSORS && (Mask & AFFINITY_MASK(Device->NtNumber)))
                break;

            Mask = 0;
        }

        Package = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*Package), PPMLIB_TAG);
        if (Package)
        {
            RtlZeroMemory(Package, sizeof(*Package));
            InitializeListHead(&Package->Members);
            Package->Mask = Mask;
            for (Index = 0; Index < PPMLIB_MAX_PROCESSORS; Index++)
            {
                if (Mask & AFFINITY_MASK(Index))
                    Package->Total++;
            }
        }
    }

    ExFreePoolWithTag(Buffer, PPMLIB_TAG);
    return Package;
}

/* Joins the synthetic per package domain, success once the package is complete */
static
NTSTATUS
NTAPI
PpmLibJoinPackage(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_PACKAGE Package = NULL;
    PLIST_ENTRY Entry;
    NTSTATUS Status;

    PpmLibAcquireGlobal();

    for (Entry = PpmLib.PackageList.Flink; Entry != &PpmLib.PackageList; Entry = Entry->Flink)
    {
        Package = CONTAINING_RECORD(Entry, PPMLIB_PACKAGE, Link);
        if (Device->NtNumber < PPMLIB_MAX_PROCESSORS && (Package->Mask & AFFINITY_MASK(Device->NtNumber)))
            break;
        Package = NULL;
    }

    if (Package == NULL)
    {
        Package = PpmLibCreatePackage(Device);
        if (Package == NULL)
        {
            PpmLibReleaseGlobal();
            return STATUS_PENDING;
        }

        InsertTailList(&PpmLib.PackageList, &Package->Link);
    }

    Device->Package = Package;
    Package->Count++;
    InsertTailList(&Package->Members, &Device->PackageLink);
    Status = (Package->Count == Package->Total) ? STATUS_SUCCESS : STATUS_PENDING;

    PpmLibReleaseGlobal();
    return Status;
}

static
BOOLEAN
NTAPI
PpmLibSameRegister(
    _In_ PPPMLIB_GAS Left,
    _In_ PPPMLIB_GAS Right)
{
    if (Left->SpaceId != Right->SpaceId ||
        Left->BitWidth != Right->BitWidth ||
        Left->BitOffset != Right->BitOffset ||
        Left->AccessBits != Right->AccessBits)
    {
        return FALSE;
    }

    /* PCC registers only compare the header */
    return (Left->SpaceId == PPMLIB_SPACE_PCC || Left->Address == Right->Address);
}

static
BOOLEAN
NTAPI
PpmLibSamePair(
    _In_ PPPMLIB_CONTROL_PAIR Left,
    _In_ PPPMLIB_CONTROL_PAIR Right)
{
    return PpmLibSameRegister(&Left->Control, &Right->Control) &&
           PpmLibSameRegister(&Left->Status, &Right->Status);
}

static
BOOLEAN
NTAPI
PpmLibSamePss(
    _In_opt_ PPPMLIB_PSS Left,
    _In_opt_ PPPMLIB_PSS Right)
{
    ULONG Index;

    if (Left == NULL || Right == NULL || Left->Count != Right->Count)
        return FALSE;

    for (Index = 0; Index < Left->Count; Index++)
    {
        if (Left->State[Index].Frequency != Right->State[Index].Frequency)
            return FALSE;
    }

    return TRUE;
}

static
BOOLEAN
NTAPI
PpmLibSameTss(
    _In_opt_ PPPMLIB_TSS Left,
    _In_opt_ PPPMLIB_TSS Right)
{
    ULONG Index;

    if (Left == NULL || Right == NULL || Left->Count != Right->Count)
        return FALSE;

    for (Index = 0; Index < Left->Count; Index++)
    {
        if (Left->State[Index].Percent != Right->State[Index].Percent)
            return FALSE;
    }

    return TRUE;
}

static
BOOLEAN
NTAPI
PpmLibSameCpc(
    _In_ PPPMLIB_CPC Left,
    _In_ PPPMLIB_CPC Right)
{
    PPPMLIB_GAS LeftRegister = &Left->Highest;
    PPPMLIB_GAS RightRegister = &Right->Highest;
    ULONG Index;

    /* The revision 1 registers, Highest through Enable */
    for (Index = 0; Index < 15; Index++)
    {
        if (!PpmLibSameRegister(&LeftRegister[Index], &RightRegister[Index]))
            return FALSE;
    }

    return TRUE;
}

/**
 * @brief Every member of a domain must describe the same states. A feature
 * on which members disagree is dropped on all of them.
 */
static
NTSTATUS
NTAPI
PpmLibValidateSymmetry(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_DEVICE Members[PPMLIB_MAX_PROCESSORS];
    PPPMLIB_DEVICE Member;
    ULONG64 Mismatch = 0;
    UCHAR Coordination;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Count;
    ULONG Index;

    PpmLibAcquireGlobal();

    Count = PpmLibGetDomainMembers(Device, Members, &Coordination);
    if (Count != 1)
    {
        for (Index = 0; Index < Count; Index++)
        {
            Member = Members[Index];
            if (Member == Device)
                continue;

            Mismatch |= (Device->Enabled ^ Member->Enabled) & (PPMLIB_PERF_MASK | PPMLIB_PEP_PERF);

            if (Device->Enabled & PPMLIB_PSTATE_MASK)
            {
                if (!PpmLibSamePair(&Device->Pct, &Member->Pct))
                    Mismatch |= PPMLIB_PSTATE_MASK;

                if ((Device->Enabled & PPMLIB_PSS_MASK) && !PpmLibSamePss(Device->Pss, Member->Pss))
                    Mismatch |= PPMLIB_PSS_MASK;
            }

            if (Device->Enabled & PPMLIB_THROTTLE_MASK)
            {
                if (!PpmLibSamePair(&Device->Ptc, &Member->Ptc) || !PpmLibSameTss(Device->Tss, Member->Tss))
                    Mismatch |= PPMLIB_THROTTLE_MASK;
            }

            if ((Device->Enabled & PPMLIB_CPPC) &&
                (Member->Cpc == NULL || !PpmLibSameCpc(Device->Cpc, Member->Cpc)))
            {
                Mismatch |= PPMLIB_CPPC;
            }
        }

        for (Index = 0; Index < Count; Index++)
            Members[Index]->Enabled &= ~Mismatch;

        if (!(Device->Enabled & (PPMLIB_PERF_MASK | PPMLIB_PEP_PERF)))
            Status = STATUS_UNSUCCESSFUL;
    }

    PpmLibReleaseGlobal();
    return Status;
}

/* REGISTRATION ***************************************************************/

/**
 * @brief Allocates a kernel perf domain block with its counters and contexts.
 */
PPPM_PERF_DOMAIN_REGISTRATION
NTAPI
PpmLibAllocateRegistration(
    _In_ ULONG ProcessorCount,
    _In_ ULONG CounterCount,
    _In_ UCHAR Coordination)
{
    PPPM_PERF_DOMAIN_REGISTRATION Registration;
    SIZE_T CounterOffset;
    SIZE_T ContextOffset;
    SIZE_T Size;

    CounterOffset = ALIGN_UP_BY(FIELD_OFFSET(PPM_PERF_DOMAIN_REGISTRATION, Processors[ProcessorCount]), sizeof(PVOID));
    ContextOffset = ALIGN_UP_BY(CounterOffset + CounterCount * sizeof(PPM_FEEDBACK_COUNTER), sizeof(PVOID));
    Size = ContextOffset + (SIZE_T)ProcessorCount * CounterCount * sizeof(PVOID);

    Registration = ExAllocatePoolWithTag(NonPagedPoolNx, Size, PPMLIB_TAG);
    if (Registration == NULL)
        return NULL;

    RtlZeroMemory(Registration, Size);
    Registration->Version = PPM_DRIVER_INTERFACE_VERSION;
    Registration->Coordination = Coordination;
    Registration->ProcessorCount = ProcessorCount;
    Registration->FeedbackCounterCount = CounterCount;
    Registration->FeedbackCounters = (PPPM_FEEDBACK_COUNTER)((PUCHAR)Registration + CounterOffset);
    Registration->CounterContexts = (PVOID *)((PUCHAR)Registration + ContextOffset);
    return Registration;
}

/* Percent of the state that _PPC, or _TPC past the last P-state, allows */
static
ULONG
NTAPI
PpmLibPercentageCap(
    _In_ PPPMLIB_PERF_TABLE Table,
    _In_ ULONG Ppc,
    _In_ ULONG Tpc)
{
    ULONG Index = 0;

    if (Table->PStateCount != 0)
        Index = Ppc;

    if ((Table->PStateCount == 0 || Ppc == Table->PStateCount - 1) && Table->TStateCount != 0)
        Index = Table->PStateCount + Tpc;

    if (Index >= Table->StateCount)
        Index = Table->StateCount - 1;

    return Table->State[Index].Percent;
}

/**
 * @brief Picks the P or T state for a target percent.
 */
static
ULONG
NTAPI
PpmLibSelectPTState(
    _In_ PVOID Context,
    _In_ ULONG TargetPercent,
    _In_ ULONG MinPercent,
    _In_ ULONG MaxPercent,
    _In_ ULONG Flags,
    _Out_ PULONG Frequency,
    _Out_ PULONG64 State)
{
    PPPMLIB_PERF_TABLE Table = ((PPPMLIB_PT_RUNTIME)Context)->Table;
    ULONG Limit;
    ULONG Index;

    if (Flags & PPM_SELECT_ALLOW_THROTTLE)
        Limit = Table->StateCount;
    else
        Limit = max(Table->PStateCount, 1);

    if (Flags & PPM_SELECT_ROUND_DOWN)
    {
        /* Fastest state not above the target */
        for (Index = 0; Index < Limit; Index++)
        {
            if (Table->State[Index].Percent <= TargetPercent)
                break;
        }

        if (Index == Limit)
            Index = Limit - 1;
    }
    else
    {
        /* Slowest state not below the target */
        Index = Limit - 1;
        while (Index > 0 && Table->State[Index].Percent < TargetPercent)
            Index -= 1;
    }

    while (Index > 0 && Table->State[Index].Percent < MinPercent)
        Index -= 1;

    while (Index < Table->StateCount - 1 && Table->State[Index].Percent > MaxPercent)
        Index++;

    *State = Index;
    *Frequency = Table->State[Index].Frequency;
    return Table->State[Index].Percent;
}

static
VOID
NTAPI
PpmLibApplyPTState(
    _In_ PPPMLIB_PERF_TABLE Table,
    _In_ PPPMLIB_PERF_STATE State)
{
    if (State->Type == PPMLIB_STATE_PERFORMANCE)
        Table->PStateHandler(Table->PStateContext, State->Control, State->Status);
    else
        Table->TStateHandler(Table->TStateContext, State->Control, State->Status);
}

/**
 * @brief Programs a P or T state, going through the P/T boundary state when
 * the kind of state changes.
 */
static
VOID
NTAPI
PpmLibControlPTState(
    _In_ PVOID Context,
    _In_ ULONG64 State,
    _In_ ULONG MinPercent,
    _In_ ULONG MaxPercent,
    _In_ ULONG TolerancePercent,
    _In_ BOOLEAN Autonomous,
    _In_ BOOLEAN Initiate,
    _In_ BOOLEAN Force)
{
    PPPMLIB_PT_RUNTIME Runtime = Context;
    PPPMLIB_PERF_TABLE Table = Runtime->Table;
    PPPMLIB_PERF_STATE Target;
    PPPMLIB_PERF_STATE Bridge = NULL;

    UNREFERENCED_PARAMETER(MinPercent);
    UNREFERENCED_PARAMETER(MaxPercent);
    UNREFERENCED_PARAMETER(TolerancePercent);
    UNREFERENCED_PARAMETER(Autonomous);

    if (Force)
    {
        Runtime->CurrentType = 0;
        return;
    }

    Target = &Table->State[State];
    if (Runtime->CurrentType != Target->Type)
    {
        if (Target->Type == PPMLIB_STATE_PERFORMANCE && Table->TStateCount != 0)
            Bridge = &Table->State[Table->PStateCount];
        else if (Target->Type != PPMLIB_STATE_PERFORMANCE && Table->PStateCount != 0)
            Bridge = &Table->State[Table->PStateCount - 1];
    }

    if (Initiate)
    {
        if (Bridge)
            PpmLibApplyPTState(Table, Bridge);
        PpmLibApplyPTState(Table, Target);
    }

    Runtime->TargetState = (ULONG)State;
    Runtime->CurrentType = Target->Type;
    Runtime->CurrentPercent = Target->Percent;
}

/* Without hardware counters the kernel sees the last programmed percent */
static
VOID
NTAPI
PpmLibReadProgrammedPercent(
    _In_ PVOID Context,
    _Out_ PULONG Value)
{
    *Value = ((PPPMLIB_PT_RUNTIME)Context)->CurrentPercent;
}

/* Counters that are zeroed on every read, the totals are kept here */
static
VOID
NTAPI
PpmLibReadResettingCounter(
    _In_ PVOID Context,
    _In_ BOOLEAN Reset,
    _Out_ PULONG64 Reference,
    _Out_ PULONG64 Actual)
{
    PPPMLIB_COUNTER Counter = Context;
    ULONG64 ReferenceDelta;
    ULONG64 ActualDelta;

    Counter->Read(Counter->ReadContext, TRUE, &ReferenceDelta, &ActualDelta);
    if (!Reset)
    {
        Counter->TotalReference += ReferenceDelta;
        Counter->TotalActual += ActualDelta;
    }

    *Reference = Counter->TotalReference;
    *Actual = Counter->TotalActual;
}

/**
 * @brief Reads free running counters and folds their wraparound into the totals.
 */
VOID
NTAPI
PpmLibReadWrappingCounter(
    _In_ PVOID Context,
    _In_ BOOLEAN Reset,
    _Out_ PULONG64 Reference,
    _Out_ PULONG64 Actual)
{
    PPPMLIB_COUNTER Counter = Context;
    ULONG64 NewReference;
    ULONG64 NewActual;

    Counter->Read(Counter->ReadContext, FALSE, &NewReference, &NewActual);

    if (Counter->ActualMask)
        NewActual &= Counter->ActualMask;
    if (Counter->ReferenceMask)
        NewReference &= Counter->ReferenceMask;

    if (!Reset)
    {
        if (Counter->ActualMask && NewActual < Counter->LastActual)
            Counter->TotalActual += Counter->ActualMask + 1;
        Counter->TotalActual += NewActual - Counter->LastActual;

        if (Counter->ReferenceMask && NewReference < Counter->LastReference)
            Counter->TotalReference += Counter->ReferenceMask + 1;
        Counter->TotalReference += NewReference - Counter->LastReference;
    }

    Counter->LastActual = NewActual;
    Counter->LastReference = NewReference;
    *Reference = Counter->TotalReference;
    *Actual = Counter->TotalActual;
}

/**
 * @brief Writes a state through an I/O control register, then checks the status register.
 */
NTSTATUS
NTAPI
PpmLibSetStateIo(
    _In_ PVOID Context,
    _In_ ULONG64 Control,
    _In_ ULONG64 Status)
{
    PPPMLIB_CONTROL_PAIR Pair = Context;

    PpmLibWriteRegister(&Pair->Control, (ULONG)Control);

    if (Pair->Status.Address != 0 &&
        Status != 0 &&
        (ULONG)PpmLibReadRegister(&Pair->Status) != (ULONG)Status)
    {
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

/* Feedback only domains have nothing to program */
static
ULONG
NTAPI
PpmLibSelectFeedback(
    _In_ PVOID Context,
    _In_ ULONG TargetPercent,
    _In_ ULONG MinPercent,
    _In_ ULONG MaxPercent,
    _In_ ULONG Flags,
    _Out_ PULONG Frequency,
    _Out_ PULONG64 State)
{
    UNREFERENCED_PARAMETER(TargetPercent);
    UNREFERENCED_PARAMETER(MinPercent);
    UNREFERENCED_PARAMETER(MaxPercent);
    UNREFERENCED_PARAMETER(Flags);

    *State = 0;
    *Frequency = ((PPPMLIB_FEEDBACK_RUNTIME)Context)->NominalFrequency;
    return 100;
}

static
VOID
NTAPI
PpmLibControlNothing(
    _In_ PVOID Context,
    _In_ ULONG64 State,
    _In_ ULONG MinPercent,
    _In_ ULONG MaxPercent,
    _In_ ULONG TolerancePercent,
    _In_ BOOLEAN Autonomous,
    _In_ BOOLEAN Initiate,
    _In_ BOOLEAN Force)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(State);
    UNREFERENCED_PARAMETER(MinPercent);
    UNREFERENCED_PARAMETER(MaxPercent);
    UNREFERENCED_PARAMETER(TolerancePercent);
    UNREFERENCED_PARAMETER(Autonomous);
    UNREFERENCED_PARAMETER(Initiate);
    UNREFERENCED_PARAMETER(Force);
}

static
VOID
NTAPI
PpmLibFillProcessor(
    _Out_ PPPM_PROCESSOR_PERF_ENTRY Entry,
    _In_ PPPMLIB_DEVICE Member,
    _In_ PVOID Context,
    _In_ ULONG PlatformCap)
{
    KeGetProcessorNumberFromIndex(Member->NtNumber, &Entry->Number);
    Entry->PerfContext = Context;
    Entry->PlatformCap = PlatformCap;
    Entry->ThermalCap = Member->ThermalCap;
    Entry->LimitReasons = (PlatformCap < 100) ? PPM_LIMIT_REASON_PLATFORM : 0;
}

/**
 * @brief Registers the _PSS and throttle states of the device's domain.
 * Global lock is taken here.
 */
static
NTSTATUS
NTAPI
PpmLibRegisterPTStates(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_DEVICE Members[PPMLIB_MAX_PROCESSORS];
    PPPM_PERF_DOMAIN_REGISTRATION Registration = NULL;
    PPPMLIB_PT_RUNTIME Runtime = NULL;
    PPPMLIB_PERF_TABLE Table = NULL;
    PPPM_FEEDBACK_COUNTER Counter;
    PPPMLIB_PERF_STATE State;
    UCHAR Coordination;
    ULONG BaseFrequency;
    ULONG BasePower;
    ULONG PCount = 0;
    ULONG TCount = 0;
    ULONG Total;
    ULONG Count;
    ULONG Index;
    NTSTATUS Status;

    if (Device->Enabled & PPMLIB_PSTATE_MASK)
        PCount = Device->Pss->Count;
    if (Device->Enabled & PPMLIB_THROTTLE_MASK)
        TCount = Device->Tss->Count;

    Total = PCount + TCount;
    if (Total == 0)
        return STATUS_UNSUCCESSFUL;

    PpmLibAcquireGlobal();

    Count = PpmLibGetDomainMembers(Device, Members, &Coordination);
    Runtime = ExAllocatePoolWithTag(NonPagedPoolNx, Count * sizeof(*Runtime), PPMLIB_TAG);
    Table = ExAllocatePoolWithTag(NonPagedPoolNx, FIELD_OFFSET(PPMLIB_PERF_TABLE, State[Total]), PPMLIB_TAG);
    Registration = PpmLibAllocateRegistration(Count, 1, Coordination);
    if (Runtime == NULL || Table == NULL || Registration == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    RtlZeroMemory(Runtime, Count * sizeof(*Runtime));
    RtlZeroMemory(Table, FIELD_OFFSET(PPMLIB_PERF_TABLE, State[Total]));

    Registration->MaxPerfPercent = 100;
    Registration->HardPlatformCap = TRUE;
    Registration->AffinitizeControl = TRUE;
    Registration->NominalRelativePerformance = 100;
    Registration->PerfSelectionHandler = PpmLibSelectPTState;
    Registration->PerfControlHandler = PpmLibControlPTState;

    if (PCount != 0)
    {
        Registration->BoostPolicyHandler = Device->SetBoostPolicy;
        Registration->BoostModeHandler = Device->SetBoostMode;
        Registration->NominalFrequency = Device->Pss->State[0].Frequency;
        Registration->NominalRelativePerformance = Device->Pss->State[0].Frequency;
        Registration->MinimumRelativePerformance = Device->Pss->State[PCount - 1].Frequency;
        Table->PStateHandler = Device->SetPState;
        Table->PStateContext = &Device->Pct;

        for (Index = 0; Index < PCount; Index++)
        {
            State = &Table->State[Index];
            State->Control = Device->Pss->State[Index].Control;
            State->Status = Device->Pss->State[Index].Status;
            State->Frequency = Device->Pss->State[Index].Frequency;
            State->Power = Device->Pss->State[Index].Power;
            State->Percent = (UCHAR)(100 * State->Frequency / Registration->NominalFrequency);
            State->Type = PPMLIB_STATE_PERFORMANCE;
            State->Latency = (USHORT)Device->Pss->State[Index].Latency;
        }

        /* Throttling scales the slowest P-state */
        BaseFrequency = Table->State[PCount - 1].Frequency;
        BasePower = Device->Pss->State[PCount - 1].Power;
    }
    else
    {
        Registration->NominalFrequency = PpmLibQueryNominalFrequency();
        if (Registration->NominalFrequency == 0)
        {
            Device->Enabled &= ~PPMLIB_TSS_MASK;
            Status = STATUS_UNSUCCESSFUL;
            goto Exit;
        }

        BaseFrequency = Registration->NominalFrequency;
        BasePower = 1000 * Registration->NominalFrequency;
    }

    if (TCount != 0)
    {
        Table->TStateHandler = Device->SetTState;
        Table->TStateContext = &Device->Ptc;

        for (Index = 0; Index < TCount; Index++)
        {
            State = &Table->State[PCount + Index];
            State->Frequency = BaseFrequency * Device->Tss->State[Index].Percent / 100;
            State->Percent = (UCHAR)(100 * State->Frequency / Registration->NominalFrequency);
            State->Control = Device->Tss->State[Index].Control;
            State->Status = Device->Tss->State[Index].Status;
            State->Type = PPMLIB_STATE_THROTTLE;
            State->Latency = (USHORT)Device->Tss->State[Index].Latency;
            State->Power = Device->Tss->State[Index].Power;
            if (State->Power == 0)
                State->Power = BasePower * Device->Tss->State[Index].Percent / 100;
        }
    }

    /* States too slow to show as a whole percent end the table */
    Table->StateCount = Total;
    for (Index = 1; Index < Total; Index++)
    {
        if (Table->State[Index].Percent == 0)
        {
            Table->StateCount = Index;
            break;
        }
    }

    if (Table->StateCount > PCount)
    {
        Table->PStateCount = PCount;
        Table->TStateCount = Table->StateCount - PCount;
        Registration->MinPerfPercent = Table->State[PCount].Percent;
    }
    else
    {
        Table->PStateCount = Table->StateCount;
        Table->TStateCount = 0;
        Registration->MinPerfPercent = Table->State[Table->StateCount - 1].Percent;
    }

    Registration->MinThrottlePercent = Table->State[Table->StateCount - 1].Percent;
    Registration->GetFFHThrottleState = Device->GetFfhThrottleState;

    Counter = &Registration->FeedbackCounters[0];
    Counter->NominalRate = 100;
    Counter->Type = PPM_FEEDBACK_FREQUENCY;
    if (Device->PStateFeedback)
    {
        Counter->DifferentialRead = (Device->Enabled & PPMLIB_NONWRAPPING_COUNTERS) ?
                                    PpmLibReadWrappingCounter : PpmLibReadResettingCounter;
        Counter->Affinitized = TRUE;
        Counter->Differential = TRUE;
    }
    else
    {
        Counter->InstantaneousRead = PpmLibReadProgrammedPercent;
    }

    for (Index = 0; Index < Count; Index++)
    {
        Runtime[Index].Table = Table;
        Runtime[Index].CurrentPercent = 100;
        PpmLibFillProcessor(&Registration->Processors[Index],
                            Members[Index],
                            &Runtime[Index],
                            PpmLibPercentageCap(Table, Members[Index]->Ppc, Members[Index]->Tpc));

        if (Device->PStateFeedback)
        {
            Runtime[Index].Hardware.Read = Device->PStateFeedback;
            Registration->CounterContexts[Index] = &Runtime[Index].Hardware;
        }
        else
        {
            Registration->CounterContexts[Index] = &Runtime[Index];
        }
    }

    Status = PpmLib.Kernel.RegisterPerfStates(Registration);
    if (NT_SUCCESS(Status))
    {
        for (Index = 0; Index < Count; Index++)
            Members[Index]->PtRuntime = &Runtime[Index];

        Runtime = NULL;
        Table = NULL;
    }

Exit:
    PpmLibReleaseGlobal();

    if (Registration)
        ExFreePoolWithTag(Registration, PPMLIB_TAG);
    if (Table)
        ExFreePoolWithTag(Table, PPMLIB_TAG);
    if (Runtime)
        ExFreePoolWithTag(Runtime, PPMLIB_TAG);

    return Status;
}

/**
 * @brief Registers a domain that only reports hardware counters.
 */
static
NTSTATUS
NTAPI
PpmLibRegisterFeedback(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_DEVICE Members[PPMLIB_MAX_PROCESSORS];
    PPPM_PERF_DOMAIN_REGISTRATION Registration = NULL;
    PPPMLIB_FEEDBACK_RUNTIME Runtime = NULL;
    PPPM_FEEDBACK_COUNTER Counter;
    UCHAR Coordination;
    ULONG Count;
    ULONG Index;
    NTSTATUS Status;

    PpmLibAcquireGlobal();

    Count = PpmLibGetDomainMembers(Device, Members, &Coordination);
    Runtime = ExAllocatePoolWithTag(NonPagedPoolNx, Count * sizeof(*Runtime), PPMLIB_TAG);
    Registration = PpmLibAllocateRegistration(Count, 1, Coordination);
    if (Runtime == NULL || Registration == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    RtlZeroMemory(Runtime, Count * sizeof(*Runtime));
    Registration->HardPlatformCap = TRUE;
    Registration->AutonomousMode = TRUE;
    Registration->MaxPerfPercent = 100;
    Registration->MinPerfPercent = 100;
    Registration->MinThrottlePercent = 100;
    Registration->MinimumRelativePerformance = 100;
    Registration->NominalRelativePerformance = 100;
    Registration->PerfSelectionHandler = PpmLibSelectFeedback;
    Registration->PerfControlHandler = PpmLibControlNothing;

    Registration->NominalFrequency = PpmLibQueryNominalFrequency();
    if (Registration->NominalFrequency == 0)
    {
        Device->Enabled &= ~PPMLIB_FEEDBACK;
        Status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }

    Registration->GetFFHThrottleState = Device->GetFfhThrottleState;

    Counter = &Registration->FeedbackCounters[0];
    Counter->DifferentialRead = PpmLibReadWrappingCounter;
    Counter->NominalRate = 100;
    Counter->Type = PPM_FEEDBACK_FREQUENCY;
    Counter->Affinitized = TRUE;
    Counter->Differential = TRUE;

    for (Index = 0; Index < Count; Index++)
    {
        PpmLibFillProcessor(&Registration->Processors[Index], Members[Index], &Runtime[Index], 100);
        Registration->Processors[Index].LimitReasons = 0;
        Runtime[Index].Hardware.Read = Members[Index]->PStateFeedback;
        Runtime[Index].NominalFrequency = Registration->NominalFrequency;
        Registration->CounterContexts[Index] = &Runtime[Index].Hardware;
    }

    Status = PpmLib.Kernel.RegisterPerfStates(Registration);
    if (NT_SUCCESS(Status))
    {
        for (Index = 0; Index < Count; Index++)
            Members[Index]->FeedbackRuntime = &Runtime[Index];

        Runtime = NULL;
    }

Exit:
    PpmLibReleaseGlobal();

    if (Registration)
        ExFreePoolWithTag(Registration, PPMLIB_TAG);
    if (Runtime)
        ExFreePoolWithTag(Runtime, PPMLIB_TAG);

    return Status;
}

/**
 * @brief Hands the processor's current platform and thermal limits to the
 * kernel. Global lock held.
 */
NTSTATUS
NTAPI
PpmLibRegisterCap(
    _In_ PPPMLIB_DEVICE Device)
{
    PPM_PROCESSOR_CAP Cap;
    ULONG PlatformCap;

    if (Device->CpcRuntime)
        PlatformCap = Device->PlatformCap;
    else if (Device->PtRuntime)
        PlatformCap = PpmLibPercentageCap(Device->PtRuntime->Table, Device->Ppc, Device->Tpc);
    else
        return STATUS_SUCCESS;

    RtlZeroMemory(&Cap, sizeof(Cap));
    Cap.Version = PPM_DRIVER_INTERFACE_VERSION;
    KeGetProcessorNumberFromIndex(Device->NtNumber, &Cap.ProcessorNumber);
    Cap.PlatformCap = PlatformCap;
    Cap.ThermalCap = Device->ThermalCap;
    if (PlatformCap < 100)
        Cap.LimitReasons = PPM_LIMIT_REASON_PLATFORM;

    PpmLib.Kernel.RegisterPerfCap(&Cap);
    return STATUS_SUCCESS;
}

/**
 * @brief Registers performance control once the device's domain is complete.
 * The last member to start registers for all of them.
 */
VOID
NTAPI
PpmLibRegisterPerformance(
    _Inout_ PPPMLIB_DEVICE Device)
{
    BOOLEAN Proceed;

    if (!(Device->Enabled & (PPMLIB_PERF_MASK | PPMLIB_PEP_PERF)))
        return;

    if (Device->Xsd)
        Proceed = PpmLibJoinPerfDomain(Device);
    else if (!PpmLib.SyntheticPsdAllowed)
        Proceed = TRUE;
    else
        Proceed = (PpmLibJoinPackage(Device) == STATUS_SUCCESS);

    if (!Proceed || !NT_SUCCESS(PpmLibValidateSymmetry(Device)))
        return;

    if (Device->Enabled & PPMLIB_CPPC)
        PpmLibRegisterCpc(Device);
    else if (Device->Enabled & (PPMLIB_PSTATE_MASK | PPMLIB_THROTTLE_MASK))
        PpmLibRegisterPTStates(Device);
    else
        PpmLibRegisterFeedback(Device);
}

/* EOF */
