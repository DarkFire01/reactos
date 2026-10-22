/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Power setting and effective power mode interfaces
 * COPYRIGHT:   Copyright 2026 Justin Miller (justin.miller@reactos.org)
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _HPOWERNOTIFY_DEF_
#define _HPOWERNOTIFY_DEF_
typedef PVOID HPOWERNOTIFY, *PHPOWERNOTIFY;
#endif

DWORD
WINAPI
PowerReadACValue(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _Out_opt_ PULONG Type,
    _Out_writes_bytes_opt_(*BufferSize) LPBYTE Buffer,
    _Inout_opt_ LPDWORD BufferSize);

DWORD
WINAPI
PowerReadDCValue(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _Out_opt_ PULONG Type,
    _Out_writes_bytes_opt_(*BufferSize) PUCHAR Buffer,
    _Inout_ LPDWORD BufferSize);

DWORD
WINAPI
PowerWriteACValueIndex(
    _In_opt_ HKEY RootPowerKey,
    _In_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_ DWORD AcValueIndex);

DWORD
WINAPI
PowerWriteDCValueIndex(
    _In_opt_ HKEY RootPowerKey,
    _In_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_ DWORD DcValueIndex);

DWORD
WINAPI
PowerGetActiveScheme(
    _In_opt_ HKEY UserRootPowerKey,
    _Outptr_ GUID **ActivePolicyGuid);

DWORD
WINAPI
PowerSetActiveScheme(
    _In_opt_ HKEY UserRootPowerKey,
    _In_opt_ const GUID *SchemeGuid);

DWORD
WINAPI
PowerSettingRegisterNotification(
    _In_ LPCGUID SettingGuid,
    _In_ DWORD Flags,
    _In_ HANDLE Recipient,
    _Out_ PHPOWERNOTIFY RegistrationHandle);

DWORD
WINAPI
PowerSettingUnregisterNotification(
    _Inout_ HPOWERNOTIFY RegistrationHandle);

typedef enum EFFECTIVE_POWER_MODE
{
    EffectivePowerModeBatterySaver,
    EffectivePowerModeEnergySaverHighSavings = EffectivePowerModeBatterySaver,
    EffectivePowerModeBetterBattery,
    EffectivePowerModeEnergySaverStandard = EffectivePowerModeBetterBattery,
    EffectivePowerModeBalanced,
    EffectivePowerModeHighPerformance,
    EffectivePowerModeMaxPerformance,
    EffectivePowerModeGameMode,
    EffectivePowerModeMixedReality
} EFFECTIVE_POWER_MODE;

#define EFFECTIVE_POWER_MODE_V1 0x00000001
#define EFFECTIVE_POWER_MODE_V2 0x00000002

typedef
_Function_class_(EFFECTIVE_POWER_MODE_CALLBACK)
VOID
WINAPI
EFFECTIVE_POWER_MODE_CALLBACK(
    _In_ EFFECTIVE_POWER_MODE Mode,
    _In_opt_ VOID *Context);

_Must_inspect_result_
HRESULT
WINAPI
PowerRegisterForEffectivePowerModeNotifications(
    _In_ ULONG Version,
    _In_ EFFECTIVE_POWER_MODE_CALLBACK *Callback,
    _In_opt_ PVOID Context,
    _Outptr_ PVOID *RegistrationHandle);

HRESULT
WINAPI
PowerUnregisterFromEffectivePowerModeNotifications(
    _In_ PVOID RegistrationHandle);

#ifdef __cplusplus
}
#endif
