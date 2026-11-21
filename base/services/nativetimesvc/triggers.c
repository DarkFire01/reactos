/*
 * PROJECT:     ReactOS Native Time Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Alarms and timers as wake triggers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "nativetimesvc.h"

#define NDEBUG
#include <debug.h>

#define NT_TICKS_PER_MINUTE     (60 * 10000000LL)

/* A trigger more than this late was missed, by a clock step or while the service was down */
#define NT_MISSED_LIMIT         (5 * NT_TICKS_PER_MINUTE)

/* Alarm times are checked against the clock and zone this often, a manual change shows up then */
#define NT_RECHECK_MS           60000

static CRITICAL_SECTION NtTriggerLock;
static NATIVETIME_TRIGGER NtTriggers[NATIVETIME_MAX_TRIGGERS];
static BOOL NtTriggerUsed[NATIVETIME_MAX_TRIGGERS];
static ULONGLONG NtRingStarted[NATIVETIME_MAX_TRIGGERS];
static DWORD NtNextId = 1;
static HANDLE NtTimer;
static HANDLE NtReschedule;
static HANDLE NtSchedulerThread;

static
LONGLONG
NtToValue(
    _In_ const FILETIME *Time)
{
    ULARGE_INTEGER Value;

    Value.LowPart = Time->dwLowDateTime;
    Value.HighPart = Time->dwHighDateTime;
    return (LONGLONG)Value.QuadPart;
}

static
FILETIME
NtFromValue(
    _In_ LONGLONG Value)
{
    FILETIME Time;

    Time.dwLowDateTime = (DWORD)Value;
    Time.dwHighDateTime = (DWORD)(Value >> 32);
    return Time;
}

static
LONGLONG
NtNow(VOID)
{
    FILETIME Now;

    GetSystemTimeAsFileTime(&Now);
    return NtToValue(&Now);
}

/* The next local Hour:Minute after now on one of the alarm days, as UTC */
static
LONGLONG
NtNextAlarm(
    _In_ const NATIVETIME_TRIGGER *Alarm,
    _In_ LONGLONG Now)
{
    SYSTEMTIME Local, Utc;
    FILETIME BaseFile, DayFile, UtcFile, NowFile;
    ULONG Step;

    NowFile = NtFromValue(Now);
    if (!FileTimeToSystemTime(&NowFile, &Utc) || !SystemTimeToTzSpecificLocalTime(NULL, &Utc, &Local))
        return 0;

    Local.wHour = Alarm->Hour;
    Local.wMinute = Alarm->Minute;
    Local.wSecond = 0;
    Local.wMilliseconds = 0;
    if (!SystemTimeToFileTime(&Local, &BaseFile))
        return 0;

    /* A week covers every day mask, one more day covers a time already passed today */
    for (Step = 0; Step <= 7; Step++)
    {
        DayFile = NtFromValue(NtToValue(&BaseFile) + Step * 24 * 60 * NT_TICKS_PER_MINUTE);
        if (!FileTimeToSystemTime(&DayFile, &Local))
            return 0;

        if (Alarm->Days && !(Alarm->Days & (1 << Local.wDayOfWeek)))
            continue;

        if (!TzSpecificLocalTimeToSystemTime(NULL, &Local, &Utc) || !SystemTimeToFileTime(&Utc, &UtcFile))
            return 0;

        if (NtToValue(&UtcFile) > Now)
            return NtToValue(&UtcFile);
    }

    return 0;
}

/* Fills Due with when the trigger rings next, zero when it never will. Caller holds the lock. */
static
VOID
NtComputeDue(
    _Inout_ NATIVETIME_TRIGGER *Trigger,
    _In_ LONGLONG Now)
{
    if (!(Trigger->Flags & NATIVETIME_FLAG_ENABLED))
    {
        if (Trigger->Kind == NativeTimeAlarm)
            Trigger->Due = NtFromValue(0);
        return;
    }

    if (NtToValue(&Trigger->SnoozeUntil))
    {
        Trigger->Due = Trigger->SnoozeUntil;
        return;
    }

    if (Trigger->Kind == NativeTimeAlarm)
        Trigger->Due = NtFromValue(NtNextAlarm(Trigger, Now));
}

static
VOID
NtSave(
    _In_ const NATIVETIME_TRIGGER *Trigger)
{
    WCHAR Name[16];
    HKEY Key;

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, NT_TRIGGERS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &Key, NULL) != ERROR_SUCCESS)
        return;

    StringCchPrintfW(Name, _countof(Name), L"%lu", Trigger->Id);
    RegSetValueExW(Key, Name, 0, REG_BINARY, (const BYTE *)Trigger, sizeof(*Trigger));
    RegCloseKey(Key);
}

static
VOID
NtForget(
    _In_ DWORD Id)
{
    WCHAR Name[16];
    HKEY Key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, NT_TRIGGERS_KEY, 0, KEY_SET_VALUE, &Key) != ERROR_SUCCESS)
        return;

    StringCchPrintfW(Name, _countof(Name), L"%lu", Id);
    RegDeleteValueW(Key, Name);
    RegCloseKey(Key);
}

static
VOID
NtLoad(VOID)
{
    NATIVETIME_TRIGGER Trigger;
    WCHAR Name[16];
    DWORD Index, NameLength, Size, Type, Slot = 0;
    LONGLONG Now = NtNow();
    HKEY Key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, NT_TRIGGERS_KEY, 0, KEY_READ, &Key) != ERROR_SUCCESS)
        return;

    for (Index = 0; Slot < NATIVETIME_MAX_TRIGGERS; Index++)
    {
        NameLength = _countof(Name);
        Size = sizeof(Trigger);
        if (RegEnumValueW(Key, Index, Name, &NameLength, NULL, &Type, (LPBYTE)&Trigger, &Size) != ERROR_SUCCESS)
            break;

        if (Type != REG_BINARY || Size != sizeof(Trigger) || !Trigger.Id)
            continue;

        Trigger.Flags &= ~NATIVETIME_FLAG_RINGING;
        Trigger.Label[NATIVETIME_MAX_LABEL - 1] = UNICODE_NULL;
        NtComputeDue(&Trigger, Now);

        NtTriggers[Slot] = Trigger;
        NtTriggerUsed[Slot] = TRUE;
        Slot++;

        if (Trigger.Id >= NtNextId)
            NtNextId = Trigger.Id + 1;
    }

    RegCloseKey(Key);
    DPRINT1("Loaded %lu trigger(s)\n", Slot);
}

static
LONG
NtFindSlot(
    _In_ DWORD Id)
{
    LONG i;

    for (i = 0; i < NATIVETIME_MAX_TRIGGERS; i++)
    {
        if (NtTriggerUsed[i] && NtTriggers[i].Id == Id)
            return i;
    }

    return -1;
}

/* Starts the clock app to ring when no client listens */
static
VOID
NtLaunchClock(VOID)
{
    WCHAR Path[MAX_PATH], CommandLine[MAX_PATH + 16];
    STARTUPINFOW Startup;
    PROCESS_INFORMATION Process;

    if (!GetSystemDirectoryW(Path, _countof(Path)) ||
        FAILED(StringCchCatW(Path, _countof(Path), L"\\" NT_CLOCK_APP)) ||
        FAILED(StringCchPrintfW(CommandLine, _countof(CommandLine), L"\"%s\" %s", Path, NATIVETIME_RING_SWITCH)))
    {
        return;
    }

    ZeroMemory(&Startup, sizeof(Startup));
    Startup.cb = sizeof(Startup);
    Startup.lpDesktop = L"WinSta0\\Default";

    if (!CreateProcessW(Path, CommandLine, NULL, NULL, FALSE, 0, NULL, NULL, &Startup, &Process))
    {
        DPRINT1("Starting the clock app failed %lu\n", GetLastError());
        return;
    }

    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
}

/* Ends the ringing of one slot. Caller holds the lock. */
static
VOID
NtStopRinging(
    _In_ LONG Slot,
    _In_ BOOL Snoozed,
    _In_ DWORD SnoozeMinutes)
{
    NATIVETIME_TRIGGER *Trigger = &NtTriggers[Slot];
    LONGLONG Now = NtNow();

    Trigger->Flags &= ~NATIVETIME_FLAG_RINGING;
    NtRingStarted[Slot] = 0;

    if (Snoozed)
    {
        Trigger->SnoozeUntil = NtFromValue(Now + SnoozeMinutes * NT_TICKS_PER_MINUTE);
    }
    else
    {
        Trigger->SnoozeUntil = NtFromValue(0);

        /* A one time alarm is done once dismissed, a timer goes away */
        if (Trigger->Kind == NativeTimeAlarm && !Trigger->Days)
            Trigger->Flags &= ~NATIVETIME_FLAG_ENABLED;
        else if (Trigger->Kind == NativeTimeTimer)
            Trigger->Flags &= ~NATIVETIME_FLAG_ENABLED;
    }

    NtComputeDue(Trigger, Now);
}

static
DWORD
WINAPI
NtSchedulerProc(
    _In_ LPVOID Parameter)
{
    NATIVETIME_TRIGGER Ring[NATIVETIME_MAX_TRIGGERS], Stopped[NATIVETIME_MAX_TRIGGERS];
    HANDLE Waits[3];
    LARGE_INTEGER DueTime;
    LONGLONG Now, Due, Earliest;
    ULONGLONG Tick;
    DWORD RingCount, StoppedCount, WaitMs, Left, Result, i;
    BOOL Changed;
    LONG Slot;

    UNREFERENCED_PARAMETER(Parameter);

    Waits[0] = NtStopEvent;
    Waits[1] = NtTimer;
    Waits[2] = NtReschedule;

    for (;;)
    {
        RingCount = 0;
        StoppedCount = 0;
        Earliest = 0;
        WaitMs = INFINITE;
        Changed = FALSE;
        Now = NtNow();
        Tick = GetTickCount64();

        EnterCriticalSection(&NtTriggerLock);
        for (Slot = 0; Slot < NATIVETIME_MAX_TRIGGERS; Slot++)
        {
            NATIVETIME_TRIGGER *Trigger = &NtTriggers[Slot];

            if (!NtTriggerUsed[Slot] || !(Trigger->Flags & NATIVETIME_FLAG_ENABLED))
                continue;

            if (Trigger->Flags & NATIVETIME_FLAG_RINGING)
            {
                Left = (DWORD)min(Tick - NtRingStarted[Slot], (ULONGLONG)NT_RING_TIMEOUT_MS);
                if (Left >= NT_RING_TIMEOUT_MS)
                {
                    DPRINT1("Trigger %lu rang unanswered, stopping it\n", Trigger->Id);
                    NtStopRinging(Slot, FALSE, 0);
                    NtSave(Trigger);
                    Stopped[StoppedCount++] = *Trigger;
                    Changed = TRUE;
                }
                else
                {
                    WaitMs = min(WaitMs, NT_RING_TIMEOUT_MS - Left);
                }
                continue;
            }

            Due = NtToValue(&Trigger->Due);
            if (!Due)
                continue;

            if (Due <= Now)
            {
                if (Now - Due > NT_MISSED_LIMIT)
                {
                    /* Late by a clock step or downtime, ring next time instead of now */
                    DPRINT1("Trigger %lu was missed, scheduling its next ring\n", Trigger->Id);
                    Trigger->SnoozeUntil = NtFromValue(0);
                    if (Trigger->Kind == NativeTimeTimer)
                        Trigger->Flags &= ~NATIVETIME_FLAG_ENABLED;
                    NtComputeDue(Trigger, Now);
                    NtSave(Trigger);
                    Changed = TRUE;
                    Due = NtToValue(&Trigger->Due);
                    if (Due && (!Earliest || Due < Earliest))
                        Earliest = Due;
                    continue;
                }

                Trigger->Flags |= NATIVETIME_FLAG_RINGING;
                Trigger->SnoozeUntil = NtFromValue(0);
                NtRingStarted[Slot] = Tick;
                Ring[RingCount++] = *Trigger;
                WaitMs = min(WaitMs, NT_RING_TIMEOUT_MS);
                continue;
            }

            if (!Earliest || Due < Earliest)
                Earliest = Due;
        }
        LeaveCriticalSection(&NtTriggerLock);

        for (i = 0; i < StoppedCount; i++)
            NtPipeBroadcast(NativeTimeEventStopped, &Stopped[i]);

        if (RingCount)
        {
            NtWakeDevice("an alarm");
            for (i = 0; i < RingCount; i++)
            {
                DPRINT1("Trigger %lu rings: %S\n", Ring[i].Id, Ring[i].Label);
                NtPipeBroadcast(NativeTimeEventRing, &Ring[i]);
            }

            if (!NtPipeHasSubscribers())
                NtLaunchClock();
        }

        if (Changed || RingCount)
            NtPipeBroadcast(NativeTimeEventChanged, NULL);

        /* Absolute due times keep their place when the clock is set */
        if (Earliest)
        {
            DueTime.QuadPart = Earliest;
            SetWaitableTimer(NtTimer, &DueTime, 0, NULL, NULL, TRUE);
        }
        else
        {
            CancelWaitableTimer(NtTimer);
        }

        Result = WaitForMultipleObjects(_countof(Waits), Waits, FALSE, min(WaitMs, NT_RECHECK_MS));
        if (Result == WAIT_OBJECT_0)
            break;

        /* Only alarms still ahead are moved, one due right now must ring first */
        if (Result == WAIT_TIMEOUT)
        {
            Now = NtNow();
            EnterCriticalSection(&NtTriggerLock);
            for (Slot = 0; Slot < NATIVETIME_MAX_TRIGGERS; Slot++)
            {
                if (NtTriggerUsed[Slot] && NtTriggers[Slot].Kind == NativeTimeAlarm &&
                    !(NtTriggers[Slot].Flags & NATIVETIME_FLAG_RINGING) &&
                    NtToValue(&NtTriggers[Slot].Due) > Now)
                {
                    NtComputeDue(&NtTriggers[Slot], Now);
                }
            }
            LeaveCriticalSection(&NtTriggerLock);
        }
    }

    CancelWaitableTimer(NtTimer);
    return 0;
}

BOOL
NtTriggersStart(VOID)
{
    InitializeCriticalSection(&NtTriggerLock);

    NtTimer = CreateWaitableTimerW(NULL, FALSE, NULL);
    NtReschedule = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!NtTimer || !NtReschedule)
        return FALSE;

    NtLoad();

    NtSchedulerThread = CreateThread(NULL, 0, NtSchedulerProc, NULL, 0, NULL);
    return NtSchedulerThread != NULL;
}

VOID
NtTriggersStop(VOID)
{
    if (NtSchedulerThread)
    {
        WaitForSingleObject(NtSchedulerThread, INFINITE);
        CloseHandle(NtSchedulerThread);
        NtSchedulerThread = NULL;
    }
}

/**
 * @brief
 * Recomputes when every alarm rings after the clock or the zone changed.
 */
VOID
NtTriggersTimeChanged(VOID)
{
    LONGLONG Now = NtNow();
    LONG Slot;

    EnterCriticalSection(&NtTriggerLock);
    for (Slot = 0; Slot < NATIVETIME_MAX_TRIGGERS; Slot++)
    {
        if (NtTriggerUsed[Slot] && !(NtTriggers[Slot].Flags & NATIVETIME_FLAG_RINGING))
            NtComputeDue(&NtTriggers[Slot], Now);
    }
    LeaveCriticalSection(&NtTriggerLock);

    SetEvent(NtReschedule);
}

DWORD
NtTriggersList(
    _Out_writes_(NATIVETIME_MAX_TRIGGERS) NATIVETIME_TRIGGER *Triggers)
{
    DWORD Count = 0;
    LONG Slot;

    EnterCriticalSection(&NtTriggerLock);
    for (Slot = 0; Slot < NATIVETIME_MAX_TRIGGERS; Slot++)
    {
        if (NtTriggerUsed[Slot])
            Triggers[Count++] = NtTriggers[Slot];
    }
    LeaveCriticalSection(&NtTriggerLock);

    return Count;
}

DWORD
NtTriggersSet(
    _In_ const NATIVETIME_TRIGGER *Trigger,
    _Out_ PDWORD Id)
{
    NATIVETIME_TRIGGER Copy = *Trigger;
    LONG Slot;

    *Id = 0;
    if ((Copy.Kind != NativeTimeAlarm && Copy.Kind != NativeTimeTimer) ||
        (Copy.Kind == NativeTimeAlarm && (Copy.Hour > 23 || Copy.Minute > 59)) ||
        (Copy.Kind == NativeTimeTimer && !NtToValue(&Copy.Due)) ||
        (Copy.Days & ~NATIVETIME_DAY_ALL))
    {
        return ERROR_INVALID_PARAMETER;
    }

    Copy.Label[NATIVETIME_MAX_LABEL - 1] = UNICODE_NULL;
    Copy.Flags &= NATIVETIME_FLAG_ENABLED;
    Copy.SnoozeUntil = NtFromValue(0);

    EnterCriticalSection(&NtTriggerLock);
    if (Copy.Id)
    {
        Slot = NtFindSlot(Copy.Id);
        if (Slot < 0)
        {
            LeaveCriticalSection(&NtTriggerLock);
            return ERROR_NOT_FOUND;
        }
    }
    else
    {
        for (Slot = 0; Slot < NATIVETIME_MAX_TRIGGERS && NtTriggerUsed[Slot]; Slot++);
        if (Slot == NATIVETIME_MAX_TRIGGERS)
        {
            LeaveCriticalSection(&NtTriggerLock);
            return ERROR_TOO_MANY_NAMES;
        }
        Copy.Id = NtNextId++;
    }

    NtComputeDue(&Copy, NtNow());
    NtTriggers[Slot] = Copy;
    NtTriggerUsed[Slot] = TRUE;
    NtRingStarted[Slot] = 0;
    NtSave(&Copy);
    *Id = Copy.Id;
    LeaveCriticalSection(&NtTriggerLock);

    DPRINT1("Trigger %lu set, kind %lu at %02u:%02u days 0x%02lx\n", Copy.Id, Copy.Kind, Copy.Hour, Copy.Minute, Copy.Days);
    SetEvent(NtReschedule);
    NtPipeBroadcast(NativeTimeEventChanged, NULL);
    return ERROR_SUCCESS;
}

DWORD
NtTriggersDelete(
    _In_ DWORD Id)
{
    NATIVETIME_TRIGGER Removed;
    BOOL WasRinging;
    LONG Slot;

    EnterCriticalSection(&NtTriggerLock);
    Slot = NtFindSlot(Id);
    if (Slot < 0)
    {
        LeaveCriticalSection(&NtTriggerLock);
        return ERROR_NOT_FOUND;
    }

    Removed = NtTriggers[Slot];
    WasRinging = (Removed.Flags & NATIVETIME_FLAG_RINGING) != 0;
    NtTriggerUsed[Slot] = FALSE;
    NtRingStarted[Slot] = 0;
    NtForget(Id);
    LeaveCriticalSection(&NtTriggerLock);

    if (WasRinging)
        NtPipeBroadcast(NativeTimeEventStopped, &Removed);

    SetEvent(NtReschedule);
    NtPipeBroadcast(NativeTimeEventChanged, NULL);
    return ERROR_SUCCESS;
}

static
DWORD
NtTriggersAnswer(
    _In_ DWORD Id,
    _In_ BOOL Snooze,
    _In_ DWORD Minutes)
{
    NATIVETIME_TRIGGER Answered;
    LONG Slot;

    EnterCriticalSection(&NtTriggerLock);
    Slot = NtFindSlot(Id);
    if (Slot < 0)
    {
        LeaveCriticalSection(&NtTriggerLock);
        return ERROR_NOT_FOUND;
    }

    NtStopRinging(Slot, Snooze, Minutes);
    NtSave(&NtTriggers[Slot]);
    Answered = NtTriggers[Slot];
    LeaveCriticalSection(&NtTriggerLock);

    DPRINT1("Trigger %lu %s\n", Id, Snooze ? "snoozed" : "dismissed");
    NtPipeBroadcast(NativeTimeEventStopped, &Answered);
    SetEvent(NtReschedule);
    NtPipeBroadcast(NativeTimeEventChanged, NULL);
    return ERROR_SUCCESS;
}

DWORD
NtTriggersSnooze(
    _In_ DWORD Id,
    _In_ DWORD Minutes)
{
    if (Minutes == 0 || Minutes > 24 * 60)
        return ERROR_INVALID_PARAMETER;

    return NtTriggersAnswer(Id, TRUE, Minutes);
}

DWORD
NtTriggersDismiss(
    _In_ DWORD Id)
{
    return NtTriggersAnswer(Id, FALSE, 0);
}
