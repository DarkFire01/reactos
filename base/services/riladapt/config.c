/*
 * PROJECT:     ReactOS RIL Adaptation Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Modem discovery and broker device setup
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "riladapt.h"
#include <newdev.h>
#include <strsafe.h>

#define NDEBUG
#include <debug.h>

#define RIL_BROKER_INF              L"telephony.inf"
#define RIL_BROKER_WAIT_MS          10000
#define RIL_OEM_SETTINGS_KEY        L"Software\\Qualcomm\\RIL"

typedef BOOL (WINAPI *PFN_UPDATE_DRIVER)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);

static
BOOL
RilReadString(
    _In_ HKEY Key,
    _In_ PCWSTR Name,
    _Out_writes_(Chars) PWSTR Buffer,
    _In_ DWORD Chars,
    _In_opt_ PCWSTR Default)
{
    WCHAR Raw[MAX_PATH];
    DWORD Size = sizeof(Raw) - sizeof(WCHAR);
    DWORD Type;

    if (RegQueryValueExW(Key, Name, NULL, &Type, (PBYTE)Raw, &Size) != ERROR_SUCCESS ||
        (Type != REG_SZ && Type != REG_EXPAND_SZ))
    {
        if (!Default)
            return FALSE;
        return SUCCEEDED(StringCchCopyW(Buffer, Chars, Default));
    }

    Raw[Size / sizeof(WCHAR)] = UNICODE_NULL;
    if (Type == REG_EXPAND_SZ)
        return ExpandEnvironmentStringsW(Raw, Buffer, Chars) != 0;

    return SUCCEEDED(StringCchCopyW(Buffer, Chars, Raw));
}

static
DWORD
RilReadDword(
    _In_ HKEY Key,
    _In_ PCWSTR Name,
    _In_ DWORD Default)
{
    DWORD Value, Size = sizeof(Value), Type;

    if (RegQueryValueExW(Key, Name, NULL, &Type, (PBYTE)&Value, &Size) != ERROR_SUCCESS ||
        Type != REG_DWORD)
    {
        return Default;
    }

    return Value;
}

/* The vendor INF puts the library tuning under the interface key, the library reads it from HKLM */
static
VOID
RilMirrorOemSettings(
    _In_ HKEY InterfaceKey,
    _In_ DWORD InitIndex)
{
    WCHAR Driver[16], Target[MAX_PATH];
    PCWSTR Names[2];
    HKEY Source, Destination;
    DWORD i;

    StringCchPrintfW(Driver, _countof(Driver), L"Driver%lu", InitIndex);
    Names[0] = L"Common";
    Names[1] = Driver;

    for (i = 0; i < _countof(Names); i++)
    {
        if (RegOpenKeyExW(InterfaceKey, Names[i], 0, KEY_READ, &Source) != ERROR_SUCCESS)
            continue;

        StringCchPrintfW(Target, _countof(Target), L"%s\\%s", RIL_OEM_SETTINGS_KEY, Names[i]);
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, Target, 0, NULL, 0, KEY_ALL_ACCESS,
                            NULL, &Destination, NULL) == ERROR_SUCCESS)
        {
            RegCopyTreeW(Source, NULL, Destination);
            RegCloseKey(Destination);
        }
        RegCloseKey(Source);
    }
}

static
BOOL
RilReadModemKey(
    _In_ HKEY Key,
    _Out_ PRIL_MODEM_CONFIG Config)
{
    ZeroMemory(Config, sizeof(*Config));

    if (!RilReadString(Key, L"RILPath", Config->LibraryPath, _countof(Config->LibraryPath), NULL))
        return FALSE;

    Config->InitIndex = RilReadDword(Key, L"RILInitIndex", 0);
    RilReadString(Key, L"MsrilDeviceId", Config->BrokerId, _countof(Config->BrokerId), L"MODEM\\MSRIL");
    RilReadString(Key, L"MsrilInstanceId", Config->BrokerInstance, _countof(Config->BrokerInstance), L"0001");
    RilReadString(Key, L"MsrilDeviceName", Config->BrokerName, _countof(Config->BrokerName), L"??\\MSRil1");
    return TRUE;
}

static
DWORD
RilFindInterfaceModems(
    _Out_writes_to_(MaxModems, return) PRIL_MODEM_CONFIG Configs,
    _In_ DWORD MaxModems)
{
    SP_DEVICE_INTERFACE_DATA InterfaceData;
    HDEVINFO DevInfo;
    DWORD Index, Found = 0;
    HKEY Key;

    DevInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_RILHOST,
                                   NULL,
                                   NULL,
                                   DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (DevInfo == INVALID_HANDLE_VALUE)
        return 0;

    InterfaceData.cbSize = sizeof(InterfaceData);
    for (Index = 0;
         Found < MaxModems &&
         SetupDiEnumDeviceInterfaces(DevInfo, NULL, &GUID_DEVINTERFACE_RILHOST, Index, &InterfaceData);
         Index++)
    {
        Key = SetupDiOpenDeviceInterfaceRegKey(DevInfo, &InterfaceData, 0, KEY_READ);
        if (Key == INVALID_HANDLE_VALUE)
            continue;

        if (RilReadModemKey(Key, &Configs[Found]))
        {
            RilMirrorOemSettings(Key, Configs[Found].InitIndex);
            Found++;
        }
        RegCloseKey(Key);
    }

    SetupDiDestroyDeviceInfoList(DevInfo);
    return Found;
}

/* Fallback for boards whose OEM component is not installed through a software device */
static
DWORD
RilFindRegistryModems(
    _Out_writes_to_(MaxModems, return) PRIL_MODEM_CONFIG Configs,
    _In_ DWORD MaxModems)
{
    WCHAR Name[16];
    DWORD Index, Found = 0, Chars;
    HKEY Modems, Key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, RILADAPT_PARAMETERS_KEY L"\\Modems", 0, KEY_READ, &Modems) != ERROR_SUCCESS)
        return 0;

    for (Index = 0; Found < MaxModems; Index++)
    {
        Chars = _countof(Name);
        if (RegEnumKeyExW(Modems, Index, Name, &Chars, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;

        if (RegOpenKeyExW(Modems, Name, 0, KEY_READ, &Key) != ERROR_SUCCESS)
            continue;

        if (RilReadModemKey(Key, &Configs[Found]))
        {
            RilMirrorOemSettings(Key, Configs[Found].InitIndex);
            Found++;
        }
        RegCloseKey(Key);
    }

    RegCloseKey(Modems);
    return Found;
}

DWORD
RilFindModems(
    _Out_writes_to_(MaxModems, return) PRIL_MODEM_CONFIG Configs,
    _In_ DWORD MaxModems)
{
    DWORD Found;

    Found = RilFindInterfaceModems(Configs, MaxModems);
    if (Found == 0)
        Found = RilFindRegistryModems(Configs, MaxModems);

    return Found;
}

static
BOOL
RilHardwareIdPresent(
    _In_ PCWSTR HardwareId)
{
    SP_DEVINFO_DATA DeviceData;
    WCHAR Ids[512];
    HDEVINFO DevInfo;
    DWORD Index;
    PCWSTR Id;
    BOOL Found = FALSE;

    DevInfo = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (DevInfo == INVALID_HANDLE_VALUE)
        return FALSE;

    DeviceData.cbSize = sizeof(DeviceData);
    for (Index = 0; !Found && SetupDiEnumDeviceInfo(DevInfo, Index, &DeviceData); Index++)
    {
        ZeroMemory(Ids, sizeof(Ids));
        if (!SetupDiGetDeviceRegistryPropertyW(DevInfo, &DeviceData, SPDRP_HARDWAREID, NULL,
                                               (PBYTE)Ids, sizeof(Ids) - 2 * sizeof(WCHAR), NULL))
        {
            continue;
        }

        for (Id = Ids; *Id; Id += wcslen(Id) + 1)
        {
            if (!_wcsicmp(Id, HardwareId))
            {
                Found = TRUE;
                break;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(DevInfo);
    return Found;
}

static
BOOL
RilCreateBrokerDevnode(
    _In_ PCWSTR HardwareId)
{
    SP_DEVINFO_DATA DeviceData;
    WCHAR IdList[66], InfPath[MAX_PATH];
    PFN_UPDATE_DRIVER UpdateDriver;
    HDEVINFO DevInfo;
    HMODULE NewDev;
    BOOL Reboot = FALSE, Result = FALSE;
    SIZE_T Length;

    Length = wcslen(HardwareId);
    if (Length + 2 > _countof(IdList))
        return FALSE;

    ZeroMemory(IdList, sizeof(IdList));
    CopyMemory(IdList, HardwareId, Length * sizeof(WCHAR));

    if (!GetWindowsDirectoryW(InfPath, _countof(InfPath)) ||
        FAILED(StringCchCatW(InfPath, _countof(InfPath), L"\\inf\\" RIL_BROKER_INF)))
    {
        return FALSE;
    }

    DevInfo = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_SYSTEM, NULL);
    if (DevInfo == INVALID_HANDLE_VALUE)
        return FALSE;

    DeviceData.cbSize = sizeof(DeviceData);
    if (SetupDiCreateDeviceInfoW(DevInfo, L"MSRIL", &GUID_DEVCLASS_SYSTEM, NULL, NULL,
                                 DICD_GENERATE_ID, &DeviceData) &&
        SetupDiSetDeviceRegistryPropertyW(DevInfo, &DeviceData, SPDRP_HARDWAREID,
                                          (PBYTE)IdList, (DWORD)((Length + 2) * sizeof(WCHAR))) &&
        SetupDiCallClassInstaller(DIF_REGISTERDEVICE, DevInfo, &DeviceData))
    {
        NewDev = LoadLibraryW(L"newdev.dll");
        if (NewDev)
        {
            UpdateDriver = (PFN_UPDATE_DRIVER)GetProcAddress(NewDev, "UpdateDriverForPlugAndPlayDevicesW");
            if (UpdateDriver)
                Result = UpdateDriver(NULL, HardwareId, InfPath, INSTALLFLAG_FORCE, &Reboot);
            FreeLibrary(NewDev);
        }
    }

    SetupDiDestroyDeviceInfoList(DevInfo);
    return Result;
}

static
PCWSTR
RilBrokerDosName(
    _In_ PRIL_MODEM_CONFIG Config)
{
    PCWSTR Name = Config->BrokerName;

    /* The vendor value is an NT path under \??, the broker is reached through that DOS name */
    if (!wcsncmp(Name, L"\\??\\", 4))
        return Name + 4;
    if (!wcsncmp(Name, L"??\\", 3))
        return Name + 3;

    return Name;
}

static
BOOL
RilBrokerExists(
    _In_ PRIL_MODEM_CONFIG Config)
{
    WCHAR Target[MAX_PATH];

    return QueryDosDeviceW(RilBrokerDosName(Config), Target, _countof(Target)) != 0;
}

BOOL
RilEnsureBrokerDevice(
    _In_ PRIL_MODEM_CONFIG Config)
{
    DWORD Waited;

    if (RilBrokerExists(Config))
        return TRUE;

    if (!RilHardwareIdPresent(Config->BrokerId) && !RilCreateBrokerDevnode(Config->BrokerId))
    {
        DPRINT1("Could not install the broker for %S\n", Config->BrokerId);
        return FALSE;
    }

    for (Waited = 0; Waited < RIL_BROKER_WAIT_MS; Waited += 250)
    {
        if (RilBrokerExists(Config))
            return TRUE;
        Sleep(250);
    }

    return FALSE;
}

BOOL
RilGetBrokerHostPath(
    _In_ PRIL_MODEM_CONFIG Config,
    _Out_writes_(Chars) PWSTR Path,
    _In_ DWORD Chars)
{
    return SUCCEEDED(StringCchPrintfW(Path, Chars, L"\\\\.\\%s%s", RilBrokerDosName(Config), MSRIL_HOST_FILE_NAME));
}
