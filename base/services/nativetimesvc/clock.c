/*
 * PROJECT:     ReactOS Native Time Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     System time and time zone from the cellular network
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "nativetimesvc.h"

#define NDEBUG
#include <debug.h>

#define NT_ZONES_KEY        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones"

/* The clock is only stepped when it is further off than this */
#define NT_STEP_THRESHOLD   (2 * 10000000LL)

/* The registry form of a time zone rule */
typedef struct _NT_ZONE_RULE
{
    LONG Bias;
    LONG StandardBias;
    LONG DaylightBias;
    SYSTEMTIME StandardDate;
    SYSTEMTIME DaylightDate;
} NT_ZONE_RULE;

static HRIL NtRil;
static CRITICAL_SECTION NtClockLock;
static NATIVETIME_STATUS NtClockStatus;

static const DWORD NtNotificationCodes[] =
{
    RIL_NOTIFY_NITZ
};

static
LONGLONG
NtFileTimeValue(
    _In_ const FILETIME *Time)
{
    ULARGE_INTEGER Value;

    Value.LowPart = Time->dwLowDateTime;
    Value.HighPart = Time->dwHighDateTime;
    return (LONGLONG)Value.QuadPart;
}

/**
 * @brief
 * Scores one registry zone against the offsets the network reported.
 *
 * @return
 * Zero when the zone does not fit, higher is a better fit.
 */
static
ULONG
NtScoreZone(
    _In_z_ LPCWSTR KeyName,
    _In_ const NT_ZONE_RULE *Rule,
    _In_ LONG StandardOffset,
    _In_ BOOL InDaylight,
    _In_z_ LPCWSTR CurrentKey)
{
    BOOL HasDaylight = Rule->DaylightDate.wMonth != 0;
    ULONG Score = 1;

    if (Rule->Bias != -StandardOffset)
        return 0;

    /* Daylight saving now means the zone must have a rule for it */
    if (InDaylight && !HasDaylight)
        return 0;

    /* Keep what the user or an earlier sync picked */
    if (!_wcsicmp(KeyName, CurrentKey))
        Score += 100;

    /* Most places that share an offset follow daylight saving, the plain names are the main zones */
    if (HasDaylight)
        Score += 10;
    if (!wcschr(KeyName, L'(') && wcsncmp(KeyName, L"UTC", 3) != 0)
        Score += 5;

    return Score;
}

/**
 * @brief
 * Picks the registry time zone that matches the network offsets and makes it current.
 *
 * @param[in] TotalOffset
 * Local time minus UTC in minutes, daylight saving included.
 *
 * @param[in] DaylightOffset
 * The daylight saving part of it in minutes.
 */
static
VOID
NtApplyZone(
    _In_ LONG TotalOffset,
    _In_ LONG DaylightOffset)
{
    DYNAMIC_TIME_ZONE_INFORMATION Current, Zone;
    WCHAR KeyName[NATIVETIME_MAX_ZONE], Best[NATIVETIME_MAX_ZONE];
    NT_ZONE_RULE Rule, BestRule;
    ULONG Score, BestScore = 0;
    DWORD Index, Size, NameLength;
    HKEY Zones, Key;
    LONG StandardOffset = TotalOffset - DaylightOffset;

    ZeroMemory(&Current, sizeof(Current));
    GetDynamicTimeZoneInformation(&Current);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, NT_ZONES_KEY, 0, KEY_READ, &Zones) != ERROR_SUCCESS)
    {
        DPRINT1("No time zone list in the registry\n");
        return;
    }

    for (Index = 0; ; Index++)
    {
        NameLength = _countof(KeyName);
        if (RegEnumKeyExW(Zones, Index, KeyName, &NameLength, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;

        if (RegOpenKeyExW(Zones, KeyName, 0, KEY_READ, &Key) != ERROR_SUCCESS)
            continue;

        Size = sizeof(Rule);
        if (RegQueryValueExW(Key, L"TZI", NULL, NULL, (LPBYTE)&Rule, &Size) == ERROR_SUCCESS &&
            Size == sizeof(Rule))
        {
            Score = NtScoreZone(KeyName, &Rule, StandardOffset, DaylightOffset != 0, Current.TimeZoneKeyName);
            if (Score > BestScore)
            {
                BestScore = Score;
                BestRule = Rule;
                StringCchCopyW(Best, _countof(Best), KeyName);
            }
        }
        RegCloseKey(Key);
    }

    if (!BestScore)
    {
        RegCloseKey(Zones);
        DPRINT1("No time zone is %ld minutes from UTC\n", StandardOffset);
        return;
    }

    if (!_wcsicmp(Best, Current.TimeZoneKeyName))
    {
        RegCloseKey(Zones);
        StringCchCopyW(NtClockStatus.Zone, _countof(NtClockStatus.Zone), Best);
        return;
    }

    ZeroMemory(&Zone, sizeof(Zone));
    Zone.Bias = BestRule.Bias;
    Zone.StandardBias = BestRule.StandardBias;
    Zone.DaylightBias = BestRule.DaylightBias;
    Zone.StandardDate = BestRule.StandardDate;
    Zone.DaylightDate = BestRule.DaylightDate;
    StringCchCopyW(Zone.TimeZoneKeyName, _countof(Zone.TimeZoneKeyName), Best);

    if (RegOpenKeyExW(Zones, Best, 0, KEY_READ, &Key) == ERROR_SUCCESS)
    {
        Size = sizeof(Zone.StandardName);
        RegQueryValueExW(Key, L"Std", NULL, NULL, (LPBYTE)Zone.StandardName, &Size);
        Size = sizeof(Zone.DaylightName);
        RegQueryValueExW(Key, L"Dlt", NULL, NULL, (LPBYTE)Zone.DaylightName, &Size);
        RegCloseKey(Key);
    }
    RegCloseKey(Zones);

    NtEnablePrivilege(SE_TIME_ZONE_NAME);
    if (!SetDynamicTimeZoneInformation(&Zone))
    {
        DPRINT1("Setting the time zone %S failed %lu\n", Best, GetLastError());
        return;
    }

    StringCchCopyW(NtClockStatus.Zone, _countof(NtClockStatus.Zone), Best);
    DPRINT1("Time zone set to %S from the network\n", Best);
}

/* Applies one network time report, called on a RIL worker */
static
VOID
NtApplyNitz(
    _In_ const RIL_NITZ_INFO *Nitz)
{
    FILETIME Network, Now;
    LONGLONG Drift;
    BOOL Changed = FALSE;

    EnterCriticalSection(&NtClockLock);

    if ((Nitz->Params & RIL_NITZPARAM_SYSTEMTIME) && SystemTimeToFileTime(&Nitz->SysTime, &Network))
    {
        GetSystemTimeAsFileTime(&Now);
        Drift = NtFileTimeValue(&Network) - NtFileTimeValue(&Now);
        if (Drift > NT_STEP_THRESHOLD || Drift < -NT_STEP_THRESHOLD)
        {
            NtEnablePrivilege(SE_SYSTEMTIME_NAME);
            if (SetSystemTime(&Nitz->SysTime))
            {
                DPRINT1("Clock stepped by %I64d s to network time %04u-%02u-%02u %02u:%02u:%02u UTC\n",
                        Drift / 10000000, Nitz->SysTime.wYear, Nitz->SysTime.wMonth, Nitz->SysTime.wDay,
                        Nitz->SysTime.wHour, Nitz->SysTime.wMinute, Nitz->SysTime.wSecond);
                Changed = TRUE;
            }
            else
            {
                DPRINT1("Setting the network time failed %lu\n", GetLastError());
            }
        }

        NtClockStatus.Source = NativeTimeSourceNetwork;
        NtClockStatus.LastSync = Network;
    }

    if (Nitz->Params & RIL_NITZPARAM_TIMEZONEOFFSET)
    {
        NtClockStatus.OffsetMinutes = Nitz->TimeZoneOffsetMinutes;
        NtApplyZone(Nitz->TimeZoneOffsetMinutes,
                    (Nitz->Params & RIL_NITZPARAM_DAYLIGHTSAVINGOFFSET) ? Nitz->DaylightSavingOffsetMinutes : 0);
        Changed = TRUE;
    }

    LeaveCriticalSection(&NtClockLock);

    if (Changed)
    {
        NtTriggersTimeChanged();
        NtPipeBroadcast(NativeTimeEventTimeSet, NULL);
    }
}

static
VOID
CALLBACK
NtRilResult(
    _In_ HRIL hRil,
    _In_ DWORD ResultType,
    _In_ ULONGLONG Context,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize,
    _In_opt_ PVOID Param)
{
    UNREFERENCED_PARAMETER(hRil);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Data);
    UNREFERENCED_PARAMETER(DataSize);
    UNREFERENCED_PARAMETER(Param);

    if (ResultType != RIL_RESULT_OK)
        DPRINT1("RIL request failed, result type %lu\n", ResultType);
}

static
VOID
CALLBACK
NtRilNotify(
    _In_ HRIL hRil,
    _In_ DWORD Code,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize,
    _In_opt_ PVOID Param)
{
    UNREFERENCED_PARAMETER(Param);

    switch (Code)
    {
        case RIL_NOTIFY_RADIOPRESENCE:
            /* Enabling waits until the broker is there, it then replays the last network time */
            if (Data && DataSize >= sizeof(DWORD) && *(const DWORD *)Data == RIL_RADIO_PRESENT)
                RIL_EnableNotifications(hRil, 0, NtNotificationCodes, _countof(NtNotificationCodes));
            break;

        case RIL_NOTIFY_NITZ:
            if (Data && DataSize >= sizeof(RIL_NITZ_INFO))
                NtApplyNitz(Data);
            break;
    }
}

BOOL
NtClockStart(VOID)
{
    HRESULT hr;

    InitializeCriticalSection(&NtClockLock);

    /* Until the network reports, the time is what the platform clock gave at boot */
    NtClockStatus.Source = NativeTimeSourceClock;
    GetSystemTimeAsFileTime(&NtClockStatus.LastSync);

    hr = RIL_Initialize(0,
                        NtRilResult,
                        NtRilNotify,
                        NtNotificationCodes,
                        _countof(NtNotificationCodes),
                        NULL,
                        NT_SERVICE_NAME,
                        &NtRil);
    if (FAILED(hr))
    {
        DPRINT1("RIL_Initialize failed 0x%lx\n", hr);
        NtRil = NULL;
        return FALSE;
    }

    return TRUE;
}

VOID
NtClockStop(VOID)
{
    if (NtRil)
    {
        RIL_Deinitialize(NtRil);
        NtRil = NULL;
    }
}

VOID
NtClockGetStatus(
    _Out_ NATIVETIME_STATUS *Status)
{
    DYNAMIC_TIME_ZONE_INFORMATION Zone;

    EnterCriticalSection(&NtClockLock);
    *Status = NtClockStatus;
    LeaveCriticalSection(&NtClockLock);

    if (!Status->Zone[0] && GetDynamicTimeZoneInformation(&Zone) != TIME_ZONE_ID_INVALID)
        StringCchCopyW(Status->Zone, _countof(Status->Zone), Zone.TimeZoneKeyName);
}
