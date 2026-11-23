/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Processor Power Management C/P states management
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

typedef struct _PPM_DISTRIBUTION_SNAP
{
    ULONG64 ActiveTime;
    ULONG64 FrequencyScaledTime;
} PPM_DISTRIBUTION_SNAP, *PPPM_DISTRIBUTION_SNAP;

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief Brings the active time and feedback of a processor up to date, runs on the processor.
 */
static
VOID
NTAPI
PpmSnapDistribution(
    _In_ PPPM_PROCESSOR Processor,
    _In_opt_ PVOID Context)
{
    PPPM_DISTRIBUTION_SNAP Snap = (PPPM_DISTRIBUTION_SNAP)Context;
    ULONG64 Now;

    Now = PpmQueryCounter();
    PpmAccrueActiveTime(Processor, Now);
    PpmFoldFeedback(Processor, FALSE, FALSE, FALSE);

    Snap->ActiveTime = Now - (Processor->IdleTimeTotal + Processor->IdleTimeLast);
    Snap->FrequencyScaledTime = PpmFrequencyScaledTime(Processor);
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief Reports the frequencies and idle states of one processor.
 *
 * @remarks Without a driver performance domain every frequency is the rated speed.
 */
VOID
NTAPI
PpmQueryProcessorPowerInformation(
    _In_ ULONG Index,
    _Out_ PPROCESSOR_POWER_INFORMATION Information)
{
    PPPM_PROCESSOR Processor = &PpmProcessors[Index];
    PPPM_PERF_CONSTRAINT Constraint = Processor->Constraint;
    PPPM_DRIVER_DOMAIN Domain = Processor->Domain;
    PPPM_IDLE_STATE_SET States = Processor->IdleStates;
    PKPRCB Prcb = KiProcessorBlock[Index];

    Information->Number = Index;
    if (Domain && Constraint)
    {
        Information->MaxMhz = Domain->NominalFrequency;
        Information->CurrentMhz = Constraint->SelectedFrequency;
        Information->MhzLimit = Domain->NominalFrequency * Domain->MaxPolicyPercent / 100;
    }
    else
    {
        Information->MaxMhz = Prcb->MHz;
        Information->CurrentMhz = Prcb->MHz;
        Information->MhzLimit = Prcb->MHz;
    }

    if (States)
    {
        Information->MaxIdleState = States->ProcessorIdleCount;
        Information->CurrentIdleState = States->TargetState + 1;
    }
    else
    {
        Information->MaxIdleState = 0;
        Information->CurrentIdleState = 0;
    }
}

/**
 * @brief Snaps the active time of a processor and how much of it ran at nominal speed.
 *
 * @return FALSE when the processor has no driver performance domain.
 */
BOOLEAN
NTAPI
PpmQueryPerformanceDistribution(
    _In_ ULONG Index,
    _Out_ PULONG64 ActiveTime,
    _Out_ PULONG64 FrequencyScaledTime)
{
    PPM_DISTRIBUTION_SNAP Snap;

    PAGED_CODE();

    *ActiveTime = 0;
    *FrequencyScaledTime = 0;
    if (!PpmProcessors[Index].Constraint)
        return FALSE;

    PpmRunOnProcessor(Index, PpmSnapDistribution, &Snap);
    *ActiveTime = Snap.ActiveTime;
    *FrequencyScaledTime = Snap.FrequencyScaledTime;
    return TRUE;
}

/**
 * @brief How far the distribution hit counts are shifted, keeps 14 bits of QPC resolution.
 */
ULONG
NTAPI
PpmQueryDistributionShift(VOID)
{
    ULONG64 Frequency = PpmQpcFrequency;
    ULONG Highest = 0;

    while (Frequency >>= 1)
        Highest++;

    return (Highest > 14) ? Highest - 14 : 0;
}

/* EOF */
