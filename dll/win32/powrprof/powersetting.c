/*
 * PROJECT:     ReactOS Power Profile Library
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Registering power settings and their metadata
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A component that wants its own entry in the power configuration registers the
 * setting, its possible values and their names here. All of it is registry state
 * below Control\Power, keyed by subgroup and setting GUID, which is where the
 * power configuration tools read it back from.
 */

#include <stdarg.h>
#include <stdio.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <powrprof.h>
#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(powrprof);

static const WCHAR szPowerKey[] = L"SYSTEM\\CurrentControlSet\\Control\\Power";
static const WCHAR szPowerSettings[] = L"PowerSettings";
static const WCHAR szPowerSchemes[] = L"User\\PowerSchemes";
static const WCHAR szDefaultValues[] = L"DefaultPowerSchemeValues";
static const WCHAR szFriendlyName[] = L"FriendlyName";
static const WCHAR szDescription[] = L"Description";

/* A path is at most four GUIDs, a value index and the fixed names between them */
#define POWER_PATH_LENGTH 256

static
VOID
PowerAppendGuid(
    _Inout_ LPWSTR Path,
    _In_ CONST GUID *Guid)
{
    swprintf(Path + wcslen(Path),
             L"\\{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
             Guid->Data1, Guid->Data2, Guid->Data3,
             Guid->Data4[0], Guid->Data4[1], Guid->Data4[2], Guid->Data4[3],
             Guid->Data4[4], Guid->Data4[5], Guid->Data4[6], Guid->Data4[7]);
}

/* The caller's root key stands in for HKLM\SYSTEM\CurrentControlSet\Control\Power */
static
DWORD
PowerOpenRoot(
    _In_opt_ HKEY RootPowerKey,
    _Out_ PHKEY Key,
    _Out_ PBOOL Close)
{
    *Close = FALSE;

    if (RootPowerKey != NULL)
    {
        *Key = RootPowerKey;
        return ERROR_SUCCESS;
    }

    *Close = TRUE;
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPowerKey, 0, KEY_READ | KEY_WRITE, Key);
}

/* Opens, and optionally creates, a key below the power root */
static
DWORD
PowerOpenPath(
    _In_opt_ HKEY RootPowerKey,
    _In_ LPCWSTR Path,
    _In_ BOOL Create,
    _Out_ PHKEY Key)
{
    HKEY Root;
    BOOL Close;
    DWORD Error;

    Error = PowerOpenRoot(RootPowerKey, &Root, &Close);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (Create)
    {
        Error = RegCreateKeyExW(Root, Path, 0, NULL, REG_OPTION_NON_VOLATILE,
                                KEY_READ | KEY_WRITE, NULL, Key, NULL);
    }
    else
    {
        Error = RegOpenKeyExW(Root, Path, 0, KEY_READ | KEY_WRITE, Key);
    }

    if (Close)
        RegCloseKey(Root);

    return Error;
}

/* PowerSettings[\{subgroup}[\{setting}]] */
static
VOID
PowerSettingPath(
    _Out_writes_(POWER_PATH_LENGTH) LPWSTR Path,
    _In_opt_ CONST GUID *SubGroupGuid,
    _In_opt_ CONST GUID *PowerSettingGuid)
{
    wcscpy(Path, szPowerSettings);

    if (SubGroupGuid != NULL)
        PowerAppendGuid(Path, SubGroupGuid);

    if (PowerSettingGuid != NULL)
        PowerAppendGuid(Path, PowerSettingGuid);
}

static
DWORD
PowerWriteSettingString(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ CONST GUID *SubGroupGuid,
    _In_opt_ CONST GUID *PowerSettingGuid,
    _In_ LPCWSTR ValueName,
    _In_reads_bytes_(BufferSize) UCHAR *Buffer,
    _In_ DWORD BufferSize)
{
    WCHAR Path[POWER_PATH_LENGTH];
    HKEY Key;
    DWORD Error;

    if (Buffer == NULL)
        return ERROR_INVALID_PARAMETER;

    PowerSettingPath(Path, SubGroupGuid, PowerSettingGuid);

    Error = PowerOpenPath(RootPowerKey, Path, TRUE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegSetValueExW(Key, ValueName, 0, REG_SZ, Buffer, BufferSize);
    RegCloseKey(Key);

    return Error;
}

static
DWORD
PowerWriteSettingDword(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ CONST GUID *SubGroupGuid,
    _In_opt_ CONST GUID *PowerSettingGuid,
    _In_ LPCWSTR ValueName,
    _In_ DWORD Value)
{
    WCHAR Path[POWER_PATH_LENGTH];
    HKEY Key;
    DWORD Error;

    PowerSettingPath(Path, SubGroupGuid, PowerSettingGuid);

    Error = PowerOpenPath(RootPowerKey, Path, TRUE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegSetValueExW(Key, ValueName, 0, REG_DWORD,
                           (CONST BYTE *)&Value, sizeof(Value));
    RegCloseKey(Key);

    return Error;
}

/* A setting key holds its possible values and defaults as child keys */
static
DWORD
PowerDeleteKeyTree(
    _In_ HKEY Parent,
    _In_ LPCWSTR Name)
{
    WCHAR Child[MAX_PATH];
    HKEY Key;
    DWORD Length;
    DWORD Error;

    Error = RegOpenKeyExW(Parent, Name, 0, KEY_READ | KEY_WRITE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* Enumerating index zero again each time is what removing the child leaves */
    for (;;)
    {
        Length = ARRAYSIZE(Child);
        Error = RegEnumKeyExW(Key, 0, Child, &Length, NULL, NULL, NULL, NULL);
        if (Error != ERROR_SUCCESS)
            break;

        Error = PowerDeleteKeyTree(Key, Child);
        if (Error != ERROR_SUCCESS)
            break;
    }

    RegCloseKey(Key);

    if (Error != ERROR_NO_MORE_ITEMS && Error != ERROR_SUCCESS)
        return Error;

    return RegDeleteKeyW(Parent, Name);
}

/* PowerSettings\{subgroup}\{setting}\<index> */
static
DWORD
PowerOpenPossibleSetting(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ CONST GUID *SubGroupGuid,
    _In_opt_ CONST GUID *PowerSettingGuid,
    _In_ ULONG PossibleSettingIndex,
    _Out_ PHKEY Key)
{
    WCHAR Path[POWER_PATH_LENGTH];

    PowerSettingPath(Path, SubGroupGuid, PowerSettingGuid);
    swprintf(Path + wcslen(Path), L"\\%lu", PossibleSettingIndex);

    return PowerOpenPath(RootPowerKey, Path, TRUE, Key);
}

/*
 * @implemented
 */
DWORD WINAPI
PowerCreateSetting(
    HKEY RootSystemPowerKey,
    const GUID *SubGroupOfPowerSettingsGuid,
    const GUID *PowerSettingGuid)
{
    WCHAR Path[POWER_PATH_LENGTH];
    HKEY Key;
    DWORD Error;

    TRACE("PowerCreateSetting(%p %s %s)\n", RootSystemPowerKey,
          debugstr_guid(SubGroupOfPowerSettingsGuid), debugstr_guid(PowerSettingGuid));

    if (SubGroupOfPowerSettingsGuid == NULL || PowerSettingGuid == NULL)
        return ERROR_INVALID_PARAMETER;

    PowerSettingPath(Path, SubGroupOfPowerSettingsGuid, PowerSettingGuid);

    Error = PowerOpenPath(RootSystemPowerKey, Path, TRUE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    RegCloseKey(Key);
    return ERROR_SUCCESS;
}

/*
 * @implemented
 */
DWORD WINAPI
PowerCreatePossibleSetting(
    HKEY RootSystemPowerKey,
    const GUID *SubGroupOfPowerSettingsGuid,
    const GUID *PowerSettingGuid,
    ULONG PossibleSettingIndex)
{
    HKEY Key;
    DWORD Error;

    TRACE("PowerCreatePossibleSetting(%p %s %s %lu)\n", RootSystemPowerKey,
          debugstr_guid(SubGroupOfPowerSettingsGuid), debugstr_guid(PowerSettingGuid),
          PossibleSettingIndex);

    if (SubGroupOfPowerSettingsGuid == NULL || PowerSettingGuid == NULL)
        return ERROR_INVALID_PARAMETER;

    Error = PowerOpenPossibleSetting(RootSystemPowerKey,
                                     SubGroupOfPowerSettingsGuid,
                                     PowerSettingGuid,
                                     PossibleSettingIndex,
                                     &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    RegCloseKey(Key);
    return ERROR_SUCCESS;
}

/*
 * @implemented
 */
DWORD WINAPI
PowerRemovePowerSetting(
    const GUID *PowerSettingSubKeyGuid,
    const GUID *PowerSettingGuid)
{
    WCHAR Path[POWER_PATH_LENGTH];
    HKEY Key;
    DWORD Error;

    TRACE("PowerRemovePowerSetting(%s %s)\n",
          debugstr_guid(PowerSettingSubKeyGuid), debugstr_guid(PowerSettingGuid));

    if (PowerSettingSubKeyGuid == NULL || PowerSettingGuid == NULL)
        return ERROR_INVALID_PARAMETER;

    PowerSettingPath(Path, PowerSettingSubKeyGuid, NULL);

    Error = PowerOpenPath(NULL, Path, FALSE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Path[0] = UNICODE_NULL;
    PowerAppendGuid(Path, PowerSettingGuid);

    /* Skip the separator the GUID carries, it is a child of the key just opened */
    Error = PowerDeleteKeyTree(Key, Path + 1);
    RegCloseKey(Key);

    return Error;
}

/*
 * @implemented
 */
DWORD WINAPI
PowerWriteFriendlyName(
    HKEY RootPowerKey,
    const GUID *SchemeGuid,
    const GUID *SubGroupOfPowerSettingsGuid,
    const GUID *PowerSettingGuid,
    UCHAR *Buffer,
    DWORD BufferSize)
{
    WCHAR Path[POWER_PATH_LENGTH];
    HKEY Key;
    DWORD Error;

    TRACE("PowerWriteFriendlyName(%p %s %s %s %p %lu)\n", RootPowerKey,
          debugstr_guid(SchemeGuid), debugstr_guid(SubGroupOfPowerSettingsGuid),
          debugstr_guid(PowerSettingGuid), Buffer, BufferSize);

    if (Buffer == NULL)
        return ERROR_INVALID_PARAMETER;

    /* A setting is named where it is defined, a scheme where it is stored */
    if (SubGroupOfPowerSettingsGuid != NULL || PowerSettingGuid != NULL)
    {
        return PowerWriteSettingString(RootPowerKey,
                                       SubGroupOfPowerSettingsGuid,
                                       PowerSettingGuid,
                                       szFriendlyName,
                                       Buffer,
                                       BufferSize);
    }

    if (SchemeGuid == NULL)
        return ERROR_INVALID_PARAMETER;

    wcscpy(Path, szPowerSchemes);
    PowerAppendGuid(Path, SchemeGuid);

    Error = PowerOpenPath(RootPowerKey, Path, TRUE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegSetValueExW(Key, szFriendlyName, 0, REG_SZ, Buffer, BufferSize);
    RegCloseKey(Key);

    return Error;
}

/*
 * @implemented
 */
DWORD WINAPI
PowerWriteDescription(
    HKEY RootPowerKey,
    const GUID *SchemeGuid,
    const GUID *SubGroupOfPowerSettingsGuid,
    const GUID *PowerSettingGuid,
    UCHAR *Buffer,
    DWORD BufferSize)
{
    WCHAR Path[POWER_PATH_LENGTH];
    HKEY Key;
    DWORD Error;

    TRACE("PowerWriteDescription(%p %s %s %s %p %lu)\n", RootPowerKey,
          debugstr_guid(SchemeGuid), debugstr_guid(SubGroupOfPowerSettingsGuid),
          debugstr_guid(PowerSettingGuid), Buffer, BufferSize);

    if (Buffer == NULL)
        return ERROR_INVALID_PARAMETER;

    if (SubGroupOfPowerSettingsGuid != NULL || PowerSettingGuid != NULL)
    {
        return PowerWriteSettingString(RootPowerKey,
                                       SubGroupOfPowerSettingsGuid,
                                       PowerSettingGuid,
                                       szDescription,
                                       Buffer,
                                       BufferSize);
    }

    if (SchemeGuid == NULL)
        return ERROR_INVALID_PARAMETER;

    wcscpy(Path, szPowerSchemes);
    PowerAppendGuid(Path, SchemeGuid);

    Error = PowerOpenPath(RootPowerKey, Path, TRUE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegSetValueExW(Key, szDescription, 0, REG_SZ, Buffer, BufferSize);
    RegCloseKey(Key);

    return Error;
}

/*
 * @implemented
 */
DWORD WINAPI
PowerWritePossibleFriendlyName(
    HKEY RootPowerKey,
    const GUID *SubGroupOfPowerSettingsGuid,
    const GUID *PowerSettingGuid,
    ULONG PossibleSettingIndex,
    UCHAR *Buffer,
    DWORD BufferSize)
{
    HKEY Key;
    DWORD Error;

    TRACE("PowerWritePossibleFriendlyName(%p %s %s %lu %p %lu)\n", RootPowerKey,
          debugstr_guid(SubGroupOfPowerSettingsGuid), debugstr_guid(PowerSettingGuid),
          PossibleSettingIndex, Buffer, BufferSize);

    if (Buffer == NULL)
        return ERROR_INVALID_PARAMETER;

    Error = PowerOpenPossibleSetting(RootPowerKey,
                                     SubGroupOfPowerSettingsGuid,
                                     PowerSettingGuid,
                                     PossibleSettingIndex,
                                     &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegSetValueExW(Key, szFriendlyName, 0, REG_SZ, Buffer, BufferSize);
    RegCloseKey(Key);

    return Error;
}

/*
 * @implemented
 */
DWORD WINAPI
PowerWritePossibleDescription(
    HKEY RootPowerKey,
    const GUID *SubGroupOfPowerSettingsGuid,
    const GUID *PowerSettingGuid,
    ULONG PossibleSettingIndex,
    UCHAR *Buffer,
    DWORD BufferSize)
{
    HKEY Key;
    DWORD Error;

    TRACE("PowerWritePossibleDescription(%p %s %s %lu %p %lu)\n", RootPowerKey,
          debugstr_guid(SubGroupOfPowerSettingsGuid), debugstr_guid(PowerSettingGuid),
          PossibleSettingIndex, Buffer, BufferSize);

    if (Buffer == NULL)
        return ERROR_INVALID_PARAMETER;

    Error = PowerOpenPossibleSetting(RootPowerKey,
                                     SubGroupOfPowerSettingsGuid,
                                     PowerSettingGuid,
                                     PossibleSettingIndex,
                                     &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegSetValueExW(Key, szDescription, 0, REG_SZ, Buffer, BufferSize);
    RegCloseKey(Key);

    return Error;
}

/*
 * @implemented
 */
DWORD WINAPI
PowerWritePossibleValue(
    HKEY RootPowerKey,
    const GUID *SubGroupOfPowerSettingsGuid,
    const GUID *PowerSettingGuid,
    ULONG Type,
    ULONG PossibleSettingIndex,
    UCHAR *Buffer,
    DWORD BufferSize)
{
    HKEY Key;
    DWORD Error;

    TRACE("PowerWritePossibleValue(%p %s %s %lu %lu %p %lu)\n", RootPowerKey,
          debugstr_guid(SubGroupOfPowerSettingsGuid), debugstr_guid(PowerSettingGuid),
          Type, PossibleSettingIndex, Buffer, BufferSize);

    if (Buffer == NULL)
        return ERROR_INVALID_PARAMETER;

    Error = PowerOpenPossibleSetting(RootPowerKey,
                                     SubGroupOfPowerSettingsGuid,
                                     PowerSettingGuid,
                                     PossibleSettingIndex,
                                     &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegSetValueExW(Key, L"SettingValue", 0, Type, Buffer, BufferSize);
    RegCloseKey(Key);

    return Error;
}

/*
 * @implemented
 */
DWORD WINAPI
PowerWriteValueUnitsSpecifier(
    HKEY RootPowerKey,
    const GUID *SubGroupOfPowerSettingsGuid,
    const GUID *PowerSettingGuid,
    UCHAR *Buffer,
    DWORD BufferSize)
{
    TRACE("PowerWriteValueUnitsSpecifier(%p %s %s %p %lu)\n", RootPowerKey,
          debugstr_guid(SubGroupOfPowerSettingsGuid), debugstr_guid(PowerSettingGuid),
          Buffer, BufferSize);

    return PowerWriteSettingString(RootPowerKey,
                                   SubGroupOfPowerSettingsGuid,
                                   PowerSettingGuid,
                                   L"ValueUnits",
                                   Buffer,
                                   BufferSize);
}

/*
 * @implemented
 */
DWORD WINAPI
PowerWriteValueIncrement(
    HKEY RootPowerKey,
    const GUID *SubGroupOfPowerSettingsGuid,
    const GUID *PowerSettingGuid,
    DWORD ValueIncrement)
{
    TRACE("PowerWriteValueIncrement(%p %s %s %lu)\n", RootPowerKey,
          debugstr_guid(SubGroupOfPowerSettingsGuid), debugstr_guid(PowerSettingGuid),
          ValueIncrement);

    return PowerWriteSettingDword(RootPowerKey,
                                  SubGroupOfPowerSettingsGuid,
                                  PowerSettingGuid,
                                  L"ValueIncrement",
                                  ValueIncrement);
}

/*
 * @implemented
 */
DWORD WINAPI
PowerWriteValueMax(
    HKEY RootPowerKey,
    const GUID *SubGroupOfPowerSettingsGuid,
    const GUID *PowerSettingGuid,
    DWORD ValueMaximum)
{
    TRACE("PowerWriteValueMax(%p %s %s %lu)\n", RootPowerKey,
          debugstr_guid(SubGroupOfPowerSettingsGuid), debugstr_guid(PowerSettingGuid),
          ValueMaximum);

    return PowerWriteSettingDword(RootPowerKey,
                                  SubGroupOfPowerSettingsGuid,
                                  PowerSettingGuid,
                                  L"ValueMax",
                                  ValueMaximum);
}

/*
 * @implemented
 */
DWORD WINAPI
PowerWriteValueMin(
    HKEY RootPowerKey,
    const GUID *SubGroupOfPowerSettingsGuid,
    const GUID *PowerSettingGuid,
    DWORD ValueMinimum)
{
    TRACE("PowerWriteValueMin(%p %s %s %lu)\n", RootPowerKey,
          debugstr_guid(SubGroupOfPowerSettingsGuid), debugstr_guid(PowerSettingGuid),
          ValueMinimum);

    return PowerWriteSettingDword(RootPowerKey,
                                  SubGroupOfPowerSettingsGuid,
                                  PowerSettingGuid,
                                  L"ValueMin",
                                  ValueMinimum);
}

/*
 * @implemented
 */
DWORD WINAPI
PowerWriteSettingAttributes(
    const GUID *SubGroupGuid,
    const GUID *PowerSettingGuid,
    DWORD Attributes)
{
    TRACE("PowerWriteSettingAttributes(%s %s 0x%lx)\n",
          debugstr_guid(SubGroupGuid), debugstr_guid(PowerSettingGuid), Attributes);

    return PowerWriteSettingDword(NULL,
                                  SubGroupGuid,
                                  PowerSettingGuid,
                                  L"Attributes",
                                  Attributes);
}

/* Both default index writers differ only in the value they set */
static
DWORD
PowerWriteDefaultIndex(
    _In_opt_ HKEY RootSystemPowerKey,
    _In_ CONST GUID *SchemePersonalityGuid,
    _In_opt_ CONST GUID *SubGroupOfPowerSettingsGuid,
    _In_ CONST GUID *PowerSettingGuid,
    _In_ LPCWSTR ValueName,
    _In_ DWORD DefaultIndex)
{
    WCHAR Path[POWER_PATH_LENGTH];
    HKEY Key;
    DWORD Error;

    if (SchemePersonalityGuid == NULL || PowerSettingGuid == NULL)
        return ERROR_INVALID_PARAMETER;

    PowerSettingPath(Path, SubGroupOfPowerSettingsGuid, PowerSettingGuid);
    wcscat(Path, L"\\");
    wcscat(Path, szDefaultValues);
    PowerAppendGuid(Path, SchemePersonalityGuid);

    Error = PowerOpenPath(RootSystemPowerKey, Path, TRUE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegSetValueExW(Key, ValueName, 0, REG_DWORD,
                           (CONST BYTE *)&DefaultIndex, sizeof(DefaultIndex));
    RegCloseKey(Key);

    return Error;
}

/*
 * @implemented
 */
DWORD WINAPI
PowerWriteACDefaultIndex(
    HKEY RootSystemPowerKey,
    const GUID *SchemePersonalityGuid,
    const GUID *SubGroupOfPowerSettingsGuid,
    const GUID *PowerSettingGuid,
    DWORD DefaultAcIndex)
{
    TRACE("PowerWriteACDefaultIndex(%p %s %s %s %lu)\n", RootSystemPowerKey,
          debugstr_guid(SchemePersonalityGuid), debugstr_guid(SubGroupOfPowerSettingsGuid),
          debugstr_guid(PowerSettingGuid), DefaultAcIndex);

    return PowerWriteDefaultIndex(RootSystemPowerKey,
                                  SchemePersonalityGuid,
                                  SubGroupOfPowerSettingsGuid,
                                  PowerSettingGuid,
                                  L"ACSettingIndex",
                                  DefaultAcIndex);
}

/*
 * @implemented
 */
DWORD WINAPI
PowerWriteDCDefaultIndex(
    HKEY RootSystemPowerKey,
    const GUID *SchemePersonalityGuid,
    const GUID *SubGroupOfPowerSettingsGuid,
    const GUID *PowerSettingGuid,
    DWORD DefaultDcIndex)
{
    TRACE("PowerWriteDCDefaultIndex(%p %s %s %s %lu)\n", RootSystemPowerKey,
          debugstr_guid(SchemePersonalityGuid), debugstr_guid(SubGroupOfPowerSettingsGuid),
          debugstr_guid(PowerSettingGuid), DefaultDcIndex);

    return PowerWriteDefaultIndex(RootSystemPowerKey,
                                  SchemePersonalityGuid,
                                  SubGroupOfPowerSettingsGuid,
                                  PowerSettingGuid,
                                  L"DCSettingIndex",
                                  DefaultDcIndex);
}

/*
 * @unimplemented
 *
 * Nothing tracks an effective power mode yet, so a subscriber is accepted and
 * never called back.
 */
HRESULT WINAPI
PowerRegisterForEffectivePowerModeNotifications(
    ULONG Version,
    EFFECTIVE_POWER_MODE_CALLBACK *Callback,
    PVOID Context,
    PVOID *RegistrationHandle)
{
    FIXME("(%lu %p %p %p) stub!\n", Version, Callback, Context, RegistrationHandle);

    if (Callback == NULL || RegistrationHandle == NULL)
        return E_INVALIDARG;

    if (Version != EFFECTIVE_POWER_MODE_V1 && Version != EFFECTIVE_POWER_MODE_V2)
        return E_INVALIDARG;

    *RegistrationHandle = (PVOID)(ULONG_PTR)Version;
    return S_OK;
}

/*
 * @unimplemented
 */
HRESULT WINAPI
PowerUnregisterFromEffectivePowerModeNotifications(
    PVOID RegistrationHandle)
{
    FIXME("(%p) stub!\n", RegistrationHandle);

    if (RegistrationHandle == NULL)
        return E_INVALIDARG;

    return S_OK;
}
