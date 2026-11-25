/*
 * PROJECT:     ReactOS Driver Install Frameworks API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Plug and play device matching, install, repair and uninstall
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES *******************************************************************/

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(difxapi);

/* GLOBALS ********************************************************************/

/* Size of the buffers device ID lists are read into, in bytes */
#define DIFX_ID_LIST_SIZE   0x800

/* Longest hardware ID read from a models section */
#define DIFX_INF_ID_LENGTH  512

typedef BOOL (WINAPI *PDIFX_UPDATE_DRIVER)(HWND, PCWSTR, PCWSTR, DWORD, PBOOL);
typedef BOOL (WINAPI *PDIFX_INSTALL_DEVICE)(HWND, HDEVINFO, PSP_DEVINFO_DATA, PSP_DRVINFO_DATA_W, DWORD, PBOOL);

/* The IDs one device of the machine reports */
typedef struct _DIFX_DEVICE
{
    WCHAR Primary[MAX_DEVICE_ID_LEN];
    PWSTR Ids;
} DIFX_DEVICE, *PDIFX_DEVICE;

/* Every device the machine knows, present or not */
typedef struct _DIFX_MACHINE
{
    ULONG Count;
    PDIFX_DEVICE Devices;
} DIFX_MACHINE, *PDIFX_MACHINE;

/* The driver a device had before this install, to go back to on failure */
typedef struct _DIFX_PREVIOUS_DRIVER
{
    struct _DIFX_PREVIOUS_DRIVER *Next;
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    WCHAR InfPath[MAX_PATH];
} DIFX_PREVIOUS_DRIVER, *PDIFX_PREVIOUS_DRIVER;

/* FUNCTIONS ******************************************************************/

static
BOOL
WINAPI
DifxReadIdList(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ DWORD Property,
    _Out_writes_bytes_(DIFX_ID_LIST_SIZE + 2 * sizeof(WCHAR)) PWSTR Buffer)
{
    /* The two characters past the read size keep the list terminated */
    ZeroMemory(Buffer, DIFX_ID_LIST_SIZE + 2 * sizeof(WCHAR));

    return SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet,
                                             DeviceInfoData,
                                             Property,
                                             NULL,
                                             (PBYTE)Buffer,
                                             DIFX_ID_LIST_SIZE,
                                             NULL);
}

static
BOOL
WINAPI
DifxGetInstanceId(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _Out_writes_(MAX_DEVICE_ID_LEN) PWSTR InstanceId)
{
    *InstanceId = UNICODE_NULL;
    return SetupDiGetDeviceInstanceIdW(DeviceInfoSet, DeviceInfoData, InstanceId, MAX_DEVICE_ID_LEN, NULL);
}

/* A device that is not in the tree right now */
static
BOOL
WINAPI
DifxIsPhantom(
    _In_ PSP_DEVINFO_DATA DeviceInfoData)
{
    ULONG Status, Problem;
    CONFIGRET Cr;

    Cr = CM_Get_DevNode_Status(&Status, &Problem, DeviceInfoData->DevInst, 0);
    return (Cr == CR_NO_SUCH_DEVINST || Cr == CR_NO_SUCH_VALUE);
}

/* The next plug in of the device installs its best driver again */
static
DWORD
WINAPI
DifxMarkForReinstall(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _Out_ PBOOL Read)
{
    DWORD ConfigFlags = 0;

    *Read = SetupDiGetDeviceRegistryPropertyW(DeviceInfoSet,
                                              DeviceInfoData,
                                              SPDRP_CONFIGFLAGS,
                                              NULL,
                                              (PBYTE)&ConfigFlags,
                                              sizeof(ConfigFlags),
                                              NULL);
    if (!*Read)
        return GetLastError();

    ConfigFlags |= CONFIGFLAG_REINSTALL;
    if (!SetupDiSetDeviceRegistryPropertyW(DeviceInfoSet,
                                           DeviceInfoData,
                                           SPDRP_CONFIGFLAGS,
                                           (PBYTE)&ConfigFlags,
                                           sizeof(ConfigFlags)))
    {
        return GetLastError();
    }

    return ERROR_SUCCESS;
}

/* The INF the driver of a device was installed from, as its driver key names it */
static
DWORD
WINAPI
DifxGetDeviceInf(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _Out_writes_(MAX_PATH) PWSTR InfPath)
{
    DWORD Size, Type;
    LONG Error;
    HKEY hKey;

    *InfPath = UNICODE_NULL;

    hKey = SetupDiOpenDevRegKey(DeviceInfoSet, DeviceInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
    if (hKey == INVALID_HANDLE_VALUE)
        return GetLastError();

    Size = (MAX_PATH - 1) * sizeof(WCHAR);
    Error = RegQueryValueExW(hKey, REGSTR_VAL_INFPATH, NULL, &Type, (PBYTE)InfPath, &Size);
    RegCloseKey(hKey);

    if (Error != ERROR_SUCCESS)
    {
        *InfPath = UNICODE_NULL;
        return Error;
    }

    InfPath[Size / sizeof(WCHAR)] = UNICODE_NULL;
    return ERROR_SUCCESS;
}

/* Whether an INF and a driver store INF are the same entry */
static
BOOL
WINAPI
DifxIsStoreEntryOf(
    _In_ PCWSTR InfPath,
    _In_ PCWSTR StoreInf)
{
    WCHAR Location[MAX_PATH];

    if (!SetupGetInfDriverStoreLocationW(InfPath, NULL, NULL, Location, ARRAYSIZE(Location), NULL))
        return FALSE;

    return (_wcsicmp(Location, StoreInf) == 0);
}

static
VOID
WINAPI
DifxFreeMachine(
    _Inout_ PDIFX_MACHINE Machine)
{
    ULONG i;

    for (i = 0; i < Machine->Count; i++)
        DifxFree(Machine->Devices[i].Ids);

    DifxFree(Machine->Devices);
    ZeroMemory(Machine, sizeof(*Machine));
}

/* Collects the hardware and compatible IDs of every device, the first one names the device */
static
DWORD
WINAPI
DifxBuildMachine(
    _Out_ PDIFX_MACHINE Machine)
{
    WCHAR IdList[DIFX_ID_LIST_SIZE / sizeof(WCHAR) + 2];
    static const DWORD Properties[] = { SPDRP_HARDWAREID, SPDRP_COMPATIBLEIDS };
    SP_DEVINFO_DATA DeviceInfoData;
    PDIFX_DEVICE Devices, Device;
    HDEVINFO DeviceInfoSet;
    DWORD Index, Error = ERROR_SUCCESS;
    PCWSTR Id;
    ULONG i;

    ZeroMemory(Machine, sizeof(*Machine));

    DeviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
        return GetLastError();

    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    for (Index = 0; SetupDiEnumDeviceInfo(DeviceInfoSet, Index, &DeviceInfoData); Index++)
    {
        if (Machine->Devices)
            Devices = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Machine->Devices, (Machine->Count + 1) * sizeof(*Devices));
        else
            Devices = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Devices));

        if (!Devices)
        {
            Error = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }

        Machine->Devices = Devices;
        Device = &Devices[Machine->Count];

        for (i = 0; i < ARRAYSIZE(Properties); i++)
        {
            if (!DifxReadIdList(DeviceInfoSet, &DeviceInfoData, Properties[i], IdList))
                continue;

            if (!Device->Primary[0])
                StringCchCopyW(Device->Primary, ARRAYSIZE(Device->Primary), IdList);

            for (Id = IdList; *Id; Id += wcslen(Id) + 1)
            {
                if (DifxListAppend(&Device->Ids, Id) != ERROR_SUCCESS)
                {
                    Error = ERROR_NOT_ENOUGH_MEMORY;
                    break;
                }
            }
        }

        Machine->Count++;
        if (Error != ERROR_SUCCESS)
            break;
    }

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);

    if (Error != ERROR_SUCCESS)
        DifxFreeMachine(Machine);

    return Error;
}

/* Adds the devices an INF hardware ID matches, named by their first ID */
static
VOID
WINAPI
DifxMatchInfId(
    _In_ PDIFX_MACHINE Machine,
    _In_ PCWSTR InfId,
    _Inout_ PWSTR *Matches)
{
    BOOL Found = FALSE, Named = FALSE;
    ULONG i;

    for (i = 0; i < Machine->Count; i++)
    {
        if (!DifxListContains(Machine->Devices[i].Ids, InfId))
            continue;

        Found = TRUE;
        if (!Machine->Devices[i].Primary[0])
            continue;

        Named = TRUE;
        if (!DifxListContains(*Matches, Machine->Devices[i].Primary))
            DifxListAppend(Matches, Machine->Devices[i].Primary);
    }

    if (Found && !Named && !DifxListContains(*Matches, InfId))
        DifxListAppend(Matches, InfId);
}

/**
 * @brief Lists the devices of the machine the INF has a driver for on this platform.
 *
 * @return ERROR_NO_DEVICE_ID when the INF names no hardware ID for this platform at all.
 */
static
DWORD
WINAPI
DifxFindInfMatches(
    _In_ HINF hInf,
    _In_ PDIFX_MACHINE Machine,
    _Inout_ PWSTR *Matches)
{
    WCHAR Models[LINE_LEN];
    WCHAR InfId[DIFX_INF_ID_LENGTH];
    INFCONTEXT Manufacturer, Model;
    BOOL Found, FoundModel;
    DWORD Field, FieldCount;
    ULONG IdCount = 0;

    for (Found = SetupFindFirstLineW(hInf, L"Manufacturer", NULL, &Manufacturer);
         Found;
         Found = SetupFindNextLine(&Manufacturer, &Manufacturer))
    {
        /* A manufacturer without models has nothing to match */
        if (!SetupGetStringFieldW(&Manufacturer, 1, Models, ARRAYSIZE(Models), NULL) || !Models[0])
            continue;

        /* Without a decoration for this platform the plain models section is looked at */
        if (!SetupDiGetActualModelsSectionW(&Manufacturer, NULL, Models, ARRAYSIZE(Models), NULL, NULL) &&
            !SetupGetStringFieldW(&Manufacturer, 1, Models, ARRAYSIZE(Models), NULL))
        {
            DifxLogError(DIFXAPI_ERROR, GetLastError(), L"An error ocurred while parsing [%s] section.", L"Manufacturer");
            continue;
        }

        if (!Models[0])
            continue;

        DifxLog(DIFXAPI_INFO, L"Looking for Model Section [%s]...", Models);

        for (FoundModel = SetupFindFirstLineW(hInf, Models, NULL, &Model);
             FoundModel;
             FoundModel = SetupFindNextLine(&Model, &Model))
        {
            /* The install section comes first, then the hardware and compatible IDs */
            FieldCount = SetupGetFieldCount(&Model);
            for (Field = 2; Field <= FieldCount; Field++)
            {
                if (!SetupGetStringFieldW(&Model, Field, InfId, ARRAYSIZE(InfId), NULL) || !InfId[0])
                    continue;

                IdCount++;
                DifxMatchInfId(Machine, InfId, Matches);
            }
        }
    }

    return IdCount ? ERROR_SUCCESS : ERROR_NO_DEVICE_ID;
}

/* Opens the INF and lists the devices it matches */
static
DWORD
WINAPI
DifxGetInfMatches(
    _In_ PCWSTR InfPath,
    _In_ DWORD OpenError,
    _Outptr_result_maybenull_ PWSTR *Matches)
{
    DIFX_MACHINE Machine;
    DWORD Error;
    HINF hInf;

    *Matches = NULL;

    /* Without the device list nothing matches, which is not an error of its own */
    if (DifxBuildMachine(&Machine) != ERROR_SUCCESS)
        WARN("Cannot list the devices of the machine\n");

    hInf = SetupOpenInfFileW(InfPath, NULL, INF_STYLE_WIN4, NULL);
    if (hInf == INVALID_HANDLE_VALUE)
    {
        Error = GetLastError();
        DifxLogError(DIFXAPI_INFO, Error, L"Could not open file %s.", InfPath);
        DifxFreeMachine(&Machine);
        return OpenError ? OpenError : Error;
    }

    Error = DifxFindInfMatches(hInf, &Machine, Matches);
    SetupCloseInfFile(hInf);
    DifxFreeMachine(&Machine);

    if (Error == ERROR_SUCCESS && !*Matches)
    {
        DifxLog(DIFXAPI_INFO, L"No matching devices found in INF \"%ws\" on the Machine.", InfPath);
        Error = ERROR_NO_SUCH_DEVINST;
    }

    return Error;
}

static
BOOL
WINAPI
DifxDeviceHasId(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ PCWSTR Id)
{
    WCHAR IdList[DIFX_ID_LIST_SIZE / sizeof(WCHAR) + 2];
    static const DWORD Properties[] = { SPDRP_HARDWAREID, SPDRP_COMPATIBLEIDS };
    PCWSTR Entry;
    ULONG i;

    for (i = 0; i < ARRAYSIZE(Properties); i++)
    {
        if (!DifxReadIdList(DeviceInfoSet, DeviceInfoData, Properties[i], IdList))
            continue;

        for (Entry = IdList; *Entry; Entry += wcslen(Entry) + 1)
        {
            if (!_wcsicmp(Entry, Id))
                return TRUE;
        }
    }

    return FALSE;
}

/* Whether the class installer picked a driver from the INF */
static
BOOL
WINAPI
DifxIsSelectedFromInf(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PCWSTR InfPath,
    _In_ PSP_DEVINFO_DATA DeviceInfoData)
{
    BYTE DetailBuffer[sizeof(SP_DRVINFO_DETAIL_DATA_W) + 2 * DIFX_ID_LIST_SIZE];
    PSP_DRVINFO_DETAIL_DATA_W Detail = (PSP_DRVINFO_DETAIL_DATA_W)DetailBuffer;
    SP_DRVINFO_DATA_W DriverInfoData;

    DriverInfoData.cbSize = sizeof(DriverInfoData);
    if (!SetupDiGetSelectedDriverW(DeviceInfoSet, DeviceInfoData, &DriverInfoData))
        return FALSE;

    ZeroMemory(DetailBuffer, sizeof(DetailBuffer));
    Detail->cbSize = sizeof(*Detail);
    if (!SetupDiGetDriverInfoDetailW(DeviceInfoSet, DeviceInfoData, &DriverInfoData, Detail, sizeof(DetailBuffer), NULL) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        return FALSE;
    }

    if (!_wcsicmp(Detail->InfFileName, InfPath))
        return TRUE;

    return DifxIsStoreEntryOf(Detail->InfFileName, InfPath);
}

/**
 * @brief Lets the class installer choose between the current driver of a device and the INF.
 *
 * @return TRUE when the INF has the better driver.
 */
static
BOOL
WINAPI
DifxIsInfBetter(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PCWSTR InfPath,
    _In_ PSP_DEVINFO_DATA DeviceInfoData)
{
    SP_DEVINSTALL_PARAMS_W InstallParams;
    WCHAR CurrentInf[MAX_PATH];

    ZeroMemory(&InstallParams, sizeof(InstallParams));
    InstallParams.cbSize = sizeof(InstallParams);

    if (SetupDiGetDeviceInstallParamsW(DeviceInfoSet, DeviceInfoData, &InstallParams))
    {
        InstallParams.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;

        /* The device already runs this very entry */
        if (DifxGetDeviceInf(DeviceInfoSet, DeviceInfoData, CurrentInf) == ERROR_SUCCESS &&
            DifxIsStoreEntryOf(CurrentInf, InfPath))
        {
            return FALSE;
        }

        /* The current driver competes first */
        InstallParams.FlagsEx |= DI_FLAGSEX_INSTALLEDDRIVER;
        if (SetupDiSetDeviceInstallParamsW(DeviceInfoSet, DeviceInfoData, &InstallParams))
            SetupDiBuildDriverInfoList(DeviceInfoSet, DeviceInfoData, SPDIT_COMPATDRIVER);
    }

    InstallParams.cbSize = sizeof(InstallParams);
    if (SetupDiGetDeviceInstallParamsW(DeviceInfoSet, DeviceInfoData, &InstallParams))
    {
        if (FAILED(StringCchCopyW(InstallParams.DriverPath, ARRAYSIZE(InstallParams.DriverPath), InfPath)))
            return FALSE;

        /* Then the drivers of the INF join the list */
        InstallParams.Flags |= DI_ENUMSINGLEINF;
        InstallParams.FlagsEx &= ~DI_FLAGSEX_INSTALLEDDRIVER;
        InstallParams.FlagsEx |= DI_FLAGSEX_APPENDDRIVERLIST | DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
        if (SetupDiSetDeviceInstallParamsW(DeviceInfoSet, DeviceInfoData, &InstallParams))
            SetupDiBuildDriverInfoList(DeviceInfoSet, DeviceInfoData, SPDIT_COMPATDRIVER);
    }

    if (!SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV, DeviceInfoSet, DeviceInfoData))
        return FALSE;

    return DifxIsSelectedFromInf(DeviceInfoSet, InfPath, DeviceInfoData);
}

/**
 * @brief Looks for devices of the set with an ID and checks the INF against their drivers.
 *
 * @return ERROR_SUCCESS when the INF is better for one of them, ERROR_NO_MORE_ITEMS when
 *         devices were found but none would gain, ERROR_NO_SUCH_DEVINST when none has the ID.
 */
static
DWORD
WINAPI
DifxFindDevices(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PCWSTR InfPath,
    _In_ PCWSTR Id,
    _In_ BOOL OnlyIfBetter,
    _Inout_opt_ PWSTR *InstanceIds)
{
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    SP_DEVINFO_DATA DeviceInfoData;
    DWORD Index, Result = ERROR_NO_SUCH_DEVINST;
    BOOL Better;

    if (!*Id)
        return ERROR_INVALID_PARAMETER;

    if (InstanceIds && *InstanceIds)
        **InstanceIds = UNICODE_NULL;

    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    for (Index = 0; SetupDiEnumDeviceInfo(DeviceInfoSet, Index, &DeviceInfoData); Index++)
    {
        if (!DifxDeviceHasId(DeviceInfoSet, &DeviceInfoData, Id))
            continue;

        Better = DifxIsInfBetter(DeviceInfoSet, InfPath, &DeviceInfoData);
        if (Better)
        {
            Result = ERROR_SUCCESS;
        }
        else
        {
            if (Result == ERROR_NO_SUCH_DEVINST)
                Result = ERROR_NO_MORE_ITEMS;

            if (OnlyIfBetter)
                continue;
        }

        if (InstanceIds && DifxGetInstanceId(DeviceInfoSet, &DeviceInfoData, InstanceId))
            DifxListAppend(InstanceIds, InstanceId);
    }

    return Result;
}

/* Remembers the drivers the devices about to be updated have now */
static
VOID
WINAPI
DifxRecordDrivers(
    _In_ HDEVINFO DeviceInfoSet,
    _In_opt_ PCWSTR InstanceIds,
    _Inout_ PDIFX_PREVIOUS_DRIVER *Record)
{
    PDIFX_PREVIOUS_DRIVER Entry;
    SP_DEVINFO_DATA DeviceInfoData;
    PCWSTR InstanceId;

    if (!InstanceIds)
        return;

    for (InstanceId = InstanceIds; *InstanceId; InstanceId += wcslen(InstanceId) + 1)
    {
        for (Entry = *Record; Entry; Entry = Entry->Next)
        {
            if (!_wcsicmp(Entry->InstanceId, InstanceId))
                break;
        }

        if (Entry)
            continue;

        DeviceInfoData.cbSize = sizeof(DeviceInfoData);
        if (!SetupDiOpenDeviceInfoW(DeviceInfoSet, InstanceId, NULL, 0, &DeviceInfoData))
            continue;

        Entry = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Entry));
        if (!Entry)
            return;

        StringCchCopyW(Entry->InstanceId, ARRAYSIZE(Entry->InstanceId), InstanceId);
        DifxGetDeviceInf(DeviceInfoSet, &DeviceInfoData, Entry->InfPath);

        Entry->Next = *Record;
        *Record = Entry;
    }
}

static
VOID
WINAPI
DifxFreeRecord(
    _Inout_ PDIFX_PREVIOUS_DRIVER *Record)
{
    PDIFX_PREVIOUS_DRIVER Entry;

    while (*Record)
    {
        Entry = *Record;
        *Record = Entry->Next;
        DifxFree(Entry);
    }
}

/* Puts the best driver of one INF on one device */
static
DWORD
WINAPI
DifxInstallInfOnDevice(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PCWSTR InfPath,
    _In_ PCWSTR InstanceId,
    _In_ PDIFX_INSTALL_DEVICE InstallDevice,
    _Out_ PBOOL NeedReboot)
{
    SP_DEVINSTALL_PARAMS_W InstallParams;
    SP_DRVINFO_DATA_W DriverInfoData;
    SP_DEVINFO_DATA DeviceInfoData;

    *NeedReboot = FALSE;

    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    if (!SetupDiOpenDeviceInfoW(DeviceInfoSet, InstanceId, NULL, 0, &DeviceInfoData) ||
        !SetupDiSetSelectedDevice(DeviceInfoSet, &DeviceInfoData))
    {
        return GetLastError();
    }

    ZeroMemory(&InstallParams, sizeof(InstallParams));
    InstallParams.cbSize = sizeof(InstallParams);
    if (!SetupDiGetDeviceInstallParamsW(DeviceInfoSet, &DeviceInfoData, &InstallParams))
        return GetLastError();

    if (FAILED(StringCchCopyW(InstallParams.DriverPath, ARRAYSIZE(InstallParams.DriverPath), InfPath)))
        return ERROR_INVALID_PARAMETER;

    InstallParams.Flags |= DI_ENUMSINGLEINF;
    InstallParams.FlagsEx &= ~(DI_FLAGSEX_INSTALLEDDRIVER | DI_FLAGSEX_APPENDDRIVERLIST);
    InstallParams.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;

    if (!SetupDiSetDeviceInstallParamsW(DeviceInfoSet, &DeviceInfoData, &InstallParams) ||
        !SetupDiBuildDriverInfoList(DeviceInfoSet, &DeviceInfoData, SPDIT_COMPATDRIVER))
    {
        return GetLastError();
    }

    SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV, DeviceInfoSet, &DeviceInfoData);

    DriverInfoData.cbSize = sizeof(DriverInfoData);
    if (!SetupDiGetSelectedDriverW(DeviceInfoSet, &DeviceInfoData, &DriverInfoData))
        return GetLastError();

    if (!InstallDevice(NULL, DeviceInfoSet, &DeviceInfoData, &DriverInfoData, 0, NeedReboot))
        return GetLastError();

    return ERROR_SUCCESS;
}

/* Gives the devices recorded before a failed install their old drivers back */
static
DWORD
WINAPI
DifxRollback(
    _In_opt_ PDIFX_PREVIOUS_DRIVER Record,
    _Out_ PBOOL NeedReboot)
{
    PDIFX_INSTALL_DEVICE InstallDevice;
    PDIFX_PREVIOUS_DRIVER Entry;
    HDEVINFO DeviceInfoSet;
    HMODULE hNewDev;
    DWORD Error, Result = ERROR_SUCCESS;
    BOOL Reboot;

    if (!Record)
        return ERROR_NO_MORE_ITEMS;

    hNewDev = LoadLibraryW(L"newdev.dll");
    if (!hNewDev)
        return GetLastError();

    InstallDevice = (PDIFX_INSTALL_DEVICE)GetProcAddress(hNewDev, "DiInstallDevice");
    if (!InstallDevice)
    {
        Error = GetLastError();
        FreeLibrary(hNewDev);
        return Error;
    }

    DeviceInfoSet = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        Error = GetLastError();
        FreeLibrary(hNewDev);
        return Error;
    }

    for (Entry = Record; Entry; Entry = Entry->Next)
    {
        if (!Entry->InfPath[0])
        {
            Result = ERROR_FILE_NOT_FOUND;
            continue;
        }

        Error = DifxInstallInfOnDevice(DeviceInfoSet, Entry->InfPath, Entry->InstanceId, InstallDevice, &Reboot);
        if (Error != ERROR_SUCCESS)
            Result = Error;
        else if (Reboot)
            *NeedReboot = TRUE;
    }

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);
    FreeLibrary(hNewDev);
    return Result;
}

/* Installs the INF on the devices with one ID, when that improves one of them or when forced */
static
DWORD
WINAPI
DifxInstallOnId(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PCWSTR Id,
    _In_ PCWSTR StoreInf,
    _In_ BOOL Force,
    _In_ PDIFX_UPDATE_DRIVER UpdateDriver,
    _Inout_ PDIFX_PREVIOUS_DRIVER *Record,
    _Out_ PBOOL NeedReboot)
{
    PWSTR InstanceIds = NULL;
    DWORD Error;

    Error = DifxFindDevices(DeviceInfoSet, StoreInf, Id, !Force, &InstanceIds);
    if (Force && Error == ERROR_NO_MORE_ITEMS)
    {
        DifxLog(DIFXAPI_INFO, L"Will force install because driver is not better and force flag is set.");
        Error = ERROR_SUCCESS;
    }

    if (Error != ERROR_SUCCESS)
        goto Quit;

    DifxRecordDrivers(DeviceInfoSet, InstanceIds, Record);

    /* The comparison is done, so the update always goes ahead */
    DifxLog(DIFXAPI_INFO, L"ENTER UpdateDriverForPlugAndPlayDevices...");
    if (UpdateDriver(NULL, Id, StoreInf, INSTALLFLAG_FORCE, NeedReboot))
    {
        DifxLog(DIFXAPI_SUCCESS, L"RETURN UpdateDriverForPlugAndPlayDevices.");
    }
    else
    {
        Error = GetLastError();
        if (Error != ERROR_SUCCESS)
            DifxLogError(DIFXAPI_ERROR, Error, L"RETURN UpdateDriverForPlugAndPlayDevices.");
    }

Quit:
    DifxFree(InstanceIds);
    return Error;
}

/* Phantom devices matching the INF install it the next time they show up */
static
VOID
WINAPI
DifxMarkPhantoms(
    _In_ PCWSTR StoreInf,
    _In_ PCWSTR Matches)
{
    WCHAR IdList[DIFX_ID_LIST_SIZE / sizeof(WCHAR) + 2];
    WCHAR OemDirectory[MAX_PATH];
    WCHAR OemInf[MAX_PATH];
    WCHAR DeviceInf[MAX_PATH];
    static const DWORD Properties[] = { SPDRP_HARDWAREID, SPDRP_COMPATIBLEIDS };
    SP_DEVINFO_DATA DeviceInfoData;
    HDEVINFO DeviceInfoSet;
    PCWSTR OemName, Id, Match;
    DWORD Index, Error;
    BOOL Handled, Read;
    ULONG i;
    HKEY hKey;

    if (!DifxGetOemInf(StoreInf, OemInf))
    {
        DifxLog(DIFXAPI_WARNING, L"PnP driver package '%s' has no OEM Inf file.", StoreInf);
        return;
    }

    if (!DifxSplitPath(OemInf, OemDirectory, ARRAYSIZE(OemDirectory), &OemName))
    {
        DifxLog(DIFXAPI_ERROR, L"Failed to obtain OEM Inf filename from '%s' .", OemInf);
        return;
    }

    DeviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
        return;

    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    for (Index = 0; SetupDiEnumDeviceInfo(DeviceInfoSet, Index, &DeviceInfoData); Index++)
    {
        if (!DifxIsPhantom(&DeviceInfoData))
            continue;

        Handled = FALSE;
        for (i = 0; i < ARRAYSIZE(Properties) && !Handled; i++)
        {
            if (!DifxReadIdList(DeviceInfoSet, &DeviceInfoData, Properties[i], IdList))
                continue;

            for (Id = IdList; *Id && !Handled; Id += wcslen(Id) + 1)
            {
                for (Match = Matches; *Match; Match += wcslen(Match) + 1)
                {
                    if (_wcsicmp(Match, Id))
                        continue;

                    Handled = TRUE;

                    hKey = SetupDiOpenDevRegKey(DeviceInfoSet, &DeviceInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
                    if (hKey != INVALID_HANDLE_VALUE)
                    {
                        RegCloseKey(hKey);

                        /* A driver key that names no INF is left alone */
                        if (DifxGetDeviceInf(DeviceInfoSet, &DeviceInfoData, DeviceInf) != ERROR_SUCCESS)
                            continue;

                        if (!_wcsicmp(DeviceInf, OemName))
                        {
                            DifxLog(DIFXAPI_INFO,
                                    L"Phantom Device with Hardware/Compatible Id '%s' already has '%s' installed on it.",
                                    Match,
                                    StoreInf);
                            continue;
                        }
                    }
                    else
                    {
                        Error = GetLastError();
                        if (Error != ERROR_KEY_DOES_NOT_EXIST)
                        {
                            DifxLog(DIFXAPI_ERROR,
                                    L"Failed to open driver key with error 0x%x for Phantom Device with Hardware/Compatible Id '%s'.",
                                    Error,
                                    Match);
                            continue;
                        }
                    }

                    if (DifxMarkForReinstall(DeviceInfoSet, &DeviceInfoData, &Read) == ERROR_SUCCESS)
                    {
                        DifxLog(DIFXAPI_INFO,
                                L"Marked Phantom Device with Hardware/Compatible Id '%s' for reinstall on next plug-in.",
                                Match);
                    }
                    else if (Read)
                    {
                        DifxLog(DIFXAPI_ERROR,
                                L"Failed to set Phantom Device with Hardware/Compatible Id '%s' for reinstall.",
                                Match);
                    }
                    else
                    {
                        DifxLog(DIFXAPI_INFO,
                                L"Failed to get reinstall data for Phantom Device with Hardware/Compatible Id '%s'.",
                                Match);
                    }
                }
            }
        }
    }

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);
}

/* Installs the INF on every present device it is better for, or on every one when forced */
static
DWORD
WINAPI
DifxInstallOnDevices(
    _In_ PCWSTR StoreInf,
    _In_ BOOL Force,
    _Inout_ PDIFX_PREVIOUS_DRIVER *Record,
    _Out_ PBOOL NeedReboot)
{
    PDIFX_UPDATE_DRIVER UpdateDriver;
    HDEVINFO DeviceInfoSet;
    HMODULE hNewDev;
    PWSTR Matches;
    PCWSTR Id;
    DWORD Error, Result;
    BOOL Stopped = FALSE;

    Error = DifxGetInfMatches(StoreInf, ERROR_FILE_INVALID, &Matches);
    if (Error != ERROR_SUCCESS)
        return Error;

    hNewDev = LoadLibraryW(L"newdev.dll");
    if (!hNewDev)
    {
        Result = GetLastError();
        goto Quit;
    }

    UpdateDriver = (PDIFX_UPDATE_DRIVER)GetProcAddress(hNewDev, "UpdateDriverForPlugAndPlayDevicesW");
    if (!UpdateDriver)
    {
        Result = GetLastError();
        goto Unload;
    }

    DeviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        Result = GetLastError();
        goto Unload;
    }

    Result = ERROR_NO_SUCH_DEVINST;
    for (Id = Matches; *Id && !Stopped; Id += wcslen(Id) + 1)
    {
        DifxLog(DIFXAPI_INFO, L"Installing devices with Id \"%ws\" using INF \"%ws\".", Id, StoreInf);

        Error = DifxInstallOnId(DeviceInfoSet, Id, StoreInf, Force, UpdateDriver, Record, NeedReboot);
        switch (Error)
        {
            case ERROR_SUCCESS:
                if (Result == ERROR_NO_SUCH_DEVINST || Result == ERROR_NO_MORE_ITEMS)
                    Result = ERROR_SUCCESS;

                DifxLog(DIFXAPI_INFO, L"Installation was successful.");
                break;

            case ERROR_NO_MORE_ITEMS:
                if (Result == ERROR_NO_SUCH_DEVINST)
                    Result = ERROR_NO_MORE_ITEMS;

                DifxLog(DIFXAPI_INFO,
                        L"Installation did not occur because the current driver on the device is the same or better.");
                break;

            case ERROR_NO_SUCH_DEVINST:
                DifxLog(DIFXAPI_INFO, L"Installation did not occur because the hardware isn't currently present.");
                break;

            case (DWORD)CERT_E_WRONG_USAGE:
                Result = Error;
                Stopped = TRUE;
                DifxLogError(DIFXAPI_WARNING,
                             Error,
                             L"Non-WHQL signed package rejected either by user or because of non-UI mode.");
                break;

            default:
                Result = Error;
                Stopped = TRUE;
                DifxLogError(DIFXAPI_ERROR, Error, L"Installation failed.");
                break;
        }
    }

    if (!Stopped)
        DifxMarkPhantoms(StoreInf, Matches);

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);

Unload:
    FreeLibrary(hNewDev);

Quit:
    DifxFree(Matches);
    return Result;
}

/* Reinstalls the INF on the devices already using it */
static
DWORD
WINAPI
DifxRepairDevices(
    _In_ PCWSTR StoreInf,
    _Out_ PBOOL NeedReboot)
{
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    WCHAR OemDirectory[MAX_PATH];
    WCHAR OemInf[MAX_PATH];
    WCHAR DeviceInf[MAX_PATH];
    PDIFX_INSTALL_DEVICE InstallDevice;
    SP_DEVINFO_DATA DeviceInfoData;
    HDEVINFO DeviceInfoSet, SingleSet;
    HMODULE hNewDev;
    PCWSTR OemName;
    DWORD Index, Error, Result = ERROR_SUCCESS;
    BOOL Matched, Failed = FALSE, Incomplete = FALSE, Reboot, Read;
    HKEY hKey;

    DifxLog(DIFXAPI_INFO, L"Attempting to repair driver with INF '%s'.", StoreInf);

    hNewDev = LoadLibraryW(L"newdev.dll");
    if (!hNewDev)
        return GetLastError();

    InstallDevice = (PDIFX_INSTALL_DEVICE)GetProcAddress(hNewDev, "DiInstallDevice");
    if (!InstallDevice)
    {
        Result = GetLastError();
        goto Unload;
    }

    OemInf[0] = UNICODE_NULL;
    if ((!SetupCopyOEMInfW(StoreInf, NULL, SPOST_NONE, SP_COPY_NOOVERWRITE, OemInf, ARRAYSIZE(OemInf), NULL, NULL) &&
         GetLastError() != ERROR_FILE_EXISTS) ||
        !DifxSplitPath(OemInf, OemDirectory, ARRAYSIZE(OemDirectory), &OemName))
    {
        DifxLog(DIFXAPI_ERROR, L"Unable to retrieve the OEM inf corresponding to %ws.", StoreInf);
        Result = ERROR_FUNCTION_FAILED;
        goto Unload;
    }

    DifxLog(DIFXAPI_INFO, L"'%s' matches OEM Inf '%s'.", StoreInf, OemInf);

    DeviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        Result = GetLastError();
        goto Unload;
    }

    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    for (Index = 0; SetupDiEnumDeviceInfo(DeviceInfoSet, Index, &DeviceInfoData); Index++)
    {
        Matched = FALSE;
        Error = ERROR_SUCCESS;

        if (!DifxGetInstanceId(DeviceInfoSet, &DeviceInfoData, InstanceId))
        {
            Error = GetLastError();
            goto Next;
        }

        hKey = SetupDiOpenDevRegKey(DeviceInfoSet, &DeviceInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (hKey == INVALID_HANDLE_VALUE)
        {
            Error = GetLastError();
            if (Error != ERROR_KEY_DOES_NOT_EXIST)
                DifxLog(DIFXAPI_WARNING, L"Unable to open driver key for device %ws", InstanceId);
            goto Next;
        }
        RegCloseKey(hKey);

        Error = DifxGetDeviceInf(DeviceInfoSet, &DeviceInfoData, DeviceInf);
        if (Error != ERROR_SUCCESS)
        {
            DifxLog(DIFXAPI_WARNING, L"Unable to get the OEM Inf for device %ws", InstanceId);
            goto Next;
        }

        if (wcscmp(DeviceInf, OemName))
            goto Next;

        Matched = TRUE;
        DifxLog(DIFXAPI_INFO, L"The OEM Inf '%s' for device '%s' matches the driver being repaired.", OemInf, InstanceId);

        if (!DifxIsPhantom(&DeviceInfoData))
        {
            DifxLog(DIFXAPI_INFO,
                    L"The device '%s' is not a phantom. Hence the driver will be installed on the device.",
                    InstanceId);

            SingleSet = SetupDiCreateDeviceInfoList(NULL, NULL);
            if (SingleSet == INVALID_HANDLE_VALUE)
            {
                Error = GetLastError();
                goto Next;
            }

            Error = DifxInstallInfOnDevice(SingleSet, StoreInf, InstanceId, InstallDevice, &Reboot);
            if (Error == ERROR_SUCCESS && Reboot)
                *NeedReboot = TRUE;

            SetupDiDestroyDeviceInfoList(SingleSet);
        }
        else
        {
            DifxLog(DIFXAPI_INFO, L"The device '%s' is a phantom and will be marked as needing reinstall.", InstanceId);
            Error = DifxMarkForReinstall(DeviceInfoSet, &DeviceInfoData, &Read);
        }

Next:
        /* A failure on a device using the INF fails the repair, any other only warns */
        if (Error != ERROR_SUCCESS)
        {
            if (Matched)
            {
                DifxLog(DIFXAPI_ERROR, L"Unable to repair the driver on device %ws", InstanceId);
                Failed = TRUE;
            }
            else
            {
                Incomplete = TRUE;
            }
        }
    }

    Error = GetLastError();
    if (Error != ERROR_NO_MORE_ITEMS)
        Result = Error;

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);

    if (Result == ERROR_SUCCESS)
    {
        if (Failed)
        {
            DifxLog(DIFXAPI_ERROR,
                    L"Repair failed. We were unable to reinstall the driver on at least one of the devices.");
            Result = ERROR_INSTALL_FAILURE;
        }
        else if (Incomplete)
        {
            DifxLog(DIFXAPI_WARNING, L"The driver may not have been reinstalled on all the matching devices.");
        }
    }

Unload:
    FreeLibrary(hNewDev);
    return Result;
}

DWORD
WINAPI
DifxProbeDevices(
    _In_ PCWSTR InfPath)
{
    HDEVINFO DeviceInfoSet;
    PWSTR Matches;
    PCWSTR Id;
    DWORD Error, Result;

    Error = DifxGetInfMatches(InfPath, ERROR_SUCCESS, &Matches);
    if (Error != ERROR_SUCCESS)
        return Error;

    DeviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        Error = GetLastError();
        DifxFree(Matches);
        return Error;
    }

    /* Any present device gaining from the INF is enough */
    Result = ERROR_NO_SUCH_DEVINST;
    for (Id = Matches; *Id; Id += wcslen(Id) + 1)
    {
        Error = DifxFindDevices(DeviceInfoSet, InfPath, Id, TRUE, NULL);
        if (Error == ERROR_SUCCESS)
        {
            Result = ERROR_SUCCESS;
        }
        else if (Error == ERROR_NO_MORE_ITEMS)
        {
            if (Result == ERROR_NO_SUCH_DEVINST)
                Result = ERROR_NO_MORE_ITEMS;
        }
        else if (Error != ERROR_NO_SUCH_DEVINST)
        {
            DifxLogError(DIFXAPI_ERROR, Error, L"Hardware probing failed.");
            Result = Error;
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);
    DifxFree(Matches);
    return Result;
}

DWORD
WINAPI
DifxInstallPnp(
    _In_ PCWSTR StoreInf,
    _In_ BOOL Force,
    _In_ BOOL Repair,
    _Out_ PBOOL NeedReboot)
{
    PDIFX_PREVIOUS_DRIVER Record = NULL;
    DWORD Error, RollbackError;

    *NeedReboot = FALSE;

    if (Repair)
        Error = DifxRepairDevices(StoreInf, NeedReboot);
    else
        Error = DifxInstallOnDevices(StoreInf, Force, &Record, NeedReboot);

    switch (Error)
    {
        case ERROR_SUCCESS:
            DifxLog(DIFXAPI_SUCCESS, L"Install completed");
            break;

        case ERROR_NO_MORE_ITEMS:
            DifxLog(DIFXAPI_INFO,
                    L"No drivers installed. Drivers contained in '%s' are not better than current one's.",
                    StoreInf);
            break;

        case ERROR_NO_SUCH_DEVINST:
            DifxLog(DIFXAPI_INFO,
                    L"No drivers installed. No devices found that match driver(s) contained in '%s'.",
                    StoreInf);
            break;

        case ERROR_NO_DEVICE_ID:
            DifxLog(DIFXAPI_WARNING, L"No device Ids found in INF '%s' for current platform.", StoreInf);
            break;

        case (DWORD)CERT_E_WRONG_USAGE:
            DifxLog(DIFXAPI_WARNING, L"User refused non WHQL-signed package.");
            break;

        default:
            if (Repair)
                break;

            DifxLogError(DIFXAPI_ERROR, Error, L"PnP Install failed.");
            DifxLog(DIFXAPI_INFO, L"Attempting to rollback ...");

            RollbackError = DifxRollback(Record, NeedReboot);
            if (RollbackError == ERROR_SUCCESS)
                DifxLog(DIFXAPI_SUCCESS, L"Rollback completed");
            else if (RollbackError == ERROR_NO_MORE_ITEMS)
                DifxLog(DIFXAPI_INFO, L"No devices to rollback");
            else if (RollbackError == ERROR_FILE_NOT_FOUND)
                DifxLog(DIFXAPI_ERROR, L"Could not find OEM Inf entry for driver package");
            else
                DifxLogError(DIFXAPI_ERROR, RollbackError, L"Rollback failed.");
            break;
    }

    DifxFreeRecord(&Record);
    return Error;
}

DWORD
WINAPI
DifxUninstallPnp(
    _In_ PCWSTR StoreInf,
    _In_ BOOL DeleteFiles,
    _Out_ PBOOL NeedReboot)
{
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    WCHAR OemDirectory[MAX_PATH];
    WCHAR OemInf[MAX_PATH];
    WCHAR DeviceInf[MAX_PATH];
    PDIFX_INSTALL_DEVICE InstallDevice;
    SP_DEVINFO_DATA DeviceInfoData;
    HDEVINFO DeviceInfoSet;
    HMODULE hNewDev;
    PCWSTR OemName;
    DWORD Index, Error, Result = ERROR_SUCCESS;
    ULONG Handled = 0;
    BOOL Reboot, Read;

    if (!*StoreInf || GetFileAttributesW(StoreInf) == INVALID_FILE_ATTRIBUTES)
        return ERROR_INVALID_PARAMETER;

    *NeedReboot = FALSE;

    if (!DifxGetOemInf(StoreInf, OemInf))
    {
        DifxLog(DIFXAPI_WARNING, L"PnP driver package '%s' has no OEM Inf file.", StoreInf);
        return ERROR_SUCCESS;
    }

    hNewDev = LoadLibraryW(L"newdev.dll");
    if (!hNewDev)
        return GetLastError();

    InstallDevice = (PDIFX_INSTALL_DEVICE)GetProcAddress(hNewDev, "DiInstallDevice");
    if (!InstallDevice)
    {
        Result = GetLastError();
        goto Unload;
    }

    if (!DifxSplitPath(OemInf, OemDirectory, ARRAYSIZE(OemDirectory), &OemName))
    {
        Result = ERROR_INVALID_PARAMETER;
        goto Unload;
    }

    /* Removing the published INF takes the package out of the store */
    if (!SetupUninstallOEMInfW(OemName, SUOI_FORCEDELETE | (DeleteFiles ? SUOI_INTERNAL1 : 0), NULL))
    {
        Result = GetLastError();
        DifxLogError(DIFXAPI_ERROR, Result, L"Could not uninstall '%s'.", OemInf);
        goto Unload;
    }

    DifxLog(DIFXAPI_INFO, L"Successfully uninstalled '%s'.", OemInf);

    DeviceInfoSet = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (DeviceInfoSet == INVALID_HANDLE_VALUE)
    {
        Result = GetLastError();
        goto Unload;
    }

    /* The devices that used the package get their next best driver */
    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    for (Index = 0; SetupDiEnumDeviceInfo(DeviceInfoSet, Index, &DeviceInfoData); Index++)
    {
        if (DifxGetDeviceInf(DeviceInfoSet, &DeviceInfoData, DeviceInf) != ERROR_SUCCESS ||
            _wcsicmp(DeviceInf, OemName))
        {
            continue;
        }

        DifxGetInstanceId(DeviceInfoSet, &DeviceInfoData, InstanceId);

        if (DifxIsPhantom(&DeviceInfoData))
        {
            Error = DifxMarkForReinstall(DeviceInfoSet, &DeviceInfoData, &Read);
            if (Error == ERROR_SUCCESS)
            {
                if (InstanceId[0])
                    DifxLog(DIFXAPI_SUCCESS, L"Phantom device with device instance ID '%s' marked for reinstall.", InstanceId);
                Handled++;
            }
            else if (InstanceId[0])
            {
                DifxLogError(DIFXAPI_ERROR, Error, L"Failed to uninstall device with device instance ID '%s'.", InstanceId);
            }
            else
            {
                DifxLogError(DIFXAPI_ERROR, Error, L"Failed to uninstall device.");
            }

            continue;
        }

        Reboot = FALSE;
        Error = ERROR_SUCCESS;
        if (!InstallDevice(NULL, DeviceInfoSet, &DeviceInfoData, NULL, 0, &Reboot))
        {
            Error = GetLastError();

            /* No other driver fits, so the device goes without one */
            if (Error == ERROR_NO_DRIVER_SELECTED)
            {
                Error = ERROR_SUCCESS;
                if (!InstallDevice(NULL, DeviceInfoSet, &DeviceInfoData, NULL, DIIDFLAG_INSTALLNULLDRIVER, &Reboot))
                    Error = GetLastError();
            }
        }

        if (Error == ERROR_SUCCESS)
        {
            if (InstanceId[0])
                DifxLog(DIFXAPI_SUCCESS, L"Device with device instance ID '%s' reinstalled.", InstanceId);
            Handled++;
        }
        else if (InstanceId[0])
        {
            DifxLogError(DIFXAPI_ERROR, Error, L"Failed to uninstall device instance ID '%s'.", InstanceId);
        }
        else
        {
            DifxLogError(DIFXAPI_ERROR, Error, L"Failed to uninstall device.");
        }

        if (Reboot)
            *NeedReboot = TRUE;
    }

    SetupDiDestroyDeviceInfoList(DeviceInfoSet);

    if (Handled)
        DifxLog(DIFXAPI_INFO, L"Successfully uninstalled %u devices for %s.", Handled, StoreInf);
    else
        DifxLog(DIFXAPI_INFO, L"No devices found for %s uninstall.", StoreInf);

Unload:
    FreeLibrary(hNewDev);
    return Result;
}
