/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     POSIX per-process alarm() timer (ApiNumber 0x09). A one-shot NT
 *              timer per process fires SIGALRM through the signal machinery.
 *              Models the NT 4.0 timer thread + alarm list (sub_1F4CFDE).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <ndk/kefuncs.h>    // NtQuerySystemTime

static HANDLE g_AlarmTimerQueue = NULL;

//
// Fires on a timer-queue worker thread when a process's alarm elapses: deliver
// SIGALRM to the process (default action terminates it, or its handler runs).
//
static VOID NTAPI
PsxAlarmCallback(IN PVOID Context, IN BOOLEAN TimerOrWaitFired)
{
    PPSX_PROCESS Process = (PPSX_PROCESS)Context;

    UNREFERENCED_PARAMETER(TimerOrWaitFired);

    Process->AlarmDeadline.QuadPart = 0;

    // PsxDeliverSignal runs under the process-table lock (its documented contract).
    RtlEnterCriticalSection(&g_PsxProcessLock);
    PsxDeliverSignal(Process, PSX_SIGALRM);
    RtlLeaveCriticalSection(&g_PsxProcessLock);
}

//
// alarm(value) -- ApiNumber 0x09. The flag byte at Data[0] (nonzero => disarm);
// the new value is a *relative* NT time (negative 100ns) at body +0x38; the
// previous timer's remaining time is returned as a LARGE_INTEGER at body +0x40
// (the client converts it to seconds). Faithful to sub_1F4CFDE.
//
VOID
PsxSrvAlarm(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    BOOLEAN Disarm = (Message->Data.Raw[0] != 0);       // +0x30 (flag)
    LARGE_INTEGER NewValue;                             // +0x38 (relative NT time)
    LARGE_INTEGER Now, Remaining;

    NewValue.LowPart  = ((PULONG)Message->Data.Raw)[2]; // +0x38
    NewValue.HighPart = ((PULONG)Message->Data.Raw)[3]; // +0x3C

    NtQuerySystemTime(&Now);

    // Remaining time on the current timer (0 if none / already fired).
    Remaining.QuadPart = 0;
    if (Process->AlarmDeadline.QuadPart != 0)
    {
        Remaining.QuadPart = Process->AlarmDeadline.QuadPart - Now.QuadPart;
        if (Remaining.QuadPart < 0)
            Remaining.QuadPart = 0;
    }

    // Cancel any existing alarm.
    if (Process->AlarmTimer != NULL)
    {
        RtlDeleteTimer(g_AlarmTimerQueue, Process->AlarmTimer, NULL);
        Process->AlarmTimer = NULL;
    }
    Process->AlarmDeadline.QuadPart = 0;

    // Arm a new one-shot unless disarming or the requested value is zero.
    if (!Disarm && (NewValue.QuadPart != 0) && (g_AlarmTimerQueue != NULL))
    {
        LONGLONG DueTime = -NewValue.QuadPart;          // relative -> positive 100ns
        ULONG DueMs = (ULONG)(DueTime / 10000);
        if (DueMs == 0)
            DueMs = 1;
        Process->AlarmDeadline.QuadPart = Now.QuadPart + DueTime;
        RtlCreateTimer(g_AlarmTimerQueue, &Process->AlarmTimer, PsxAlarmCallback,
                       Process, DueMs, 0, 0);
    }

    ((PULONG)Message->Data.Raw)[4] = Remaining.LowPart;   // +0x40
    ((PULONG)Message->Data.Raw)[5] = Remaining.HighPart;  // +0x44
    Message->Errno = 0;
    Message->ReturnValue = 0;
}

//
// Cancel a process's pending alarm (called when the process dies) so the timer
// callback cannot fire against a torn-down record.
//
VOID
PsxCancelAlarm(IN PPSX_PROCESS Process)
{
    if (Process->AlarmTimer != NULL)
    {
        RtlDeleteTimer(g_AlarmTimerQueue, Process->AlarmTimer, NULL);
        Process->AlarmTimer = NULL;
    }
    Process->AlarmDeadline.QuadPart = 0;
}

//
// Create the timer queue and register the alarm handler.
//
VOID
PsxInitTimerOps(VOID)
{
    extern PPSX_API_HANDLER g_OpHandlers[];

    RtlCreateTimerQueue(&g_AlarmTimerQueue);
    g_OpHandlers[PsxApiAlarm] = PsxSrvAlarm;    // 0x09
}
