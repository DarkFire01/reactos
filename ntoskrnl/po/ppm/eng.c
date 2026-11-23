/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Processor Power Management core engine infrastructure
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Engine tunables, the values the reference policy ships with */
#define PPM_SINGLE_STEP_PERCENT         5
#define PPM_MAKEUP_LIMIT                64
#define PPM_LATENCY_BOOST_PERCENT       99
#define PPM_BOOT_OVERRIDE_DELAY         (-900000000LL)

PPM_PROCESSOR PpmProcessors[MAXIMUM_PROCESSORS];
ULONG64 PpmQpcFrequency;
LIST_ENTRY PpmDriverDomainList;
ULONG PpmDriverDomainCount;
KAFFINITY PpmPerfRegistered;
KAFFINITY PpmDomainsToUpdate;
ULONG64 PpmCheckPeriodFloor;

/* The policy lock, the sequencer may take it and release it from a DPC */
static volatile LONG PpmPolicyLockHeld;
static KEVENT PpmPolicyEvent;
static PKTHREAD PpmPolicyOwner;

static KAFFINITY PpmProcessorsToUpdate;

/* 0 is disarmed, 1 is armed with the first check pending, else the time of the last check */
static volatile LONG64 PpmCheckLastExecution;
static ULONG64 PpmCheckPeriod;
static BOOLEAN PpmCheckArmed;
static ULONG PpmTimeWindowMs;
static ULONG PpmCheckMakeupCount;
static volatile ULONG PpmCheckStep;
static volatile LONG PpmCheckOutstanding;
static KDPC PpmCheckStartDpc;
static KDPC PpmCheckDpc;

/* Every domain runs at its constrained maximum while this is not zero */
static volatile LONG PpmBootOverride = 1;
static KTIMER PpmBootOverrideTimer;
static KDPC PpmBootOverrideDpc;
static WORK_QUEUE_ITEM PpmBootOverrideWorkItem;

/* PRIVATE FUNCTIONS **********************************************************/

static
ULONG
PpmCountProcessors(
    _In_ KAFFINITY Set)
{
    ULONG Count = 0;

    while (Set)
    {
        Set &= Set - 1;
        Count++;
    }

    return Count;
}

/**
 * @brief Queues an action on the targeted DPC of a processor.
 *
 * @remarks The DPC is only inserted by whoever finds the mask empty.
 */
static
VOID
PpmQueueAction(
    _Inout_ PPPM_PROCESSOR Processor,
    _In_ LONG Action)
{
    if (InterlockedOr(&Processor->ActionMask, Action) == 0)
        KeInsertQueueDpc(&Processor->ActionDpc, NULL, NULL);
}

/**
 * @brief Reads the scaled active times the feedback counters accumulated.
 */
static
VOID
PpmReadScaledTimes(
    _In_ PPPM_PROCESSOR Processor,
    _Out_ PULONG64 FrequencyScaled,
    _Out_ PULONG64 PerformanceScaled,
    _Out_ PULONG64 ScalabilityScaled)
{
    ULONG64 Unscaled = 100 * Processor->UnscaledTime;

    if (Processor->Counters[PPM_FEEDBACK_FREQUENCY])
        *FrequencyScaled = Processor->ScaledTime[PPM_FEEDBACK_FREQUENCY];
    else if (Processor->Counters[PPM_FEEDBACK_PERFORMANCE])
        *FrequencyScaled = Processor->ScaledTime[PPM_FEEDBACK_PERFORMANCE];
    else
        *FrequencyScaled = Unscaled;

    if (Processor->Counters[PPM_FEEDBACK_PERFORMANCE])
        *PerformanceScaled = Processor->ScaledTime[PPM_FEEDBACK_PERFORMANCE];
    else
        *PerformanceScaled = *FrequencyScaled;

    if (Processor->Counters[PPM_FEEDBACK_SCALABILITY])
        *ScalabilityScaled = Processor->ScaledTime[PPM_FEEDBACK_SCALABILITY];
    else
        *ScalabilityScaled = Unscaled;
}

/**
 * @brief Folds the feedback of a remote processor, only while it stays idle.
 */
static
BOOLEAN
PpmFoldRemoteIdle(
    _Inout_ PPPM_PROCESSOR Processor)
{
    LONG64 Entry;

    Entry = PpmAtomicRead64(&Processor->IdleTimeEntry);
    if (!Entry)
        return FALSE;

    if (!PpmFoldFeedback(Processor, FALSE, TRUE, FALSE))
        return FALSE;

    return (PpmAtomicRead64(&Processor->IdleTimeEntry) == Entry);
}

/**
 * @brief Takes a utilization snapshot of a processor and derives the latest percents.
 *
 * @return FALSE when the snapshot must be taken on the processor itself.
 */
static
BOOLEAN
PpmSnapUtility(
    _Inout_ PPPM_PROCESSOR Processor)
{
    BOOLEAN Remote = (Processor->Index != KeGetCurrentProcessorNumber());
    ULONG64 Now, Entry = 0, Active, Cycles, Frequency, Performance, Scalability;
    ULONG64 ActiveDelta, Divisor, CycleDelta;
    PPPM_PERF_CONSTRAINT Constraint;
    ULONG Percent;

    Now = PpmQueryCounter();
    if (Remote)
    {
        Entry = (ULONG64)PpmAtomicRead64(&Processor->IdleTimeEntry);
        if (!Entry)
            return FALSE;
    }
    else
    {
        PpmAccrueActiveTime(Processor, Now);
    }

    if (Now < Entry)
        Now = Entry;

    Cycles = Processor->CyclesActive;
    if (!PpmFoldFeedback(Processor, FALSE, Remote, FALSE))
        return FALSE;

    PpmReadScaledTimes(Processor, &Frequency, &Performance, &Scalability);
    if (Remote)
    {
        if ((ULONG64)PpmAtomicRead64(&Processor->IdleTimeEntry) != Entry)
            return FALSE;

        Active = Entry - Processor->IdleTimeTotal;
    }
    else
    {
        Active = Now - (Processor->IdleTimeTotal + Processor->IdleTimeLast);
    }

    if (Now <= Processor->Check.Time)
        return FALSE;

    ActiveDelta = Active - Processor->Check.Active;
    if (Remote && ActiveDelta > Now - Processor->Check.Time)
        return FALSE;

    Divisor = ActiveDelta ? ActiveDelta : 1;
    Percent = (ULONG)((Performance - Processor->Check.PerformanceScaled) / Divisor);
    if (Percent)
        Processor->LatestPerformancePercent = Percent;

    /* There is no affinitized cycle accounting, every active cycle counts as affinitized */
    CycleDelta = Cycles - Processor->Check.CyclesActive;
    if (CycleDelta)
        Processor->LatestAffinitizedPercent = 100;

    Constraint = Processor->Constraint;
    if (Constraint)
    {
        Percent = (ULONG)((Frequency - Processor->Check.FrequencyScaled) / Divisor);
        if (Percent)
            Constraint->LatestFrequencyPercent = Percent;

        Percent = (ULONG)((Scalability - Processor->Check.ScalabilityScaled) / Divisor);
        if (!Percent)
            Percent = Constraint->FrequencySensitivity;
        Constraint->FrequencySensitivity = min(Percent, 100);
    }

    Processor->Check.Time = Now;
    Processor->Check.Active = Active;
    Processor->Check.FrequencyScaled = Frequency;
    Processor->Check.PerformanceScaled = Performance;
    Processor->Check.ScalabilityScaled = Scalability;
    Processor->Check.CyclesActive = Cycles;
    Processor->Check.CyclesAffinitized = Cycles;
    return TRUE;
}

/**
 * @brief Restarts the utilization history of a processor, runs on the processor.
 */
VOID
NTAPI
PpmResetUtility(
    _In_ PPPM_PROCESSOR Processor,
    _In_opt_ PVOID Context)
{
    ULONG64 Now;

    UNREFERENCED_PARAMETER(Context);

    Processor->CyclesLast = PpmReadCycles();
    Processor->LastUpdateTime = PpmQueryCounter();

    Now = PpmQueryCounter();
    PpmAccrueActiveTime(Processor, Now);
    PpmFoldFeedback(Processor, TRUE, FALSE, FALSE);

    Processor->Check.Time = Now;
    Processor->Check.Active = Now - (Processor->IdleTimeTotal + Processor->IdleTimeLast);
    PpmReadScaledTimes(Processor,
                       &Processor->Check.FrequencyScaled,
                       &Processor->Check.PerformanceScaled,
                       &Processor->Check.ScalabilityScaled);
    Processor->Check.CyclesActive = Processor->CyclesActive;
    Processor->Check.CyclesAffinitized = Processor->CyclesActive;
    Processor->Check.LastTime = Processor->Check.Time;
    Processor->Check.LastActive = Processor->Check.Active;
}

/**
 * @brief Hands the period the check measures over to the driver.
 */
static
VOID
NTAPI
PpmApplyTimeWindow(
    _In_ PPPM_PROCESSOR Processor,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);

    if (Processor->Domain && Processor->Domain->TimeWindowHandler)
        Processor->Domain->TimeWindowHandler(Processor->Constraint->PerfContext, PpmTimeWindowMs);
}

/**
 * @brief Turns the period of the last check into a utilization of nominal performance.
 *
 * @remarks 10000 means busy for the whole period at nominal performance.
 */
static
VOID
PpmRecordUtility(
    _Inout_ PPPM_PROCESSOR Processor)
{
    ULONG64 TimeDelta, ActiveDelta;
    ULONG Performance;

    TimeDelta = Processor->Check.Time - Processor->Check.LastTime;
    ActiveDelta = Processor->Check.Active - Processor->Check.LastActive;

    /* A late check replays the missed periods one slice at a time */
    if (PpmCheckMakeupCount)
    {
        TimeDelta /= (PpmCheckMakeupCount + 1);
        ActiveDelta = min(ActiveDelta, TimeDelta);
    }

    Processor->Check.LastTime += TimeDelta;
    Processor->Check.LastActive += ActiveDelta;

    ActiveDelta = min(ActiveDelta, TimeDelta);
    TimeDelta = max(TimeDelta, 1);

    Performance = Processor->LatestPerformancePercent;
    Processor->Utility = (ULONG)(ActiveDelta * 100 * Performance / TimeDelta);
    Processor->AffinitizedUtility = (ULONG)(ActiveDelta * Performance *
                                            Processor->LatestAffinitizedPercent / TimeDelta);
    Processor->AveragePerformancePercent = Performance;
}

/**
 * @brief Picks the next target of one processor from its utilization.
 */
static
VOID
PpmSelectProcessorState(
    _Inout_ PPPM_PROCESSOR Processor)
{
    PPPM_PERF_CONSTRAINT Constraint = Processor->Constraint;
    PPPM_DRIVER_DOMAIN Domain = Processor->Domain;
    UCHAR Increase, Decrease, IncreasePolicy, DecreasePolicy;
    ULONG Ceiling, Flags, Tolerance, Target, Utility, Middle, Ratio, Frequency;
    ULONG Sensitivity;
    ULONG64 State;

    if (PpmPolicy.PerfBoostMode == PPM_BOOST_MODE_ENABLED ||
        PpmPolicy.PerfBoostMode == PPM_BOOST_MODE_EFFICIENT_ENABLED)
    {
        Ceiling = Domain->MaxPercent;
    }
    else
    {
        Ceiling = 100;
    }

    Flags = PPM_SELECT_ALLOW_THROTTLE;
    Tolerance = 100;
    Target = Constraint->TargetPercent;
    Utility = Processor->Utility;
    Increase = PpmPolicy.PerfIncreaseThreshold;
    Decrease = PpmPolicy.PerfDecreaseThreshold;

    /* A processor that gains little from clock speed moves its thresholds towards 100 */
    Sensitivity = Constraint->FrequencySensitivity;
    if (Sensitivity != 100)
    {
        Decrease = (UCHAR)(((LONG)Sensitivity * ((LONG)Decrease - 100) + 10000) / 100);
        Increase = (UCHAR)(((LONG)Sensitivity * ((LONG)Increase - 100) + 10000) / 100);
    }

    IncreasePolicy = PpmPolicy.PerfIncreasePolicy;
    DecreasePolicy = PpmPolicy.PerfDecreasePolicy;
    Middle = ((ULONG)Increase + Decrease) / 2;
    if (!Middle)
    {
        if (IncreasePolicy == 0)
            IncreasePolicy = 2;
        if (DecreasePolicy == 0)
            DecreasePolicy = 2;
    }

    Ratio = Utility / max(Target, 1);
    if (Ratio > Increase)
    {
        if (Target < Ceiling)
        {
            if (IncreasePolicy == 0)
                Target = Utility / Middle;
            else if (IncreasePolicy == 1)
                Target += PPM_SINGLE_STEP_PERCENT;
            else if (IncreasePolicy == 2)
                Target = Ceiling;

            Target = min(Target, Ceiling);
        }
    }
    else if (Ratio < Decrease && Target > 1)
    {
        Flags |= PPM_SELECT_ROUND_DOWN;
        if (DecreasePolicy == 0)
            Target = Utility / Middle;
        else if (DecreasePolicy == 1)
            Target = (Target > PPM_SINGLE_STEP_PERCENT) ? Target - PPM_SINGLE_STEP_PERCENT : 1;
        else if (DecreasePolicy == 2)
            Target = 1;
    }

    if (Increase && Utility / Increase < 100)
        Tolerance = Utility / Increase;

    Constraint->TargetPercent = Domain->SelectionHandler(Constraint->PerfContext,
                                                         Target,
                                                         Domain->MinThrottlePercent,
                                                         Ceiling,
                                                         Flags,
                                                         &Frequency,
                                                         &State);
    Constraint->TolerancePercent = Tolerance;
}

/**
 * @brief Moves the desired level of a domain towards the highest member target.
 *
 * @return TRUE when the domain has to be applied again.
 */
static
BOOLEAN
PpmSelectDomainState(
    _Inout_ PPPM_DRIVER_DOMAIN Domain)
{
    BOOLEAN Update;
    ULONG Highest = 1;
    ULONG Index;

    if (Domain->PerfChangeIntervalCount != MAXULONG)
        Domain->PerfChangeIntervalCount++;

    for (Index = 0; Index < Domain->ProcessorCount; Index++)
        Highest = max(Highest, Domain->Constraints[Index].TargetPercent);

    if ((Highest > Domain->DesiredPercent &&
         Domain->PerfChangeIntervalCount >= PpmPolicy.PerfIncreaseTime) ||
        (Highest < Domain->DesiredPercent &&
         Domain->PerfChangeIntervalCount >= PpmPolicy.PerfDecreaseTime))
    {
        Domain->DesiredPercent = Highest;
    }

    Update = (Domain->DesiredPercent != Domain->SelectedPercent);
    for (Index = 0; Index < Domain->ProcessorCount; Index++)
    {
        if (Domain->Constraints[Index].DesiredPercent != Domain->Constraints[Index].SelectedPercent)
            Update = TRUE;
    }

    return Update;
}

/**
 * @brief Clips the desired level of a domain to policy and caps and snaps it to a state.
 *
 * @return TRUE when the selected state of the domain or of a member changed.
 */
static
BOOLEAN
PpmApplyDomainState(
    _Inout_ PPPM_DRIVER_DOMAIN Domain,
    _In_ BOOLEAN LatencyBoost)
{
    PPPM_PERF_CONSTRAINT Constraint;
    ULONG Maximum, Minimum, Floor, Flags, Guaranteed, Tolerance, Target, Percent, Frequency;
    BOOLEAN Capped = FALSE, Changed;
    ULONG Index;
    ULONG64 State;

    Maximum = Domain->MaxPolicyPercent;
    Minimum = Domain->MinPolicyPercent;
    if (PpmPolicy.AllowThrottling)
    {
        Floor = Domain->MinThrottlePercent;
        Flags = PPM_SELECT_ALLOW_THROTTLE;
    }
    else
    {
        Floor = Domain->MinPerfPercent;
        Flags = 0;
    }

    Maximum = max(Maximum, Floor);
    Minimum = max(Minimum, Floor);

    /* The thermal cap always limits, the platform cap only when the firmware made it hard */
    Guaranteed = 100;
    Tolerance = 0;
    for (Index = 0; Index < Domain->ProcessorCount; Index++)
    {
        Constraint = &Domain->Constraints[Index];
        if (Domain->HardPlatformCap && Maximum > Constraint->PlatformCap)
        {
            Maximum = Constraint->PlatformCap;
            Capped = TRUE;
        }

        if (Maximum > Constraint->ThermalCap)
        {
            Maximum = Constraint->ThermalCap;
            Capped = TRUE;
        }

        Guaranteed = min(Guaranteed, Constraint->PlatformCap);
        Tolerance = max(Tolerance, Constraint->TolerancePercent);
    }

    Maximum = max(Maximum, Domain->MinThrottlePercent);
    Minimum = max(Minimum, Domain->MinThrottlePercent);
    Minimum = min(Minimum, Maximum);
    Guaranteed = min(Guaranteed, Maximum);

    if (PpmPolicy.PerfBoostMode != PPM_BOOST_MODE_DISABLED && Maximum == 100)
        Maximum = Domain->MaxPercent;

    Domain->ConstrainedMaxPercent = Maximum;
    Domain->ConstrainedMinPercent = Minimum;

    if (ReadNoFence(&PpmBootOverride))
    {
        Target = Maximum;
        Tolerance = Maximum;
        Target = max(Target, Minimum);
    }
    else
    {
        Target = Domain->DesiredPercent;
        if (LatencyBoost)
        {
            Tolerance = PPM_LATENCY_BOOST_PERCENT;
            Target = max(Target, PPM_LATENCY_BOOST_PERCENT);
            Domain->DesiredPercent = Target;
        }

        if (Target > Maximum)
        {
            Target = Maximum;
            if (Capped)
                Flags |= PPM_SELECT_ROUND_DOWN;
        }
        else
        {
            Target = max(Target, Minimum);
        }
    }

    if (Target >= 100 &&
        (PpmPolicy.PerfBoostMode == PPM_BOOST_MODE_AGGRESSIVE ||
         PpmPolicy.PerfBoostMode == PPM_BOOST_MODE_EFFICIENT_AGGRESSIVE))
    {
        Target = Maximum;
    }

    if (Tolerance > Target)
        Tolerance = Target;
    else
        Tolerance = max(Tolerance, Minimum);
    Tolerance = min(Tolerance, Guaranteed);

    Percent = Domain->SelectionHandler(PpmProcessors[Domain->Master].Constraint->PerfContext,
                                       Target,
                                       Minimum,
                                       Maximum,
                                       Flags,
                                       &Frequency,
                                       &State);

    Changed = (Domain->Force ||
               Domain->GuaranteedPercent != Guaranteed ||
               Domain->SelectedState != State);

    for (Index = 0; Index < Domain->ProcessorCount; Index++)
    {
        Constraint = &Domain->Constraints[Index];
        Constraint->PreviousPercent = Constraint->SelectedPercent;
        Constraint->PreviousFrequency = Constraint->SelectedFrequency;
        if (Constraint->SelectedPercent != Percent)
        {
            Constraint->SelectedPercent = Percent;
            Constraint->SelectedState = State;
            Constraint->SelectedFrequency = Frequency;
            Changed = TRUE;
        }
    }

    if (Changed)
    {
        Domain->Force = FALSE;
        Domain->GuaranteedPercent = Guaranteed;
        Domain->SelectedPercent = Percent;
        Domain->SelectedFrequency = Frequency;
        Domain->TolerancePercent = Tolerance;
        Domain->SelectedState = State;
        Domain->PerfChangeTime = PpmQueryCounter();
        Domain->PerfChangeIntervalCount = 0;
    }

    return Changed;
}

/**
 * @brief Hands the selected state of the domain to the driver for one member.
 *
 * @return FALSE when the call has to be made from the member processor itself.
 */
static
BOOLEAN
PpmApplyProcessorState(
    _Inout_ PPPM_PROCESSOR Processor,
    _In_ BOOLEAN Reapply)
{
    PPPM_DRIVER_DOMAIN Domain = Processor->Domain;
    PPPM_PERF_CONSTRAINT Constraint = Processor->Constraint;
    BOOLEAN Initiate, Local;

    /* With SW_ANY only the master programs the hardware, the others just record the state */
    Initiate = (Processor->Index == Domain->Master ||
                Domain->Coordination != PPM_COORDINATION_SW_ANY);

    Local = (Processor->Index == KeGetCurrentProcessorNumber());
    if (!Local && (Processor->Index == Domain->Master || (Initiate && Domain->AffinitizeControl)))
        return FALSE;

    if (Local)
    {
        PpmAccrueActiveTime(Processor, PpmQueryCounter());
        PpmFoldFeedback(Processor, FALSE, FALSE, FALSE);
    }
    else if (!PpmFoldRemoteIdle(Processor))
    {
        return FALSE;
    }

    Domain->ControlHandler(Constraint->PerfContext,
                           Constraint->SelectedState,
                           Domain->ConstrainedMinPercent,
                           Domain->ConstrainedMaxPercent,
                           Domain->TolerancePercent,
                           Domain->AutonomousMode,
                           Initiate,
                           Reapply);

    if (!Reapply)
    {
        Processor->GuaranteedPercent = Domain->GuaranteedPercent;
        PpmScaleIdlePolicy(Processor);
    }

    return TRUE;
}

/**
 * @brief Runs one action on every processor of a set, queueing it where it cannot run here.
 *
 * @return TRUE when nothing was queued.
 */
static
BOOLEAN
PpmRunOrQueue(
    _In_ KAFFINITY Set,
    _In_ LONG Action)
{
    PPPM_PROCESSOR Processor;
    BOOLEAN Done;
    ULONG Index;

    for (Index = 0; Index < (ULONG)KeNumberProcessors; Index++)
    {
        if (!(Set & AFFINITY_MASK(Index)))
            continue;

        Processor = &PpmProcessors[Index];
        if (Action == PPM_ACTION_SNAP_UTILITY)
            Done = PpmSnapUtility(Processor);
        else
            Done = PpmApplyProcessorState(Processor, (Action == PPM_ACTION_REAPPLY_STATE));

        if (Done)
            Set &= ~AFFINITY_MASK(Index);
    }

    if (!Set)
        return TRUE;

    /* The count must be complete before the first DPC can finish */
    PpmCheckOutstanding = (LONG)PpmCountProcessors(Set);
    for (Index = 0; Index < (ULONG)KeNumberProcessors; Index++)
    {
        if (Set & AFFINITY_MASK(Index))
            PpmQueueAction(&PpmProcessors[Index], Action);
    }

    return FALSE;
}

static
BOOLEAN
PpmApplyDomainStates(VOID)
{
    PPPM_DRIVER_DOMAIN Domain;
    PLIST_ENTRY Entry;

    for (Entry = PpmDriverDomainList.Flink; Entry != &PpmDriverDomainList; Entry = Entry->Flink)
    {
        Domain = CONTAINING_RECORD(Entry, PPM_DRIVER_DOMAIN, Link);
        if (!(PpmDomainsToUpdate & AFFINITY_MASK(Domain->Master)))
            continue;

        if (PpmApplyDomainState(Domain, FALSE))
            PpmProcessorsToUpdate |= Domain->Members;
    }

    PpmDomainsToUpdate = 0;
    return TRUE;
}

static
BOOLEAN
PpmApplyProcessorStates(VOID)
{
    KAFFINITY Set = PpmProcessorsToUpdate;

    PpmProcessorsToUpdate = 0;
    return PpmRunOrQueue(Set, PPM_ACTION_APPLY_STATE);
}

/**
 * @brief Makes every member of the marked domains drop what its driver cached.
 */
static
BOOLEAN
PpmForceDomainStates(VOID)
{
    PPPM_DRIVER_DOMAIN Domain;
    KAFFINITY Members = 0;
    PLIST_ENTRY Entry;

    for (Entry = PpmDriverDomainList.Flink; Entry != &PpmDriverDomainList; Entry = Entry->Flink)
    {
        Domain = CONTAINING_RECORD(Entry, PPM_DRIVER_DOMAIN, Link);
        if (!(PpmDomainsToUpdate & AFFINITY_MASK(Domain->Master)))
            continue;

        Domain->Force = TRUE;
        Members |= Domain->Members;
    }

    return PpmRunOrQueue(Members, PPM_ACTION_REAPPLY_STATE);
}

/**
 * @brief Runs the check sequence from its current step until it ends or waits.
 *
 * @remarks Runs at DISPATCH_LEVEL with the policy lock owned by the sequence.
 * Whoever completes a wait advances the step and queues the check DPC.
 */
static
VOID
PpmCheckRun(VOID)
{
    PPPM_DRIVER_DOMAIN Domain;
    PLIST_ENTRY Entry;
    ULONG Index;

    for (;;)
    {
        switch (PpmCheckStep)
        {
            case PpmStepSnapUtility:
                if (!PpmRunOrQueue(PpmPerfRegistered, PPM_ACTION_SNAP_UTILITY))
                    return;
                break;

            case PpmStepRecordUtility:
                for (Index = 0; Index < (ULONG)KeNumberProcessors; Index++)
                {
                    if (PpmPerfRegistered & AFFINITY_MASK(Index))
                        PpmRecordUtility(&PpmProcessors[Index]);
                }
                break;

            case PpmStepSelectProcessors:
                for (Index = 0; Index < (ULONG)KeNumberProcessors; Index++)
                {
                    if (PpmProcessors[Index].Constraint)
                        PpmSelectProcessorState(&PpmProcessors[Index]);
                }
                break;

            case PpmStepSelectDomains:
                for (Entry = PpmDriverDomainList.Flink; Entry != &PpmDriverDomainList; Entry = Entry->Flink)
                {
                    Domain = CONTAINING_RECORD(Entry, PPM_DRIVER_DOMAIN, Link);
                    if (PpmSelectDomainState(Domain))
                        PpmDomainsToUpdate |= AFFINITY_MASK(Domain->Master);
                }
                break;

            case PpmStepApplyDomains:
            case PpmStepAsyncApplyDomains:
                PpmApplyDomainStates();
                break;

            case PpmStepMakeupChecks:
                if (PpmCheckMakeupCount)
                {
                    PpmCheckMakeupCount--;
                    PpmCheckStep = PpmStepRecordUtility;
                    continue;
                }
                break;

            case PpmStepApplyProcessors:
            case PpmStepAsyncApplyProcessors:
                if (!PpmApplyProcessorStates())
                    return;
                break;

            case PpmStepForceDomains:
                if (!PpmForceDomainStates())
                    return;
                break;

            case PpmStepPeriodicEnd:
            case PpmStepAsyncEnd:
            default:
                PpmCheckStep = PpmStepNone;
                PpmReleasePolicyLock();
                return;
        }

        PpmCheckStep++;
    }
}

static
VOID
NTAPI
PpmCheckDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    PpmCheckRun();
}

/**
 * @brief Starts a periodic check, skipped when someone holds the policy lock.
 *
 * @param[in] SystemArgument1
 * How many check periods were missed and have to be replayed.
 */
static
VOID
NTAPI
PpmCheckStartDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument2);

    if (InterlockedExchange(&PpmPolicyLockHeld, 1) != 0)
        return;

    PpmPolicyOwner = NULL;
    PpmCheckMakeupCount = (ULONG)(ULONG_PTR)SystemArgument1;
    PpmCheckStep = PpmStepSnapUtility;
    PpmCheckRun();
}

/**
 * @brief Runs the queued actions of one processor on that processor.
 */
static
VOID
NTAPI
PpmActionDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    PPPM_PROCESSOR Processor = (PPPM_PROCESSOR)DeferredContext;
    LONG Actions;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    Actions = InterlockedExchange(&Processor->ActionMask, 0);
    if (Actions & PPM_ACTION_SNAP_UTILITY)
        PpmSnapUtility(Processor);
    if (Actions & PPM_ACTION_REAPPLY_STATE)
        PpmApplyProcessorState(Processor, TRUE);
    if (Actions & PPM_ACTION_APPLY_STATE)
        PpmApplyProcessorState(Processor, FALSE);

    if (InterlockedDecrement(&PpmCheckOutstanding) == 0)
    {
        PpmCheckStep++;
        KeInsertQueueDpc(&PpmCheckDpc, NULL, NULL);
    }
}

static
VOID
NTAPI
PpmBootOverrideWorker(
    _In_ PVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);

    PpmAcquirePolicyLock();

    /* A scheme without processor settings leaves the boot policy, use Balanced instead */
    if (!PpmPolicy.ThrottlePolicyPushed)
        PpmApplyDefaultScheme();

    if (InterlockedDecrement(&PpmBootOverride) == 0)
    {
        DPRINT1("PPM: boot performance override released, %lu perf domains\n", PpmDriverDomainCount);
        PpmUpdateAllDomains();
        return;
    }

    PpmReleasePolicyLock();
}

static
VOID
NTAPI
PpmBootOverrideDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    ExInitializeWorkItem(&PpmBootOverrideWorkItem, PpmBootOverrideWorker, NULL);
    ExQueueWorkItem(&PpmBootOverrideWorkItem, DelayedWorkQueue);
}

/* PUBLIC FUNCTIONS ***********************************************************/

VOID
NTAPI
PpmEngineInitialize(VOID)
{
    PpmQpcFrequency = PpmQueryCounterFrequency();

    KeInitializeEvent(&PpmPolicyEvent, SynchronizationEvent, FALSE);
    InitializeListHead(&PpmDriverDomainList);

    KeInitializeDpc(&PpmCheckStartDpc, PpmCheckStartDpcRoutine, NULL);
    KeSetImportanceDpc(&PpmCheckStartDpc, HighImportance);
    KeInitializeDpc(&PpmCheckDpc, PpmCheckDpcRoutine, NULL);
    KeSetImportanceDpc(&PpmCheckDpc, HighImportance);
}

/**
 * @brief Sets up the power management block of a processor as it starts.
 */
VOID
NTAPI
PpmInitializeProcessor(
    _In_ PKPRCB Prcb)
{
    PPPM_PROCESSOR Processor = &PpmProcessors[Prcb->Number];

    RtlZeroMemory(Processor, sizeof(*Processor));
    Processor->Index = Prcb->Number;
    Processor->LatestPerformancePercent = 100;
    Processor->AveragePerformancePercent = 100;
    Processor->GuaranteedPercent = 100;
    Processor->KernelTimesIndex = PPM_FEEDBACK_TYPES;

    KeInitializeDpc(&Processor->ActionDpc, PpmActionDpcRoutine, Processor);
    KeSetImportanceDpc(&Processor->ActionDpc, HighImportance);
    KeSetTargetProcessorDpc(&Processor->ActionDpc, (CCHAR)Prcb->Number);
}

VOID
NTAPI
PpmAcquirePolicyLock(VOID)
{
    PAGED_CODE();

    /* Every release signals the event, so a waiter that misses the lock wakes for the next one */
    KeEnterGuardedRegion();
    while (InterlockedExchange(&PpmPolicyLockHeld, 1) != 0)
        KeWaitForSingleObject(&PpmPolicyEvent, Executive, KernelMode, FALSE, NULL);
    PpmPolicyOwner = KeGetCurrentThread();
}

/**
 * @brief Releases the policy lock, also from the DPC that ends a check sequence.
 */
VOID
NTAPI
PpmReleasePolicyLock(VOID)
{
    PKTHREAD Owner = PpmPolicyOwner;

    PpmPolicyOwner = NULL;
    InterlockedExchange(&PpmPolicyLockHeld, 0);
    KeSetEvent(&PpmPolicyEvent, IO_NO_INCREMENT, FALSE);
    if (Owner)
        KeLeaveGuardedRegion();
}

/**
 * @brief Runs a routine on one processor at DISPATCH_LEVEL and waits for it.
 */
VOID
NTAPI
PpmRunOnProcessor(
    _In_ ULONG Index,
    _In_ PPPM_PROCESSOR_ROUTINE Routine,
    _In_opt_ PVOID Context)
{
    PKTHREAD Thread = KeGetCurrentThread();
    KAFFINITY Previous = 0;
    KIRQL OldIrql;

    PAGED_CODE();

    /* A caller that is already pinned gets its pinning back */
    if (Thread->SystemAffinityActive)
        Previous = Thread->Affinity;

    KeSetSystemAffinityThread(AFFINITY_MASK(Index));
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Routine(&PpmProcessors[Index], Context);
    KeLowerIrql(OldIrql);

    if (Previous)
        KeSetSystemAffinityThread(Previous);
    else
        KeRevertToUserAffinityThread();
}

/**
 * @brief Runs a routine on every processor of a set, one at a time.
 */
VOID
NTAPI
PpmRunOnProcessors(
    _In_ KAFFINITY Processors,
    _In_ PPPM_PROCESSOR_ROUTINE Routine,
    _In_opt_ PVOID Context)
{
    ULONG Index;

    PAGED_CODE();

    for (Index = 0; Index < (ULONG)KeNumberProcessors; Index++)
    {
        if (Processors & AFFINITY_MASK(Index))
            PpmRunOnProcessor(Index, Routine, Context);
    }
}

/**
 * @brief Charges the time since the last update as active time, on the processor itself.
 */
VOID
NTAPI
PpmAccrueActiveTime(
    _Inout_ PPPM_PROCESSOR Processor,
    _In_ ULONG64 Now)
{
    ULONG64 Cycles;

    PpmAtomicAdd64(&Processor->UnaccountedTime, (LONG64)(Now - Processor->LastUpdateTime));
    Processor->LastUpdateTime = Now;

    Cycles = PpmReadCycles();
    if (Cycles > Processor->CyclesLast)
        Processor->CyclesActive += Cycles - Processor->CyclesLast;
    Processor->CyclesLast = Cycles;
}

/**
 * @brief Reads one feedback counter as a percent of nominal performance.
 */
ULONG
NTAPI
PpmReadFeedbackSource(
    _Inout_ PPPM_FEEDBACK_SOURCE Source,
    _In_ BOOLEAN Reset)
{
    ULONG64 Reference, Actual;
    ULONG Value;

    if (!Source->Differential)
    {
        Value = 0;
        Source->InstantaneousRead(Source->Context, &Value);
        return Value;
    }

    Reference = 0;
    Actual = 0;
    Source->DifferentialRead(Source->Context, Reset, &Reference, &Actual);
    if (Reference != Source->LastReference)
    {
        Source->CachedPercent = (ULONG)(100 * (Actual - Source->LastActual) /
                                        (Reference - Source->LastReference));
        Source->LastReference = Reference;
        Source->LastActual = Actual;
    }

    return Source->CachedPercent;
}

/**
 * @brief Weighs the unaccounted active time with what the feedback counters report.
 *
 * @param[in] Remote
 * The processor is not the current one, counters bound to their processor fail the call.
 *
 * @param[in] Lazy
 * Skip the fold when no counter is bound to its processor, it can be done remotely later.
 */
BOOLEAN
NTAPI
PpmFoldFeedback(
    _Inout_ PPPM_PROCESSOR Processor,
    _In_ BOOLEAN Reset,
    _In_ BOOLEAN Remote,
    _In_ BOOLEAN Lazy)
{
    BOOLEAN Affinitized = FALSE, Enabled;
    ULONG64 Delta;
    ULONG Type;

    if (PpmAtomicRead64(&Processor->UnaccountedTime) == 0)
        return TRUE;

    for (Type = 0; Type < PPM_FEEDBACK_TYPES; Type++)
    {
        if (!Processor->Counters[Type] || !Processor->Counters[Type]->Affinitized)
            continue;

        if (Remote)
            return FALSE;
        Affinitized = TRUE;
    }

    if (Lazy && !Affinitized)
        return TRUE;

    Enabled = KeDisableInterrupts();
    PpmLockFeedback(Processor);

    Delta = (ULONG64)PpmAtomicExchange64(&Processor->UnaccountedTime, 0);
    Processor->UnscaledTime += Delta;
    for (Type = 0; Type < PPM_FEEDBACK_TYPES; Type++)
    {
        if (Processor->Counters[Type])
            Processor->ScaledTime[Type] += Delta * PpmReadFeedbackSource(Processor->Counters[Type], Reset);
    }

    PpmUnlockFeedback(Processor);
    KeRestoreInterrupts(Enabled);
    return TRUE;
}

ULONG64
NTAPI
PpmFrequencyScaledTime(
    _In_ PPPM_PROCESSOR Processor)
{
    ULONG64 Frequency, Performance, Scalability;

    PpmReadScaledTimes(Processor, &Frequency, &Performance, &Scalability);
    return Frequency;
}

VOID
NTAPI
PpmCalculateMinMax(
    _Inout_ PPPM_DRIVER_DOMAIN Domain)
{
    Domain->MinPolicyPercent = PpmPolicy.PerfMinPolicy;
    Domain->MaxPolicyPercent = max(PpmPolicy.PerfMaxPolicy, PpmPolicy.PerfMinPolicy);
    Domain->ConstrainedMinPercent = PpmPolicy.PerfMinPolicy;
    Domain->ConstrainedMaxPercent = max(1, Domain->MaxPolicyPercent);
}

/**
 * @brief Arms or disarms the periodic check for the current registrations and policy.
 *
 * @remarks Called at PASSIVE_LEVEL with the policy lock held.
 */
VOID
NTAPI
PpmCheckReInitialize(VOID)
{
    ULONG64 Period;
    BOOLEAN Enable;

    PAGED_CODE();

    Enable = (PpmPerfRegistered != 0 && PpmPolicy.PerfMinPolicy != PpmPolicy.PerfMaxPolicy);
    if (!Enable)
    {
        if (PpmCheckArmed)
        {
            PpmAtomicExchange64(&PpmCheckLastExecution, 0);
            PpmCheckArmed = FALSE;
            DPRINT1("PPM: performance check disarmed\n");
        }
        return;
    }

    Period = max(10000ULL * PpmPolicy.PerfTimeCheck, PpmCheckPeriodFloor);
    if (PpmCheckArmed && Period != PpmCheckPeriod)
    {
        PpmAtomicExchange64(&PpmCheckLastExecution, 0);
        PpmCheckArmed = FALSE;
    }

    if (PpmCheckArmed)
        return;

    PpmCheckPeriod = Period;
    PpmTimeWindowMs = (ULONG)(((Period > KeMaximumIncrement) ? Period - KeMaximumIncrement : 0) / 10000);
    PpmRunOnProcessors(PpmPerfRegistered, PpmResetUtility, NULL);
    PpmRunOnProcessors(PpmPerfRegistered, PpmApplyTimeWindow, NULL);

    PpmAtomicExchange64(&PpmCheckLastExecution, 1);
    PpmCheckArmed = TRUE;
    DPRINT1("PPM: performance check armed, period %lu ms, policy %u-%u%%\n",
            (ULONG)(Period / 10000), PpmPolicy.PerfMinPolicy, PpmPolicy.PerfMaxPolicy);
}

/**
 * @brief Runs a performance sequence from PASSIVE_LEVEL on behalf of the lock holder.
 *
 * @remarks The sequence owns the policy lock from here and releases it when it ends,
 * which may be after this returns.
 */
VOID
NTAPI
PpmRunPerfSequence(
    _In_ PPM_CHECK_STEP Start)
{
    KIRQL OldIrql;

    PpmPolicyOwner = NULL;
    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    PpmCheckStep = Start;
    PpmCheckRun();
    KeLowerIrql(OldIrql);
    KeLeaveGuardedRegion();
}

/**
 * @brief Reapplies every domain after a policy change, consumes the policy lock.
 */
VOID
NTAPI
PpmUpdateAllDomains(VOID)
{
    PPPM_DRIVER_DOMAIN Domain;
    PLIST_ENTRY Entry;

    for (Entry = PpmDriverDomainList.Flink; Entry != &PpmDriverDomainList; Entry = Entry->Flink)
    {
        Domain = CONTAINING_RECORD(Entry, PPM_DRIVER_DOMAIN, Link);
        PpmDomainsToUpdate |= AFFINITY_MASK(Domain->Master);
    }

    PpmRunPerfSequence(PpmStepAsyncApplyDomains);
}

/**
 * @brief Starts a check when its period elapsed, from the clock interrupt.
 *
 * @param[in] InterruptTime
 * The interrupt time of this tick, in 100ns units.
 */
VOID
FASTCALL
PpmCheckClockTick(
    _In_ ULONG64 InterruptTime)
{
    ULONG64 Last, Period, Stamp, Behind;
    ULONG Makeup = 0;

    Last = (ULONG64)PpmAtomicRead64(&PpmCheckLastExecution);
    if (!Last)
        return;

    Period = max(PpmCheckPeriod, KeTimeIncrement);
    if (Last + Period > InterruptTime)
        return;

    Stamp = InterruptTime;
    if (Last != 1)
    {
        /* Stay in phase unless a whole tick was lost */
        if (Last + Period + KeMaximumIncrement > InterruptTime)
            Stamp = Last + Period;

        Behind = Stamp - Period - Last;
        if (Behind >= Period)
            Makeup = (ULONG)min(Behind / Period, PPM_MAKEUP_LIMIT);
    }

    if ((ULONG64)InterlockedCompareExchange64(&PpmCheckLastExecution, (LONG64)Stamp, (LONG64)Last) == Last)
        KeInsertQueueDpc(&PpmCheckStartDpc, (PVOID)(ULONG_PTR)Makeup, NULL);
}

/**
 * @brief Keeps every domain at its maximum for the first 90 seconds after boot.
 */
VOID
NTAPI
PpmStartBootOverrideTimer(VOID)
{
    LARGE_INTEGER DueTime;

    KeInitializeDpc(&PpmBootOverrideDpc, PpmBootOverrideDpcRoutine, NULL);
    KeInitializeTimer(&PpmBootOverrideTimer);

    DueTime.QuadPart = PPM_BOOT_OVERRIDE_DELAY;
    KeSetTimer(&PpmBootOverrideTimer, DueTime, &PpmBootOverrideDpc);
}

/* EOF */
