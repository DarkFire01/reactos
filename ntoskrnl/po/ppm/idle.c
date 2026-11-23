/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Processor Power Management idle processor handling support
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* The transition was neither canceled by the kernel nor failed by the driver */
#define PPM_IDLE_NOT_CANCELED          3

/* Serializes idle state registration and idle policy changes */
static EX_PUSH_LOCK PpmIdlePolicyLock;

/* One clock tick in QPC units, the clock interrupt ends every idle period */
static ULONG64 PpmIdleTickDuration;

typedef struct _PPM_IDLE_INSTALL
{
    PPPM_IDLE_STATE_SET States;
    PPPM_IDLE_ACCOUNTING Accounting;
    BOOLEAN OnlyIfEmpty;
} PPM_IDLE_INSTALL, *PPPM_IDLE_INSTALL;

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief Clears the idle statistics, the histogram buckets double from 1 ms.
 */
static
VOID
PpmResetIdleAccounting(
    _Inout_ PPPM_IDLE_ACCOUNTING Accounting)
{
    ULONG Count = Accounting->StateCount;
    ULONG Resets = Accounting->ResetCount;
    ULONG64 Limit = PpmQpcFrequency;
    ULONG Index;

    RtlZeroMemory(Accounting, FIELD_OFFSET(PPM_IDLE_ACCOUNTING, State[Count]));
    for (Index = 0; Index < PPM_IDLE_BUCKET_COUNT; Index++)
    {
        Accounting->BucketLimits[Index] = (Limit + 999) / 1000;
        Limit *= 2;
    }

    for (Index = 0; Index < Count; Index++)
        Accounting->State[Index].MinTime = (ULONG64)-1;

    Accounting->StateCount = Count;
    Accounting->ResetCount = Resets + 1;
    Accounting->StartTime = PpmQueryCounter();
}

static
VOID
PpmRecordIdleTime(
    _Inout_ PPPM_IDLE_ACCOUNTING Accounting,
    _In_ ULONG State,
    _In_ ULONG64 IdleTime)
{
    PPPM_IDLE_STATE_COUNTERS Counters;
    ULONG Bucket;

    if (State >= Accounting->StateCount)
        return;

    Counters = &Accounting->State[State];
    Counters->TotalTime += IdleTime;
    Counters->IdleTransitions++;
    Counters->MinTime = min(Counters->MinTime, IdleTime);
    Counters->MaxTime = max(Counters->MaxTime, IdleTime);

    for (Bucket = 0; Bucket < PPM_IDLE_BUCKET_COUNT - 1; Bucket++)
    {
        if (IdleTime < Accounting->BucketLimits[Bucket])
            break;
    }

    Counters->BucketHits[Bucket]++;
}

/**
 * @brief Loads the idle policy into one processor and restarts its statistics.
 */
static
VOID
PpmResetIdlePolicy(
    _Inout_ PPPM_PROCESSOR Processor)
{
    if (!Processor->IdleStates)
        return;

    Processor->IdleAllowScaling = PpmPolicy.IdleAllowScaling;
    Processor->IdlePromoteBase = PpmPolicy.IdlePromotePercent;
    Processor->IdleDemoteBase = PpmPolicy.IdleDemotePercent;
    Processor->IdlePromotePercent = Processor->IdlePromoteBase;
    Processor->IdleDemotePercent = Processor->IdleDemoteBase;
    Processor->IdleStates->PrepareInfo.Constraints.TimeCheck =
        (ULONG64)PpmPolicy.IdleTimeCheck * PpmQpcFrequency / 1000000;

    PpmScaleIdlePolicy(Processor);
    if (Processor->IdleAccounting)
        PpmResetIdleAccounting(Processor->IdleAccounting);
}

static
VOID
NTAPI
PpmResetIdlePolicyRoutine(
    _In_ PPPM_PROCESSOR Processor,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    PpmResetIdlePolicy(Processor);
}

/**
 * @brief Swaps in the new idle states of a processor, runs on that processor.
 *
 * @remarks The blocks that were replaced, or the new ones when nothing was
 * replaced, are handed back in the context for the caller to free.
 */
static
VOID
NTAPI
PpmPublishIdleStates(
    _In_ PPPM_PROCESSOR Processor,
    _In_opt_ PVOID Context)
{
    PPPM_IDLE_INSTALL Install = (PPPM_IDLE_INSTALL)Context;
    PPPM_IDLE_STATE_SET Old = Processor->IdleStates;
    PPPM_IDLE_ACCOUNTING OldAccounting = Processor->IdleAccounting;

    if (Install->OnlyIfEmpty && Old)
        return;

    /* A forced state and the current target survive a re-registration */
    if (Old)
    {
        Install->States->ForceIdle = Old->ForceIdle;
        Install->States->OverrideIndex = Old->OverrideIndex;
        if (Old->TargetState < Install->States->ProcessorIdleCount)
            Install->States->TargetState = Old->TargetState;
    }

    if (OldAccounting)
        Install->Accounting->ResetCount = OldAccounting->ResetCount + 1;

    Processor->IdleStates = Install->States;
    Processor->IdleAccounting = Install->Accounting;
    PpmResetIdlePolicy(Processor);

    Install->States = Old;
    Install->Accounting = OldAccounting;
}

static
VOID
NTAPI
PpmRemoveIdleStates(
    _In_ PPPM_PROCESSOR Processor,
    _In_opt_ PVOID Context)
{
    PPPM_IDLE_INSTALL Install = (PPPM_IDLE_INSTALL)Context;

    Install->States = Processor->IdleStates;
    Install->Accounting = Processor->IdleAccounting;
    Processor->IdleStates = NULL;
    Processor->IdleAccounting = NULL;
}

static
VOID
PpmFreeIdleBlocks(
    _In_ PPPM_IDLE_INSTALL Install)
{
    if (Install->States)
        ExFreePoolWithTag(Install->States, TAG_PPM_IDLE);
    if (Install->Accounting)
        ExFreePoolWithTag(Install->Accounting, TAG_PPM_IDLE);
}

/**
 * @brief Copies the idle states of a registration and installs them on a processor.
 *
 * @param[in] OnlyIfEmpty
 * Leave a processor alone that already has idle states.
 */
static
NTSTATUS
PpmInstallIdleStates(
    _In_ ULONG Number,
    _In_ PPPM_IDLE_STATES_REGISTRATION Registration,
    _In_ BOOLEAN OnlyIfEmpty)
{
    ULONG Count = Registration->ProcessorIdleCount;
    PPPM_IDLE_STATE_SET States;
    PPPM_IDLE_STATE_SLOT Slot;
    PPM_IDLE_INSTALL Install;
    SIZE_T Size, DependencyOffset;
    ULONG Index, Flags;

    if (OnlyIfEmpty && PpmProcessors[Number].IdleStates)
        return STATUS_SUCCESS;

    /* Counts no allocation could hold fail the same way a failed allocation does */
    if (Count > MAXUSHORT || Registration->MaximumDependencies > MAXUSHORT)
        return STATUS_INSUFFICIENT_RESOURCES;

    DependencyOffset = ALIGN_UP_BY(FIELD_OFFSET(PPM_IDLE_STATE_SET, State[Count]), sizeof(ULONG_PTR));
    Size = DependencyOffset + Registration->MaximumDependencies * sizeof(PPM_IDLE_DEPENDENCY);

    States = ExAllocatePoolZero(NonPagedPoolNx, Size, TAG_PPM_IDLE);
    if (!States)
        return STATUS_INSUFFICIENT_RESOURCES;

    Install.Accounting = ExAllocatePoolZero(NonPagedPoolNx,
                                            FIELD_OFFSET(PPM_IDLE_ACCOUNTING, State[Count]),
                                            TAG_PPM_IDLE);
    if (!Install.Accounting)
    {
        ExFreePoolWithTag(States, TAG_PPM_IDLE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    States->EstimateIdleDuration = Registration->EstimateIdleDuration;
    States->ProcessorIdleCount = Count;
    States->IdlePrepare = Registration->IdlePrepare;
    States->IdleExecute = Registration->IdleExecute;
    States->IdleComplete = Registration->IdleComplete;
    States->IdleCancel = Registration->IdleCancel;
    States->IdleIsHalted = Registration->IdleIsHalted;
    States->IdleInitiateWake = Registration->IdleInitiateWake;
    States->PrepareInfo.Context = Registration->Context;
    States->PrepareInfo.DependencyCount = Registration->MaximumDependencies;
    if (Registration->MaximumDependencies)
        States->PrepareInfo.DependencyArray = (PUCHAR)States + DependencyOffset;

    for (Index = 0; Index < Count; Index++)
    {
        Flags = Registration->State[Index].StateFlags;
        Slot = &States->State[Index];
        Slot->StateFlags = Flags;
        Slot->StateType = (UCHAR)((Flags & PPM_IDLE_STATE_CSTATE_MASK) >> PPM_IDLE_STATE_CSTATE_SHIFT);
        Slot->Interruptible = !!(Flags & PPM_IDLE_STATE_INTERRUPTIBLE);
        Slot->ContextRetained = !!(Flags & PPM_IDLE_STATE_CONTEXT_RETAINED);
        Slot->CacheCoherent = !!(Flags & PPM_IDLE_STATE_CACHE_COHERENT);
        Slot->InterruptsEnabled = !!(Flags & PPM_IDLE_STATE_INTERRUPTS_ENABLED);
        Slot->Latency = Registration->State[Index].Latency;
        Slot->BreakEvenDuration = Registration->State[Index].BreakEvenDuration;
        Slot->Power = Registration->State[Index].Power;
    }

    Install.Accounting->StateCount = Count;
    Install.States = States;
    Install.OnlyIfEmpty = OnlyIfEmpty;
    PpmRunOnProcessor(Number, PpmPublishIdleStates, &Install);
    PpmFreeIdleBlocks(&Install);

    return STATUS_SUCCESS;
}

/**
 * @brief Fills the constraints IdlePrepare decides on.
 *
 * @return The QPC time of the idle entry.
 */
static
ULONG64
PpmEvaluateIdleConstraints(
    _In_ PPPM_PROCESSOR Processor,
    _Inout_ PPPM_IDLE_STATE_SET States)
{
    PPPM_IDLE_CONSTRAINTS Constraints = &States->PrepareInfo.Constraints;
    ULONG64 Now = PpmQueryCounter();

    Constraints->TotalTime = Now;
    Constraints->IdleTime = Processor->IdleTimeTotal;
    Constraints->ExpectedIdleDuration = PpmIdleTickDuration;
    Constraints->OverrideState = States->ForceIdle ? States->OverrideIndex : PPM_IDLE_INDEX_NONE;
    Constraints->PromotePercent = Processor->IdlePromotePercent;
    Constraints->DemotePercent = Processor->IdleDemotePercent;
    Constraints->Parked = FALSE;
    Constraints->Interruptible = TRUE;
    Constraints->PlatformIdle = FALSE;
    return Now;
}

/**
 * @brief Enters one idle state through the driver and accounts for the time spent.
 *
 * @param[out] CancelCode
 * PPM_IDLE_NOT_CANCELED unless the kernel gave up before entering the state.
 */
static
NTSTATUS
PpmExecuteIdleTransition(
    _Inout_ PPPM_PROCESSOR Processor,
    _Inout_ PPPM_IDLE_STATE_SET States,
    _In_ ULONG State,
    _In_ ULONG PlatformState,
    _In_ ULONG64 EnterTime,
    _Out_ PULONG CancelCode)
{
    PPPM_IDLE_STATE_SLOT Slot = &States->State[State];
    BOOLEAN DeepSleep;
    NTSTATUS Status;
    ULONG64 Now;

    *CancelCode = PPM_IDLE_NOT_CANCELED;

    /* The HAL has no timer hand off, the clock keeps running, but a deep state may drop the TLB */
    DeepSleep = (!Slot->InterruptsEnabled && Slot->StateType >= 2);

    PpmAccrueActiveTime(Processor, EnterTime);
    PpmFoldFeedback(Processor, FALSE, FALSE, TRUE);
    PpmAtomicExchange64(&Processor->IdleTimeEntry, (LONG64)EnterTime);
    States->ActualState = State;

    Status = States->IdleExecute(States->PrepareInfo.Context, State, PlatformState);

    if (DeepSleep)
        KeFlushCurrentTb();

    Now = PpmQueryCounter();
    PpmAtomicExchange64(&Processor->IdleTimeEntry, 0);
    Processor->LastUpdateTime = Now;
    Processor->CyclesLast = PpmReadCycles();
    Processor->IdleTimeLast = Now - EnterTime;

    if (NT_SUCCESS(Status) && States->IdleComplete)
        States->IdleComplete(States->PrepareInfo.Context, State, PlatformState);

    return Status;
}

static
VOID
NTAPI
PpmDefaultIdlePrepare(
    _Inout_ PPROCESSOR_IDLE_PREPARE_INFO PrepareInfo)
{
    PrepareInfo->ProcessorIdleStateIndex = 0;
    PrepareInfo->PlatformIdleStateIndex = PPM_IDLE_INDEX_NONE;
    PrepareInfo->DependencyUsed = 0;
}

static
NTSTATUS
NTAPI
PpmDefaultIdleExecute(
    _In_ PVOID Context,
    _In_ ULONG ProcessorState,
    _In_ ULONG PlatformState)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(ProcessorState);
    UNREFERENCED_PARAMETER(PlatformState);

    HalProcessorIdle();
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
PpmDefaultIdleComplete(
    _In_ PVOID Context,
    _In_ ULONG ProcessorState,
    _In_ ULONG PlatformState)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(ProcessorState);
    UNREFERENCED_PARAMETER(PlatformState);
}

static
VOID
NTAPI
PpmDefaultIdleCancel(
    _In_ PVOID Context,
    _In_ ULONG CancelCode)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(CancelCode);
}

static
BOOLEAN
NTAPI
PpmDefaultIdleQuery(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    return FALSE;
}

/* PUBLIC FUNCTIONS ***********************************************************/

VOID
NTAPI
PpmIdleInitialize(VOID)
{
    ExInitializePushLock(&PpmIdlePolicyLock);
    PpmIdleTickDuration = (ULONG64)KeMaximumIncrement * PpmQpcFrequency / 10000000;
}

/**
 * @brief Scales the promote and demote thresholds with the square of the selected performance.
 */
VOID
NTAPI
PpmScaleIdlePolicy(
    _Inout_ PPPM_PROCESSOR Processor)
{
    ULONG Square;

    if (!Processor->IdleAllowScaling || !Processor->Constraint)
        return;

    Square = (UCHAR)Processor->Constraint->SelectedPercent;
    Square *= Square;
    Processor->IdlePromotePercent = (UCHAR)(Square * Processor->IdlePromoteBase / 10000);
    Processor->IdleDemotePercent = (UCHAR)(Square * Processor->IdleDemoteBase / 10000);
}

VOID
NTAPI
PpmAcquireIdlePolicyLock(VOID)
{
    PAGED_CODE();

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&PpmIdlePolicyLock);
}

VOID
NTAPI
PpmReleaseIdlePolicyLock(VOID)
{
    ExReleasePushLockExclusive(&PpmIdlePolicyLock);
    KeLeaveCriticalRegion();
}

/**
 * @brief Loads a changed idle policy into every processor with idle states.
 *
 * @remarks Called with the idle policy lock held.
 */
VOID
NTAPI
PpmReapplyIdlePolicy(VOID)
{
    PAGED_CODE();

    PpmRunOnProcessors(KeActiveProcessors, PpmResetIdlePolicyRoutine, NULL);
}

/**
 * @brief Gives every processor without driver idle states a single halt state.
 */
VOID
NTAPI
PpmInstallDefaultIdleStates(VOID)
{
    PPM_IDLE_STATES_REGISTRATION Registration;
    ULONG Index;

    PAGED_CODE();

    PpmAcquireIdlePolicyLock();
    for (Index = 0; Index < (ULONG)KeNumberProcessors; Index++)
    {
        RtlZeroMemory(&Registration, sizeof(Registration));
        Registration.Version = PPM_DRIVER_INTERFACE_VERSION;
        Registration.Context = &KiProcessorBlock[Index]->PowerState;
        Registration.IdlePrepare = PpmDefaultIdlePrepare;
        Registration.IdleExecute = PpmDefaultIdleExecute;
        Registration.IdleComplete = PpmDefaultIdleComplete;
        Registration.IdleCancel = PpmDefaultIdleCancel;
        Registration.IdleIsHalted = PpmDefaultIdleQuery;
        Registration.IdleInitiateWake = PpmDefaultIdleQuery;
        Registration.ProcessorIdleCount = 1;
        Registration.State[0].StateFlags = PPM_IDLE_STATE_INTERRUPTIBLE |
                                           PPM_IDLE_STATE_CACHE_COHERENT |
                                           PPM_IDLE_STATE_CONTEXT_RETAINED |
                                           (1 << PPM_IDLE_STATE_CSTATE_SHIFT) |
                                           PPM_IDLE_STATE_INTERRUPTS_ENABLED;

        PpmInstallIdleStates(Index, &Registration, TRUE);
    }
    PpmReleaseIdlePolicyLock();
}

/**
 * @brief Installs or removes the idle states of one processor for its driver.
 *
 * @remarks A ProcessorIdleCount of 0 removes the states, the processor then
 * idles through HalProcessorIdle and not through the default halt state.
 */
NTSTATUS
NTAPI
PpmRegisterIdleStates(
    _In_ PPPM_IDLE_STATES_REGISTRATION Registration)
{
    PPM_IDLE_INSTALL Install;
    NTSTATUS Status;
    ULONG Number;

    PAGED_CODE();

    if (Registration->Version != PPM_DRIVER_INTERFACE_VERSION)
        return STATUS_REVISION_MISMATCH;

    PpmAcquireIdlePolicyLock();

    Number = PpmProcessorIndex(&Registration->Processor);
    if (Number == INVALID_PROCESSOR_INDEX)
    {
        Status = STATUS_INVALID_PARAMETER;
    }
    else if (Registration->ProcessorIdleCount == 0)
    {
        Install.States = NULL;
        Install.Accounting = NULL;
        PpmRunOnProcessor(Number, PpmRemoveIdleStates, &Install);
        PpmFreeIdleBlocks(&Install);
        Status = STATUS_SUCCESS;
    }
    else if (!Registration->IdlePrepare || !Registration->IdleExecute)
    {
        Status = STATUS_INVALID_PARAMETER;
    }
    else
    {
        Status = PpmInstallIdleStates(Number, Registration, FALSE);
        if (NT_SUCCESS(Status) && Number == 0)
        {
            DPRINT1("PPM: driver idle states on processor 0, %lu states\n",
                    Registration->ProcessorIdleCount);
        }
    }

    PpmReleaseIdlePolicyLock();
    return Status;
}

/**
 * @brief Stores which processors share each idle state, only coordinated idle needs it.
 */
NTSTATUS
NTAPI
PpmRegisterIdleDomains(
    _In_ PPPM_IDLE_DOMAINS_REGISTRATION Registration)
{
    PPPM_IDLE_STATE_SET States;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Index, State;

    PAGED_CODE();

    if (Registration->Version != PPM_DRIVER_INTERFACE_VERSION)
        return STATUS_REVISION_MISMATCH;

    PpmAcquireIdlePolicyLock();

    if (Registration->TargetProcessors & ~KeActiveProcessors)
    {
        PpmReleaseIdlePolicyLock();
        return STATUS_INVALID_PARAMETER;
    }

    for (Index = 0; Index < (ULONG)KeNumberProcessors; Index++)
    {
        if (!(Registration->TargetProcessors & AFFINITY_MASK(Index)))
            continue;

        States = PpmProcessors[Index].IdleStates;
        if (!States || States->ProcessorIdleCount != Registration->Count)
        {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        for (State = 0; State < Registration->Count; State++)
            States->State[State].DomainMembers = Registration->State[State].Members;
    }

    PpmReleaseIdlePolicyLock();
    return Status;
}

/**
 * @brief Idles the current processor through its idle states.
 *
 * @remarks The idle loop calls this with interrupts disabled. The state's
 * IdleExecute enables them together with halting and they are on when it returns.
 */
VOID
FASTCALL
PpmIdle(
    _In_ PPROCESSOR_POWER_STATE PowerState)
{
    PPPM_IDLE_ACCOUNTING Accounting;
    PPPM_IDLE_STATE_SET States;
    PPPM_PROCESSOR Processor;
    ULONG State, PlatformState, Failures, Bit, CancelCode;
    ULONG64 EnterTime, LastIdle;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(PowerState);

    /* Disabled idle spins in the idle loop */
    if (PpmPolicy.IdleDisable)
        return;

    Processor = &PpmProcessors[KeGetCurrentProcessorNumber()];
    States = Processor->IdleStates;
    if (!States)
    {
        HalProcessorIdle();
        return;
    }

    Accounting = Processor->IdleAccounting;
    LastIdle = Processor->IdleTimeLast;
    if (LastIdle)
    {
        Processor->IdleTimeTotal += LastIdle;
        Processor->IdleTimeLast = 0;
        if (NT_SUCCESS(States->PreviousStatus))
            PpmRecordIdleTime(Accounting, States->ActualState, LastIdle);
    }

    /* A failed transition skips one accounting update */
    if (!NT_SUCCESS(States->PreviousStatus))
        States->PreviousStatus = STATUS_SUCCESS;

    EnterTime = PpmEvaluateIdleConstraints(Processor, States);
    States->IdlePrepare(&States->PrepareInfo);
    State = States->PrepareInfo.ProcessorIdleStateIndex;
    PlatformState = States->PrepareInfo.PlatformIdleStateIndex;

    if (State == PPM_IDLE_INDEX_HAL || State >= States->ProcessorIdleCount)
    {
        if (State == PPM_IDLE_INDEX_NONE)
            Accounting->AbortCount++;
        else
            HalProcessorIdle();
        return;
    }

    Failures = States->PrepareInfo.IdleSelectFailureMask;
    States->PrepareInfo.IdleSelectFailureMask = 0;
    for (Bit = 0; Failures; Bit++, Failures >>= 1)
    {
        if ((Failures & 1) && Bit < Accounting->StateCount)
            Accounting->State[Bit].FailedTransitions++;
    }

    if (State != States->TargetState)
    {
        States->OldState = States->TargetState;
        States->TargetState = State;
    }

    if (++Accounting->TotalTransitions == 0)
        PpmResetIdleAccounting(Accounting);

    Status = PpmExecuteIdleTransition(Processor, States, State, PlatformState, EnterTime, &CancelCode);
    if (CancelCode != PPM_IDLE_NOT_CANCELED)
    {
        Accounting->State[State].CanceledTransitions++;
        if (States->IdleCancel)
            States->IdleCancel(States->PrepareInfo.Context, CancelCode);
    }
    else if (!NT_SUCCESS(Status))
    {
        States->PreviousStatus = Status;
        Accounting->State[State].FailedTransitions++;
    }
}

/* EOF */
