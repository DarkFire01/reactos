/*
 * PROJECT:     ReactOS Power Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hands the active power scheme's setting values to the kernel
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "powersvc.h"

#define NDEBUG
#include <debug.h>

/* The scheme used when none was ever made active */
#define PW_BALANCED_SCHEME          L"381b4222-f694-41f0-9685-ff5bb260df2e"

/* Settings filed directly under PowerSettings keep their user values under this subgroup */
#define PW_NO_SUBGROUP              L"fea3413e-7e05-4911-9a71-700331f1c294"

#define PW_SETTINGS_KEY             L"PowerSettings"
#define PW_SCHEMES_KEY              L"User\\PowerSchemes"
#define PW_DEFAULTS_KEY             L"DefaultPowerSchemeValues"

static HKEY PwPowerKey;
static HANDLE PwChangeEvent;

static
VOID
PwReadActiveScheme(
    _Out_writes_(Count) PWCHAR Scheme,
    _In_ DWORD Count)
{
    DWORD Size = Count * sizeof(WCHAR);

    if (RegGetValueW(PwPowerKey, PW_SCHEMES_KEY, L"ActivePowerScheme", RRF_RT_REG_SZ, NULL, Scheme, &Size) != ERROR_SUCCESS ||
        wcslen(Scheme) != POWER_GUID_CHARS)
    {
        StringCchCopyW(Scheme, Count, PW_BALANCED_SCHEME);
    }
}

static
BOOL
PwReadIndex(
    _In_ PCWSTR SubKey,
    _In_ PCWSTR IndexName,
    _Out_ PDWORD Value)
{
    DWORD Size = sizeof(*Value);

    return RegGetValueW(PwPowerKey, SubKey, IndexName, RRF_RT_REG_DWORD, NULL, Value, &Size) == ERROR_SUCCESS;
}

/* The user's value for the scheme comes first, the scheme default after it */
static
BOOL
PwSettingValue(
    _In_ PCWSTR Scheme,
    _In_ PCWSTR Subgroup,
    _In_ PCWSTR Setting,
    _In_ PCWSTR SettingKey,
    _In_ PCWSTR IndexName,
    _Out_ PDWORD Value)
{
    WCHAR Path[MAX_PATH];

    StringCchPrintfW(Path, ARRAYSIZE(Path), PW_SCHEMES_KEY L"\\%s\\%s\\%s", Scheme, Subgroup, Setting);
    if (PwReadIndex(Path, IndexName, Value))
        return TRUE;

    StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\" PW_DEFAULTS_KEY L"\\%s", SettingKey, Scheme);
    return PwReadIndex(Path, IndexName, Value);
}

static
VOID
PwPushValue(
    _In_ PCWSTR Setting,
    _In_ SYSTEM_POWER_CONDITION Condition,
    _In_ DWORD Value)
{
    ULONG Buffer[(FIELD_OFFSET(SET_POWER_SETTING_VALUE, Data) + sizeof(DWORD) + sizeof(ULONG) - 1) / sizeof(ULONG)];
    PSET_POWER_SETTING_VALUE Request = (PSET_POWER_SETTING_VALUE)Buffer;
    WCHAR Braced[POWER_GUID_CHARS + 3];
    UNICODE_STRING String;
    NTSTATUS Status;

    StringCchPrintfW(Braced, ARRAYSIZE(Braced), L"{%s}", Setting);
    RtlInitUnicodeString(&String, Braced);
    if (!NT_SUCCESS(RtlGUIDFromString(&String, &Request->Guid)))
        return;

    Request->Version = POWER_SETTING_VALUE_VERSION;
    Request->PowerCondition = Condition;
    Request->DataLength = sizeof(Value);
    CopyMemory(Request->Data, &Value, sizeof(Value));

    Status = NtPowerInformation(SetPowerSettingValue,
                                Request,
                                FIELD_OFFSET(SET_POWER_SETTING_VALUE, Data) + sizeof(Value),
                                NULL,
                                0);
    if (!NT_SUCCESS(Status))
        DPRINT1("Setting %S for condition %d was refused 0x%lx\n", Setting, Condition, Status);
}

static
VOID
PwPushSetting(
    _In_ PCWSTR Scheme,
    _In_ PCWSTR Subgroup,
    _In_ PCWSTR Setting,
    _In_ PCWSTR SettingKey)
{
    DWORD Value;

    if (PwSettingValue(Scheme, Subgroup, Setting, SettingKey, L"ACSettingIndex", &Value))
        PwPushValue(Setting, PoAc, Value);

    if (PwSettingValue(Scheme, Subgroup, Setting, SettingKey, L"DCSettingIndex", &Value))
        PwPushValue(Setting, PoDc, Value);
}

/* A key that holds scheme defaults itself is a setting, any other is a subgroup of settings */
static
VOID
PwPushSubgroup(
    _In_ PCWSTR Scheme,
    _In_ PCWSTR Subgroup)
{
    WCHAR SubgroupKey[MAX_PATH];
    WCHAR SettingKey[MAX_PATH];
    WCHAR Setting[POWER_GUID_CHARS + 1];
    DWORD Index, Length;
    HKEY Key, Defaults;

    StringCchPrintfW(SubgroupKey, ARRAYSIZE(SubgroupKey), PW_SETTINGS_KEY L"\\%s", Subgroup);
    if (RegOpenKeyExW(PwPowerKey, SubgroupKey, 0, KEY_READ, &Key) != ERROR_SUCCESS)
        return;

    if (RegOpenKeyExW(Key, PW_DEFAULTS_KEY, 0, KEY_READ, &Defaults) == ERROR_SUCCESS)
    {
        RegCloseKey(Defaults);
        PwPushSetting(Scheme, PW_NO_SUBGROUP, Subgroup, SubgroupKey);
        RegCloseKey(Key);
        return;
    }

    for (Index = 0; ; Index++)
    {
        Length = ARRAYSIZE(Setting);
        if (RegEnumKeyExW(Key, Index, Setting, &Length, NULL, NULL, NULL, NULL) == ERROR_NO_MORE_ITEMS)
            break;
        if (Length != POWER_GUID_CHARS)
            continue;

        StringCchPrintfW(SettingKey, ARRAYSIZE(SettingKey), L"%s\\%s", SubgroupKey, Setting);
        PwPushSetting(Scheme, Subgroup, Setting, SettingKey);
    }

    RegCloseKey(Key);
}

/**
 * @brief Hands every setting of the active scheme to the kernel, for AC and DC.
 *
 * @remarks The kernel drops values that did not change, so this is safe to repeat.
 */
VOID
PwSchemePushAll(VOID)
{
    WCHAR Scheme[POWER_GUID_CHARS + 1];
    WCHAR Subgroup[POWER_GUID_CHARS + 1];
    DWORD Index, Length;
    HKEY Settings;

    PwReadActiveScheme(Scheme, ARRAYSIZE(Scheme));

    if (RegOpenKeyExW(PwPowerKey, PW_SETTINGS_KEY, 0, KEY_READ, &Settings) != ERROR_SUCCESS)
    {
        DPRINT1("No power settings in the registry\n");
        return;
    }

    for (Index = 0; ; Index++)
    {
        Length = ARRAYSIZE(Subgroup);
        if (RegEnumKeyExW(Settings, Index, Subgroup, &Length, NULL, NULL, NULL, NULL) == ERROR_NO_MORE_ITEMS)
            break;
        if (Length == POWER_GUID_CHARS)
            PwPushSubgroup(Scheme, Subgroup);
    }

    RegCloseKey(Settings);
    DPRINT1("Power scheme %S handed to the kernel\n", Scheme);
}

/**
 * @brief Asks for the change event to be set on the next change under the power key.
 *
 * @remarks The request fires once, so it is made again before every push.
 */
BOOL
PwSchemeWatch(VOID)
{
    return RegNotifyChangeKeyValue(PwPowerKey,
                                   TRUE,
                                   REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                   PwChangeEvent,
                                   TRUE) == ERROR_SUCCESS;
}

HANDLE
PwSchemeChangeEvent(VOID)
{
    return PwChangeEvent;
}

BOOL
PwSchemeInitialize(VOID)
{
    BOOLEAN WasEnabled;
    NTSTATUS Status;
    LONG Error;

    /* The kernel takes setting values only from callers holding the shutdown privilege */
    Status = RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &WasEnabled);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Shutdown privilege not available 0x%lx\n", Status);
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Control\\Power",
                          0,
                          KEY_READ | KEY_NOTIFY,
                          &PwPowerKey);
    if (Error != ERROR_SUCCESS)
    {
        SetLastError(Error);
        return FALSE;
    }

    PwChangeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!PwChangeEvent)
    {
        RegCloseKey(PwPowerKey);
        PwPowerKey = NULL;
        return FALSE;
    }

    return TRUE;
}

VOID
PwSchemeUninitialize(VOID)
{
    if (PwChangeEvent)
        CloseHandle(PwChangeEvent);
    if (PwPowerKey)
        RegCloseKey(PwPowerKey);

    PwChangeEvent = NULL;
    PwPowerKey = NULL;
}
