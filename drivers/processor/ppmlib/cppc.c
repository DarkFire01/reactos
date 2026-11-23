/*
 * PROJECT:     ReactOS Processor Power Management Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Collaborative processor performance control through _CPC
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ppmlib.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* What a _CPC register may be besides a real register */
#define PPMLIB_CPC_STATIC_OK            0x01
#define PPMLIB_CPC_NULL_OK              0x02

typedef struct _PPMLIB_CPC_RULE
{
    ULONG Offset;
    UCHAR Revision1;
    UCHAR Revision2;
} PPMLIB_CPC_RULE;

#define PPMLIB_CPC_RULE_ENTRY(Field, Rev1, Rev2) { FIELD_OFFSET(PPMLIB_CPC, Field), (Rev1), (Rev2) }

/* The first 15 apply to revision 1, all 19 to revision 2 and later */
static const PPMLIB_CPC_RULE PpmLibCpcRules[] =
{
    PPMLIB_CPC_RULE_ENTRY(Highest, PPMLIB_CPC_STATIC_OK, PPMLIB_CPC_STATIC_OK),
    PPMLIB_CPC_RULE_ENTRY(Nominal, PPMLIB_CPC_STATIC_OK, PPMLIB_CPC_STATIC_OK),
    PPMLIB_CPC_RULE_ENTRY(LowestNonlinear, PPMLIB_CPC_STATIC_OK, PPMLIB_CPC_STATIC_OK),
    PPMLIB_CPC_RULE_ENTRY(Lowest, PPMLIB_CPC_STATIC_OK, PPMLIB_CPC_STATIC_OK),
    PPMLIB_CPC_RULE_ENTRY(Guaranteed, PPMLIB_CPC_NULL_OK, PPMLIB_CPC_NULL_OK),
    PPMLIB_CPC_RULE_ENTRY(Desired, 0, PPMLIB_CPC_NULL_OK),
    PPMLIB_CPC_RULE_ENTRY(Minimum, PPMLIB_CPC_NULL_OK, PPMLIB_CPC_NULL_OK),
    PPMLIB_CPC_RULE_ENTRY(Maximum, PPMLIB_CPC_NULL_OK, PPMLIB_CPC_NULL_OK),
    PPMLIB_CPC_RULE_ENTRY(Tolerance, PPMLIB_CPC_NULL_OK, PPMLIB_CPC_NULL_OK),
    PPMLIB_CPC_RULE_ENTRY(TimeWindow, PPMLIB_CPC_NULL_OK, PPMLIB_CPC_NULL_OK),
    PPMLIB_CPC_RULE_ENTRY(Wraparound, PPMLIB_CPC_STATIC_OK | PPMLIB_CPC_NULL_OK, PPMLIB_CPC_STATIC_OK | PPMLIB_CPC_NULL_OK),
    PPMLIB_CPC_RULE_ENTRY(ReferenceCounter, 0, 0),
    PPMLIB_CPC_RULE_ENTRY(DeliveredCounter, 0, 0),
    PPMLIB_CPC_RULE_ENTRY(Limited, 0, 0),
    PPMLIB_CPC_RULE_ENTRY(Enable, PPMLIB_CPC_NULL_OK, PPMLIB_CPC_NULL_OK),
    PPMLIB_CPC_RULE_ENTRY(AutonomousEnable, 0, PPMLIB_CPC_STATIC_OK | PPMLIB_CPC_NULL_OK),
    PPMLIB_CPC_RULE_ENTRY(ActivityWindow, 0, PPMLIB_CPC_NULL_OK),
    PPMLIB_CPC_RULE_ENTRY(EnergyPreference, 0, PPMLIB_CPC_NULL_OK),
    PPMLIB_CPC_RULE_ENTRY(ReferencePerformance, 0, PPMLIB_CPC_STATIC_OK | PPMLIB_CPC_NULL_OK),
};

#define PPMLIB_CPC_REV1_REGISTERS       15

/* PRIVATE FUNCTIONS **********************************************************/

FORCEINLINE
PPPMLIB_GAS
PpmLibCpcRegister(
    _In_ PPPMLIB_CPC Cpc,
    _In_ ULONG Index)
{
    return (PPPMLIB_GAS)((PUCHAR)Cpc + PpmLibCpcRules[Index].Offset);
}

FORCEINLINE
ULONG
PpmLibCpcRegisterCount(
    _In_ PPPMLIB_CPC Cpc)
{
    return (Cpc->Revision == 1) ? PPMLIB_CPC_REV1_REGISTERS : RTL_NUMBER_OF(PpmLibCpcRules);
}

static
NTSTATUS
NTAPI
PpmLibValidateCpcRegister(
    _In_ PPPMLIB_GAS Register,
    _In_ UCHAR Rule)
{
    if (!PpmLibRegisterPresent(Register))
        return (Rule & PPMLIB_CPC_NULL_OK) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;

    switch (Register->SpaceId)
    {
        case PPMLIB_SPACE_IO:
            if (Register->Address == 0 || Register->Address > 0xFFFF)
                return STATUS_INVALID_PARAMETER;
            if (Register->BitWidth == 0 || (ULONG)Register->BitWidth + Register->BitOffset > Register->AccessBits)
                return STATUS_INVALID_PARAMETER;
            return STATUS_SUCCESS;

        case PPMLIB_SPACE_PCC:
            return STATUS_SUCCESS;

        case PPMLIB_SPACE_STATIC:
            return (Rule & PPMLIB_CPC_STATIC_OK) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;

        case PPMLIB_SPACE_FFH:
            if (Register->AccessBits != 64 || Register->Address == 0)
                return STATUS_INVALID_PARAMETER;
            return STATUS_SUCCESS;

        default:
            return STATUS_NOT_SUPPORTED;
    }
}

static
NTSTATUS
NTAPI
PpmLibValidateCpc(
    _In_ PPPMLIB_CPC Cpc)
{
    ULONG Count = PpmLibCpcRegisterCount(Cpc);
    NTSTATUS Status;
    ULONG Index;

    if ((Cpc->Revision == 1 && Cpc->Entries != 17) ||
        (Cpc->Revision == 2 && Cpc->Entries != 21) ||
        (Cpc->Revision > 2 && Cpc->Entries < 21))
    {
        return STATUS_UNSUCCESSFUL;
    }

    for (Index = 0; Index < Count; Index++)
    {
        Status = PpmLibValidateCpcRegister(PpmLibCpcRegister(Cpc, Index),
                                           (Cpc->Revision == 1) ? PpmLibCpcRules[Index].Revision1 :
                                                                  PpmLibCpcRules[Index].Revision2);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* Something must take the requested performance */
    if (PpmLibRegisterPresent(&Cpc->Desired) ||
        Cpc->Desired.SpaceId == PPMLIB_SPACE_STATIC ||
        Cpc->Desired.Address == 1)
    {
        return STATUS_SUCCESS;
    }

    return STATUS_UNSUCCESSFUL;
}

/* Maps a percent of nominal back onto the abstract performance scale */
static
ULONG64
NTAPI
PpmLibCpcPerformance(
    _In_ PPPMLIB_CPC_RUNTIME Runtime,
    _In_ ULONG Percent)
{
    if (Percent == Runtime->HighestPercent)
        return Runtime->Highest;
    if (Percent == 100)
        return Runtime->Nominal;
    if (Runtime->GuaranteedPresent && Percent == Runtime->GuaranteedPercent)
        return Runtime->Guaranteed;
    if (Percent == Runtime->LowestNonlinearPercent)
        return Runtime->LowestNonlinear;
    if (Percent == Runtime->LowestPercent)
        return Runtime->Lowest;

    return Runtime->Nominal * Percent / 100;
}

static
ULONG
NTAPI
PpmLibSelectCpc(
    _In_ PVOID Context,
    _In_ ULONG TargetPercent,
    _In_ ULONG MinPercent,
    _In_ ULONG MaxPercent,
    _In_ ULONG Flags,
    _Out_ PULONG Frequency,
    _Out_ PULONG64 State)
{
    PPPMLIB_CPC_RUNTIME Runtime = Context;
    ULONG Percent = TargetPercent;
    ULONG64 Performance;

    UNREFERENCED_PARAMETER(Flags);

    if (Percent < MinPercent)
        Percent = MinPercent;
    if (Percent > MaxPercent)
        Percent = MaxPercent;

    Performance = PpmLibCpcPerformance(Runtime, Percent);
    *State = Performance;

    /* The reported frequency stops at the guaranteed level */
    *Frequency = (ULONG)(Runtime->NominalFrequency * min(Performance, Runtime->Guaranteed) / Runtime->Nominal);
    return Percent;
}

/* Desired, Minimum, Maximum and Tolerance each live in their own register */
static
VOID
NTAPI
PpmLibControlCpc(
    _In_ PVOID Context,
    _In_ ULONG64 State,
    _In_ ULONG MinPercent,
    _In_ ULONG MaxPercent,
    _In_ ULONG TolerancePercent,
    _In_ BOOLEAN Autonomous,
    _In_ BOOLEAN Initiate,
    _In_ BOOLEAN Force)
{
    PPPMLIB_CPC_RUNTIME Runtime = Context;
    PPPMLIB_CPC Cpc = Runtime->Cpc;

    if (!Initiate && !Force)
        return;

    Runtime->Selected = State;

    if (Runtime->DesiredPresent)
        PpmLibWriteRegister(&Cpc->Desired, Autonomous ? 0 : State);
    if (Runtime->MinimumPresent)
        PpmLibWriteRegister(&Cpc->Minimum, PpmLibCpcPerformance(Runtime, MinPercent));
    if (Runtime->MaximumPresent)
        PpmLibWriteRegister(&Cpc->Maximum, PpmLibCpcPerformance(Runtime, MaxPercent));
    if (Runtime->TolerancePresent)
        PpmLibWriteRegister(&Cpc->Tolerance, PpmLibCpcPerformance(Runtime, TolerancePercent));
}

/* All control fields share one register, such as a request MSR */
static
VOID
NTAPI
PpmLibControlCpcShared(
    _In_ PVOID Context,
    _In_ ULONG64 State,
    _In_ ULONG MinPercent,
    _In_ ULONG MaxPercent,
    _In_ ULONG TolerancePercent,
    _In_ BOOLEAN Autonomous,
    _In_ BOOLEAN Initiate,
    _In_ BOOLEAN Force)
{
    PPPMLIB_CPC_RUNTIME Runtime = Context;
    PPPMLIB_CPC Cpc = Runtime->Cpc;
    ULONG64 Value;

    if (!Initiate && !Force)
        return;

    Runtime->Selected = State;
    Value = PpmLibReadRaw(&Runtime->SharedRegister);

    if (Runtime->DesiredPresent)
        Value = PpmLibMergeField(&Cpc->Desired, Value, Autonomous ? 0 : State);
    if (Runtime->MinimumPresent)
        Value = PpmLibMergeField(&Cpc->Minimum, Value, PpmLibCpcPerformance(Runtime, MinPercent));
    if (Runtime->MaximumPresent)
        Value = PpmLibMergeField(&Cpc->Maximum, Value, PpmLibCpcPerformance(Runtime, MaxPercent));
    if (Runtime->TolerancePresent)
        Value = PpmLibMergeField(&Cpc->Tolerance, Value, PpmLibCpcPerformance(Runtime, TolerancePercent));

    PpmLibWriteRaw(&Runtime->SharedRegister, Value);
}

static
VOID
NTAPI
PpmLibControlCpcNothing(
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

/* Policy is 0 to 100, EPP is 0 to 255 */
static
VOID
NTAPI
PpmLibCpcSetEnergyPreference(
    _In_ PVOID Context,
    _In_ ULONG Value)
{
    PPPMLIB_CPC_RUNTIME Runtime = Context;

    PpmLibWriteRegister(&Runtime->Cpc->EnergyPreference, 255 * Value / 100);
}

/* The window is a 7 bit mantissa with a 3 bit power of ten exponent */
static
VOID
NTAPI
PpmLibCpcSetActivityWindow(
    _In_ PVOID Context,
    _In_ ULONG Value)
{
    PPPMLIB_CPC_RUNTIME Runtime = Context;
    ULONG Exponent = 0;

    while (Value > 0x7F)
    {
        Value /= 10;
        Exponent++;
    }

    PpmLibWriteRegister(&Runtime->Cpc->ActivityWindow, (Value & 0x7F) | ((Exponent & 7) << 7));
}

static
VOID
NTAPI
PpmLibCpcSetAutonomous(
    _In_ PVOID Context,
    _In_ ULONG Value)
{
    PPPMLIB_CPC_RUNTIME Runtime = Context;

    PpmLibWriteRegister(&Runtime->Cpc->AutonomousEnable, Value != 0);
}

static
VOID
NTAPI
PpmLibCpcSetTimeWindow(
    _In_ PVOID Context,
    _In_ ULONG Value)
{
    PPPMLIB_CPC_RUNTIME Runtime = Context;

    PpmLibWriteRegister(&Runtime->Cpc->TimeWindow, Value);
}

/* Restores what a power transition may have cleared */
static
VOID
NTAPI
PpmLibCpcReinitialize(
    _In_ PVOID Context)
{
    PPPMLIB_CPC_RUNTIME Runtime = Context;

    if (Runtime->EnablePresent)
        PpmLibWriteRegister(&Runtime->Cpc->Enable, 1);

    if (Runtime->NativeInterruptEnabled && PpmLib.Vendor.EnableNativeInterrupt)
        PpmLib.Vendor.EnableNativeInterrupt();
}

/* Reference and delivered performance counters */
static
VOID
NTAPI
PpmLibCpcReadCounters(
    _In_ PVOID Context,
    _In_ BOOLEAN Reset,
    _Out_ PULONG64 Reference,
    _Out_ PULONG64 Actual)
{
    PPPMLIB_CPC Cpc = Context;

    UNREFERENCED_PARAMETER(Reset);

    *Reference = PpmLibReadRegister(&Cpc->ReferenceCounter);
    *Actual = PpmLibReadRegister(&Cpc->DeliveredCounter);
}

static
ULONG64
NTAPI
PpmLibFieldMask(
    _In_ UCHAR BitWidth)
{
    return (BitWidth < 64) ? ((1ULL << BitWidth) - 1) : 0;
}

/* Clamped guaranteed performance and its percent of nominal */
static
VOID
NTAPI
PpmLibCpcReadGuaranteed(
    _Inout_ PPPMLIB_CPC_RUNTIME Runtime)
{
    ULONG64 Guaranteed;
    ULONG Percent;

    Guaranteed = PpmLibReadRegister(&Runtime->Cpc->Guaranteed);
    Guaranteed = min(Guaranteed, Runtime->Nominal);
    Guaranteed = max(Guaranteed, Runtime->Lowest);

    Percent = (ULONG)(100 * Guaranteed / Runtime->Nominal);
    if (Percent <= Runtime->LowestPercent)
        Percent = Runtime->LowestPercent;

    Runtime->Guaranteed = Guaranteed;
    Runtime->GuaranteedPercent = Percent;
}

static
VOID
NTAPI
PpmLibNativeWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    IoFreeWorkItem((PIO_WORKITEM)Context);
    PpmLibCpcGuaranteedChange(DeviceObject->DeviceExtension);
}

static
VOID
NTAPI
PpmLibNativeDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PPPMLIB_DEVICE Device = DeferredContext;
    PIO_WORKITEM WorkItem;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    WorkItem = IoAllocateWorkItem(Device->Self);
    if (WorkItem)
        IoQueueWorkItem(WorkItem, PpmLibNativeWorker, CriticalWorkQueue, WorkItem);
}

/* Hooks the vendor's performance change interrupt on the member's processor */
static
VOID
NTAPI
PpmLibCpcConnectNative(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_CPC_RUNTIME Runtime = Device->CpcRuntime;
    PROCESSOR_NUMBER Number;
    GROUP_AFFINITY Affinity;
    GROUP_AFFINITY Previous;

    KeInitializeDpc(&Runtime->NativeDpc, PpmLibNativeDpc, Device);
    KeGetCurrentProcessorNumberEx(&Number);
    KeSetTargetProcessorDpcEx(&Runtime->NativeDpc, &Number);
    KeSetImportanceDpc(&Runtime->NativeDpc, MediumHighImportance);

    if (PpmLib.Vendor.ConnectNativeInterrupt == NULL)
        return;

    KeGetProcessorNumberFromIndex(Device->NtNumber, &Number);
    RtlZeroMemory(&Affinity, sizeof(Affinity));
    Affinity.Group = Number.Group;
    Affinity.Mask = AFFINITY_MASK(Number.Number);
    KeSetSystemGroupAffinityThread(&Affinity, &Previous);

    if (NT_SUCCESS(PpmLib.Vendor.ConnectNativeInterrupt(Device)))
    {
        Runtime->NativeInterruptEnabled = TRUE;
        if (PpmLib.Vendor.EnableNativeInterrupt)
            PpmLib.Vendor.EnableNativeInterrupt();
    }

    KeRevertToUserGroupAffinityThread(&Previous);
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief Reads and checks _CPC, enables CPPC and samples the capability levels.
 *
 * @remarks PCC registers are not supported, such a _CPC fails so the caller
 * falls back to _PSS.
 */
NTSTATUS
NTAPI
PpmLibInitCpc(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_CPC Cpc;
    NTSTATUS Status;
    ULONG Index;

    PpmLibAcquireAcpi(Device);

    Status = PpmLibEvaluateCpc(Device, &Device->Cpc);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Cpc = Device->Cpc;
    Status = PpmLibValidateCpc(Cpc);
    if (!NT_SUCCESS(Status))
    {
        Device->Invalidated |= PPMLIB_ERROR_CPC;
        goto Exit;
    }

    PpmLibAcquireGlobal();

    for (Index = 0; Index < PpmLibCpcRegisterCount(Cpc); Index++)
    {
        if (PpmLibCpcRegister(Cpc, Index)->SpaceId == PPMLIB_SPACE_PCC)
        {
            DPRINT1("PPM: _CPC uses PCC, which is not supported\n");
            Status = STATUS_NOT_SUPPORTED;
            break;
        }
    }

    if (NT_SUCCESS(Status))
    {
        /* Without a reference level the counters run at nominal */
        if (!PpmLibRegisterPresent(&Cpc->ReferencePerformance) ||
            (Cpc->ReferencePerformance.SpaceId == PPMLIB_SPACE_STATIC && Cpc->ReferencePerformance.Address == 0))
        {
            Cpc->ReferencePerformance = Cpc->Nominal;
        }

        if (PpmLibRegisterPresent(&Cpc->Enable))
            PpmLibWriteRegister(&Cpc->Enable, 1);

        Device->Highest = PpmLibReadRegister(&Cpc->Highest);
        Device->Nominal = PpmLibReadRegister(&Cpc->Nominal);
        Device->LowestNonlinear = PpmLibReadRegister(&Cpc->LowestNonlinear);
        Device->Lowest = PpmLibReadRegister(&Cpc->Lowest);
    }

    PpmLibReleaseGlobal();

Exit:
    PpmLibReleaseAcpi(Device);
    return Status;
}

/**
 * @brief Registers the _CPC controls of the device's domain with the kernel.
 */
NTSTATUS
NTAPI
PpmLibRegisterCpc(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_DEVICE Members[PPMLIB_MAX_PROCESSORS];
    PPPM_PERF_DOMAIN_REGISTRATION Registration = NULL;
    PPPMLIB_CPC_RUNTIME Runtime = NULL;
    PPPMLIB_CPC_RUNTIME Entry;
    PPPMLIB_GAS Controls[4];
    PPMLIB_GAS Shared;
    PPPM_FEEDBACK_COUNTER Counter;
    PPPMLIB_CPC Cpc;
    BOOLEAN SecondCounter;
    BOOLEAN SingleRegister = TRUE;
    BOOLEAN EfficientThrottle = TRUE;
    BOOLEAN AnyControl = FALSE;
    BOOLEAN Autonomous = FALSE;
    BOOLEAN AutonomousWritable = FALSE;
    BOOLEAN TimeWindowPresent = FALSE;
    BOOLEAN EnergyPresent = FALSE;
    BOOLEAN WindowPresent = FALSE;
    UCHAR Coordination;
    ULONG NominalFrequency;
    ULONG CounterCount;
    ULONG Percent;
    ULONG Count;
    ULONG Index;
    ULONG Slot;
    NTSTATUS Status;

    NominalFrequency = PpmLibQueryNominalFrequency();
    if (NominalFrequency == 0)
    {
        Device->Enabled &= ~PPMLIB_CPPC;
        return STATUS_UNSUCCESSFUL;
    }

    SecondCounter = (Device->PStateFeedback != NULL);

    PpmLibAcquireGlobal();

    Count = PpmLibGetDomainMembers(Device, Members, &Coordination);
    Runtime = ExAllocatePoolWithTag(NonPagedPoolNx, Count * sizeof(*Runtime), PPMLIB_TAG);
    if (Runtime == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    RtlZeroMemory(Runtime, Count * sizeof(*Runtime));

    /* Which controls exist and whether they all sit in one register */
    for (Index = 0; Index < Count; Index++)
    {
        Entry = &Runtime[Index];
        Cpc = Members[Index]->Cpc;

        Entry->Cpc = Cpc;
        Entry->Device = Members[Index];
        Entry->EnablePresent = PpmLibRegisterPresent(&Cpc->Enable);
        Entry->DesiredPresent = PpmLibRegisterPresent(&Cpc->Desired);
        Entry->MinimumPresent = PpmLibRegisterPresent(&Cpc->Minimum);
        Entry->MaximumPresent = PpmLibRegisterPresent(&Cpc->Maximum);
        Entry->TolerancePresent = PpmLibRegisterPresent(&Cpc->Tolerance);
        Entry->GuaranteedPresent = PpmLibRegisterPresent(&Cpc->Guaranteed);
        TimeWindowPresent = PpmLibRegisterPresent(&Cpc->TimeWindow);
        EnergyPresent = PpmLibRegisterPresent(&Cpc->EnergyPreference);
        WindowPresent = PpmLibRegisterPresent(&Cpc->ActivityWindow);

        /* FFH counters are per processor MSRs, the hardware pair would duplicate them */
        if (Cpc->ReferenceCounter.SpaceId == PPMLIB_SPACE_FFH || Cpc->DeliveredCounter.SpaceId == PPMLIB_SPACE_FFH)
            SecondCounter = FALSE;

        if (PpmLib.Vendor.IsThrottleEfficient == NULL || !PpmLib.Vendor.IsThrottleEfficient(Cpc))
            EfficientThrottle = FALSE;

        Controls[0] = &Cpc->Desired;
        Controls[1] = &Cpc->Minimum;
        Controls[2] = &Cpc->Maximum;
        Controls[3] = &Cpc->Tolerance;
        RtlZeroMemory(&Shared, sizeof(Shared));
        for (Slot = 0; Slot < RTL_NUMBER_OF(Controls); Slot++)
        {
            if (!PpmLibRegisterPresent(Controls[Slot]))
                continue;

            AnyControl = TRUE;
            if (!PpmLibRegisterPresent(&Shared))
            {
                Shared.SpaceId = Controls[Slot]->SpaceId;
                Shared.AccessBits = Controls[Slot]->AccessBits;
                Shared.Address = Controls[Slot]->Address;
            }
            else if (Shared.SpaceId != Controls[Slot]->SpaceId ||
                     Shared.Address != Controls[Slot]->Address ||
                     Shared.AccessBits != Controls[Slot]->AccessBits)
            {
                SingleRegister = FALSE;
                break;
            }
        }

        if (AnyControl && SingleRegister)
            Entry->SharedRegister = Shared;
    }

    /* Capability levels must agree across the domain */
    for (Index = 0; Index < Count; Index++)
    {
        Entry = &Runtime[Index];
        Cpc = Entry->Cpc;

        Entry->NominalFrequency = NominalFrequency;
        Entry->Highest = PpmLibReadRegister(&Cpc->Highest);
        Entry->Nominal = PpmLibReadRegister(&Cpc->Nominal);
        Entry->LowestNonlinear = PpmLibReadRegister(&Cpc->LowestNonlinear);
        Entry->Lowest = PpmLibReadRegister(&Cpc->Lowest);
        Entry->Reference = PpmLibReadRegister(&Cpc->ReferencePerformance);

        if (Entry->Nominal == 0)
        {
            Status = STATUS_INVALID_PARAMETER;
            goto Exit;
        }

        Entry->HighestPercent = (ULONG)(100 * Entry->Highest / Entry->Nominal);
        Entry->HighestObservedPercent = Entry->HighestPercent;
        Entry->LowestNonlinearPercent = max(1, (ULONG)(100 * Entry->LowestNonlinear / Entry->Nominal));
        Entry->LowestPercent = max(1, (ULONG)(100 * Entry->Lowest / Entry->Nominal));

        if (PpmLibRegisterPresent(&Cpc->AutonomousEnable))
        {
            Autonomous = (PpmLibReadRegister(&Cpc->AutonomousEnable) != 0);
            AutonomousWritable = (Cpc->AutonomousEnable.SpaceId != PPMLIB_SPACE_STATIC);
        }

        if (Index != 0 &&
            (Entry->Highest != Runtime[0].Highest ||
             Entry->Nominal != Runtime[0].Nominal ||
             Entry->LowestNonlinear != Runtime[0].LowestNonlinear ||
             Entry->Lowest != Runtime[0].Lowest))
        {
            Status = STATUS_INVALID_PARAMETER;
            goto Exit;
        }
    }

    if (Runtime[0].Highest < Runtime[0].Nominal ||
        Runtime[0].Nominal < Runtime[0].LowestNonlinear ||
        Runtime[0].LowestNonlinear < Runtime[0].Lowest)
    {
        Status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    CounterCount = SecondCounter ? 2 : 1;
    Registration = PpmLibAllocateRegistration(Count, CounterCount, Coordination);
    if (Registration == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    Registration->PerfSelectionHandler = PpmLibSelectCpc;
    if (!AnyControl)
        Registration->PerfControlHandler = PpmLibControlCpcNothing;
    else if (SingleRegister)
        Registration->PerfControlHandler = PpmLibControlCpcShared;
    else
        Registration->PerfControlHandler = PpmLibControlCpc;

    if (Runtime[Count - 1].EnablePresent || (Device->Enabled & PPMLIB_NATIVE_INTERRUPT))
        Registration->ReinitializeHandler = PpmLibCpcReinitialize;

    Registration->AffinitizeControl = AnyControl;
    Registration->EfficientThrottle = EfficientThrottle;
    Registration->HardPlatformCap = FALSE;
    Registration->AutonomousMode = Autonomous;
    Registration->NominalFrequency = NominalFrequency;
    Registration->MaxPerfPercent = Runtime[0].HighestPercent;
    Registration->MinPerfPercent = Runtime[0].LowestNonlinearPercent;
    Registration->MinThrottlePercent = Runtime[0].LowestPercent;
    Registration->MinimumRelativePerformance = (ULONG)Runtime[0].Lowest;
    Registration->NominalRelativePerformance = (ULONG)Runtime[0].Nominal;
    Registration->GetFFHThrottleState = Device->GetFfhThrottleState;

    /* Boost mode stays with the P-state path, CPPC only takes the bias policy */
    Registration->BoostPolicyHandler = Device->SetBoostPolicy;
    if (EnergyPresent)
        Registration->EnergyPerfPreferenceHandler = PpmLibCpcSetEnergyPreference;
    if (WindowPresent)
        Registration->AutonomousActivityWindowHandler = PpmLibCpcSetActivityWindow;
    if (AutonomousWritable)
        Registration->AutonomousModeHandler = PpmLibCpcSetAutonomous;
    if (TimeWindowPresent)
        Registration->TimeWindowHandler = PpmLibCpcSetTimeWindow;

    Counter = &Registration->FeedbackCounters[0];
    Counter->DifferentialRead = PpmLibReadWrappingCounter;
    Counter->NominalRate = (Runtime[0].Reference == Runtime[0].Nominal) ?
                           100 : (ULONG)(100 * Runtime[0].Reference / Runtime[0].Nominal);
    Counter->Type = PPM_FEEDBACK_PERFORMANCE;
    Counter->Affinitized = TRUE;
    Counter->Differential = TRUE;

    if (SecondCounter)
    {
        Counter = &Registration->FeedbackCounters[1];
        Counter->DifferentialRead = PpmLibReadWrappingCounter;
        Counter->NominalRate = 100;
        Counter->Type = PPM_FEEDBACK_FREQUENCY;
        Counter->Affinitized = TRUE;
        Counter->Differential = TRUE;
    }

    for (Index = 0; Index < Count; Index++)
    {
        Entry = &Runtime[Index];
        Cpc = Entry->Cpc;
        Percent = 100;

        if (Entry->GuaranteedPresent)
        {
            PpmLibCpcReadGuaranteed(Entry);
            Percent = Entry->GuaranteedPercent;

            /* The limit is recorded on the registering device, as Windows does */
            if (Percent != Members[Index]->PlatformCap)
                PpmLibCapChange(Device, Members[Index]->Ppc, Members[Index]->Tpc, Percent);
        }

        KeGetProcessorNumberFromIndex(Members[Index]->NtNumber, &Registration->Processors[Index].Number);
        Registration->Processors[Index].PerfContext = Entry;
        Registration->Processors[Index].PlatformCap = Percent;
        Registration->Processors[Index].ThermalCap = Members[Index]->ThermalCap;
        Registration->Processors[Index].LimitReasons = (Percent < 100) ? PPM_LIMIT_REASON_PLATFORM : 0;

        Entry->Delivered.Read = PpmLibCpcReadCounters;
        Entry->Delivered.ReadContext = Cpc;
        Entry->Delivered.ActualMask = PpmLibFieldMask(Cpc->DeliveredCounter.BitWidth);
        Entry->Delivered.ReferenceMask = PpmLibFieldMask(Cpc->ReferenceCounter.BitWidth);
        Registration->CounterContexts[Index * CounterCount] = &Entry->Delivered;

        if (SecondCounter)
        {
            Entry->Hardware.Read = Members[Index]->PStateFeedback;
            Registration->CounterContexts[Index * CounterCount + 1] = &Entry->Hardware;
        }
    }

    Status = PpmLib.Kernel.RegisterPerfStates(Registration);
    if (NT_SUCCESS(Status))
    {
        for (Index = 0; Index < Count; Index++)
            Members[Index]->CpcRuntime = &Runtime[Index];

        if (Device->Enabled & PPMLIB_NATIVE_INTERRUPT)
        {
            for (Index = 0; Index < Count; Index++)
                PpmLibCpcConnectNative(Members[Index]);
        }

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
 * @brief Guaranteed performance changed, notify 0x83 or the vendor interrupt.
 */
VOID
NTAPI
PpmLibCpcGuaranteedChange(
    _In_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_CPC_RUNTIME Runtime = Device->CpcRuntime;

    if (!(Device->Enabled & PPMLIB_CPPC) || Runtime == NULL || !Runtime->GuaranteedPresent)
        return;

    PpmLibAcquireGlobal();

    PpmLibCpcReadGuaranteed(Runtime);
    if (Runtime->GuaranteedPercent != Device->PlatformCap)
    {
        PpmLibCapChange(Device, Device->Ppc, Device->Tpc, Runtime->GuaranteedPercent);
        PpmLibRegisterCap(Device);
    }

    PpmLibReleaseGlobal();
}

/**
 * @brief Highest performance changed, notify 0x85. Raises the domain's ceiling.
 */
VOID
NTAPI
PpmLibCpcHighestChange(
    _In_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_DEVICE Members[PPMLIB_MAX_PROCESSORS];
    PPPMLIB_CPC_RUNTIME Runtime = Device->CpcRuntime;
    PPM_PERF_DOMAIN_UPDATE Update;
    UCHAR Coordination;
    ULONG64 Highest;
    ULONG Count;
    ULONG Index;

    if (!(Device->Enabled & PPMLIB_CPPC) || Runtime == NULL)
        return;

    if (Runtime->Cpc->Highest.SpaceId == PPMLIB_SPACE_STATIC)
        return;

    PpmLibAcquireGlobal();

    Highest = PpmLibReadRegister(&Runtime->Cpc->Highest);
    Runtime->Highest = max(Highest, Runtime->Nominal);
    Runtime->HighestPercent = (ULONG)(100 * Runtime->Highest / Runtime->Nominal);
    if (Runtime->HighestPercent > Runtime->HighestObservedPercent)
        Runtime->HighestObservedPercent = Runtime->HighestPercent;

    RtlZeroMemory(&Update, sizeof(Update));
    Update.Version = PPM_DRIVER_INTERFACE_VERSION;
    Update.MaxPerfPercent = Runtime->HighestPercent;

    Count = PpmLibGetDomainMembers(Device, Members, &Coordination);
    for (Index = 0; Index < Count; Index++)
    {
        if (Members[Index]->NtNumber < PPMLIB_MAX_PROCESSORS)
            Update.TargetProcessors |= AFFINITY_MASK(Members[Index]->NtNumber);

        if (Members[Index] != Device && Members[Index]->CpcRuntime)
        {
            Members[Index]->CpcRuntime->Highest = Runtime->Highest;
            Members[Index]->CpcRuntime->HighestPercent = Runtime->HighestPercent;
            Members[Index]->CpcRuntime->HighestObservedPercent = Runtime->HighestObservedPercent;
        }
    }

    PpmLib.Kernel.UpdatePerfStates(&Update);
    PpmLibReleaseGlobal();
}

/* EOF */
