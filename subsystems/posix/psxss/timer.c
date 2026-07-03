/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Per-process alarm() timer
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <ndk/kefuncs.h>    // NtQuerySystemTime

static HANDLE g_AlarmTimerQueue = NULL;

/**
 * @brief Timer queue callback for an elapsed alarm. Delivers SIGALRM to the process.
 */
static
VOID
NTAPI
PsxAlarmCallback(
    _In_ PVOID Context,
    _In_ BOOLEAN TimerOrWaitFired)
{
    PPSX_PROCESS Process = (PPSX_PROCESS)Context;

    UNREFERENCED_PARAMETER(TimerOrWaitFired);

    Process->AlarmDeadline.QuadPart = 0;

    /* PsxDeliverSignal expects the process table lock to be held */
    RtlEnterCriticalSection(&g_PsxProcessLock);
    PsxDeliverSignal(Process, PSX_SIGALRM);
    RtlLeaveCriticalSection(&g_PsxProcessLock);
}

/**
 * @brief alarm() (ApiNumber 0x09).
 *
 * Body layout: +0x30 disarm flag (nonzero disarms), +0x38 new value as a relative
 * NT time, +0x40 remaining time of the previous alarm (reply).
 */
VOID
PsxSrvAlarm(
    _Inout_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    BOOLEAN Disarm = (Message->Data.Raw[0] != 0);
    LARGE_INTEGER NewValue;
    LARGE_INTEGER Now;
    LARGE_INTEGER Remaining;
    LONGLONG DueTime;
    ULONG DueMs;

    NewValue.LowPart = Args[2];
    NewValue.HighPart = Args[3];

    NtQuerySystemTime(&Now);

    /* Zero if no alarm is armed or it already fired */
    Remaining.QuadPart = 0;
    if (Process->AlarmDeadline.QuadPart != 0)
    {
        Remaining.QuadPart = Process->AlarmDeadline.QuadPart - Now.QuadPart;
        if (Remaining.QuadPart < 0)
            Remaining.QuadPart = 0;
    }

    if (Process->AlarmTimer != NULL)
    {
        RtlDeleteTimer(g_AlarmTimerQueue, Process->AlarmTimer, NULL);
        Process->AlarmTimer = NULL;
    }
    Process->AlarmDeadline.QuadPart = 0;

    if (!Disarm && (NewValue.QuadPart != 0) && (g_AlarmTimerQueue != NULL))
    {
        /* Relative NT times are negative */
        DueTime = -NewValue.QuadPart;
        DueMs = (ULONG)(DueTime / 10000);
        if (DueMs == 0)
            DueMs = 1;

        Process->AlarmDeadline.QuadPart = Now.QuadPart + DueTime;
        RtlCreateTimer(g_AlarmTimerQueue,
                       &Process->AlarmTimer,
                       PsxAlarmCallback,
                       Process,
                       DueMs,
                       0,
                       0);
    }

    Args[4] = Remaining.LowPart;
    Args[5] = Remaining.HighPart;
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

/**
 * @brief Cancels a pending alarm so the callback cannot run against a dead record.
 */
VOID
PsxCancelAlarm(
    _Inout_ PPSX_PROCESS Process)
{
    if (Process->AlarmTimer != NULL)
    {
        RtlDeleteTimer(g_AlarmTimerQueue, Process->AlarmTimer, NULL);
        Process->AlarmTimer = NULL;
    }
    Process->AlarmDeadline.QuadPart = 0;
}

/**
 * @brief Creates the alarm timer queue and registers the alarm() handler.
 */
VOID
PsxInitTimerOps(VOID)
{
    extern PPSX_API_HANDLER g_OpHandlers[];

    RtlCreateTimerQueue(&g_AlarmTimerQueue);
    g_OpHandlers[PsxApiAlarm] = PsxSrvAlarm;
}
