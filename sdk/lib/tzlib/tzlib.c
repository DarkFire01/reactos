/*
 * PROJECT:     ReactOS TimeZone Utilities Library
 * LICENSE:     GPL-2.0 (https://spdx.org/licenses/GPL-2.0)
 * PURPOSE:     Provides time-zone utility wrappers around Win32 functions,
 *              that are used by different ReactOS modules such as
 *              timedate.cpl, syssetup.dll.
 * COPYRIGHT:   Copyright 2004-2005 Eric Kohl
 *              Copyright 2016 Carlo Bramini
 *              Copyright 2020 Hermes Belusca-Maito
 */

#include <stdlib.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>

#include "tzlib.h"

BOOL
GetTimeZoneListIndex(
    IN OUT PULONG pIndex)
{
    LONG lError;
    HKEY hKey;
    DWORD dwType;
    DWORD dwValueSize;
    DWORD Length;
    LPWSTR Buffer;
    LPWSTR Ptr, End;
    BOOL bFound = FALSE;
    unsigned long iLanguageID;
    WCHAR szLanguageIdString[9];

    if (*pIndex == -1)
    {
        *pIndex = 85; /* fallback to GMT time zone */

        lError = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                               L"SYSTEM\\CurrentControlSet\\Control\\NLS\\Language",
                               0,
                               KEY_QUERY_VALUE,
                               &hKey);
        if (lError != ERROR_SUCCESS)
        {
            return FALSE;
        }

        dwValueSize = sizeof(szLanguageIdString);
        lError = RegQueryValueExW(hKey,
                                  L"Default",
                                  NULL,
                                  NULL,
                                  (LPBYTE)szLanguageIdString,
                                  &dwValueSize);
        if (lError != ERROR_SUCCESS)
        {
            RegCloseKey(hKey);
            return FALSE;
        }

        iLanguageID = wcstoul(szLanguageIdString, NULL, 16);
        RegCloseKey(hKey);
    }
    else
    {
        iLanguageID = *pIndex;
    }

    lError = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                           L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones",
                           0,
                           KEY_QUERY_VALUE,
                           &hKey);
    if (lError != ERROR_SUCCESS)
    {
        return FALSE;
    }

    dwValueSize = 0;
    lError = RegQueryValueExW(hKey,
                              L"IndexMapping",
                              NULL,
                              &dwType,
                              NULL,
                              &dwValueSize);
    if ((lError != ERROR_SUCCESS) || (dwType != REG_MULTI_SZ))
    {
        RegCloseKey(hKey);
        return FALSE;
    }

    Buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwValueSize);
    if (Buffer == NULL)
    {
        RegCloseKey(hKey);
        return FALSE;
    }

    lError = RegQueryValueExW(hKey,
                              L"IndexMapping",
                              NULL,
                              &dwType,
                              (LPBYTE)Buffer,
                              &dwValueSize);

    RegCloseKey(hKey);

    if ((lError != ERROR_SUCCESS) || (dwType != REG_MULTI_SZ))
    {
        HeapFree(GetProcessHeap(), 0, Buffer);
        return FALSE;
    }

    Ptr = Buffer;
    while (*Ptr != 0)
    {
        Length = wcslen(Ptr);
        if (wcstoul(Ptr, NULL, 16) == iLanguageID)
            bFound = TRUE;

        Ptr = Ptr + Length + 1;
        if (*Ptr == 0)
            break;

        if (bFound)
        {
            *pIndex = wcstoul(Ptr, &End, 10);
            HeapFree(GetProcessHeap(), 0, Buffer);
            return TRUE;
        }

        Length = wcslen(Ptr);
        Ptr = Ptr + Length + 1;
    }

    HeapFree(GetProcessHeap(), 0, Buffer);
    return FALSE;
}

LONG
QueryTimeZoneData(
    IN HKEY hZoneKey,
    OUT PULONG Index OPTIONAL,
    OUT PREG_TZI_FORMAT TimeZoneInfo,
    OUT PWCHAR Description OPTIONAL,
    IN OUT PULONG DescriptionSize OPTIONAL,
    OUT PWCHAR StandardName OPTIONAL,
    IN OUT PULONG StandardNameSize OPTIONAL,
    OUT PWCHAR DaylightName OPTIONAL,
    IN OUT PULONG DaylightNameSize OPTIONAL)
{
    LONG lError;
    DWORD dwValueSize;

    if (Index)
    {
        dwValueSize = sizeof(*Index);
        lError = RegQueryValueExW(hZoneKey,
                                  L"Index",
                                  NULL,
                                  NULL,
                                  (LPBYTE)Index,
                                  &dwValueSize);
        if (lError != ERROR_SUCCESS)
            *Index = 0;
    }

    /* The time zone information structure is mandatory for a valid time zone */
    dwValueSize = sizeof(*TimeZoneInfo);
    lError = RegQueryValueExW(hZoneKey,
                              L"TZI",
                              NULL,
                              NULL,
                              (LPBYTE)TimeZoneInfo,
                              &dwValueSize);
    if (lError != ERROR_SUCCESS)
        return lError;

    if (Description && DescriptionSize && *DescriptionSize > 0)
    {
        lError = RegQueryValueExW(hZoneKey,
                                  L"Display",
                                  NULL,
                                  NULL,
                                  (LPBYTE)Description,
                                  DescriptionSize);
        if (lError != ERROR_SUCCESS)
            *Description = 0;
    }

    if (StandardName && StandardNameSize && *StandardNameSize > 0)
    {
        lError = RegQueryValueExW(hZoneKey,
                                  L"Std",
                                  NULL,
                                  NULL,
                                  (LPBYTE)StandardName,
                                  StandardNameSize);
        if (lError != ERROR_SUCCESS)
            *StandardName = 0;
    }

    if (DaylightName && DaylightNameSize && *DaylightNameSize > 0)
    {
        lError = RegQueryValueExW(hZoneKey,
                                  L"Dlt",
                                  NULL,
                                  NULL,
                                  (LPBYTE)DaylightName,
                                  DaylightNameSize);
        if (lError != ERROR_SUCCESS)
            *DaylightName = 0;
    }

    return ERROR_SUCCESS;
}

//
// NOTE: Very similar to the EnumDynamicTimeZoneInformation() function
// introduced in Windows 8.
//
VOID
EnumerateTimeZoneList(
    IN PENUM_TIMEZONE_CALLBACK Callback,
    IN PVOID Context OPTIONAL)
{
    LONG lError;
    HKEY hZonesKey;
    HKEY hZoneKey;
    DWORD dwIndex;
    DWORD dwNameSize;
    WCHAR szKeyName[256];

    /* Open the registry key containing the list of time zones */
    lError = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                           L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones",
                           0,
                           KEY_ENUMERATE_SUB_KEYS,
                           &hZonesKey);
    if (lError != ERROR_SUCCESS)
        return;

    /* Enumerate it */
    for (dwIndex = 0; ; dwIndex++)
    {
        dwNameSize = sizeof(szKeyName);
        lError = RegEnumKeyExW(hZonesKey,
                               dwIndex,
                               szKeyName,
                               &dwNameSize,
                               NULL,
                               NULL,
                               NULL,
                               NULL);
        // if (lError != ERROR_SUCCESS && lError != ERROR_MORE_DATA)
        if (lError == ERROR_NO_MORE_ITEMS)
            break;

        /* Open the time zone sub-key */
        if (RegOpenKeyExW(hZonesKey,
                          szKeyName,
                          0,
                          KEY_QUERY_VALUE,
                          &hZoneKey))
        {
            /* We failed, continue with another sub-key */
            continue;
        }

        /* Call the user-provided callback */
        lError = Callback(hZoneKey, szKeyName, Context);
        // lError = QueryTimeZoneData(hZoneKey, Context);

        RegCloseKey(hZoneKey);
    }

    RegCloseKey(hZonesKey);
}

// Returns TRUE if AutoDaylight is ON.
// Returns FALSE if AutoDaylight is OFF.
BOOL
GetAutoDaylight(VOID)
{
    LONG lError;
    HKEY hKey;
    DWORD dwType;
    DWORD dwDisabled;
    DWORD dwValueSize;

    lError = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                           L"SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation",
                           0,
                           KEY_QUERY_VALUE,
                           &hKey);
    if (lError != ERROR_SUCCESS)
        return FALSE;

    // NOTE: On Vista+: REG_DWORD "DynamicDaylightTimeDisabled"
    dwValueSize = sizeof(dwDisabled);
    lError = RegQueryValueExW(hKey,
                              L"DisableAutoDaylightTimeSet",
                              NULL,
                              &dwType,
                              (LPBYTE)&dwDisabled,
                              &dwValueSize);

    RegCloseKey(hKey);

    if ((lError != ERROR_SUCCESS) || (dwType != REG_DWORD) || (dwValueSize != sizeof(dwDisabled)))
    {
        /*
         * The call failed (non zero) because the registry value isn't available,
         * which means auto-daylight shouldn't be disabled.
         */
        dwDisabled = FALSE;
    }

    return !dwDisabled;
}

VOID
SetAutoDaylight(
    IN BOOL EnableAutoDaylightTime)
{
    LONG lError;
    HKEY hKey;
    DWORD dwDisabled = TRUE;

    lError = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                           L"SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation",
                           0,
                           KEY_SET_VALUE,
                           &hKey);
    if (lError != ERROR_SUCCESS)
        return;

    if (!EnableAutoDaylightTime)
    {
        /* Auto-Daylight disabled: set the value to TRUE */
        // NOTE: On Vista+: REG_DWORD "DynamicDaylightTimeDisabled"
        RegSetValueExW(hKey,
                       L"DisableAutoDaylightTimeSet",
                       0,
                       REG_DWORD,
                       (LPBYTE)&dwDisabled,
                       sizeof(dwDisabled));
    }
    else
    {
        /* Auto-Daylight enabled: just delete the value */
        RegDeleteValueW(hKey, L"DisableAutoDaylightTimeSet");
    }

    RegCloseKey(hKey);
}

/* Copies a name that may be longer than the 32 characters of the time zone structures */
static
VOID
CopyTimeZoneName(
    _Out_writes_(32) PWCHAR Destination,
    _In_ PCWSTR Source)
{
    wcsncpy(Destination, Source, 31);
    Destination[31] = L'\0';
}

/* Enables or restores SeTimeZonePrivilege of the process token */
static
BOOL
AdjustTimeZonePrivilege(
    _In_ HANDLE hToken,
    _In_ PTOKEN_PRIVILEGES NewState,
    _Out_opt_ PTOKEN_PRIVILEGES OldState)
{
    DWORD dwSize;

    return AdjustTokenPrivileges(hToken,
                                 FALSE,
                                 NewState,
                                 OldState ? sizeof(*OldState) : 0,
                                 OldState,
                                 OldState ? &dwSize : NULL) &&
           GetLastError() == ERROR_SUCCESS;
}

/*
 * Makes a zone of the Time Zones list the current one with its key name,
 * the way the Windows control panel does. With AutoDaylight off a zone
 * that has daylight time keeps standard time all year.
 */
BOOL
SetLocalTimeZoneByKey(
    _In_ PCWSTR KeyName,
    _In_ BOOL AutoDaylight)
{
    DYNAMIC_TIME_ZONE_INFORMATION TimeZone;
    TIME_ZONE_INFORMATION YearZone;
    TOKEN_PRIVILEGES Privilege;
    TOKEN_PRIVILEGES OldPrivilege;
    REG_TZI_FORMAT ZoneInfo;
    SYSTEMTIME LocalTime;
    WCHAR szStandardName[64];
    WCHAR szDaylightName[64];
    ULONG StandardNameSize = sizeof(szStandardName);
    ULONG DaylightNameSize = sizeof(szDaylightName);
    HANDLE hToken;
    HKEY hZonesKey;
    HKEY hZoneKey;
    LONG lError;
    BOOL bRet;

    if (wcslen(KeyName) >= ARRAYSIZE(TimeZone.TimeZoneKeyName))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    lError = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                           L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones",
                           0,
                           KEY_QUERY_VALUE,
                           &hZonesKey);
    if (lError == ERROR_SUCCESS)
    {
        lError = RegOpenKeyExW(hZonesKey, KeyName, 0, KEY_QUERY_VALUE, &hZoneKey);
        RegCloseKey(hZonesKey);
    }
    if (lError == ERROR_SUCCESS)
    {
        lError = QueryTimeZoneData(hZoneKey,
                                   NULL,
                                   &ZoneInfo,
                                   NULL,
                                   NULL,
                                   szStandardName,
                                   &StandardNameSize,
                                   szDaylightName,
                                   &DaylightNameSize);
        RegCloseKey(hZoneKey);
    }
    if (lError != ERROR_SUCCESS)
    {
        SetLastError(lError);
        return FALSE;
    }

    ZeroMemory(&TimeZone, sizeof(TimeZone));
    wcscpy(TimeZone.TimeZoneKeyName, KeyName);
    CopyTimeZoneName(TimeZone.StandardName, szStandardName);
    CopyTimeZoneName(TimeZone.DaylightName, szDaylightName);

    /* Take this year's rules when the zone has Dynamic DST data */
    GetLocalTime(&LocalTime);
    if (GetTimeZoneInformationForYear(LocalTime.wYear, &TimeZone, &YearZone))
    {
        ZoneInfo.Bias = YearZone.Bias;
        ZoneInfo.StandardBias = YearZone.StandardBias;
        ZoneInfo.DaylightBias = YearZone.DaylightBias;
        ZoneInfo.StandardDate = YearZone.StandardDate;
        ZoneInfo.DaylightDate = YearZone.DaylightDate;
    }

    TimeZone.Bias = ZoneInfo.Bias;
    TimeZone.StandardBias = ZoneInfo.StandardBias;
    TimeZone.DaylightBias = ZoneInfo.DaylightBias;
    TimeZone.StandardDate = ZoneInfo.StandardDate;
    TimeZone.DaylightDate = ZoneInfo.DaylightDate;

    if (TimeZone.StandardDate.wMonth == 0)
    {
        /* Without daylight time the daylight part repeats the standard one */
        TimeZone.DaylightBias = TimeZone.StandardBias;
        TimeZone.DaylightDate = TimeZone.StandardDate;
    }
    else if (!AutoDaylight)
    {
        TimeZone.StandardBias = 0;
        TimeZone.DaylightBias = 0;
        ZeroMemory(&TimeZone.StandardDate, sizeof(TimeZone.StandardDate));
        ZeroMemory(&TimeZone.DaylightDate, sizeof(TimeZone.DaylightDate));
        CopyMemory(TimeZone.DaylightName, TimeZone.StandardName, sizeof(TimeZone.DaylightName));
        TimeZone.DynamicDaylightTimeDisabled = TRUE;
    }

    /* Changing the time zone takes SeTimeZonePrivilege, which is off by default */
    OldPrivilege.PrivilegeCount = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        hToken = NULL;

    Privilege.PrivilegeCount = 1;
    Privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (hToken && LookupPrivilegeValueW(NULL, L"SeTimeZonePrivilege", &Privilege.Privileges[0].Luid))
        AdjustTimeZonePrivilege(hToken, &Privilege, &OldPrivilege);

    /* Without the privilege the kernel refuses the call */
    bRet = SetDynamicTimeZoneInformation(&TimeZone);
    lError = GetLastError();

    if (hToken)
    {
        if (OldPrivilege.PrivilegeCount > 0)
            AdjustTimeZonePrivilege(hToken, &OldPrivilege, NULL);
        CloseHandle(hToken);
    }

    SetLastError(lError);
    return bRet;
}
