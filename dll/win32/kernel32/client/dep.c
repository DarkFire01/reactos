/*
 * PROJECT:         ReactOS system libraries
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * FILE:            dll/win32/kernel32/client/dep.c
 * PURPOSE:         Data Execution Prevention (DEP) functions
 * COPYRIGHT:       Copyright 2021 Oleg Dubinskiy (oleg.dubinskij2013@yandex.ua)
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

const CHAR *LayersKey = "\\Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
DEP_SYSTEM_POLICY_TYPE SystemDEPPolicy = OptIn;
BOOL IsWow64;

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
DEP_SYSTEM_POLICY_TYPE
WINAPI
GetSystemDEPPolicy(VOID)
{
    CHAR Buffer[MAX_PATH + 10];
    DWORD Size = sizeof(Buffer);
    HKEY hKey = 0;
    HKEY AppKey = 0;
    DWORD ret;
    LSTATUS (WINAPI *pRegOpenKeyA)(HKEY,LPCSTR,PHKEY);
    LSTATUS (WINAPI *pRegQueryValueExA)(HKEY,LPCSTR,LPDWORD,LPDWORD,LPBYTE,LPDWORD);
    LSTATUS (WINAPI *pRegCloseKey)(HKEY);

    pRegOpenKeyA = (PVOID)GetProcAddress(GetModuleHandleA("advapi32"), "RegOpenKeyA");
    pRegQueryValueExA = (PVOID)GetProcAddress(GetModuleHandleA("advapi32"), "RegQueryValueExA");
    pRegCloseKey = (PVOID)GetProcAddress(GetModuleHandleA("advapi32"), "RegCloseKey");

    if (!pRegOpenKeyA || !pRegQueryValueExA || !pRegCloseKey) return OptIn;
    if (pRegOpenKeyA(HKEY_LOCAL_MACHINE, LayersKey, &hKey)) hKey = 0;

    ret = GetModuleFileNameA(0, Buffer, MAX_PATH);

    if (ret && ret < MAX_PATH)
    {
        HKEY TmpKey;

        if (!pRegOpenKeyA(HKEY_LOCAL_MACHINE, LayersKey, &TmpKey))
        {
            CHAR *p, *AppName = Buffer;
            if ((p = strrchr(AppName, '/'))) AppName = p + 1;
            if ((p = strrchr(AppName, '\\'))) AppName = p + 1;
            strcat(AppName, "\\");
            DPRINT1("AppName = [%s]\n", AppName);
            if (pRegOpenKeyA(TmpKey, AppName, &AppKey)) AppKey = 0;
            pRegCloseKey(TmpKey);
        }
    }

    if (hKey || AppKey)
    {
        if ((AppKey && !pRegQueryValueExA(AppKey, "DisableNXShowUI", 0, NULL, (BYTE *)Buffer, &Size)) ||
            (hKey && !pRegQueryValueExA(hKey, "DisableNXShowUI", 0, NULL, (BYTE *)Buffer, &Size)))
        {
            if (!strcmp(Buffer,"OptIn"))
            {
                DPRINT1("System DEP policy set to OptIn\n");
                SystemDEPPolicy = OptIn;
            }
            else if (!strcmp(Buffer,"OptOut"))
            {
                DPRINT1("System DEP policy set to OptOut\n");
                SystemDEPPolicy = OptIn;
            }
            else if (!strcmp(Buffer,"AlwaysOn"))
            {
                DPRINT1("System DEP policy set to AlwaysOn\n");
                SystemDEPPolicy = AlwaysOn;
            }
            else if (!strcmp(Buffer,"AlwaysOff"))
            {
                DPRINT1("System DEP policy set to AlwaysOff\n");
                SystemDEPPolicy = AlwaysOff;
            }
        }
    }

    if (AppKey) pRegCloseKey(AppKey);
    if (hKey) pRegCloseKey(hKey);

    return SystemDEPPolicy;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetProcessDEPPolicy(DWORD dwFlags)
{
    ULONG Flags = 0;
    NTSTATUS Status;

    if (IsWow64 || (SystemDEPPolicy != OptIn && SystemDEPPolicy != OptOut))
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    switch (dwFlags)
    {
        case 0:
        Flags = MEM_EXECUTE_OPTION_ENABLE;
        break;

        case PROCESS_DEP_ENABLE:
        Flags = MEM_EXECUTE_OPTION_DISABLE | MEM_EXECUTE_OPTION_PERMANENT;
        break;

        case PROCESS_DEP_DISABLE_ATL_THUNK_EMULATION:
        Flags |= MEM_EXECUTE_OPTION_DISABLE_THUNK_EMULATION;
        break;
    }

    Status = NtSetInformationProcess(GetCurrentProcess(),
                                     ProcessExecuteFlags,
                                     &Flags, sizeof(Flags));

    if (NT_SUCCESS(Status))
        SetLastError(RtlNtStatusToDosError(Status));

    return !Status;
}

/* EOF */
