/*
 * New device installer (newdev.dll)
 *
 * Copyright 2005-2006 Hervé Poussineau (hpoussin@reactos.org)
 *           2005 Christoph von Wittich (Christoph@ActiveVB.de)
 *           2009 Colin Finck (colin@reactos.org)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "newdev_private.h"

#include <stdio.h>
#include <winnls.h>
#include <wincon.h>
#include <cfgmgr32.h>
#include <shlobj.h>

/* Global variables */
HINSTANCE hDllInstance;

static BOOL
SearchDriver(
    IN PDEVINSTDATA DevInstData,
    IN LPCWSTR Directory OPTIONAL,
    IN LPCWSTR InfFile OPTIONAL);

/*
* @implemented
*/
BOOL WINAPI
UpdateDriverForPlugAndPlayDevicesW(
    IN HWND hwndParent,
    IN LPCWSTR HardwareId,
    IN LPCWSTR FullInfPath,
    IN DWORD InstallFlags,
    OUT PBOOL bRebootRequired OPTIONAL)
{
    DEVINSTDATA DevInstData;
    SP_DEVINSTALL_PARAMS InstallParams;
    DWORD i;
    LPWSTR Buffer = NULL;
    DWORD BufferSize;
    LPCWSTR CurrentHardwareId; /* Pointer into Buffer */
    DWORD Property;
    BOOL FoundHardwareId, FoundAtLeastOneDevice = FALSE;
    BOOL RebootRequired = FALSE;
    BOOL ret = FALSE;

    DevInstData.hDevInfo = INVALID_HANDLE_VALUE;

    TRACE("UpdateDriverForPlugAndPlayDevicesW(%p %s %s 0x%x %p)\n",
        hwndParent, debugstr_w(HardwareId), debugstr_w(FullInfPath), InstallFlags, bRebootRequired);

    /* Check flags */
    if (InstallFlags & ~(INSTALLFLAG_FORCE | INSTALLFLAG_READONLY | INSTALLFLAG_NONINTERACTIVE))
    {
        TRACE("Unknown flags: 0x%08lx\n", InstallFlags & ~(INSTALLFLAG_FORCE | INSTALLFLAG_READONLY | INSTALLFLAG_NONINTERACTIVE));
        SetLastError(ERROR_INVALID_FLAGS);
        goto cleanup;
    }

    /* Enumerate all devices of the system */
    DevInstData.hDevInfo = SetupDiGetClassDevsW(NULL, NULL, hwndParent, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (DevInstData.hDevInfo == INVALID_HANDLE_VALUE)
        goto cleanup;
    DevInstData.devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    for (i = 0; ; i++)
    {
        if (!SetupDiEnumDeviceInfo(DevInstData.hDevInfo, i, &DevInstData.devInfoData))
        {
            if (GetLastError() != ERROR_NO_MORE_ITEMS)
            {
                TRACE("SetupDiEnumDeviceInfo() failed with error 0x%x\n", GetLastError());
                goto cleanup;
            }
            /* This error was expected */
            break;
        }

        /* Match Hardware ID */
        FoundHardwareId = FALSE;
        Property = SPDRP_HARDWAREID;
        while (TRUE)
        {
            /* Get IDs data */
            Buffer = NULL;
            BufferSize = 0;
            while (!SetupDiGetDeviceRegistryPropertyW(DevInstData.hDevInfo,
                                                      &DevInstData.devInfoData,
                                                      Property,
                                                      NULL,
                                                      (PBYTE)Buffer,
                                                      BufferSize,
                                                      &BufferSize))
            {
                if (GetLastError() == ERROR_FILE_NOT_FOUND)
                {
                    break;
                }
                else if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                {
                    TRACE("SetupDiGetDeviceRegistryPropertyW() failed with error 0x%x\n", GetLastError());
                    goto cleanup;
                }
                /* This error was expected */
                HeapFree(GetProcessHeap(), 0, Buffer);
                Buffer = HeapAlloc(GetProcessHeap(), 0, BufferSize);
                if (!Buffer)
                {
                    TRACE("HeapAlloc() failed\n", GetLastError());
                    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                    goto cleanup;
                }
            }
            if (Buffer)
            {
                /* Check if we match the given hardware ID */
                for (CurrentHardwareId = Buffer; *CurrentHardwareId != UNICODE_NULL; CurrentHardwareId += wcslen(CurrentHardwareId) + 1)
                {
                    if (_wcsicmp(CurrentHardwareId, HardwareId) == 0)
                    {
                        FoundHardwareId = TRUE;
                        break;
                    }
                }
            }
            if (FoundHardwareId || Property == SPDRP_COMPATIBLEIDS)
            {
                break;
            }
            Property = SPDRP_COMPATIBLEIDS;
        }
        if (!FoundHardwareId)
            continue;

        /* We need to try to update the driver of this device */

        /* Get Instance ID */
        HeapFree(GetProcessHeap(), 0, Buffer);
        Buffer = NULL;
        if (SetupDiGetDeviceInstanceIdW(DevInstData.hDevInfo, &DevInstData.devInfoData, NULL, 0, &BufferSize))
        {
            /* Error, as the output buffer should be too small */
            SetLastError(ERROR_GEN_FAILURE);
            goto cleanup;
        }
        else if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            TRACE("SetupDiGetDeviceInstanceIdW() failed with error 0x%x\n", GetLastError());
            goto cleanup;
        }
        else if ((Buffer = HeapAlloc(GetProcessHeap(), 0, BufferSize * sizeof(WCHAR))) == NULL)
        {
            TRACE("HeapAlloc() failed\n", GetLastError());
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            goto cleanup;
        }
        else if (!SetupDiGetDeviceInstanceIdW(DevInstData.hDevInfo, &DevInstData.devInfoData, Buffer, BufferSize, NULL))
        {
            TRACE("SetupDiGetDeviceInstanceIdW() failed with error 0x%x\n", GetLastError());
            goto cleanup;
        }
        TRACE("Trying to update the driver of %s\n", debugstr_w(Buffer));

        /* Search driver in the specified .inf file */
        if (!SearchDriver(&DevInstData, NULL, FullInfPath))
        {
            TRACE("SearchDriver() failed with error 0x%x\n", GetLastError());
            continue;
        }

        /* FIXME: HACK! We shouldn't check of ERROR_PRIVILEGE_NOT_HELD */
        //if (!InstallCurrentDriver(&DevInstData))
        if (!InstallCurrentDriver(&DevInstData) && GetLastError() != ERROR_PRIVILEGE_NOT_HELD)
        {
            TRACE("InstallCurrentDriver() failed with error 0x%x\n", GetLastError());
            continue;
        }

        /* Do we need to reboot? */
        InstallParams.cbSize = sizeof(InstallParams);
        if (SetupDiGetDeviceInstallParams(DevInstData.hDevInfo,
                                          &DevInstData.devInfoData,
                                          &InstallParams))
        {
            if (InstallParams.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT))
                RebootRequired = TRUE;
        }

        FoundAtLeastOneDevice = TRUE;
    }

    if (FoundAtLeastOneDevice)
    {
        if (bRebootRequired)
            *bRebootRequired = RebootRequired;

        SetLastError(NO_ERROR);
        ret = TRUE;
    }
    else
    {
        TRACE("No device found with HardwareID %s\n", debugstr_w(HardwareId));
        SetLastError(ERROR_NO_SUCH_DEVINST);
    }

cleanup:
    if (DevInstData.hDevInfo != INVALID_HANDLE_VALUE)
        SetupDiDestroyDeviceInfoList(DevInstData.hDevInfo);
    HeapFree(GetProcessHeap(), 0, Buffer);
    return ret;
}

/*
* @implemented
*/
BOOL WINAPI
UpdateDriverForPlugAndPlayDevicesA(
    IN HWND hwndParent,
    IN LPCSTR HardwareId,
    IN LPCSTR FullInfPath,
    IN DWORD InstallFlags,
    OUT PBOOL bRebootRequired OPTIONAL)
{
    BOOL Result;
    LPWSTR HardwareIdW = NULL;
    LPWSTR FullInfPathW = NULL;

    int len = MultiByteToWideChar(CP_ACP, 0, HardwareId, -1, NULL, 0);
    HardwareIdW = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));
    if (!HardwareIdW)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    MultiByteToWideChar(CP_ACP, 0, HardwareId, -1, HardwareIdW, len);

    len = MultiByteToWideChar(CP_ACP, 0, FullInfPath, -1, NULL, 0);
    FullInfPathW = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));
    if (!FullInfPathW)
    {
        HeapFree(GetProcessHeap(), 0, HardwareIdW);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    MultiByteToWideChar(CP_ACP, 0, FullInfPath, -1, FullInfPathW, len);

    Result = UpdateDriverForPlugAndPlayDevicesW(
        hwndParent,
        HardwareIdW,
        FullInfPathW,
        InstallFlags,
        bRebootRequired);

    HeapFree(GetProcessHeap(), 0, HardwareIdW);
    HeapFree(GetProcessHeap(), 0, FullInfPathW);

    return Result;
}

static BOOL
IsCallerLocalSystem(VOID)
{
    BYTE SidBuffer[SECURITY_MAX_SID_SIZE];
    DWORD SidSize = sizeof(SidBuffer);
    BOOL IsMember = FALSE;

    if (!CreateWellKnownSid(WinLocalSystemSid, NULL, SidBuffer, &SidSize))
        return FALSE;

    if (!CheckTokenMembership(NULL, SidBuffer, &IsMember))
        return FALSE;

    return IsMember;
}

static BOOL
IsRunningInWow64(VOID)
{
    BOOL Wow64 = FALSE;

    if (!IsWow64Process(GetCurrentProcess(), &Wow64))
        return FALSE;

    return Wow64;
}

static BOOL
DeviceNeedsReboot(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData)
{
    SP_DEVINSTALL_PARAMS_W InstallParams;

    InstallParams.cbSize = sizeof(InstallParams);
    if (!SetupDiGetDeviceInstallParamsW(DeviceInfoSet, DeviceInfoData, &InstallParams))
        return FALSE;

    return (InstallParams.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0;
}

/* Hands the reboot state to the caller, or asks the user when there is nowhere to report it */
static VOID
ReportReboot(
    _In_opt_ HWND hwndParent,
    _In_ BOOL RebootNeeded,
    _Out_opt_ PBOOL NeedReboot)
{
    if (NeedReboot)
        *NeedReboot = RebootNeeded;
    else if (RebootNeeded)
        RestartDialogEx(hwndParent,
                        NULL,
                        EWX_REBOOT,
                        SHTDN_REASON_FLAG_PLANNED | SHTDN_REASON_MAJOR_HARDWARE | SHTDN_REASON_MINOR_INSTALLATION);
}

/* Runs the class installer steps that put the selected driver on the device */
static BOOL
InstallSelectedDriverOnDevice(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData)
{
    static const DI_FUNCTION InstallSteps[] =
    {
        DIF_ALLOW_INSTALL,
        DIF_INSTALLDEVICEFILES,
        DIF_REGISTER_COINSTALLERS,
        DIF_INSTALLINTERFACES,
        DIF_INSTALLDEVICE
    };
    ULONG i;

    for (i = 0; i < ARRAYSIZE(InstallSteps); i++)
    {
        if (!SetupDiCallClassInstaller(InstallSteps[i], DeviceInfoSet, DeviceInfoData))
        {
            TRACE("SetupDiCallClassInstaller(0x%lx) failed with error 0x%lx\n", InstallSteps[i], GetLastError());
            return FALSE;
        }
    }

    return TRUE;
}

static VOID
InstallNullDriverOnDevice(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _Out_ PBOOL RebootNeeded)
{
    ULONG Status, Problem, ConfigFlags, Size;

    *RebootNeeded = FALSE;

    /* Nothing to do for a device that is not in the tree */
    if (CM_Get_DevNode_Status(&Status, &Problem, DeviceInfoData->DevInst, 0) != CR_SUCCESS)
        return;

    SetupDiDeleteDevRegKey(DeviceInfoSet, DeviceInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DRV);
    CM_Set_DevNode_Registry_PropertyW(DeviceInfoData->DevInst, CM_DRP_DRIVER, NULL, 0, 0);
    CM_Set_DevNode_Registry_PropertyW(DeviceInfoData->DevInst, CM_DRP_SERVICE, NULL, 0, 0);

    Size = sizeof(ConfigFlags);
    if (CM_Get_DevNode_Registry_PropertyW(DeviceInfoData->DevInst,
                                          CM_DRP_CONFIGFLAGS,
                                          NULL,
                                          &ConfigFlags,
                                          &Size,
                                          0) == CR_SUCCESS)
    {
        ConfigFlags &= ~(CONFIGFLAG_REINSTALL | CONFIGFLAG_FAILEDINSTALL);
        CM_Set_DevNode_Registry_PropertyW(DeviceInfoData->DevInst,
                                          CM_DRP_CONFIGFLAGS,
                                          &ConfigFlags,
                                          sizeof(ConfigFlags),
                                          0);
    }

    /* A running function driver stays loaded until the next boot */
    if (Status & DN_STARTED)
        *RebootNeeded = TRUE;
}

static BOOL
OpenDeviceDriverKey(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _Out_ PHKEY DriverKey)
{
    *DriverKey = SetupDiOpenDevRegKey(DeviceInfoSet,
                                      DeviceInfoData,
                                      DICS_FLAG_GLOBAL,
                                      0,
                                      DIREG_DRV,
                                      KEY_QUERY_VALUE);
    return (*DriverKey != INVALID_HANDLE_VALUE);
}

static BOOL
IsDeviceUsingInf(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ PCWSTR InfName)
{
    WCHAR CurrentInf[MAX_PATH];
    PCWSTR CurrentName;
    DWORD Size, Type;
    HKEY hKey;
    LONG Error;

    if (!OpenDeviceDriverKey(DeviceInfoSet, DeviceInfoData, &hKey))
        return FALSE;

    ZeroMemory(CurrentInf, sizeof(CurrentInf));
    Size = sizeof(CurrentInf) - sizeof(WCHAR);
    Error = RegQueryValueExW(hKey, REGSTR_VAL_INFPATH, NULL, &Type, (PBYTE)CurrentInf, &Size);
    RegCloseKey(hKey);

    if (Error != ERROR_SUCCESS || Type != REG_SZ)
        return FALSE;

    CurrentName = wcsrchr(CurrentInf, L'\\');
    CurrentName = CurrentName ? CurrentName + 1 : CurrentInf;

    return (_wcsicmp(CurrentName, InfName) == 0);
}

static DWORDLONG
ParseDriverVersion(
    _In_ PCWSTR VersionString)
{
    USHORT Parts[4] = { 0, 0, 0, 0 };

    swscanf(VersionString, L"%hu.%hu.%hu.%hu", &Parts[0], &Parts[1], &Parts[2], &Parts[3]);

    return ((DWORDLONG)Parts[0] << 48) | ((DWORDLONG)Parts[1] << 32) |
           ((DWORDLONG)Parts[2] << 16) | (DWORDLONG)Parts[3];
}

/* Newer date wins, then newer version. A device without a working driver always takes it. */
static BOOL
IsDriverBetterThanCurrent(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ PSP_DRVINFO_DATA_W DriverInfoData)
{
    WCHAR VersionString[64];
    FILETIME CurrentDate;
    ULONG Status, Problem;
    DWORD Size, Type;
    LONG Compare;
    BOOL Better = TRUE;
    HKEY hKey;

    if (CM_Get_DevNode_Status(&Status, &Problem, DeviceInfoData->DevInst, 0) == CR_SUCCESS &&
        (Status & DN_HAS_PROBLEM))
    {
        return TRUE;
    }

    if (!OpenDeviceDriverKey(DeviceInfoSet, DeviceInfoData, &hKey))
        return TRUE;

    Size = sizeof(CurrentDate);
    if (RegQueryValueExW(hKey, L"DriverDateData", NULL, &Type, (PBYTE)&CurrentDate, &Size) == ERROR_SUCCESS &&
        Type == REG_BINARY && Size == sizeof(CurrentDate))
    {
        Compare = CompareFileTime(&DriverInfoData->DriverDate, &CurrentDate);
        if (Compare < 0)
        {
            Better = FALSE;
        }
        else if (Compare == 0)
        {
            ZeroMemory(VersionString, sizeof(VersionString));
            Size = sizeof(VersionString) - sizeof(WCHAR);
            if (RegQueryValueExW(hKey, L"DriverVersion", NULL, &Type, (PBYTE)VersionString, &Size) == ERROR_SUCCESS &&
                Type == REG_SZ)
            {
                Better = (DriverInfoData->DriverVersion > ParseDriverVersion(VersionString));
            }
        }
    }

    RegCloseKey(hKey);
    return Better;
}

/*
* @implemented
*/
BOOL
WINAPI
DiInstallDriverW(
    _In_opt_ HWND hwndParent,
    _In_ LPCWSTR InfPath,
    _In_ DWORD Flags,
    _Out_opt_ PBOOL NeedReboot)
{
    WIN32_FILE_ATTRIBUTE_DATA FileData;
    WCHAR FullPath[MAX_PATH];
    WCHAR SourceDir[MAX_PATH];
    WCHAR OemInf[MAX_PATH];
    PWSTR FilePart;
    PCWSTR OemName;
    DEVINSTDATA DevInstData;
    SP_DRVINFO_DATA_W DriverInfo;
    DWORD AllowedFlags, Length, Index;
    BOOL RebootNeeded = FALSE;
    HINF hInf;

    TRACE("DiInstallDriverW(%p %s 0x%lx %p)\n", hwndParent, debugstr_w(InfPath), Flags, NeedReboot);

    if (NeedReboot)
        *NeedReboot = FALSE;

    if (hwndParent && !IsWindow(hwndParent))
    {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }

    AllowedFlags = IsCallerLocalSystem() ? DIIRFLAG_SYSTEM_BITS : DIIRFLAG_BITS;
    if (Flags & ~AllowedFlags)
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }

    if (IsRunningInWow64())
    {
        SetLastError(ERROR_IN_WOW64);
        return FALSE;
    }

    if (!InfPath || !*InfPath)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Length = GetFullPathNameW(InfPath, ARRAYSIZE(FullPath), FullPath, &FilePart);
    if (Length == 0 || Length >= ARRAYSIZE(FullPath) || !FilePart)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!GetFileAttributesExW(FullPath, GetFileExInfoStandard, &FileData))
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }

    /* Windows reports an empty file as an INF syntax error */
    if (!(FileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        FileData.nFileSizeHigh == 0 && FileData.nFileSizeLow == 0)
    {
        SetLastError(ERROR_GENERAL_SYNTAX);
        return FALSE;
    }

    hInf = SetupOpenInfFileW(FullPath, NULL, INF_STYLE_WIN4, NULL);
    if (hInf == INVALID_HANDLE_VALUE)
        return FALSE;
    SetupCloseInfFile(hInf);

    if (Flags & DIIRFLAG_INF_ALREADY_COPIED)
    {
        OemName = FilePart;
    }
    else
    {
        Length = (DWORD)(FilePart - FullPath);
        CopyMemory(SourceDir, FullPath, Length * sizeof(WCHAR));
        SourceDir[Length] = UNICODE_NULL;

        /* An identical INF already in the INF directory is reused */
        if (!SetupCopyOEMInfW(FullPath,
                              SourceDir,
                              SPOST_PATH,
                              SP_COPY_NOOVERWRITE,
                              OemInf,
                              ARRAYSIZE(OemInf),
                              NULL,
                              NULL) &&
            GetLastError() != ERROR_FILE_EXISTS)
        {
            return FALSE;
        }

        OemName = wcsrchr(OemInf, L'\\');
        OemName = OemName ? OemName + 1 : OemInf;
    }

    TRACE("Staged %s as %s\n", debugstr_w(FullPath), debugstr_w(OemName));

    ZeroMemory(&DevInstData, sizeof(DevInstData));
    DevInstData.hDevInfo = SetupDiGetClassDevsW(NULL, NULL, hwndParent, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (DevInstData.hDevInfo == INVALID_HANDLE_VALUE)
        return FALSE;

    DevInstData.devInfoData.cbSize = sizeof(DevInstData.devInfoData);
    for (Index = 0; SetupDiEnumDeviceInfo(DevInstData.hDevInfo, Index, &DevInstData.devInfoData); Index++)
    {
        if ((Flags & DIIRFLAG_HW_USING_THE_INF) &&
            !IsDeviceUsingInf(DevInstData.hDevInfo, &DevInstData.devInfoData, OemName))
        {
            continue;
        }

        /* Collect what this INF offers for the device and pick its best match */
        if (!SearchDriver(&DevInstData, NULL, FullPath))
            continue;

        DriverInfo.cbSize = sizeof(DriverInfo);
        if (SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV, DevInstData.hDevInfo, &DevInstData.devInfoData) &&
            SetupDiGetSelectedDriverW(DevInstData.hDevInfo, &DevInstData.devInfoData, &DriverInfo) &&
            ((Flags & DIIRFLAG_FORCE_INF) ||
             IsDriverBetterThanCurrent(DevInstData.hDevInfo, &DevInstData.devInfoData, &DriverInfo)))
        {
            if (InstallSelectedDriverOnDevice(DevInstData.hDevInfo, &DevInstData.devInfoData))
            {
                if (DeviceNeedsReboot(DevInstData.hDevInfo, &DevInstData.devInfoData))
                    RebootNeeded = TRUE;
            }
            else
            {
                WARN("Installing %s on device %lu failed with error 0x%lx\n",
                     debugstr_w(FullPath), Index, GetLastError());
            }
        }

        SetupDiDestroyDriverInfoList(DevInstData.hDevInfo, &DevInstData.devInfoData, SPDIT_COMPATDRIVER);
    }

    SetupDiDestroyDeviceInfoList(DevInstData.hDevInfo);

    ReportReboot(hwndParent, RebootNeeded, NeedReboot);
    SetLastError(NO_ERROR);
    return TRUE;
}

/*
* @implemented
*/
BOOL
WINAPI
DiInstallDriverA(
    _In_opt_ HWND hwndParent,
    _In_ LPCSTR InfPath,
    _In_ DWORD Flags,
    _Out_opt_ PBOOL NeedReboot)
{
    PWSTR InfPathW;
    DWORD Error;
    BOOL Result;
    int Length;

    if (!InfPath)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Length = MultiByteToWideChar(CP_ACP, 0, InfPath, -1, NULL, 0);
    InfPathW = HeapAlloc(GetProcessHeap(), 0, Length * sizeof(WCHAR));
    if (!InfPathW)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    MultiByteToWideChar(CP_ACP, 0, InfPath, -1, InfPathW, Length);

    Result = DiInstallDriverW(hwndParent, InfPathW, Flags, NeedReboot);

    Error = GetLastError();
    HeapFree(GetProcessHeap(), 0, InfPathW);
    SetLastError(Error);

    return Result;
}

/*
* @implemented
*/
BOOL
WINAPI
DiInstallDevice(
    _In_opt_ HWND hwndParent,
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_opt_ PSP_DRVINFO_DATA_W DriverInfoData,
    _In_ DWORD Flags,
    _Out_opt_ PBOOL NeedReboot)
{
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    SP_DRVINFO_DATA_W SelectedDriver;
    DEVINSTDATA DevInstData;
    BOOL RebootNeeded = FALSE;
    BOOL ValidDevice;

    TRACE("DiInstallDevice(%p %p %p %p 0x%lx %p)\n",
          hwndParent, DeviceInfoSet, DeviceInfoData, DriverInfoData, Flags, NeedReboot);

    if (NeedReboot)
        *NeedReboot = FALSE;

    if (Flags & ~DIIDFLAG_BITS)
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }

    if (IsRunningInWow64())
    {
        SetLastError(ERROR_IN_WOW64);
        return FALSE;
    }

    if (!DeviceInfoSet || DeviceInfoSet == INVALID_HANDLE_VALUE || !DeviceInfoData)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Device data that setupapi does not know must not reach the driver calls */
    ValidDevice = SetupDiGetDeviceInstanceIdW(DeviceInfoSet,
                                              DeviceInfoData,
                                              InstanceId,
                                              ARRAYSIZE(InstanceId),
                                              NULL);

    if (Flags & DIIDFLAG_INSTALLNULLDRIVER)
    {
        if (ValidDevice)
            InstallNullDriverOnDevice(DeviceInfoSet, DeviceInfoData, &RebootNeeded);

        ReportReboot(hwndParent, RebootNeeded, NeedReboot);
        SetLastError(NO_ERROR);
        return TRUE;
    }

    if (DriverInfoData)
    {
        if (!ValidDevice)
            return FALSE;

        if (!SetupDiSetSelectedDriverW(DeviceInfoSet, DeviceInfoData, DriverInfoData))
            return FALSE;

        if (!InstallSelectedDriverOnDevice(DeviceInfoSet, DeviceInfoData))
            return FALSE;
    }
    else
    {
        if (!ValidDevice)
        {
            SetLastError(ERROR_NO_DRIVER_SELECTED);
            return FALSE;
        }

        /* Search the INF directory for the best driver */
        SetupDiDestroyDriverInfoList(DeviceInfoSet, DeviceInfoData, SPDIT_COMPATDRIVER);

        ZeroMemory(&DevInstData, sizeof(DevInstData));
        DevInstData.hDevInfo = DeviceInfoSet;
        DevInstData.devInfoData = *DeviceInfoData;

        SelectedDriver.cbSize = sizeof(SelectedDriver);
        if (!SearchDriver(&DevInstData, NULL, NULL) ||
            !SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV, DeviceInfoSet, DeviceInfoData) ||
            !SetupDiGetSelectedDriverW(DeviceInfoSet, DeviceInfoData, &SelectedDriver))
        {
            if (Flags & DIIDFLAG_SHOWSEARCHUI)
                FIXME("Driver search UI is not supported\n");

            SetupDiDestroyDriverInfoList(DeviceInfoSet, DeviceInfoData, SPDIT_COMPATDRIVER);
            SetLastError(ERROR_NO_DRIVER_SELECTED);
            return FALSE;
        }

        if (!InstallSelectedDriverOnDevice(DeviceInfoSet, DeviceInfoData))
            return FALSE;
    }

    ReportReboot(hwndParent, DeviceNeedsReboot(DeviceInfoSet, DeviceInfoData), NeedReboot);
    SetLastError(NO_ERROR);
    return TRUE;
}

static BOOL
RemoveSingleDevice(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _Inout_ PBOOL RebootNeeded)
{
    SP_REMOVEDEVICE_PARAMS RemoveParams;
    DWORD Error;
    BOOL Result;

    RemoveParams.ClassInstallHeader.cbSize = sizeof(RemoveParams.ClassInstallHeader);
    RemoveParams.ClassInstallHeader.InstallFunction = DIF_REMOVE;
    RemoveParams.Scope = DI_REMOVEDEVICE_GLOBAL;
    RemoveParams.HwProfile = 0;

    if (!SetupDiSetClassInstallParamsW(DeviceInfoSet,
                                       DeviceInfoData,
                                       &RemoveParams.ClassInstallHeader,
                                       sizeof(RemoveParams)))
    {
        return FALSE;
    }

    Result = SetupDiCallClassInstaller(DIF_REMOVE, DeviceInfoSet, DeviceInfoData);
    Error = GetLastError();

    if (Result && DeviceNeedsReboot(DeviceInfoSet, DeviceInfoData))
        *RebootNeeded = TRUE;

    SetupDiSetClassInstallParamsW(DeviceInfoSet, DeviceInfoData, NULL, 0);
    SetLastError(Error);
    return Result;
}

/* Appends the instance IDs below Parent, deepest first, to a MULTI_SZ list */
static BOOL
CollectDescendants(
    _In_ DEVINST Parent,
    _Inout_ PWSTR *IdList,
    _Inout_ PSIZE_T IdListLength)
{
    WCHAR DeviceId[MAX_DEVICE_ID_LEN];
    SIZE_T IdLength;
    PWSTR NewList;
    DEVINST Child;
    CONFIGRET Cr;

    Cr = CM_Get_Child(&Child, Parent, 0);
    while (Cr == CR_SUCCESS)
    {
        if (!CollectDescendants(Child, IdList, IdListLength))
            return FALSE;

        if (CM_Get_Device_IDW(Child, DeviceId, ARRAYSIZE(DeviceId), 0) == CR_SUCCESS)
        {
            IdLength = wcslen(DeviceId) + 1;
            if (*IdList)
                NewList = HeapReAlloc(GetProcessHeap(), 0, *IdList, (*IdListLength + IdLength + 1) * sizeof(WCHAR));
            else
                NewList = HeapAlloc(GetProcessHeap(), 0, (IdLength + 1) * sizeof(WCHAR));

            if (!NewList)
            {
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return FALSE;
            }

            wcscpy(NewList + *IdListLength, DeviceId);
            *IdListLength += IdLength;
            NewList[*IdListLength] = UNICODE_NULL;
            *IdList = NewList;
        }

        Cr = CM_Get_Sibling(&Child, Child, 0);
    }

    return TRUE;
}

static BOOL
UninstallDeviceTree(
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _Out_ PBOOL RebootNeeded)
{
    WCHAR VetoName[MAX_PATH];
    PNP_VETO_TYPE VetoType;
    SP_DEVINFO_DATA ChildData;
    HDEVINFO ChildSet;
    PWSTR IdList = NULL;
    PWSTR CurrentId;
    SIZE_T IdListLength = 0;

    *RebootNeeded = FALSE;

    if (!CollectDescendants(DeviceInfoData->DevInst, &IdList, &IdListLength))
    {
        HeapFree(GetProcessHeap(), 0, IdList);
        return FALSE;
    }

    /* Stop the subtree first. A veto leaves the removal to the next boot. */
    if (CM_Query_And_Remove_SubTreeW(DeviceInfoData->DevInst,
                                     &VetoType,
                                     VetoName,
                                     ARRAYSIZE(VetoName),
                                     CM_REMOVE_NO_RESTART) == CR_REMOVE_VETOED)
    {
        *RebootNeeded = TRUE;
    }

    if (IdList)
    {
        ChildSet = SetupDiCreateDeviceInfoList(NULL, NULL);
        if (ChildSet != INVALID_HANDLE_VALUE)
        {
            for (CurrentId = IdList; *CurrentId != UNICODE_NULL; CurrentId += wcslen(CurrentId) + 1)
            {
                ChildData.cbSize = sizeof(ChildData);
                if (SetupDiOpenDeviceInfoW(ChildSet, CurrentId, NULL, 0, &ChildData))
                    RemoveSingleDevice(ChildSet, &ChildData, RebootNeeded);
            }

            SetupDiDestroyDeviceInfoList(ChildSet);
        }

        HeapFree(GetProcessHeap(), 0, IdList);
    }

    return RemoveSingleDevice(DeviceInfoSet, DeviceInfoData, RebootNeeded);
}

/*
* @implemented
*/
BOOL
WINAPI
DiUninstallDevice(
    _In_ HWND hwndParent,
    _In_ HDEVINFO DeviceInfoSet,
    _In_ PSP_DEVINFO_DATA DeviceInfoData,
    _In_ DWORD Flags,
    _Out_opt_ PBOOL NeedReboot)
{
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    BOOL RebootNeeded;

    TRACE("DiUninstallDevice(%p %p %p 0x%lx %p)\n",
          hwndParent, DeviceInfoSet, DeviceInfoData, Flags, NeedReboot);

    if (Flags != 0)
    {
        SetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }

    if (NeedReboot)
        *NeedReboot = FALSE;

    if (!SetupDiGetDeviceInstanceIdW(DeviceInfoSet, DeviceInfoData, InstanceId, ARRAYSIZE(InstanceId), NULL))
        return FALSE;

    TRACE("Uninstalling %s\n", debugstr_w(InstanceId));

    if (!UninstallDeviceTree(DeviceInfoSet, DeviceInfoData, &RebootNeeded))
        return FALSE;

    ReportReboot(hwndParent, RebootNeeded, NeedReboot);
    SetLastError(NO_ERROR);
    return TRUE;
}

/* Directory and InfFile MUST NOT be specified simultaneously */
static BOOL
SearchDriver(
    IN PDEVINSTDATA DevInstData,
    IN LPCWSTR Directory OPTIONAL,
    IN LPCWSTR InfFile OPTIONAL)
{
    SP_DEVINSTALL_PARAMS_W DevInstallParams = {0,};
    BOOL ret;

    DevInstallParams.cbSize = sizeof(SP_DEVINSTALL_PARAMS_W);
    if (!SetupDiGetDeviceInstallParamsW(DevInstData->hDevInfo, &DevInstData->devInfoData, &DevInstallParams))
    {
        TRACE("SetupDiGetDeviceInstallParams() failed with error 0x%x\n", GetLastError());
        return FALSE;
    }
    DevInstallParams.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;

    if (InfFile)
    {
        DevInstallParams.Flags |= DI_ENUMSINGLEINF;
        wcsncpy(DevInstallParams.DriverPath, InfFile, MAX_PATH);
    }
    else if (Directory)
    {
        DevInstallParams.Flags &= ~DI_ENUMSINGLEINF;
        wcsncpy(DevInstallParams.DriverPath, Directory, MAX_PATH);
    }
    else
    {
        DevInstallParams.Flags &= ~DI_ENUMSINGLEINF;
        *DevInstallParams.DriverPath = '\0';
    }

    ret = SetupDiSetDeviceInstallParamsW(
        DevInstData->hDevInfo,
        &DevInstData->devInfoData,
        &DevInstallParams);
    if (!ret)
    {
        TRACE("SetupDiSetDeviceInstallParams() failed with error 0x%x\n", GetLastError());
        return FALSE;
    }

    ret = SetupDiBuildDriverInfoList(
        DevInstData->hDevInfo,
        &DevInstData->devInfoData,
        SPDIT_COMPATDRIVER);
    if (!ret)
    {
        TRACE("SetupDiBuildDriverInfoList() failed with error 0x%x\n", GetLastError());
        return FALSE;
    }

    DevInstData->drvInfoData.cbSize = sizeof(SP_DRVINFO_DATA);
    ret = SetupDiEnumDriverInfoW(
        DevInstData->hDevInfo,
        &DevInstData->devInfoData,
        SPDIT_COMPATDRIVER,
        0,
        &DevInstData->drvInfoData);
    if (!ret)
    {
        if (GetLastError() == ERROR_NO_MORE_ITEMS)
            return FALSE;
        TRACE("SetupDiEnumDriverInfo() failed with error 0x%x\n", GetLastError());
        return FALSE;
    }

    return TRUE;
}

static BOOL
IsDots(IN LPCWSTR str)
{
    if(wcscmp(str, L".") && wcscmp(str, L"..")) return FALSE;
    return TRUE;
}

static LPCWSTR
GetFileExt(IN LPWSTR FileName)
{
    LPCWSTR Dot;

    Dot = wcsrchr(FileName, '.');
    if (!Dot)
        return L"";

    return Dot;
}

static BOOL
SearchDriverRecursive(
    IN PDEVINSTDATA DevInstData,
    IN LPCWSTR Path)
{
    WIN32_FIND_DATAW wfd;
    WCHAR DirPath[MAX_PATH];
    WCHAR FileName[MAX_PATH];
    WCHAR FullPath[MAX_PATH];
    WCHAR LastDirPath[MAX_PATH] = L"";
    WCHAR PathWithPattern[MAX_PATH];
    BOOL ok = TRUE;
    BOOL retval = FALSE;
    HANDLE hFindFile = INVALID_HANDLE_VALUE;

    wcscpy(DirPath, Path);

    if (DirPath[wcslen(DirPath) - 1] != '\\')
        wcscat(DirPath, L"\\");

    wcscpy(PathWithPattern, DirPath);
    wcscat(PathWithPattern, L"*");

    for (hFindFile = FindFirstFileW(PathWithPattern, &wfd);
        ok && hFindFile != INVALID_HANDLE_VALUE;
        ok = FindNextFileW(hFindFile, &wfd))
    {

        wcscpy(FileName, wfd.cFileName);
        if (IsDots(FileName))
            continue;

        if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            /* Recursive search */
            wcscpy(FullPath, DirPath);
            wcscat(FullPath, FileName);
            if (SearchDriverRecursive(DevInstData, FullPath))
            {
                retval = TRUE;
                /* We continue the search for a better driver */
            }
        }
        else
        {
            LPCWSTR pszExtension = GetFileExt(FileName);

            if ((_wcsicmp(pszExtension, L".inf") == 0) && (wcscmp(LastDirPath, DirPath) != 0))
            {
                wcscpy(LastDirPath, DirPath);

                if (wcslen(DirPath) > MAX_PATH)
                    /* Path is too long to be searched */
                    continue;

                if (SearchDriver(DevInstData, DirPath, NULL))
                {
                    retval = TRUE;
                    /* We continue the search for a better driver */
                }

            }
        }
    }

    if (hFindFile != INVALID_HANDLE_VALUE)
        FindClose(hFindFile);
    return retval;
}

BOOL
CheckBestDriver(
    _In_ PDEVINSTDATA DevInstData,
    _In_ PCWSTR pszDir)
{
    return SearchDriverRecursive(DevInstData, pszDir);
}

BOOL
ScanFoldersForDriver(
    IN PDEVINSTDATA DevInstData)
{
    BOOL result;

    /* Search in default location */
    result = SearchDriver(DevInstData, NULL, NULL);

    if (DevInstData->CustomSearchPath)
    {
        /* Search only in specified paths */
        /* We need to check all specified directories to be
         * sure to find the best driver for the device.
         */
        LPCWSTR Path;
        for (Path = DevInstData->CustomSearchPath; *Path != '\0'; Path += wcslen(Path) + 1)
        {
            TRACE("Search driver in %s\n", debugstr_w(Path));
            if (wcslen(Path) == 2 && Path[1] == ':')
            {
                if (SearchDriverRecursive(DevInstData, Path))
                    result = TRUE;
            }
            else
            {
                if (SearchDriver(DevInstData, Path, NULL))
                    result = TRUE;
            }
        }
    }

    return result;
}

BOOL
PrepareFoldersToScan(
    IN PDEVINSTDATA DevInstData,
    IN BOOL IncludeRemovableDevices,
    IN BOOL IncludeCustomPath,
    IN HWND hwndCombo OPTIONAL)
{
    WCHAR drive[] = {'?',':',0};
    DWORD dwDrives = 0;
    DWORD i;
    UINT nType;
    DWORD CustomTextLength = 0;
    DWORD LengthNeeded = 0;
    LPWSTR Buffer;
    INT idx = (INT)SendMessageW(hwndCombo, CB_GETCURSEL, 0, 0);

    /* Calculate length needed to store the search paths */
    if (IncludeRemovableDevices)
    {
        dwDrives = GetLogicalDrives();
        for (drive[0] = 'A', i = 1; drive[0] <= 'Z'; drive[0]++, i <<= 1)
        {
            if (dwDrives & i)
            {
                nType = GetDriveTypeW(drive);
                if (nType == DRIVE_REMOVABLE || nType == DRIVE_CDROM)
                {
                    LengthNeeded += 3;
                }
            }
        }
    }
    if (IncludeCustomPath)
    {
        CustomTextLength = 1 + ((idx != CB_ERR) ?
        (INT)SendMessageW(hwndCombo, CB_GETLBTEXTLEN, idx, 0) : ComboBox_GetTextLength(hwndCombo));
        LengthNeeded += CustomTextLength;
    }

    /* Allocate space for search paths */
    HeapFree(GetProcessHeap(), 0, DevInstData->CustomSearchPath);
    DevInstData->CustomSearchPath = Buffer = HeapAlloc(
        GetProcessHeap(),
        0,
        (LengthNeeded + 1) * sizeof(WCHAR));
    if (!Buffer)
    {
        TRACE("HeapAlloc() failed\n");
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    /* Fill search paths */
    if (IncludeRemovableDevices)
    {
        for (drive[0] = 'A', i = 1; drive[0] <= 'Z'; drive[0]++, i <<= 1)
        {
            if (dwDrives & i)
            {
                nType = GetDriveTypeW(drive);
                if (nType == DRIVE_REMOVABLE || nType == DRIVE_CDROM)
                {
                    Buffer += 1 + _swprintf(Buffer, drive);
                }
            }
        }
    }
    if (IncludeCustomPath)
    {
        Buffer += 1 + ((idx != CB_ERR) ?
        SendMessageW(hwndCombo, CB_GETLBTEXT, idx, (LPARAM)Buffer) :
        GetWindowTextW(hwndCombo, Buffer, CustomTextLength));
    }
    *Buffer = '\0';

    return TRUE;
}

BOOL
InstallCurrentDriver(
    IN PDEVINSTDATA DevInstData)
{
    BOOL ret;

    TRACE("Installing driver %s: %s\n",
        debugstr_w(DevInstData->drvInfoData.MfgName),
        debugstr_w(DevInstData->drvInfoData.Description));

    ret = SetupDiCallClassInstaller(
        DIF_SELECTBESTCOMPATDRV,
        DevInstData->hDevInfo,
        &DevInstData->devInfoData);
    if (!ret)
    {
        TRACE("SetupDiCallClassInstaller(DIF_SELECTBESTCOMPATDRV) failed with error 0x%x\n", GetLastError());
        return FALSE;
    }

    ret = SetupDiCallClassInstaller(
        DIF_ALLOW_INSTALL,
        DevInstData->hDevInfo,
        &DevInstData->devInfoData);
    if (!ret)
    {
        TRACE("SetupDiCallClassInstaller(DIF_ALLOW_INSTALL) failed with error 0x%x\n", GetLastError());
        return FALSE;
    }

    ret = SetupDiCallClassInstaller(
        DIF_NEWDEVICEWIZARD_PREANALYZE,
        DevInstData->hDevInfo,
        &DevInstData->devInfoData);
    if (!ret)
    {
        TRACE("SetupDiCallClassInstaller(DIF_NEWDEVICEWIZARD_PREANALYZE) failed with error 0x%x\n", GetLastError());
        return FALSE;
    }

    ret = SetupDiCallClassInstaller(
        DIF_NEWDEVICEWIZARD_POSTANALYZE,
        DevInstData->hDevInfo,
        &DevInstData->devInfoData);
    if (!ret)
    {
        TRACE("SetupDiCallClassInstaller(DIF_NEWDEVICEWIZARD_POSTANALYZE) failed with error 0x%x\n", GetLastError());
        return FALSE;
    }

    ret = SetupDiCallClassInstaller(
        DIF_INSTALLDEVICEFILES,
        DevInstData->hDevInfo,
        &DevInstData->devInfoData);
    if (!ret)
    {
        TRACE("SetupDiCallClassInstaller(DIF_INSTALLDEVICEFILES) failed with error 0x%x\n", GetLastError());
        return FALSE;
    }

    ret = SetupDiCallClassInstaller(
        DIF_REGISTER_COINSTALLERS,
        DevInstData->hDevInfo,
        &DevInstData->devInfoData);
    if (!ret)
    {
        TRACE("SetupDiCallClassInstaller(DIF_REGISTER_COINSTALLERS) failed with error 0x%x\n", GetLastError());
        return FALSE;
    }

    ret = SetupDiCallClassInstaller(
        DIF_INSTALLINTERFACES,
        DevInstData->hDevInfo,
        &DevInstData->devInfoData);
    if (!ret)
    {
        TRACE("SetupDiCallClassInstaller(DIF_INSTALLINTERFACES) failed with error 0x%x\n", GetLastError());
        return FALSE;
    }

    ret = SetupDiCallClassInstaller(
        DIF_INSTALLDEVICE,
        DevInstData->hDevInfo,
        &DevInstData->devInfoData);
    if (!ret)
    {
        TRACE("SetupDiCallClassInstaller(DIF_INSTALLDEVICE) failed with error 0x%x\n", GetLastError());
        return FALSE;
    }

    ret = SetupDiCallClassInstaller(
        DIF_NEWDEVICEWIZARD_FINISHINSTALL,
        DevInstData->hDevInfo,
        &DevInstData->devInfoData);
    if (!ret)
    {
        TRACE("SetupDiCallClassInstaller(DIF_NEWDEVICEWIZARD_FINISHINSTALL) failed with error 0x%x\n", GetLastError());
        return FALSE;
    }

    ret = SetupDiCallClassInstaller(
        DIF_DESTROYPRIVATEDATA,
        DevInstData->hDevInfo,
        &DevInstData->devInfoData);
    if (!ret)
    {
        TRACE("SetupDiCallClassInstaller(DIF_DESTROYPRIVATEDATA) failed with error 0x%x\n", GetLastError());
        return FALSE;
    }

    return TRUE;
}

/*
* @implemented
*/
BOOL WINAPI
DevInstallW(
    IN HWND hWndParent,
    IN HINSTANCE hInstance,
    IN LPCWSTR InstanceId,
    IN INT Show)
{
    PDEVINSTDATA DevInstData = NULL;
    BOOL ret;
    DWORD config_flags;
    BOOL retval = FALSE;

    TRACE("(%p, %p, %s, %d)\n", hWndParent, hInstance, debugstr_w(InstanceId), Show);

    if (!IsUserAdmin())
    {
        /* XP kills the process... */
        ExitProcess(ERROR_ACCESS_DENIED);
    }

    DevInstData = HeapAlloc(GetProcessHeap(), 0, sizeof(DEVINSTDATA));
    if (!DevInstData)
    {
        TRACE("HeapAlloc() failed\n");
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        goto cleanup;
    }

    /* Clear devinst data */
    ZeroMemory(DevInstData, sizeof(DEVINSTDATA));
    DevInstData->devInfoData.cbSize = 0; /* Tell if the devInfoData is valid */

    /* Fill devinst data */
    DevInstData->hDevInfo = SetupDiCreateDeviceInfoListExW(NULL, NULL, NULL, NULL);
    if (DevInstData->hDevInfo == INVALID_HANDLE_VALUE)
    {
        TRACE("SetupDiCreateDeviceInfoListExW() failed with error 0x%x\n", GetLastError());
        goto cleanup;
    }

    DevInstData->devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    ret = SetupDiOpenDeviceInfoW(
        DevInstData->hDevInfo,
        InstanceId,
        NULL,
        0, /* Open flags */
        &DevInstData->devInfoData);
    if (!ret)
    {
        TRACE("SetupDiOpenDeviceInfoW() failed with error 0x%x (InstanceId %s)\n",
            GetLastError(), debugstr_w(InstanceId));
        DevInstData->devInfoData.cbSize = 0;
        goto cleanup;
    }

    SetLastError(ERROR_GEN_FAILURE);
    ret = SetupDiGetDeviceRegistryProperty(
        DevInstData->hDevInfo,
        &DevInstData->devInfoData,
        SPDRP_DEVICEDESC,
        &DevInstData->regDataType,
        NULL, 0,
        &DevInstData->requiredSize);

    if (!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER && DevInstData->regDataType == REG_SZ)
    {
        DevInstData->buffer = HeapAlloc(GetProcessHeap(), 0, DevInstData->requiredSize);
        if (!DevInstData->buffer)
        {
            TRACE("HeapAlloc() failed\n");
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        }
        else
        {
            ret = SetupDiGetDeviceRegistryPropertyW(
                DevInstData->hDevInfo,
                &DevInstData->devInfoData,
                SPDRP_DEVICEDESC,
                &DevInstData->regDataType,
                DevInstData->buffer, DevInstData->requiredSize,
                &DevInstData->requiredSize);
        }
    }
    if (!ret)
    {
        TRACE("SetupDiGetDeviceRegistryProperty() failed with error 0x%x (InstanceId %s)\n",
            GetLastError(), debugstr_w(InstanceId));
        goto cleanup;
    }

    if (SetupDiGetDeviceRegistryPropertyW(
        DevInstData->hDevInfo,
        &DevInstData->devInfoData,
        SPDRP_CONFIGFLAGS,
        NULL,
        (BYTE *)&config_flags,
        sizeof(config_flags),
        NULL))
    {
        if (config_flags & CONFIGFLAG_FAILEDINSTALL)
        {
            /* The device is disabled */
            TRACE("Device is disabled\n");
            retval = TRUE;
            goto cleanup;
        }
    }

    TRACE("Installing %s (%s)\n", debugstr_w((PCWSTR)DevInstData->buffer), debugstr_w(InstanceId));

    /* Search driver in default location and removable devices */
    if (!PrepareFoldersToScan(DevInstData, FALSE, FALSE, NULL))
    {
        TRACE("PrepareFoldersToScan() failed with error 0x%lx\n", GetLastError());
        goto cleanup;
    }
    if (ScanFoldersForDriver(DevInstData))
    {
        /* Driver found ; install it */
        retval = InstallCurrentDriver(DevInstData);
        TRACE("InstallCurrentDriver() returned %d\n", retval);
        if (retval && Show != SW_HIDE)
        {
            /* Should we display the 'Need to reboot' page? */
            SP_DEVINSTALL_PARAMS installParams;
            installParams.cbSize = sizeof(SP_DEVINSTALL_PARAMS);
            if (SetupDiGetDeviceInstallParams(
                DevInstData->hDevInfo,
                &DevInstData->devInfoData,
                &installParams))
            {
                if (installParams.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT))
                {
                    TRACE("Displaying 'Reboot' wizard page\n");
                    retval = DisplayWizard(DevInstData, hWndParent, IDD_NEEDREBOOT);
                }
            }
        }
        goto cleanup;
    }
    else if (Show == SW_HIDE)
    {
        /* We can't show the wizard. Fail the install */
        TRACE("No wizard\n");
        goto cleanup;
    }

    /* Prepare the wizard, and display it */
    TRACE("Need to show install wizard\n");
    retval = DisplayWizard(DevInstData, hWndParent, IDD_WELCOMEPAGE);

cleanup:
    if (DevInstData)
    {
        if (DevInstData->devInfoData.cbSize != 0)
        {
            if (!SetupDiDestroyDriverInfoList(DevInstData->hDevInfo, &DevInstData->devInfoData, SPDIT_COMPATDRIVER))
                TRACE("SetupDiDestroyDriverInfoList() failed with error 0x%lx\n", GetLastError());
        }
        if (DevInstData->hDevInfo != INVALID_HANDLE_VALUE)
        {
            if (!SetupDiDestroyDeviceInfoList(DevInstData->hDevInfo))
                TRACE("SetupDiDestroyDeviceInfoList() failed with error 0x%lx\n", GetLastError());
        }
        HeapFree(GetProcessHeap(), 0, DevInstData->buffer);
        HeapFree(GetProcessHeap(), 0, DevInstData);
    }

    return retval;
}


BOOL
WINAPI
InstallDevInstEx(
    IN HWND hWndParent,
    IN LPCWSTR InstanceId,
    IN BOOL bUpdate,
    OUT LPDWORD lpReboot,
    IN DWORD Unknown)
{
    PDEVINSTDATA DevInstData = NULL;
    BOOL ret;
    BOOL retval = FALSE;

    TRACE("InstllDevInstEx(%p, %s, %d, %p, %lx)\n",
          hWndParent, debugstr_w(InstanceId), bUpdate, lpReboot, Unknown);

    DevInstData = HeapAlloc(GetProcessHeap(), 0, sizeof(DEVINSTDATA));
    if (!DevInstData)
    {
        TRACE("HeapAlloc() failed\n");
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        goto cleanup;
    }

    /* Clear devinst data */
    ZeroMemory(DevInstData, sizeof(DEVINSTDATA));
    DevInstData->devInfoData.cbSize = 0; /* Tell if the devInfoData is valid */
    DevInstData->bUpdate = bUpdate;

    /* Fill devinst data */
    DevInstData->hDevInfo = SetupDiCreateDeviceInfoListExW(NULL, NULL, NULL, NULL);
    if (DevInstData->hDevInfo == INVALID_HANDLE_VALUE)
    {
        TRACE("SetupDiCreateDeviceInfoListExW() failed with error 0x%x\n", GetLastError());
        goto cleanup;
    }

    DevInstData->devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    ret = SetupDiOpenDeviceInfoW(
        DevInstData->hDevInfo,
        InstanceId,
        NULL,
        0, /* Open flags */
        &DevInstData->devInfoData);
    if (!ret)
    {
        TRACE("SetupDiOpenDeviceInfoW() failed with error 0x%x (InstanceId %s)\n",
            GetLastError(), debugstr_w(InstanceId));
        DevInstData->devInfoData.cbSize = 0;
        goto cleanup;
    }

    SetLastError(ERROR_GEN_FAILURE);
    ret = SetupDiGetDeviceRegistryProperty(
        DevInstData->hDevInfo,
        &DevInstData->devInfoData,
        SPDRP_DEVICEDESC,
        &DevInstData->regDataType,
        NULL, 0,
        &DevInstData->requiredSize);

    if (!ret && GetLastError() == ERROR_INSUFFICIENT_BUFFER && DevInstData->regDataType == REG_SZ)
    {
        DevInstData->buffer = HeapAlloc(GetProcessHeap(), 0, DevInstData->requiredSize);
        if (!DevInstData->buffer)
        {
            TRACE("HeapAlloc() failed\n");
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        }
        else
        {
            ret = SetupDiGetDeviceRegistryPropertyW(
                DevInstData->hDevInfo,
                &DevInstData->devInfoData,
                SPDRP_DEVICEDESC,
                &DevInstData->regDataType,
                DevInstData->buffer, DevInstData->requiredSize,
                &DevInstData->requiredSize);
        }
    }

    if (!ret)
    {
        TRACE("SetupDiGetDeviceRegistryProperty() failed with error 0x%x (InstanceId %s)\n",
            GetLastError(), debugstr_w(InstanceId));
        goto cleanup;
    }

    /* Prepare the wizard, and display it */
    TRACE("Need to show install wizard\n");
    retval = DisplayWizard(DevInstData, hWndParent, IDD_WELCOMEPAGE);

cleanup:
    if (DevInstData)
    {
        if (DevInstData->devInfoData.cbSize != 0)
        {
            if (!SetupDiDestroyDriverInfoList(DevInstData->hDevInfo, &DevInstData->devInfoData, SPDIT_COMPATDRIVER))
                TRACE("SetupDiDestroyDriverInfoList() failed with error 0x%lx\n", GetLastError());
        }
        if (DevInstData->hDevInfo != INVALID_HANDLE_VALUE)
        {
            if (!SetupDiDestroyDeviceInfoList(DevInstData->hDevInfo))
                TRACE("SetupDiDestroyDeviceInfoList() failed with error 0x%lx\n", GetLastError());
        }
        HeapFree(GetProcessHeap(), 0, DevInstData->buffer);
        HeapFree(GetProcessHeap(), 0, DevInstData);
    }

    return retval;
}


/*
 * @implemented
 */
BOOL
WINAPI
InstallDevInst(
    IN HWND hWndParent,
    IN LPCWSTR InstanceId,
    IN BOOL bUpdate,
    OUT LPDWORD lpReboot)
{
    return InstallDevInstEx(hWndParent, InstanceId, bUpdate, lpReboot, 0);
}


/*
* @implemented
*/
BOOL WINAPI
ClientSideInstallW(
    IN HWND hWndOwner,
    IN HINSTANCE hInstance,
    IN LPWSTR lpNamedPipeName,
    IN INT Show)
{
    BOOL ReturnValue = FALSE;
    BOOL ShowWizard;
    DWORD BytesRead;
    DWORD Value;
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    PWSTR DeviceInstance = NULL;
    PWSTR InstallEventName = NULL;
    HANDLE hInstallEvent;

    /* Open the pipe */
    hPipe = CreateFileW(lpNamedPipeName, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if(hPipe == INVALID_HANDLE_VALUE)
    {
        ERR("CreateFileW failed with error %u\n", GetLastError());
        goto cleanup;
    }

    /* Read the data. Some is just included for compatibility with Windows right now and not yet used by ReactOS.
       See umpnpmgr for more details. */
    if(!ReadFile(hPipe, &Value, sizeof(Value), &BytesRead, NULL))
    {
        ERR("ReadFile failed with error %u\n", GetLastError());
        goto cleanup;
    }

    InstallEventName = (PWSTR)HeapAlloc(GetProcessHeap(), 0, Value);

    if(!ReadFile(hPipe, InstallEventName, Value, &BytesRead, NULL))
    {
        ERR("ReadFile failed with error %u\n", GetLastError());
        goto cleanup;
    }

    /* I couldn't figure out what the following value means under Windows XP.
       Therefore I used it in umpnpmgr to pass the ShowWizard variable. */
    if(!ReadFile(hPipe, &ShowWizard, sizeof(ShowWizard), &BytesRead, NULL))
    {
        ERR("ReadFile failed with error %u\n", GetLastError());
        goto cleanup;
    }

    /* Next one is again size in bytes of the following string */
    if(!ReadFile(hPipe, &Value, sizeof(Value), &BytesRead, NULL))
    {
        ERR("ReadFile failed with error %u\n", GetLastError());
        goto cleanup;
    }

    DeviceInstance = (PWSTR)HeapAlloc(GetProcessHeap(), 0, Value);

    if(!ReadFile(hPipe, DeviceInstance, Value, &BytesRead, NULL))
    {
        ERR("ReadFile failed with error %u\n", GetLastError());
        goto cleanup;
    }

    ReturnValue = DevInstallW(NULL, NULL, DeviceInstance, ShowWizard ? SW_SHOWNOACTIVATE : SW_HIDE);
    if(!ReturnValue)
    {
        ERR("DevInstallW failed with error %lu\n", GetLastError());
        goto cleanup;
    }

    hInstallEvent = CreateEventW(NULL, TRUE, FALSE, InstallEventName);
    if(!hInstallEvent)
    {
        TRACE("CreateEventW('%ls') failed with error %lu\n", InstallEventName, GetLastError());
        goto cleanup;
    }

    SetEvent(hInstallEvent);
    CloseHandle(hInstallEvent);

cleanup:
    if(hPipe != INVALID_HANDLE_VALUE)
        CloseHandle(hPipe);

    if(InstallEventName)
        HeapFree(GetProcessHeap(), 0, InstallEventName);

    if(DeviceInstance)
        HeapFree(GetProcessHeap(), 0, DeviceInstance);

    return ReturnValue;
}

BOOL WINAPI
DllMain(
    IN HINSTANCE hInstance,
    IN DWORD dwReason,
    IN LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        INITCOMMONCONTROLSEX InitControls;

        DisableThreadLibraryCalls(hInstance);

        InitControls.dwSize = sizeof(INITCOMMONCONTROLSEX);
        InitControls.dwICC = ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&InitControls);
        hDllInstance = hInstance;
    }

    return TRUE;
}
