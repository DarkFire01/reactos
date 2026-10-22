/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Multimedia timer resolution and time functions kernel32 hosts since Windows 8
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <k32.h>
#include <mmsystem.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

#define BASEP_TIMER_PERIOD_MIN      1
#define BASEP_TIMER_PERIOD_MAX      1000000
#define BASEP_TIMER_PERIOD_SLOTS    32
#define BASEP_HUNDRED_NS_PER_MS     10000

typedef struct _BASEP_TIMER_PERIOD
{
    UINT Period;
    ULONG Count;
} BASEP_TIMER_PERIOD, *PBASEP_TIMER_PERIOD;

static RTL_SRWLOCK BasepTimerPeriodLock = RTL_SRWLOCK_INIT;
static BASEP_TIMER_PERIOD BasepTimerPeriods[BASEP_TIMER_PERIOD_SLOTS];
static ULONG BasepTimerPeriodCount;
static UINT BasepTimerPeriodApplied;

/* FUNCTIONS ******************************************************************/

/* The process asks for the finest period anyone still holds, or for nothing */
static
VOID
BasepApplyTimerPeriod(VOID)
{
    UINT Finest = 0;
    ULONG Index, Actual;

    for (Index = 0; Index < BasepTimerPeriodCount; Index++)
    {
        if (Finest == 0 || BasepTimerPeriods[Index].Period < Finest)
            Finest = BasepTimerPeriods[Index].Period;
    }

    if (Finest == BasepTimerPeriodApplied)
        return;

    if (Finest == 0)
        NtSetTimerResolution(0, FALSE, &Actual);
    else
        NtSetTimerResolution(min(Finest, MAXULONG / BASEP_HUNDRED_NS_PER_MS) * BASEP_HUNDRED_NS_PER_MS, TRUE, &Actual);

    BasepTimerPeriodApplied = Finest;
}

static
PBASEP_TIMER_PERIOD
BasepFindTimerPeriod(
    _In_ UINT Period)
{
    ULONG Index;

    for (Index = 0; Index < BasepTimerPeriodCount; Index++)
    {
        if (BasepTimerPeriods[Index].Period == Period)
            return &BasepTimerPeriods[Index];
    }

    return NULL;
}

/*
 * @implemented
 */
MMRESULT
WINAPI
timeBeginPeriod(
    _In_ UINT uPeriod)
{
    PBASEP_TIMER_PERIOD Entry;
    MMRESULT Result = TIMERR_NOERROR;

    if (uPeriod < BASEP_TIMER_PERIOD_MIN)
        return TIMERR_NOCANDO;

    RtlAcquireSRWLockExclusive(&BasepTimerPeriodLock);

    Entry = BasepFindTimerPeriod(uPeriod);
    if (Entry == NULL && BasepTimerPeriodCount < BASEP_TIMER_PERIOD_SLOTS)
    {
        Entry = &BasepTimerPeriods[BasepTimerPeriodCount++];
        Entry->Period = uPeriod;
        Entry->Count = 0;
    }

    if (Entry != NULL)
    {
        Entry->Count++;
        BasepApplyTimerPeriod();
    }
    else
    {
        Result = TIMERR_NOCANDO;
    }

    RtlReleaseSRWLockExclusive(&BasepTimerPeriodLock);
    return Result;
}

/*
 * @implemented
 */
MMRESULT
WINAPI
timeEndPeriod(
    _In_ UINT uPeriod)
{
    PBASEP_TIMER_PERIOD Entry;
    MMRESULT Result = TIMERR_NOERROR;

    if (uPeriod < BASEP_TIMER_PERIOD_MIN)
        return TIMERR_NOCANDO;

    RtlAcquireSRWLockExclusive(&BasepTimerPeriodLock);

    Entry = BasepFindTimerPeriod(uPeriod);
    if (Entry != NULL)
    {
        if (--Entry->Count == 0)
            *Entry = BasepTimerPeriods[--BasepTimerPeriodCount];
        BasepApplyTimerPeriod();
    }
    else
    {
        Result = TIMERR_NOCANDO;
    }

    RtlReleaseSRWLockExclusive(&BasepTimerPeriodLock);
    return Result;
}

/*
 * @implemented
 */
MMRESULT
WINAPI
timeGetDevCaps(
    _Out_writes_bytes_(cbtc) LPTIMECAPS ptc,
    _In_ UINT cbtc)
{
    if (ptc == NULL || cbtc < sizeof(*ptc))
        return TIMERR_NOCANDO;

    ptc->wPeriodMin = BASEP_TIMER_PERIOD_MIN;
    ptc->wPeriodMax = BASEP_TIMER_PERIOD_MAX;
    return TIMERR_NOERROR;
}

/*
 * @implemented
 */
DWORD
WINAPI
timeGetTime(VOID)
{
    LARGE_INTEGER InterruptTime;

    /* The shared page is updated without a lock; reread until both high parts agree */
    do
    {
        InterruptTime.HighPart = SharedUserData->InterruptTime.High1Time;
        InterruptTime.LowPart = SharedUserData->InterruptTime.LowPart;
    } while (InterruptTime.HighPart != SharedUserData->InterruptTime.High2Time);

    return (DWORD)(InterruptTime.QuadPart / BASEP_HUNDRED_NS_PER_MS);
}
