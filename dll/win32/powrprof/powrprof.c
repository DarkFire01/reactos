/*
 * Copyright (C) 2005 Benjamin Cutler
 * Copyright (C) 2008 Dmitry Chapyshev
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */


#include <stdarg.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#define NTOS_MODE_USER
#include <ndk/pofuncs.h>
#include <ndk/rtlfuncs.h>
#include <ndk/setypes.h>
#include <powrprof.h>
#include <wine/debug.h>
#include <wine/unicode.h>

WINE_DEFAULT_DEBUG_CHANNEL(powrprof);


static const WCHAR szPowerCfgSubKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\PowerCfg";
static const WCHAR szUserPowerConfigSubKey[] =
    L"Control Panel\\PowerCfg";
static const WCHAR szCurrentPowerPolicies[] =
    L"CurrentPowerPolicy";
static const WCHAR szPolicies[] = L"Policies";
static const WCHAR szName[] = L"Name";
static const WCHAR szDescription[] = L"Description";
static const WCHAR szDiskMax[] = L"DiskSpindownMax";
static const WCHAR szDiskMin[] = L"DiskSpindownMin";
static const WCHAR szLastID[] = L"LastID";

static UINT g_LastID = (UINT)-1;
static HANDLE PPRegSemaphore = NULL;


/**
 * @brief
 * Creates a security descriptor for the registry semaphore.
 *
 * @param[out]  PowrProfSd
 * A pointer to an allocated security descriptor for the semaphore.
 *
 * @return
 * Returns TRUE if the function succeeds, otherwise FALSE is returned.
 *
 * @remarks
 * Authenticated users are only given a subset of specific rights for
 * the semaphore access, local system and administrators have full power.
 **/
static BOOLEAN
CreatePwrProfSemaphoreSecurity(
    _Out_ PSECURITY_DESCRIPTOR *PowrProfSd)
{
    BOOLEAN Success = FALSE;
    PSID AuthenticatedUsersSid = NULL, SystemSid = NULL, AdminsSid = NULL;
    PACL Dacl = NULL;
    PSECURITY_DESCRIPTOR RelSd = NULL;
    ULONG DaclSize, RelSDSize = 0;
    SECURITY_DESCRIPTOR AbsSd;
    static SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};

    if (!AllocateAndInitializeSid(&NtAuthority,
                                  1,
                                  SECURITY_AUTHENTICATED_USER_RID,
                                  0, 0, 0, 0, 0, 0, 0,
                                  &AuthenticatedUsersSid))
    {
        return FALSE;
    }

    if (!AllocateAndInitializeSid(&NtAuthority,
                                  1,
                                  SECURITY_LOCAL_SYSTEM_RID,
                                  0, 0, 0, 0, 0, 0, 0,
                                  &SystemSid))
    {
        goto Quit;
    }

    if (!AllocateAndInitializeSid(&NtAuthority,
                                  2,
                                  SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0,
                                  &AdminsSid))
    {
        goto Quit;
    }

    if (!InitializeSecurityDescriptor(&AbsSd, SECURITY_DESCRIPTOR_REVISION))
        goto Quit;

    /* Compute the size needed for the DACL and allocate it */
    DaclSize = sizeof(ACL) +
               sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(AuthenticatedUsersSid) +
               sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(SystemSid) +
               sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(AdminsSid);

    Dacl = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DaclSize);
    if (!Dacl || !InitializeAcl(Dacl, DaclSize, ACL_REVISION))
        goto Quit;

    if (!AddAccessAllowedAce(Dacl,
                             ACL_REVISION,
                             SYNCHRONIZE | READ_CONTROL | SEMAPHORE_MODIFY_STATE | SEMAPHORE_QUERY_STATE,
                             AuthenticatedUsersSid))
    {
        goto Quit;
    }

    if (!AddAccessAllowedAce(Dacl,
                             ACL_REVISION,
                             SEMAPHORE_ALL_ACCESS,
                             SystemSid))
    {
        goto Quit;
    }

    if (!AddAccessAllowedAce(Dacl,
                             ACL_REVISION,
                             SEMAPHORE_ALL_ACCESS,
                             AdminsSid))
    {
        goto Quit;
    }

    if (!SetSecurityDescriptorDacl(&AbsSd, TRUE, Dacl, FALSE))
        goto Quit;

    if (!SetSecurityDescriptorOwner(&AbsSd, AdminsSid, FALSE))
        goto Quit;

    if (!SetSecurityDescriptorGroup(&AbsSd, SystemSid, FALSE))
        goto Quit;

    /* Retrieve the size needed for the relative SD */
    if (MakeSelfRelativeSD(&AbsSd, NULL, &RelSDSize) ||
        (GetLastError() != ERROR_INSUFFICIENT_BUFFER))
    {
        goto Quit;
    }

    /* Build the relative SD */
    RelSd = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, RelSDSize);
    if (!RelSd || !MakeSelfRelativeSD(&AbsSd, RelSd, &RelSDSize))
    {
        if (RelSd)
            HeapFree(GetProcessHeap(), 0, RelSd);
        goto Quit;
    }

    *PowrProfSd = RelSd;
    Success = TRUE;

Quit:
    if (Dacl)
        HeapFree(GetProcessHeap(), 0, Dacl);

    if (AdminsSid)
        FreeSid(AdminsSid);

    if (SystemSid)
        FreeSid(SystemSid);

    if (AuthenticatedUsersSid)
        FreeSid(AuthenticatedUsersSid);

    return Success;
}

/**
 * @brief
 * Creates or opens a registry-synchronizing semaphore, with access for
 * SYNCHRONIZE | READ_CONTROL | SEMAPHORE_MODIFY_STATE | SEMAPHORE_QUERY_STATE
 * by everyone.
 *
 * @note
 * Because this semaphore aims at synchronizing registry accesses for both
 * the global HKEY_LOCAL_MACHINE and the per-user HKEY_CURRENT_USER, this
 * Windows <= 2003 - compatible mechanism isn't fully reliable. On Win7+,
 * synchronization is ensured with the help of the "User-mode Power Service"
 * ("Power", umpo.dll), using RPC calls.
 * https://redplait.blogspot.com/2010/11/umpodll-rpc-interfaces.html
 **/
static BOOLEAN
CreatePwrProfSemaphore(VOID)
{
    SECURITY_ATTRIBUTES SecAttrs;
    PSECURITY_DESCRIPTOR Sd;
    HANDLE hSemaphore;
    DWORD dwError;

    while (TRUE)
    {
    /* Return early if we have already obtained the semaphore handle */
    if (PPRegSemaphore)
        return TRUE;

    /*
     * Suppose first that the semaphore already exists and try only to open it
     * with the necessary access rights. The semaphore may have been created
     * by another instance from a different user (e.g. powrprof.dll loaded by
     * a LocalSystem service) and attempting to create-or-open it instead would
     * fail with ERROR_ACCESS_DENIED.
     */
    hSemaphore = OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE | SEMAPHORE_QUERY_STATE,
                                FALSE, L"PowerProfileRegistrySemaphore");
    if (hSemaphore)
        break;
    dwError = GetLastError();
    if (dwError == ERROR_ACCESS_DENIED)
    {
        ERR("Couldn't open POWRPROF semaphore: %ld\n", dwError);
        return FALSE;
    }

    /* The semaphore may not exist. Create it so that all logged-in users
     * can access it with the necessary but limited access rights. */
    if (!CreatePwrProfSemaphoreSecurity(&Sd))
    {
        ERR("Couldn't create POWRPROF semaphore security descriptor\n");
        return FALSE;
    }
    // TODO: Consider using no SD (or no security attributes i.e. falling back to defaults?) if creating the SD failed.

    SecAttrs.nLength = sizeof(SecAttrs);
    SecAttrs.lpSecurityDescriptor = Sd;
    SecAttrs.bInheritHandle = FALSE;

    hSemaphore = CreateSemaphoreW(&SecAttrs, 1, 1, L"PowerProfileRegistrySemaphore");
    dwError = GetLastError();
    HeapFree(GetProcessHeap(), 0, Sd);
    if (hSemaphore)
        break;

    /* If a process from a different user created the semaphore in the
     * meantime, we failed with ERROR_ACCESS_DENIED. Then, retry again
     * by only opening the semaphore this time. Otherwise, bail out. */
    if (dwError != ERROR_ACCESS_DENIED)
    {
        ERR("Couldn't create POWRPROF semaphore: %ld\n", dwError);
        return FALSE;
    }
    }

    /* Store the handle; if another thread already succeeded, close the handle */
    if (InterlockedCompareExchangePointer((PVOID*)&PPRegSemaphore, hSemaphore, NULL) != NULL)
        CloseHandle(hSemaphore);
    return TRUE;
}

static BOOLEAN
AcquirePwrProfSemaphore(VOID)
{
    if (!PPRegSemaphore && !CreatePwrProfSemaphore())
        return FALSE;
    return (WaitForSingleObject(PPRegSemaphore, INFINITE) == WAIT_OBJECT_0);
}


BOOLEAN WINAPI WritePwrPolicy(PUINT puiID, PPOWER_POLICY pPowerPolicy);

NTSTATUS WINAPI
CallNtPowerInformation(POWER_INFORMATION_LEVEL InformationLevel,
                       PVOID lpInputBuffer,
                       ULONG nInputBufferSize,
                       PVOID lpOutputBuffer,
                       ULONG nOutputBufferSize)
{
    BOOLEAN old;

	//Lohnegrim: In order to get the right results, we have to adjust our Privileges
    RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &old);
    RtlAdjustPrivilege(SE_CREATE_PAGEFILE_PRIVILEGE, TRUE, FALSE, &old);

    return NtPowerInformation(InformationLevel,
                              lpInputBuffer,
                              nInputBufferSize,
                              lpOutputBuffer,
                              nOutputBufferSize);
}

BOOLEAN WINAPI
CanUserWritePwrScheme(VOID)
{
    HKEY hKey = NULL;
    LONG Ret;

    TRACE("()\n");

    Ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPowerCfgSubKey, 0, KEY_READ | KEY_WRITE, &hKey);
    if (Ret != ERROR_SUCCESS)
    {
        TRACE("RegOpenKeyEx failed: %d\n", Ret);
        SetLastError(Ret);
        return FALSE;
    }

    RegCloseKey(hKey);
    return TRUE;
}

BOOLEAN WINAPI
DeletePwrScheme(UINT uiIndex)
{
    WCHAR Buf[MAX_PATH];
    UINT Current;
    LONG Err;

    swprintf(Buf, L"Control Panel\\PowerCfg\\PowerPolicies\\%d", uiIndex);

    if (!GetActivePwrScheme(&Current))
        return FALSE;

    if (Current == uiIndex)
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    Err = RegDeleteKey(HKEY_CURRENT_USER, (LPCTSTR)Buf);
    if (Err != ERROR_SUCCESS)
    {
        TRACE("RegDeleteKey failed: %d\n", Err);
        SetLastError(Err);
        return FALSE;
    }

    return TRUE;
}

static BOOLEAN
POWRPROF_GetUserPowerPolicy(LPWSTR szNum,
                            PUSER_POWER_POLICY puserPwrPolicy,
                            DWORD cchName, LPWSTR szName,
                            DWORD cchDesc, LPWSTR szDesc)
{
    HKEY hSubKey = NULL;
    DWORD dwSize;
    LONG Err;
    WCHAR szPath[MAX_PATH];
    BOOL bRet = FALSE;

    swprintf(szPath, L"Control Panel\\PowerCfg\\PowerPolicies\\%s", szNum);

    Err = RegOpenKeyExW(HKEY_CURRENT_USER, szPath, 0, KEY_READ, &hSubKey);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegOpenKeyExW failed: %d\n", Err);
        SetLastError(Err);
        return FALSE;
    }

    dwSize = cchName * sizeof(WCHAR);
    Err = RegQueryValueExW(hSubKey, L"Name", NULL, NULL, (LPBYTE)szName, &dwSize);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegQueryValueExW failed: %d\n", Err);
        SetLastError(Err);
        goto cleanup;
    }

    dwSize = cchDesc * sizeof(WCHAR);
    Err = RegQueryValueExW(hSubKey, L"Description", NULL, NULL, (LPBYTE)szDesc, &dwSize);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegQueryValueExW failed: %d\n", Err);
        SetLastError(Err);
        goto cleanup;
    }

    dwSize = sizeof(USER_POWER_POLICY);
    Err = RegQueryValueExW(hSubKey, L"Policies", NULL, NULL, (LPBYTE)puserPwrPolicy, &dwSize);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegQueryValueExW failed: %d\n", Err);
        SetLastError(Err);
        goto cleanup;
    }

    bRet = TRUE;

cleanup:
    RegCloseKey(hSubKey);

    return bRet;
}

static BOOLEAN
POWRPROF_GetMachinePowerPolicy(LPWSTR szNum, PMACHINE_POWER_POLICY pmachinePwrPolicy)
{
    HKEY hKey;
    LONG Err;
    WCHAR szPath[MAX_PATH];
    DWORD dwSize;

    swprintf(szPath, L"Software\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\PowerCfg\\PowerPolicies\\%s", szNum);

    Err = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPath, 0, KEY_READ, &hKey);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegOpenKeyExW failed: %d\n", Err);
        SetLastError(Err);
        return FALSE;
    }

    dwSize = sizeof(MACHINE_POWER_POLICY);
    Err = RegQueryValueExW(hKey, L"Policies", NULL, NULL, (LPBYTE)pmachinePwrPolicy, &dwSize);

    if (Err != ERROR_SUCCESS)
    {
        ERR("RegQueryValueExW failed: %d\n", Err);
        SetLastError(Err);
        RegCloseKey(hKey);
        return FALSE;
    }

    RegCloseKey(hKey);

    return TRUE;
}

BOOLEAN WINAPI
EnumPwrSchemes(PWRSCHEMESENUMPROC lpfnPwrSchemesEnumProc,
               LPARAM lParam)
{
    HKEY hKey;
    LONG Err;
    DWORD dwSize, dwNameSize = MAX_PATH, dwDescSize = MAX_PATH, dwIndex = 0;
    WCHAR szNum[3 + 1], szName[MAX_PATH], szDesc[MAX_PATH];
    POWER_POLICY PwrPolicy;
    USER_POWER_POLICY userPwrPolicy;
    MACHINE_POWER_POLICY machinePwrPolicy;
    BOOLEAN bRet = FALSE;

    if (!lpfnPwrSchemesEnumProc)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Err = RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\PowerCfg\\PowerPolicies", 0, KEY_READ, &hKey);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegOpenKeyW failed: %d\n", Err);
        SetLastError(Err);
        return FALSE;
    }

    ReleaseSemaphore(PPRegSemaphore, 1, NULL);

    dwSize = sizeof(szNum) / sizeof(WCHAR);

    while (RegEnumKeyExW(hKey, dwIndex, szNum, &dwSize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
    {
        if (!POWRPROF_GetUserPowerPolicy(szNum, &userPwrPolicy,
                                         dwNameSize, szName,
                                         dwDescSize, szDesc))
        {
            WARN("POWRPROF_GetUserPowerPolicy failed\n");
            goto cleanup;
        }

        if (!POWRPROF_GetMachinePowerPolicy(szNum, &machinePwrPolicy))
        {
            WARN("POWRPROF_GetMachinePowerPolicy failed\n");
            goto cleanup;
        }

        memcpy(&PwrPolicy.user, &userPwrPolicy, sizeof(USER_POWER_POLICY));
        memcpy(&PwrPolicy.mach, &machinePwrPolicy, sizeof(MACHINE_POWER_POLICY));

        if (!lpfnPwrSchemesEnumProc(_wtoi(szNum), (wcslen(szName) + 1) * sizeof(WCHAR), szName, (wcslen(szDesc) + 1) * sizeof(WCHAR), szDesc, &PwrPolicy, lParam))
            goto cleanup;
        else
            bRet = TRUE;

        dwSize = sizeof(szNum) / sizeof(WCHAR);
        dwIndex++;
    }

cleanup:
    RegCloseKey(hKey);
    ReleaseSemaphore(PPRegSemaphore, 1, NULL);

    return bRet;
}

BOOLEAN WINAPI
GetActivePwrScheme(PUINT puiID)
{
    HKEY hKey;
    WCHAR szBuf[MAX_PATH];
    DWORD dwSize;
    LONG Err;

    TRACE("GetActivePwrScheme(%u)", puiID);

    Err = RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\PowerCfg", 0, KEY_READ, &hKey);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegOpenKey failed: %d\n", Err);
        SetLastError(Err);
        return FALSE;
    }

    dwSize = sizeof(szBuf);
    Err = RegQueryValueExW(hKey, L"CurrentPowerPolicy",
                           NULL, NULL,
                           (LPBYTE)&szBuf, &dwSize);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegQueryValueEx failed: %d\n", Err);
        RegCloseKey(hKey);
        SetLastError(Err);
        return FALSE;
    }

    RegCloseKey(hKey);
    *puiID = _wtoi(szBuf);

    return TRUE;
}

BOOLEAN WINAPI
GetCurrentPowerPolicies(PGLOBAL_POWER_POLICY pGlobalPowerPolicy,
                        PPOWER_POLICY pPowerPolicy)
{
    /*
    SYSTEM_POWER_POLICY ACPower, DCPower;

    FIXME("(%p, %p) stub!\n", pGlobalPowerPolicy, pPowerPolicy);

    NtPowerInformation(SystemPowerPolicyAc, 0, 0, &ACPower, sizeof(SYSTEM_POWER_POLICY));
    NtPowerInformation(SystemPowerPolicyDc, 0, 0, &DCPower, sizeof(SYSTEM_POWER_POLICY));

    return FALSE;
    */
/*
   Lohnegrim: I don't know why this Function should call NtPowerInformation, because as far as I know,
      it simply returns the GlobalPowerPolicy and the AktivPowerScheme!
 */
    UINT uiID;

    if (pGlobalPowerPolicy != NULL)
    {
        if (!ReadGlobalPwrPolicy(pGlobalPowerPolicy))
            return FALSE;
    }
    if (pPowerPolicy != NULL)
    {
        if (!GetActivePwrScheme(&uiID))
            return FALSE;

        if (!ReadPwrScheme(uiID, pPowerPolicy))
            return FALSE;
    }

    return TRUE;
}

BOOLEAN WINAPI
GetPwrCapabilities(PSYSTEM_POWER_CAPABILITIES lpSystemPowerCapabilities)
{
    NTSTATUS Status;

    TRACE("(%p)\n", lpSystemPowerCapabilities);

    if (!lpSystemPowerCapabilities)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Status = NtPowerInformation(SystemPowerCapabilities, 0, 0, lpSystemPowerCapabilities, sizeof(SYSTEM_POWER_CAPABILITIES));
    if(!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return TRUE;
}

BOOLEAN WINAPI
GetPwrDiskSpindownRange(PUINT RangeMax, PUINT RangeMin)
{
    HKEY hKey;
    BYTE lpValue[40];
    LONG Ret;
    DWORD cbValue = sizeof(lpValue);

    TRACE("(%p, %p)\n", RangeMax, RangeMin);

    if (RangeMax == NULL || RangeMin == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!AcquirePwrProfSemaphore())
        return FALSE;

    Ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPowerCfgSubKey, 0, KEY_READ, &hKey);
    if (Ret != ERROR_SUCCESS)
    {
        TRACE("RegOpenKeyEx failed: %d\n", Ret);
        TRACE("Using defaults: 3600, 3\n");
        *RangeMax = 3600;
        *RangeMin = 3;
        ReleaseSemaphore(PPRegSemaphore, 1, NULL);
        return TRUE;
    }

    Ret = RegQueryValueExW(hKey, szDiskMax, 0, 0, lpValue, &cbValue);
    if (Ret != ERROR_SUCCESS)
    {
        TRACE("Couldn't open DiskSpinDownMax: %d\n", Ret);
        TRACE("Using default: 3600\n");
        *RangeMax = 3600;
    }
    else
    {
        *RangeMax = _wtoi((LPCWSTR)lpValue);
    }

    cbValue = sizeof(lpValue);

    Ret = RegQueryValueExW(hKey, szDiskMin, 0, 0, lpValue, &cbValue);
    if (Ret != ERROR_SUCCESS)
    {
        TRACE("Couldn't open DiskSpinDownMin: %d\n", Ret);
        TRACE("Using default: 3\n");
        *RangeMin = 3;
    }
    else
    {
        *RangeMin = _wtoi((LPCWSTR)lpValue);
    }

    RegCloseKey(hKey);

    ReleaseSemaphore(PPRegSemaphore, 1, NULL);

    return TRUE;
}

BOOLEAN WINAPI
IsAdminOverrideActive(PADMINISTRATOR_POWER_POLICY p)
{
    FIXME("( %p) stub!\n", p);
    return FALSE;
}

BOOLEAN WINAPI
IsPwrHibernateAllowed(VOID)
{
    SYSTEM_POWER_CAPABILITIES PowerCaps;
    NTSTATUS Status;
    BOOLEAN old;

    RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &old);

    Status = NtPowerInformation(SystemPowerCapabilities, NULL, 0, &PowerCaps, sizeof(PowerCaps));
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return IS_PWR_HIBERNATE_ALLOWED(&PowerCaps);
}

BOOLEAN WINAPI
IsPwrShutdownAllowed(VOID)
{
    SYSTEM_POWER_CAPABILITIES PowerCaps;
    NTSTATUS Status;
    BOOLEAN old;

    RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &old);

    Status = NtPowerInformation(SystemPowerCapabilities, NULL, 0, &PowerCaps, sizeof(PowerCaps));
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    return IS_PWR_POWEROFF_ALLOWED(&PowerCaps);
}

BOOLEAN WINAPI
IsPwrSuspendAllowed(VOID)
{
    SYSTEM_POWER_CAPABILITIES PowerCaps;
    NTSTATUS Status;
    BOOLEAN old;

    RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &old);

    Status = NtPowerInformation(SystemPowerCapabilities, NULL, 0, &PowerCaps, sizeof(PowerCaps));
    if (!NT_SUCCESS(Status))
    {
        SetLastError(RtlNtStatusToDosError(Status));
        return FALSE;
    }

    // NOTE for possible future wine-syncs: wine erroneously use '&&'
    // instead of: PowerCaps.SystemS1 || PowerCaps.SystemS2 || PowerCaps.SystemS3;
    return IS_PWR_SUSPEND_ALLOWED(&PowerCaps);
}

DWORD WINAPI
PowerGetActiveScheme(HKEY UserRootPowerKey, GUID **polguid)
{
   FIXME("(%p,%p) stub!\n", UserRootPowerKey, polguid);
   return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI PowerSetActiveScheme(HKEY UserRootPowerKey, const GUID *polguid)
{
   FIXME("(%p,%s) stub!\n", UserRootPowerKey, wine_dbgstr_guid(polguid));
   return ERROR_SUCCESS;
}

POWER_PLATFORM_ROLE WINAPI PowerDeterminePlatformRole(void)
{
   FIXME("stub\n");
   return PlatformRoleDesktop;
}

POWER_PLATFORM_ROLE WINAPI PowerDeterminePlatformRoleEx(ULONG version)
{
    FIXME("%lu stub.\n", version);
    return PlatformRoleDesktop;
}

DWORD WINAPI PowerEnumerate(HKEY key, const GUID *scheme, const GUID *subgroup, POWER_DATA_ACCESSOR flags,
                        ULONG index, UCHAR *buffer, DWORD *buffer_size)
{
   FIXME("(%p,%s,%s,%d,%ld,%p,%p) stub!\n", key, debugstr_guid(scheme), debugstr_guid(subgroup),
                flags, index, buffer, buffer_size);
   return ERROR_CALL_NOT_IMPLEMENTED;
}

DWORD WINAPI PowerRegisterSuspendResumeNotification(DWORD flags, HANDLE recipient, PHPOWERNOTIFY handle)
{
    FIXME("(0x%08lx,%p,%p) stub!\n", flags, recipient, handle);
    *handle = (HPOWERNOTIFY)0xdeadbeef;
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerUnregisterSuspendResumeNotification(HPOWERNOTIFY handle)
{
    FIXME("(%p) stub!\n", handle);
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerSettingRegisterNotification(const GUID *setting, DWORD flags, HANDLE recipient, PHPOWERNOTIFY handle)
{
    FIXME("(%s,0x%08lx,%p,%p) stub!\n", debugstr_guid(setting), flags, recipient, handle);
    *handle = (PHPOWERNOTIFY)0xdeadbeef;
    return ERROR_SUCCESS;
}

DWORD WINAPI PowerSettingUnregisterNotification(HPOWERNOTIFY handle)
{
    FIXME("(%p) stub!\n", handle);
    return ERROR_SUCCESS;
}

BOOLEAN WINAPI
ReadGlobalPwrPolicy(PGLOBAL_POWER_POLICY pGlobalPowerPolicy)
{
    GLOBAL_MACHINE_POWER_POLICY glMachPwrPolicy;
    GLOBAL_USER_POWER_POLICY glUserPwrPolicy;
    HKEY hKey = NULL;
    DWORD dwSize;
    LONG Err;
    BOOL bRet = FALSE;

    ReleaseSemaphore(PPRegSemaphore, 1, NULL);

    // Getting user global power policy
    Err = RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\PowerCfg\\GlobalPowerPolicy", 0, KEY_READ, &hKey);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegOpenKeyW failed: %d\n", Err);
        SetLastError(Err);
        goto cleanup;
    }

    dwSize = sizeof(glUserPwrPolicy);
    Err = RegQueryValueExW(hKey, L"Policies", NULL, NULL, (LPBYTE)&glUserPwrPolicy, &dwSize);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegQueryValueExW failed: %d\n", Err);
        SetLastError(Err);
        goto cleanup;
    }

    RegCloseKey(hKey);

    // Getting machine global power policy
    Err = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\PowerCfg\\GlobalPowerPolicy", 0, KEY_READ, &hKey);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegOpenKeyW failed: %d\n", Err);
        SetLastError(Err);
        goto cleanup;
    }

    dwSize = sizeof(glMachPwrPolicy);
    Err = RegQueryValueExW(hKey, L"Policies", NULL, NULL, (LPBYTE)&glMachPwrPolicy, &dwSize);
    if (Err != ERROR_SUCCESS)
    {
        ERR("RegQueryValueExW failed: %d\n", Err);
        SetLastError(Err);
        goto cleanup;
    }

    memcpy(&pGlobalPowerPolicy->user, &glUserPwrPolicy, sizeof(GLOBAL_USER_POWER_POLICY));
    memcpy(&pGlobalPowerPolicy->mach, &glMachPwrPolicy, sizeof(GLOBAL_MACHINE_POWER_POLICY));
    bRet = TRUE;

cleanup:
    if(hKey)
        RegCloseKey(hKey);
    ReleaseSemaphore(PPRegSemaphore, 1, NULL);

    return bRet;
}


BOOLEAN WINAPI
ReadProcessorPwrScheme(UINT uiID,
                       PMACHINE_PROCESSOR_POWER_POLICY pMachineProcessorPowerPolicy)
{
    HKEY hKey;
    WCHAR szPath[MAX_PATH];
    DWORD dwSize = sizeof(MACHINE_PROCESSOR_POWER_POLICY);

    swprintf(szPath, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\PowerCfg\\ProcessorPolicies\\%i", uiID);
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, szPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return FALSE;

    if (RegQueryValueExW(hKey, szPolicies, NULL, 0, (LPBYTE)pMachineProcessorPowerPolicy, &dwSize) == ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        return TRUE;
    }

    RegCloseKey(hKey);
    if (uiID != 0)
        return ReadProcessorPwrScheme(0, pMachineProcessorPowerPolicy);

    return FALSE;
}


BOOLEAN WINAPI
ReadPwrScheme(UINT uiID,
    PPOWER_POLICY pPowerPolicy)
{
    USER_POWER_POLICY userPwrPolicy;
    MACHINE_POWER_POLICY machinePwrPolicy;
    WCHAR szNum[16]; // max number - 999

    ReleaseSemaphore(PPRegSemaphore, 1, NULL);

    swprintf(szNum, L"%d", uiID);

    if (!POWRPROF_GetUserPowerPolicy(szNum, &userPwrPolicy, 0, NULL, 0, NULL))
    {
        ReleaseSemaphore(PPRegSemaphore, 1, NULL);
        return FALSE;
    }

    if (!POWRPROF_GetMachinePowerPolicy(szNum, &machinePwrPolicy))
    {
        ReleaseSemaphore(PPRegSemaphore, 1, NULL);
        return FALSE;
    }

    memcpy(&pPowerPolicy->user, &userPwrPolicy, sizeof(userPwrPolicy));
    memcpy(&pPowerPolicy->mach, &machinePwrPolicy, sizeof(machinePwrPolicy));

    ReleaseSemaphore(PPRegSemaphore, 1, NULL);

    return TRUE;
}

BOOLEAN WINAPI
SetActivePwrScheme(UINT uiID,
                   PGLOBAL_POWER_POLICY lpGlobalPowerPolicy,
                   PPOWER_POLICY lpPowerPolicy)
{
    POWER_POLICY tmp;
    HKEY hKey;
    WCHAR Buf[16];

    if (!ReadPwrScheme(uiID, &tmp))
        return FALSE;

    if (RegOpenKeyEx(HKEY_CURRENT_USER, szUserPowerConfigSubKey, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return FALSE;

    swprintf(Buf, L"%i", uiID);

    if (RegSetValueExW(hKey, szCurrentPowerPolicies, 0, REG_SZ, (PBYTE)Buf, strlenW(Buf)*sizeof(WCHAR)) != ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        return FALSE;
    }
    RegCloseKey(hKey);

    if (lpGlobalPowerPolicy != NULL || lpPowerPolicy != NULL)
    {
        if (!ValidatePowerPolicies(lpGlobalPowerPolicy, lpPowerPolicy))
            return FALSE;

        if (lpGlobalPowerPolicy != NULL && !WriteGlobalPwrPolicy(lpGlobalPowerPolicy))
            return FALSE;

        if (lpPowerPolicy != NULL && !WritePwrPolicy(&uiID,lpPowerPolicy))
            return FALSE;
    }

    return TRUE;
}

BOOLEAN WINAPI
SetSuspendState(BOOLEAN Hibernate,
                BOOLEAN ForceCritical,
                BOOLEAN DisableWakeEvent)
{
    FIXME("(%d, %d, %d) stub!\n", Hibernate, ForceCritical, DisableWakeEvent);
    return TRUE;
}

BOOLEAN WINAPI
WriteGlobalPwrPolicy(PGLOBAL_POWER_POLICY pGlobalPowerPolicy)
{
    HKEY hKey;
    GLOBAL_USER_POWER_POLICY gupp;
    GLOBAL_MACHINE_POWER_POLICY gmpp;

    gupp = pGlobalPowerPolicy->user;
    gmpp = pGlobalPowerPolicy->mach;

    if (RegOpenKeyEx(HKEY_CURRENT_USER,
                    L"Control Panel\\PowerCfg\\GlobalPowerPolicy",
                    0,
                    KEY_WRITE,
                    &hKey) != ERROR_SUCCESS)
        return FALSE;

    if (RegSetValueExW(hKey, szPolicies, 0, REG_BINARY, (PBYTE)&gupp, sizeof(gupp)) != ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        return FALSE;
    }

    RegCloseKey(hKey);

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\PowerCfg\\GlobalPowerPolicy",
                     0,
                     KEY_ALL_ACCESS,
                     &hKey))
        return FALSE;

    if (RegSetValueExW(hKey,szPolicies, 0, REG_BINARY, (PBYTE)&gmpp, sizeof(gmpp)) != ERROR_SUCCESS)
    {
        RegCloseKey(hKey);
        return FALSE;
    }

    RegCloseKey(hKey);
    return TRUE;
}

BOOLEAN WINAPI
WriteProcessorPwrScheme(UINT ID,
                        PMACHINE_PROCESSOR_POWER_POLICY pMachineProcessorPowerPolicy)
{
    WCHAR Buf[MAX_PATH];
    HKEY hKey;

    swprintf(Buf, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\PowerCfg\\ProcessorPolicies\\%i", ID);

    if (RegCreateKey(HKEY_LOCAL_MACHINE, Buf, &hKey) != ERROR_SUCCESS)
        return FALSE;

    RegSetValueExW(hKey, szPolicies, 0, REG_BINARY, (PBYTE)pMachineProcessorPowerPolicy, sizeof(MACHINE_PROCESSOR_POWER_POLICY));
    RegCloseKey(hKey);
    return TRUE;
}

static VOID
SetLastID(VOID)
{
    WCHAR Buf[16];
    HKEY hKey;

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                    szPowerCfgSubKey,
                    0,
                    KEY_WRITE,
                    &hKey) != ERROR_SUCCESS)
        return;
    swprintf(Buf, L"%i", g_LastID);
    RegSetValueExW(hKey, szLastID, 0, REG_SZ, (PBYTE)Buf, strlenW(Buf)*sizeof(WCHAR));
    RegCloseKey(hKey);
}

BOOLEAN WINAPI
WritePwrScheme(PUINT puiID,
               LPWSTR lpszName,
               LPWSTR lpszDescription,
               PPOWER_POLICY pPowerPolicy)
{
    WCHAR Buf[MAX_PATH];
    HKEY hKey;

    if (*puiID == -1)
    {
        g_LastID++;
        *puiID = g_LastID;
        SetLastID();
    }

    swprintf(Buf, L"Control Panel\\PowerCfg\\PowerPolicies\\%i", *puiID);

    if (RegCreateKey(HKEY_CURRENT_USER, Buf, &hKey) != ERROR_SUCCESS)
        return FALSE;

    RegSetValueExW(hKey, szName, 0, REG_SZ, (PBYTE)lpszName, strlenW(lpszName)*sizeof(WCHAR));
    RegSetValueExW(hKey, szDescription, 0, REG_SZ, (PBYTE)lpszDescription, strlenW(lpszDescription)*sizeof(WCHAR));
    RegCloseKey(hKey);
    return WritePwrPolicy(puiID, pPowerPolicy);
}

static BOOLEAN
CheckPowerActionPolicy(PPOWER_ACTION_POLICY pPAP, SYSTEM_POWER_CAPABILITIES PowerCaps)
{
/*
   Lohnegrim: this is an Helper function, it checks if the POWERACTIONPOLICY is valid
   Also, if the System doesn't support Hibernation, then change the PowerAction
*/
    switch (pPAP->Action)
    {
    case PowerActionNone:
        return TRUE;
    case PowerActionReserved:
        if (PowerCaps.SystemS1 || PowerCaps.SystemS2 || PowerCaps.SystemS3)
            pPAP->Action = PowerActionSleep;
        else
            pPAP->Action = PowerActionReserved;
    case PowerActionSleep:
        return TRUE;
    case PowerActionHibernate:
        if (!(PowerCaps.SystemS4 && PowerCaps.HiberFilePresent))
        {
            if (PowerCaps.SystemS1 || PowerCaps.SystemS2 || PowerCaps.SystemS3)
                pPAP->Action = PowerActionSleep;
            else
                pPAP->Action = PowerActionReserved;
        }
    case PowerActionShutdown:
    case PowerActionShutdownReset:
    case PowerActionShutdownOff:
    case PowerActionWarmEject:
        return TRUE;
    default:
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    };
}

static VOID
FixSystemPowerState(PSYSTEM_POWER_STATE Psps, SYSTEM_POWER_CAPABILITIES PowerCaps)
{
	//Lohnegrim: If the System doesn't support the Powerstates, then we have to change them
    if (!PowerCaps.SystemS1 && *Psps == PowerSystemSleeping1)
        *Psps = PowerSystemSleeping2;
    if (!PowerCaps.SystemS2 && *Psps == PowerSystemSleeping2)
        *Psps = PowerSystemSleeping3;
    if (!PowerCaps.SystemS3 && *Psps == PowerSystemSleeping3)
        *Psps = PowerSystemHibernate;
    if (!(PowerCaps.SystemS4 && PowerCaps.HiberFilePresent) && *Psps == PowerSystemHibernate)
        *Psps = PowerSystemSleeping2;
    if (!PowerCaps.SystemS1 && *Psps == PowerSystemSleeping1)
        *Psps = PowerSystemSleeping2;
    if (!PowerCaps.SystemS2 && *Psps == PowerSystemSleeping2)
        *Psps = PowerSystemSleeping3;
    if (!PowerCaps.SystemS3 && *Psps == PowerSystemSleeping3)
        *Psps = PowerSystemShutdown;

}

BOOLEAN WINAPI
ValidatePowerPolicies(PGLOBAL_POWER_POLICY pGPP, PPOWER_POLICY pPP)
{
    SYSTEM_POWER_CAPABILITIES PowerCaps;
    NTSTATUS ret;
    BOOLEAN old;

    RtlAdjustPrivilege(SE_SHUTDOWN_PRIVILEGE, TRUE, FALSE, &old);
    ret = NtPowerInformation(SystemPowerCapabilities, NULL, 0, &PowerCaps, sizeof(PowerCaps));
    if (ret != STATUS_SUCCESS)
    {
        SetLastError(RtlNtStatusToDosError(ret));
        return FALSE;
    }

    if (pGPP)
    {
        if (pGPP->user.Revision != 1 || pGPP->mach.Revision != 1)
        {
            SetLastError(ERROR_REVISION_MISMATCH);
            return FALSE;
        }
        if (pGPP->mach.LidOpenWakeAc == PowerSystemUnspecified)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if ((int)pGPP->mach.LidOpenWakeAc > PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if (pGPP->mach.LidOpenWakeDc < PowerSystemWorking)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if ((int)pGPP->mach.LidOpenWakeDc > PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
		//Lohnegrim: unneeded
        /*if ((pGPP->mach.LidOpenWakeDc < PowerSystemWorking) || (pGPP->mach.LidOpenWakeDc >= PowerSystemMaximum))
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }*/
        if (!CheckPowerActionPolicy(&pGPP->user.LidCloseAc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pGPP->user.LidCloseDc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pGPP->user.PowerButtonAc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pGPP->user.PowerButtonDc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pGPP->user.SleepButtonAc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pGPP->user.SleepButtonDc,PowerCaps))
        {
            return FALSE;
        }
        //Lohnegrim: The BroadcastCapacityResolution presents the Powerlevel in Percent, if invalid set th 100 == FULL
        if (pGPP->mach.BroadcastCapacityResolution > 100)
            pGPP->mach.BroadcastCapacityResolution = 100;

		//Lohnegrim: I have no idea, if they are really needed, or if they are specific for my System, or what they mean, so I removed them
        //pGPP->user.DischargePolicy[1].PowerPolicy.EventCode = pGPP->user.DischargePolicy[1].PowerPolicy.EventCode | 0x010000;
        //pGPP->user.DischargePolicy[2].PowerPolicy.EventCode = pGPP->user.DischargePolicy[2].PowerPolicy.EventCode | 0x020000;
        //pGPP->user.DischargePolicy[3].PowerPolicy.EventCode = pGPP->user.DischargePolicy[3].PowerPolicy.EventCode | 0x030000;

        FixSystemPowerState(&pGPP->mach.LidOpenWakeAc,PowerCaps);
        FixSystemPowerState(&pGPP->mach.LidOpenWakeDc,PowerCaps);

    }

    if (pPP)
    {
        if (pPP->user.Revision != 1 || pPP->mach.Revision != 1)
        {
            SetLastError(ERROR_REVISION_MISMATCH);
            return FALSE;
        }

		//Lohnegrim: unneeded
        //if (pPP->mach.MinSleepAc < PowerSystemWorking)
        //{
        //    SetLastError(ERROR_GEN_FAILURE);
        //    return FALSE;
        //}
        if ((int)pPP->mach.MinSleepAc >= PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
		//Lohnegrim: unneeded
        //if (pPP->mach.MinSleepDc < PowerSystemWorking)
        //{
        //    SetLastError(ERROR_GEN_FAILURE);
        //    return FALSE;
        //}
        if ((int)pPP->mach.MinSleepDc >= PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if ((int)pPP->mach.ReducedLatencySleepAc == PowerSystemUnspecified)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if ((int)pPP->mach.ReducedLatencySleepAc > PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if ((int)pPP->mach.ReducedLatencySleepDc < PowerSystemWorking)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
        if ((int)pPP->mach.ReducedLatencySleepDc > PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }

        if (!CheckPowerActionPolicy(&pPP->mach.OverThrottledAc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pPP->mach.OverThrottledDc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pPP->user.IdleAc,PowerCaps))
        {
            return FALSE;
        }
        if (!CheckPowerActionPolicy(&pPP->user.IdleDc,PowerCaps))
        {
            return FALSE;
        }
        if (pPP->user.MaxSleepAc < PowerSystemWorking)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
		//Lohnegrim: unneeded
        /*if ((int)pPP->user.MaxSleepAc > PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }*/
        if (pPP->user.MaxSleepDc < PowerSystemWorking)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }
		//Lohnegrim: unneeded
        /*if ((int)pPP->user.MaxSleepDc >= PowerSystemShutdown)
        {
            SetLastError(ERROR_GEN_FAILURE);
            return FALSE;
        }*/
        if (PowerCaps.SystemS1)
        {
            pPP->mach.MinSleepAc=PowerSystemSleeping1;
            pPP->mach.MinSleepDc=PowerSystemSleeping1;
        }
        else if (PowerCaps.SystemS2)
        {
            pPP->mach.MinSleepAc=PowerSystemSleeping2;
            pPP->mach.MinSleepDc=PowerSystemSleeping2;
        }
        else if (PowerCaps.SystemS3)
        {
            pPP->mach.MinSleepAc=PowerSystemSleeping3;
            pPP->mach.MinSleepDc=PowerSystemSleeping3;
        }

        if (PowerCaps.SystemS4)
        {
            pPP->user.MaxSleepAc=PowerSystemSleeping3;
            pPP->user.MaxSleepDc=PowerSystemSleeping3;
        }
        else if (PowerCaps.SystemS3)
        {
            pPP->user.MaxSleepAc=PowerSystemSleeping2;
            pPP->user.MaxSleepDc=PowerSystemSleeping2;
        }
        else if (PowerCaps.SystemS1)
        {
            pPP->user.MaxSleepAc=PowerSystemSleeping1;
            pPP->user.MaxSleepDc=PowerSystemSleeping1;
        }
		//Lohnegrim: I don't know where to get this info from, so I removed it
        //pPP->user.OptimizeForPowerAc=TRUE;
        //pPP->user.OptimizeForPowerDc=TRUE;

        FixSystemPowerState(&pPP->mach.ReducedLatencySleepAc,PowerCaps);
        FixSystemPowerState(&pPP->mach.ReducedLatencySleepDc,PowerCaps);
    }

    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOLEAN WINAPI WritePwrPolicy(PUINT puiID, PPOWER_POLICY pPowerPolicy)
{
    WCHAR Buf[MAX_PATH];
    HKEY hKey;

    swprintf(Buf, L"Control Panel\\PowerCfg\\PowerPolicies\\%i", *puiID);

    if (RegCreateKey(HKEY_CURRENT_USER, Buf, &hKey) != ERROR_SUCCESS)
        return FALSE;

    RegSetValueExW(hKey, szPolicies, 0, REG_BINARY, (const unsigned char *)&pPowerPolicy->user, sizeof(USER_POWER_POLICY));
    RegCloseKey(hKey);

    swprintf(Buf, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Controls Folder\\PowerCfg\\PowerPolicies\\%i", *puiID);

    if (RegCreateKey(HKEY_LOCAL_MACHINE, Buf, &hKey) != ERROR_SUCCESS)
        return FALSE;

    RegSetValueExW(hKey, szPolicies, 0, REG_BINARY, (const unsigned char *)&pPowerPolicy->mach, sizeof(MACHINE_POWER_POLICY));
    RegCloseKey(hKey);

    return TRUE;
}

/* GUID based power settings, kept in the same registry layout Windows uses */

#define PWRP_GUID_CCH 37

static const WCHAR szPowerSettingsKey[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerSettings";
static const WCHAR szPowerSchemesKey[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Power\\User\\PowerSchemes";
static const WCHAR szDefaultSchemeValues[] = L"DefaultPowerSchemeValues";
static const WCHAR szFriendlyName[] = L"FriendlyName";
static const WCHAR szIconResource[] = L"IconResource";
static const WCHAR szAttributes[] = L"Attributes";
static const WCHAR szSettingValue[] = L"SettingValue";
static const WCHAR szValueMin[] = L"ValueMin";
static const WCHAR szValueMax[] = L"ValueMax";
static const WCHAR szValueIncrement[] = L"ValueIncrement";
static const WCHAR szValueUnits[] = L"ValueUnits";
static const WCHAR szACSettingIndex[] = L"ACSettingIndex";
static const WCHAR szDCSettingIndex[] = L"DCSettingIndex";
static const WCHAR szProvACSettingIndex[] = L"ProvAcSettingIndex";
static const WCHAR szProvDCSettingIndex[] = L"ProvDcSettingIndex";

static const GUID PwrpNoSubGroup =
    {0xfea3413e, 0x7e05, 0x4911, {0x9a, 0x71, 0x70, 0x03, 0x31, 0xf1, 0xc2, 0x94}};
static const GUID PwrpBalancedScheme =
    {0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
static const GUID PwrpHighPerformanceScheme =
    {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
static const GUID PwrpPowerSaverScheme =
    {0xa1841308, 0x3541, 0x4fab, {0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a}};
static const GUID PwrpBestEfficiencyOverlay =
    {0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
static const GUID PwrpBetterPerformanceOverlay =
    {0x3af9b8d9, 0x7c97, 0x431d, {0xad, 0x78, 0x34, 0xa8, 0xbf, 0xea, 0x43, 0x9f}};
static const GUID PwrpBestPerformanceOverlay =
    {0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};

static VOID
PwrpGuidToKeyName(
    _In_ const GUID *Guid,
    _Out_writes_(PWRP_GUID_CCH) PWSTR Name)
{
    swprintf(Name,
             L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             Guid->Data1,
             Guid->Data2,
             Guid->Data3,
             Guid->Data4[0],
             Guid->Data4[1],
             Guid->Data4[2],
             Guid->Data4[3],
             Guid->Data4[4],
             Guid->Data4[5],
             Guid->Data4[6],
             Guid->Data4[7]);
}

static LONG
PwrpOpenGuidKey(
    _In_ HKEY Parent,
    _In_ const GUID *Guid,
    _In_ BOOLEAN Create,
    _In_ REGSAM Access,
    _Out_ PHKEY Key)
{
    WCHAR Name[PWRP_GUID_CCH];

    PwrpGuidToKeyName(Guid, Name);
    if (Create)
        return RegCreateKeyExW(Parent, Name, 0, NULL, 0, Access, NULL, Key, NULL);

    return RegOpenKeyExW(Parent, Name, 0, Access, Key);
}

static BOOLEAN
PwrpIsConsoleSession(VOID)
{
    DWORD SessionId;

    if (!ProcessIdToSessionId(GetCurrentProcessId(), &SessionId))
        return FALSE;

    return (SessionId == WTSGetActiveConsoleSessionId());
}

static BOOLEAN
PwrpIsUserAdmin(VOID)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    BOOL IsMember = FALSE;
    PSID AdminsSid;

    if (!AllocateAndInitializeSid(&NtAuthority,
                                  2,
                                  SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0,
                                  &AdminsSid))
    {
        return FALSE;
    }

    if (!CheckTokenMembership(NULL, AdminsSid, &IsMember))
        IsMember = FALSE;

    FreeSid(AdminsSid);
    return !!IsMember;
}

/**
 * @brief
 * Opens <RootKey>\<SubGroup>\<Setting>, or the subgroup key when no setting
 * is given. Without a real subgroup the setting is looked up under the root
 * first and then under every subgroup, and FoundGroup receives the name of
 * the subgroup key it was found in, or an empty string for the root.
 */
static LONG
PwrpOpenGuidPath(
    _In_ HKEY RootKey,
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ BOOLEAN Create,
    _In_ REGSAM Access,
    _Out_ PHKEY Key,
    _Out_writes_opt_(MAX_PATH) PWSTR FoundGroup)
{
    WCHAR GroupName[MAX_PATH];
    DWORD Index, NameLength;
    LONG Error, EnumError;
    HKEY GroupKey;

    if (!SubGroup && !Setting)
        return ERROR_INVALID_PARAMETER;

    if (SubGroup && !IsEqualGUID(SubGroup, &PwrpNoSubGroup))
    {
        Error = PwrpOpenGuidKey(RootKey, SubGroup, Create, Access, &GroupKey);
        if (Error != ERROR_SUCCESS)
            return Error;

        if (!Setting)
        {
            *Key = GroupKey;
            return ERROR_SUCCESS;
        }

        Error = PwrpOpenGuidKey(GroupKey, Setting, Create, Access, Key);
        RegCloseKey(GroupKey);
        return Error;
    }

    if (!Setting)
        return RegOpenKeyExW(RootKey, NULL, 0, Access, Key);

    Error = PwrpOpenGuidKey(RootKey, Setting, FALSE, Access, Key);
    if (Error == ERROR_SUCCESS && FoundGroup)
        FoundGroup[0] = UNICODE_NULL;

    for (Index = 0; Error == ERROR_FILE_NOT_FOUND; Index++)
    {
        NameLength = _countof(GroupName);
        EnumError = RegEnumKeyExW(RootKey, Index, GroupName, &NameLength, NULL, NULL, NULL, NULL);
        if (EnumError == ERROR_NO_MORE_ITEMS)
            break;
        if (EnumError != ERROR_SUCCESS)
            continue;

        if (RegOpenKeyExW(RootKey, GroupName, 0, Access, &GroupKey) != ERROR_SUCCESS)
            continue;

        Error = PwrpOpenGuidKey(GroupKey, Setting, FALSE, Access, Key);
        RegCloseKey(GroupKey);
        if (Error == ERROR_SUCCESS && FoundGroup)
            wcscpy(FoundGroup, GroupName);
    }

    /* A setting that does not exist anywhere yet goes under the root */
    if (Error == ERROR_FILE_NOT_FOUND && Create)
    {
        Error = PwrpOpenGuidKey(RootKey, Setting, TRUE, Access, Key);
        if (Error == ERROR_SUCCESS && FoundGroup)
            FoundGroup[0] = UNICODE_NULL;
    }

    return Error;
}

static LONG
PwrpOpenSettingKey(
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ BOOLEAN Create,
    _In_ REGSAM Access,
    _Out_ PHKEY Key)
{
    HKEY RootKey;
    LONG Error;

    if (!SubGroup && !Setting)
        return ERROR_INVALID_PARAMETER;

    if (Create)
    {
        Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                                szPowerSettingsKey,
                                0,
                                NULL,
                                0,
                                Access | KEY_ENUMERATE_SUB_KEYS,
                                NULL,
                                &RootKey,
                                NULL);
    }
    else
    {
        Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                              szPowerSettingsKey,
                              0,
                              Access | KEY_ENUMERATE_SUB_KEYS,
                              &RootKey);
    }
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = PwrpOpenGuidPath(RootKey, SubGroup, Setting, Create, Access, Key, NULL);
    RegCloseKey(RootKey);
    return Error;
}

/**
 * @brief
 * Opens the key that holds a name or description. The scheme is only used
 * when neither a subgroup nor a setting is given.
 */
static LONG
PwrpOpenNameKey(
    _In_opt_ const GUID *Scheme,
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ BOOLEAN Create,
    _In_ REGSAM Access,
    _Out_ PHKEY Key)
{
    HKEY SchemesKey;
    LONG Error;

    if (SubGroup || Setting)
        return PwrpOpenSettingKey(SubGroup, Setting, Create, Access, Key);

    if (!Scheme)
        return ERROR_INVALID_PARAMETER;

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPowerSchemesKey, 0, Access, &SchemesKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = PwrpOpenGuidKey(SchemesKey, Scheme, FALSE, Access, Key);
    RegCloseKey(SchemesKey);
    return Error;
}

static LONG
PwrpOpenIndexKey(
    _In_ HKEY SettingKey,
    _In_ ULONG Index,
    _In_ BOOLEAN Create,
    _In_ REGSAM Access,
    _Out_ PHKEY Key)
{
    WCHAR Name[16];

    swprintf(Name, L"%lu", Index);
    if (Create)
        return RegCreateKeyExW(SettingKey, Name, 0, NULL, 0, Access, NULL, Key, NULL);

    return RegOpenKeyExW(SettingKey, Name, 0, Access, Key);
}

/**
 * @brief
 * Opens DefaultPowerSchemeValues\<Scheme> of a setting. When reading, a
 * scheme without its own defaults falls back to the balanced defaults.
 */
static LONG
PwrpOpenDefaultKey(
    _In_opt_ const GUID *Scheme,
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ BOOLEAN Create,
    _Out_ PHKEY Key)
{
    REGSAM Access = Create ? (KEY_WRITE | KEY_QUERY_VALUE) : KEY_READ;
    HKEY SettingKey, ValuesKey;
    LONG Error;

    if (!Scheme || !Setting)
        return ERROR_INVALID_PARAMETER;

    Error = PwrpOpenSettingKey(SubGroup, Setting, Create, Access, &SettingKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (Create)
    {
        Error = RegCreateKeyExW(SettingKey,
                                szDefaultSchemeValues,
                                0,
                                NULL,
                                0,
                                Access,
                                NULL,
                                &ValuesKey,
                                NULL);
    }
    else
    {
        Error = RegOpenKeyExW(SettingKey, szDefaultSchemeValues, 0, Access, &ValuesKey);
    }
    RegCloseKey(SettingKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = PwrpOpenGuidKey(ValuesKey, Scheme, Create, Access, Key);
    if (Error == ERROR_FILE_NOT_FOUND && !Create)
        Error = PwrpOpenGuidKey(ValuesKey, &PwrpBalancedScheme, FALSE, Access, Key);

    RegCloseKey(ValuesKey);
    return Error;
}

/**
 * @brief
 * Returns a value with the PowerRead size rules: a NULL buffer only reports
 * the size and a short buffer fails with ERROR_MORE_DATA.
 */
static DWORD
PwrpCopyOut(
    _In_reads_bytes_(DataSize) const VOID *Data,
    _In_ DWORD DataSize,
    _Out_writes_bytes_opt_(*BufferSize) PUCHAR Buffer,
    _Inout_ LPDWORD BufferSize)
{
    if (!Buffer)
    {
        *BufferSize = DataSize;
        return ERROR_SUCCESS;
    }

    if (*BufferSize < DataSize)
    {
        *BufferSize = DataSize;
        return ERROR_MORE_DATA;
    }

    CopyMemory(Buffer, Data, DataSize);
    *BufferSize = DataSize;
    return ERROR_SUCCESS;
}

static DWORD
PwrpQueryValue(
    _In_ HKEY Key,
    _In_ PCWSTR ValueName,
    _Out_opt_ PULONG Type,
    _Out_writes_bytes_opt_(*BufferSize) PUCHAR Buffer,
    _Inout_ LPDWORD BufferSize)
{
    DWORD DataType, DataSize = 0;
    PBYTE Data;
    LONG Error;

    Error = RegQueryValueExW(Key, ValueName, NULL, &DataType, NULL, &DataSize);
    if (Error != ERROR_SUCCESS)
        return Error;

    Data = HeapAlloc(GetProcessHeap(), 0, DataSize ? DataSize : 1);
    if (!Data)
        return ERROR_NOT_ENOUGH_MEMORY;

    Error = RegQueryValueExW(Key, ValueName, NULL, &DataType, Data, &DataSize);
    if (Error == ERROR_SUCCESS)
    {
        if (Type)
            *Type = DataType;
        Error = PwrpCopyOut(Data, DataSize, Buffer, BufferSize);
    }

    HeapFree(GetProcessHeap(), 0, Data);
    return Error;
}

/**
 * @brief
 * Resolves an "@file,-id[,fallback]" string to the resource text, or to the
 * fallback text when the resource cannot be loaded.
 *
 * @return
 * A heap string the caller frees, or NULL to use the stored text as is.
 */
static PWSTR
PwrpResolveIndirectString(
    _In_ PCWSTR Value)
{
    WCHAR Path[MAX_PATH], Expanded[MAX_PATH], Resource[512];
    PCWSTR Comma, Fallback, Text = NULL;
    SIZE_T PathLength, Length = 0;
    HMODULE Module;
    PWSTR Result;
    DWORD Needed;
    INT Loaded;

    if (Value[0] != L'@')
        return NULL;

    Comma = wcschr(Value, L',');
    if (!Comma || Comma[1] != L'-')
        return NULL;

    PathLength = Comma - (Value + 1);
    if (PathLength >= _countof(Path))
        return NULL;

    CopyMemory(Path, Value + 1, PathLength * sizeof(WCHAR));
    Path[PathLength] = UNICODE_NULL;
    Fallback = wcschr(Comma + 1, L',');

    Needed = ExpandEnvironmentStringsW(Path, Expanded, _countof(Expanded));
    if (Needed && Needed <= _countof(Expanded))
    {
        Module = LoadLibraryExW(Expanded, NULL, LOAD_LIBRARY_AS_DATAFILE);
        if (Module)
        {
            Loaded = LoadStringW(Module, _wtoi(Comma + 2), Resource, (INT)_countof(Resource));
            FreeLibrary(Module);
            if (Loaded > 0)
            {
                Text = Resource;
                Length = Loaded;
            }
        }
    }

    if (!Text && Fallback)
    {
        Text = Fallback + 1;
        Length = wcslen(Text);
    }

    if (!Text)
        return NULL;

    Result = HeapAlloc(GetProcessHeap(), 0, (Length + 1) * sizeof(WCHAR));
    if (!Result)
        return NULL;

    CopyMemory(Result, Text, Length * sizeof(WCHAR));
    Result[Length] = UNICODE_NULL;
    return Result;
}

static DWORD
PwrpQueryString(
    _In_ HKEY Key,
    _In_ PCWSTR ValueName,
    _Out_writes_bytes_opt_(*BufferSize) PUCHAR Buffer,
    _Inout_ LPDWORD BufferSize)
{
    DWORD RawSize = 0;
    PWSTR Raw, Text;
    LONG Error;

    Error = RegQueryValueExW(Key, ValueName, NULL, NULL, NULL, &RawSize);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* Leave room for a terminator the stored data may lack */
    Raw = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, RawSize + sizeof(WCHAR));
    if (!Raw)
        return ERROR_NOT_ENOUGH_MEMORY;

    Error = RegQueryValueExW(Key, ValueName, NULL, NULL, (PBYTE)Raw, &RawSize);
    if (Error == ERROR_SUCCESS)
    {
        Text = PwrpResolveIndirectString(Raw);
        if (Text)
        {
            Error = PwrpCopyOut(Text, (DWORD)(wcslen(Text) + 1) * sizeof(WCHAR), Buffer, BufferSize);
            HeapFree(GetProcessHeap(), 0, Text);
        }
        else
        {
            Error = PwrpCopyOut(Raw, (DWORD)(wcslen(Raw) + 1) * sizeof(WCHAR), Buffer, BufferSize);
        }
    }

    HeapFree(GetProcessHeap(), 0, Raw);
    return Error;
}

static DWORD
PwrpReadNameValue(
    _In_opt_ const GUID *Scheme,
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ PCWSTR ValueName,
    _In_ BOOLEAN Resolve,
    _Out_writes_bytes_opt_(*BufferSize) PUCHAR Buffer,
    _Inout_ LPDWORD BufferSize)
{
    HKEY Key;
    LONG Error;

    if (!BufferSize)
        return ERROR_INVALID_PARAMETER;

    Error = PwrpOpenNameKey(Scheme, SubGroup, Setting, FALSE, KEY_READ, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (Resolve)
        Error = PwrpQueryString(Key, ValueName, Buffer, BufferSize);
    else
        Error = PwrpQueryValue(Key, ValueName, NULL, Buffer, BufferSize);

    RegCloseKey(Key);
    return Error;
}

static DWORD
PwrpWriteNameValue(
    _In_opt_ const GUID *Scheme,
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ PCWSTR ValueName,
    _In_reads_bytes_opt_(BufferSize) const UCHAR *Buffer,
    _In_ DWORD BufferSize)
{
    HKEY Key;
    LONG Error;

    if (!Buffer)
        return RPC_X_NULL_REF_POINTER;
    if ((SubGroup || Setting) && !PwrpIsUserAdmin())
        return ERROR_ACCESS_DENIED;
    if (!BufferSize)
        return ERROR_INVALID_PARAMETER;

    Error = PwrpOpenNameKey(Scheme, SubGroup, Setting, TRUE, KEY_WRITE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegSetValueExW(Key, ValueName, 0, REG_EXPAND_SZ, Buffer, BufferSize);
    RegCloseKey(Key);
    return Error;
}

static DWORD
PwrpReadSettingDword(
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ PCWSTR ValueName,
    _Out_opt_ LPDWORD Value)
{
    DWORD Data, Size = sizeof(Data);
    HKEY Key;
    LONG Error;

    Error = PwrpOpenSettingKey(SubGroup, Setting, FALSE, KEY_READ, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = PwrpQueryValue(Key, ValueName, NULL, (PUCHAR)&Data, &Size);
    RegCloseKey(Key);

    if (Error == ERROR_SUCCESS && Value)
        *Value = Data;
    return Error;
}

static DWORD
PwrpWriteSettingData(
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ PCWSTR ValueName,
    _In_ ULONG Type,
    _In_reads_bytes_(Size) const VOID *Data,
    _In_ DWORD Size)
{
    HKEY Key;
    LONG Error;

    if (!PwrpIsUserAdmin())
        return ERROR_ACCESS_DENIED;
    if (!Size)
        return ERROR_INVALID_PARAMETER;

    Error = PwrpOpenSettingKey(SubGroup, Setting, TRUE, KEY_WRITE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegSetValueExW(Key, ValueName, 0, Type, Data, Size);
    RegCloseKey(Key);
    return Error;
}

/**
 * @brief
 * Tells whether a setting is described by a value range instead of a list
 * of possible values, and returns its bounds.
 */
static BOOLEAN
PwrpGetSettingRange(
    _In_ HKEY SettingKey,
    _Out_ LPDWORD Minimum,
    _Out_ LPDWORD Maximum)
{
    DWORD Increment, Size;

    Size = sizeof(*Minimum);
    if (PwrpQueryValue(SettingKey, szValueMin, NULL, (PUCHAR)Minimum, &Size) != ERROR_SUCCESS)
        return FALSE;

    Size = sizeof(*Maximum);
    if (PwrpQueryValue(SettingKey, szValueMax, NULL, (PUCHAR)Maximum, &Size) != ERROR_SUCCESS)
        return FALSE;

    Size = sizeof(Increment);
    return (PwrpQueryValue(SettingKey, szValueIncrement, NULL, (PUCHAR)&Increment, &Size) == ERROR_SUCCESS);
}

/**
 * @brief
 * Reads a possible value or its name. For range settings every index in
 * the range is its own value and the names are the setting's own.
 */
static DWORD
PwrpReadPossibleValue(
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ ULONG Index,
    _In_ PCWSTR ValueName,
    _In_ BOOLEAN Resolve,
    _Out_opt_ PULONG Type,
    _Out_writes_bytes_opt_(*BufferSize) PUCHAR Buffer,
    _Inout_ LPDWORD BufferSize)
{
    HKEY SettingKey, IndexKey;
    DWORD Minimum, Maximum;
    LONG Error;

    if (!Setting || !BufferSize)
        return ERROR_INVALID_PARAMETER;

    Error = PwrpOpenSettingKey(SubGroup, Setting, FALSE, KEY_READ, &SettingKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (PwrpGetSettingRange(SettingKey, &Minimum, &Maximum))
    {
        if (Resolve)
        {
            Error = PwrpQueryString(SettingKey, ValueName, Buffer, BufferSize);
        }
        else
        {
            if (Type)
                *Type = REG_DWORD;

            if (Index < Minimum || Index > Maximum)
            {
                *BufferSize = sizeof(Index);
                Error = ERROR_INVALID_DATA;
            }
            else
            {
                Error = PwrpCopyOut(&Index, sizeof(Index), Buffer, BufferSize);
            }
        }

        RegCloseKey(SettingKey);
        return Error;
    }

    Error = PwrpOpenIndexKey(SettingKey, Index, FALSE, KEY_READ, &IndexKey);
    RegCloseKey(SettingKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (Resolve)
        Error = PwrpQueryString(IndexKey, ValueName, Buffer, BufferSize);
    else
        Error = PwrpQueryValue(IndexKey, ValueName, Type, Buffer, BufferSize);

    RegCloseKey(IndexKey);
    return Error;
}

/**
 * @brief
 * Writes one value of a possible setting. Settings described by a value
 * range cannot have a list of possible values.
 */
static DWORD
PwrpWritePossibleValue(
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ ULONG Index,
    _In_ PCWSTR ValueName,
    _In_ ULONG Type,
    _In_reads_bytes_opt_(BufferSize) const UCHAR *Buffer,
    _In_ DWORD BufferSize)
{
    HKEY SettingKey, IndexKey;
    DWORD Minimum, Maximum;
    LONG Error;

    if (!Buffer)
        return RPC_X_NULL_REF_POINTER;
    if (!PwrpIsUserAdmin())
        return ERROR_ACCESS_DENIED;
    if (!BufferSize || !Setting)
        return ERROR_INVALID_PARAMETER;

    Error = PwrpOpenSettingKey(SubGroup, Setting, TRUE, KEY_WRITE | KEY_QUERY_VALUE, &SettingKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (PwrpGetSettingRange(SettingKey, &Minimum, &Maximum))
    {
        RegCloseKey(SettingKey);
        return ERROR_INVALID_PARAMETER;
    }

    Error = PwrpOpenIndexKey(SettingKey, Index, TRUE, KEY_WRITE, &IndexKey);
    RegCloseKey(SettingKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegSetValueExW(IndexKey, ValueName, 0, Type, Buffer, BufferSize);
    RegCloseKey(IndexKey);
    return Error;
}

/**
 * @brief
 * Reads a default index. A provisioned default takes precedence over the
 * stock one.
 */
static DWORD
PwrpReadDefaultIndex(
    _In_opt_ const GUID *Scheme,
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ BOOLEAN Ac,
    _Out_opt_ LPDWORD Index)
{
    DWORD Data, Size = sizeof(Data);
    HKEY Key;
    LONG Error;

    Error = PwrpOpenDefaultKey(Scheme, SubGroup, Setting, FALSE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = PwrpQueryValue(Key, Ac ? szProvACSettingIndex : szProvDCSettingIndex, NULL, (PUCHAR)&Data, &Size);
    if (Error == ERROR_FILE_NOT_FOUND)
    {
        Size = sizeof(Data);
        Error = PwrpQueryValue(Key, Ac ? szACSettingIndex : szDCSettingIndex, NULL, (PUCHAR)&Data, &Size);
    }
    RegCloseKey(Key);

    if (Error == ERROR_SUCCESS && Index)
        *Index = Data;
    return Error;
}

static DWORD
PwrpWriteDefaultIndex(
    _In_opt_ const GUID *Scheme,
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ BOOLEAN Ac,
    _In_ DWORD Index)
{
    PCWSTR ValueName;
    HKEY Key;
    LONG Error;

    if (!PwrpIsUserAdmin())
        return ERROR_ACCESS_DENIED;

    Error = PwrpOpenDefaultKey(Scheme, SubGroup, Setting, TRUE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* Keep a provisioned default in charge if there is one */
    ValueName = Ac ? szProvACSettingIndex : szProvDCSettingIndex;
    if (RegQueryValueExW(Key, ValueName, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
        ValueName = Ac ? szACSettingIndex : szDCSettingIndex;

    Error = RegSetValueExW(Key, ValueName, 0, REG_DWORD, (const BYTE *)&Index, sizeof(Index));
    RegCloseKey(Key);
    return Error;
}

/**
 * @brief
 * Reads a scheme's AC or DC value for a setting. When the scheme does not
 * store one, the setting's default for that scheme applies.
 */
static DWORD
PwrpReadSchemeValue(
    _In_opt_ const GUID *Scheme,
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ BOOLEAN Ac,
    _Out_opt_ PULONG Type,
    _Out_writes_bytes_opt_(*BufferSize) PUCHAR Buffer,
    _Inout_ LPDWORD BufferSize)
{
    HKEY SchemesKey, SchemeKey, Key;
    DWORD Index;
    LONG Error;

    if (!Scheme || !Setting || !BufferSize)
        return ERROR_INVALID_PARAMETER;

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPowerSchemesKey, 0, KEY_READ, &SchemesKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = PwrpOpenGuidKey(SchemesKey, Scheme, FALSE, KEY_READ, &SchemeKey);
    RegCloseKey(SchemesKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = PwrpOpenGuidPath(SchemeKey, SubGroup, Setting, FALSE, KEY_READ, &Key, NULL);
    RegCloseKey(SchemeKey);
    if (Error == ERROR_SUCCESS)
    {
        Error = PwrpQueryValue(Key, Ac ? szACSettingIndex : szDCSettingIndex, Type, Buffer, BufferSize);
        RegCloseKey(Key);
    }
    if (Error != ERROR_FILE_NOT_FOUND)
        return Error;

    Error = PwrpReadDefaultIndex(Scheme, SubGroup, Setting, Ac, &Index);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (Type)
        *Type = REG_DWORD;
    return PwrpCopyOut(&Index, sizeof(Index), Buffer, BufferSize);
}

/**
 * @brief
 * Writes a scheme's AC or DC value. Without a subgroup the value is stored
 * under the subgroup that defines the setting.
 */
static DWORD
PwrpWriteSchemeValue(
    _In_opt_ const GUID *Scheme,
    _In_opt_ const GUID *SubGroup,
    _In_opt_ const GUID *Setting,
    _In_ BOOLEAN Ac,
    _In_ DWORD Index)
{
    HKEY SchemesKey, SchemeKey, SettingsKey, GroupKey, Key;
    WCHAR GroupName[MAX_PATH];
    LONG Error;

    if (!Scheme || !Setting)
        return ERROR_INVALID_PARAMETER;

    GroupName[0] = UNICODE_NULL;
    if ((!SubGroup || IsEqualGUID(SubGroup, &PwrpNoSubGroup)) &&
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPowerSettingsKey, 0, KEY_READ, &SettingsKey) == ERROR_SUCCESS)
    {
        if (PwrpOpenGuidPath(SettingsKey, NULL, Setting, FALSE, KEY_READ, &Key, GroupName) == ERROR_SUCCESS)
            RegCloseKey(Key);
        RegCloseKey(SettingsKey);
    }

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPowerSchemesKey, 0, KEY_READ, &SchemesKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = PwrpOpenGuidKey(SchemesKey, Scheme, FALSE, KEY_READ | KEY_WRITE, &SchemeKey);
    RegCloseKey(SchemesKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (GroupName[0])
    {
        Error = RegCreateKeyExW(SchemeKey, GroupName, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &GroupKey, NULL);
        if (Error == ERROR_SUCCESS)
        {
            Error = PwrpOpenGuidKey(GroupKey, Setting, TRUE, KEY_READ | KEY_WRITE, &Key);
            RegCloseKey(GroupKey);
        }
    }
    else
    {
        Error = PwrpOpenGuidPath(SchemeKey, SubGroup, Setting, TRUE, KEY_READ | KEY_WRITE, &Key, NULL);
    }
    RegCloseKey(SchemeKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegSetValueExW(Key,
                           Ac ? szACSettingIndex : szDCSettingIndex,
                           0,
                           REG_DWORD,
                           (const BYTE *)&Index,
                           sizeof(Index));
    RegCloseKey(Key);
    return Error;
}

DWORD
WINAPI
PowerWriteACValueIndex(
    _In_opt_ HKEY RootPowerKey,
    _In_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_ DWORD AcValueIndex)
{
    return PwrpWriteSchemeValue(SchemeGuid,
                                SubGroupOfPowerSettingsGuid,
                                PowerSettingGuid,
                                TRUE,
                                AcValueIndex);
}

DWORD
WINAPI
PowerWriteDCValueIndex(
    _In_opt_ HKEY RootPowerKey,
    _In_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_ DWORD DcValueIndex)
{
    return PwrpWriteSchemeValue(SchemeGuid,
                                SubGroupOfPowerSettingsGuid,
                                PowerSettingGuid,
                                FALSE,
                                DcValueIndex);
}

DWORD
WINAPI
PowerReadACValue(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _Out_opt_ PULONG Type,
    _Out_writes_bytes_opt_(*BufferSize) LPBYTE Buffer,
    _Inout_opt_ LPDWORD BufferSize)
{
    return PwrpReadSchemeValue(SchemeGuid,
                               SubGroupOfPowerSettingsGuid,
                               PowerSettingGuid,
                               TRUE,
                               Type,
                               Buffer,
                               BufferSize);
}

DWORD
WINAPI
PowerReadDCValue(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _Out_opt_ PULONG Type,
    _Out_writes_bytes_opt_(*BufferSize) PUCHAR Buffer,
    _Inout_ LPDWORD BufferSize)
{
    return PwrpReadSchemeValue(SchemeGuid,
                               SubGroupOfPowerSettingsGuid,
                               PowerSettingGuid,
                               FALSE,
                               Type,
                               Buffer,
                               BufferSize);
}

DWORD
WINAPI
PowerReadACValueIndex(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _Out_ LPDWORD AcValueIndex)
{
    DWORD Size = sizeof(*AcValueIndex);

    return PwrpReadSchemeValue(SchemeGuid,
                               SubGroupOfPowerSettingsGuid,
                               PowerSettingGuid,
                               TRUE,
                               NULL,
                               (PUCHAR)AcValueIndex,
                               &Size);
}

DWORD
WINAPI
PowerReadDCValueIndex(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _Out_ LPDWORD DcValueIndex)
{
    DWORD Size = sizeof(*DcValueIndex);

    return PwrpReadSchemeValue(SchemeGuid,
                               SubGroupOfPowerSettingsGuid,
                               PowerSettingGuid,
                               FALSE,
                               NULL,
                               (PUCHAR)DcValueIndex,
                               &Size);
}

DWORD
WINAPI
PowerReadFriendlyName(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _Out_writes_bytes_opt_(*BufferSize) PUCHAR Buffer,
    _Inout_ LPDWORD BufferSize)
{
    return PwrpReadNameValue(SchemeGuid,
                             SubGroupOfPowerSettingsGuid,
                             PowerSettingGuid,
                             szFriendlyName,
                             TRUE,
                             Buffer,
                             BufferSize);
}

DWORD
WINAPI
PowerReadDescription(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _Out_writes_bytes_opt_(*BufferSize) PUCHAR Buffer,
    _Inout_ LPDWORD BufferSize)
{
    return PwrpReadNameValue(SchemeGuid,
                             SubGroupOfPowerSettingsGuid,
                             PowerSettingGuid,
                             szDescription,
                             TRUE,
                             Buffer,
                             BufferSize);
}

DWORD
WINAPI
PowerReadIconResourceSpecifier(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _Out_writes_bytes_opt_(*BufferSize) PUCHAR Buffer,
    _Inout_ LPDWORD BufferSize)
{
    return PwrpReadNameValue(SchemeGuid,
                             SubGroupOfPowerSettingsGuid,
                             PowerSettingGuid,
                             szIconResource,
                             FALSE,
                             Buffer,
                             BufferSize);
}

DWORD
WINAPI
PowerReadPossibleValue(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _Out_opt_ PULONG Type,
    _In_ ULONG PossibleSettingIndex,
    _Out_writes_bytes_opt_(*BufferSize) PUCHAR Buffer,
    _Inout_ LPDWORD BufferSize)
{
    return PwrpReadPossibleValue(SubGroupOfPowerSettingsGuid,
                                 PowerSettingGuid,
                                 PossibleSettingIndex,
                                 szSettingValue,
                                 FALSE,
                                 Type,
                                 Buffer,
                                 BufferSize);
}

DWORD
WINAPI
PowerReadPossibleFriendlyName(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_ ULONG PossibleSettingIndex,
    _Out_writes_bytes_opt_(*BufferSize) PUCHAR Buffer,
    _Inout_ LPDWORD BufferSize)
{
    return PwrpReadPossibleValue(SubGroupOfPowerSettingsGuid,
                                 PowerSettingGuid,
                                 PossibleSettingIndex,
                                 szFriendlyName,
                                 TRUE,
                                 NULL,
                                 Buffer,
                                 BufferSize);
}

DWORD
WINAPI
PowerReadPossibleDescription(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_ ULONG PossibleSettingIndex,
    _Out_writes_bytes_opt_(*BufferSize) PUCHAR Buffer,
    _Inout_ LPDWORD BufferSize)
{
    return PwrpReadPossibleValue(SubGroupOfPowerSettingsGuid,
                                 PowerSettingGuid,
                                 PossibleSettingIndex,
                                 szDescription,
                                 TRUE,
                                 NULL,
                                 Buffer,
                                 BufferSize);
}

DWORD
WINAPI
PowerReadValueMin(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _Out_ LPDWORD ValueMinimum)
{
    return PwrpReadSettingDword(SubGroupOfPowerSettingsGuid, PowerSettingGuid, szValueMin, ValueMinimum);
}

DWORD
WINAPI
PowerReadValueMax(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _Out_ LPDWORD ValueMaximum)
{
    return PwrpReadSettingDword(SubGroupOfPowerSettingsGuid, PowerSettingGuid, szValueMax, ValueMaximum);
}

DWORD
WINAPI
PowerReadValueIncrement(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _Out_ LPDWORD ValueIncrement)
{
    return PwrpReadSettingDword(SubGroupOfPowerSettingsGuid, PowerSettingGuid, szValueIncrement, ValueIncrement);
}

DWORD
WINAPI
PowerReadValueUnitsSpecifier(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _Out_writes_bytes_opt_(*BufferSize) UCHAR *Buffer,
    _Inout_ LPDWORD BufferSize)
{
    HKEY Key;
    LONG Error;

    if (!BufferSize)
        return ERROR_INVALID_PARAMETER;

    Error = PwrpOpenSettingKey(SubGroupOfPowerSettingsGuid, PowerSettingGuid, FALSE, KEY_READ, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = PwrpQueryString(Key, szValueUnits, Buffer, BufferSize);
    RegCloseKey(Key);
    return Error;
}

DWORD
WINAPI
PowerReadACDefaultIndex(
    _In_opt_ HKEY RootPowerKey,
    _In_ const GUID *SchemePersonalityGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_ const GUID *PowerSettingGuid,
    _Out_ LPDWORD AcDefaultIndex)
{
    return PwrpReadDefaultIndex(SchemePersonalityGuid,
                                SubGroupOfPowerSettingsGuid,
                                PowerSettingGuid,
                                TRUE,
                                AcDefaultIndex);
}

DWORD
WINAPI
PowerReadDCDefaultIndex(
    _In_opt_ HKEY RootPowerKey,
    _In_ const GUID *SchemePersonalityGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_ const GUID *PowerSettingGuid,
    _Out_ LPDWORD DcDefaultIndex)
{
    return PwrpReadDefaultIndex(SchemePersonalityGuid,
                                SubGroupOfPowerSettingsGuid,
                                PowerSettingGuid,
                                FALSE,
                                DcDefaultIndex);
}

/**
 * @brief
 * Returns the subgroup and setting attributes combined. When the last lookup
 * fails the result is 0 and the error is available from GetLastError.
 */
DWORD
WINAPI
PowerReadSettingAttributes(
    _In_opt_ const GUID *SubGroupGuid,
    _In_opt_ const GUID *PowerSettingGuid)
{
    DWORD GroupAttributes = 0, SettingAttributes = 0;
    DWORD Error = ERROR_SUCCESS;

    if (SubGroupGuid)
    {
        Error = PwrpReadSettingDword(SubGroupGuid, NULL, szAttributes, &GroupAttributes);
        if (Error != ERROR_SUCCESS)
            GroupAttributes = 0;
    }

    if (PowerSettingGuid)
    {
        Error = PwrpReadSettingDword(SubGroupGuid, PowerSettingGuid, szAttributes, &SettingAttributes);
        if (Error != ERROR_SUCCESS)
            SettingAttributes = 0;
    }

    if (Error != ERROR_SUCCESS)
    {
        SetLastError(Error);
        return 0;
    }

    return GroupAttributes | SettingAttributes;
}

DWORD
WINAPI
PowerWriteFriendlyName(
    _In_opt_ HKEY RootPowerKey,
    _In_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_reads_bytes_(BufferSize) UCHAR *Buffer,
    _In_ DWORD BufferSize)
{
    return PwrpWriteNameValue(SchemeGuid,
                              SubGroupOfPowerSettingsGuid,
                              PowerSettingGuid,
                              szFriendlyName,
                              Buffer,
                              BufferSize);
}

DWORD
WINAPI
PowerWriteDescription(
    _In_opt_ HKEY RootPowerKey,
    _In_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_reads_bytes_(BufferSize) UCHAR *Buffer,
    _In_ DWORD BufferSize)
{
    return PwrpWriteNameValue(SchemeGuid,
                              SubGroupOfPowerSettingsGuid,
                              PowerSettingGuid,
                              szDescription,
                              Buffer,
                              BufferSize);
}

DWORD
WINAPI
PowerWriteIconResourceSpecifier(
    _In_opt_ HKEY RootPowerKey,
    _In_ const GUID *SchemeGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_reads_bytes_(BufferSize) UCHAR *Buffer,
    _In_ DWORD BufferSize)
{
    return PwrpWriteNameValue(SchemeGuid,
                              SubGroupOfPowerSettingsGuid,
                              PowerSettingGuid,
                              szIconResource,
                              Buffer,
                              BufferSize);
}

DWORD
WINAPI
PowerWritePossibleValue(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_ ULONG Type,
    _In_ ULONG PossibleSettingIndex,
    _In_reads_bytes_(BufferSize) UCHAR *Buffer,
    _In_ DWORD BufferSize)
{
    return PwrpWritePossibleValue(SubGroupOfPowerSettingsGuid,
                                  PowerSettingGuid,
                                  PossibleSettingIndex,
                                  szSettingValue,
                                  Type,
                                  Buffer,
                                  BufferSize);
}

DWORD
WINAPI
PowerWritePossibleFriendlyName(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_ ULONG PossibleSettingIndex,
    _In_reads_bytes_(BufferSize) UCHAR *Buffer,
    _In_ DWORD BufferSize)
{
    return PwrpWritePossibleValue(SubGroupOfPowerSettingsGuid,
                                  PowerSettingGuid,
                                  PossibleSettingIndex,
                                  szFriendlyName,
                                  REG_EXPAND_SZ,
                                  Buffer,
                                  BufferSize);
}

DWORD
WINAPI
PowerWritePossibleDescription(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_ ULONG PossibleSettingIndex,
    _In_reads_bytes_(BufferSize) UCHAR *Buffer,
    _In_ DWORD BufferSize)
{
    return PwrpWritePossibleValue(SubGroupOfPowerSettingsGuid,
                                  PowerSettingGuid,
                                  PossibleSettingIndex,
                                  szDescription,
                                  REG_EXPAND_SZ,
                                  Buffer,
                                  BufferSize);
}

DWORD
WINAPI
PowerWriteValueMin(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_ DWORD ValueMinimum)
{
    return PwrpWriteSettingData(SubGroupOfPowerSettingsGuid,
                                PowerSettingGuid,
                                szValueMin,
                                REG_DWORD,
                                &ValueMinimum,
                                sizeof(ValueMinimum));
}

DWORD
WINAPI
PowerWriteValueMax(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_ DWORD ValueMaximum)
{
    return PwrpWriteSettingData(SubGroupOfPowerSettingsGuid,
                                PowerSettingGuid,
                                szValueMax,
                                REG_DWORD,
                                &ValueMaximum,
                                sizeof(ValueMaximum));
}

DWORD
WINAPI
PowerWriteValueIncrement(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_ DWORD ValueIncrement)
{
    return PwrpWriteSettingData(SubGroupOfPowerSettingsGuid,
                                PowerSettingGuid,
                                szValueIncrement,
                                REG_DWORD,
                                &ValueIncrement,
                                sizeof(ValueIncrement));
}

DWORD
WINAPI
PowerWriteValueUnitsSpecifier(
    _In_opt_ HKEY RootPowerKey,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_reads_bytes_(BufferSize) UCHAR *Buffer,
    _In_ DWORD BufferSize)
{
    if (!Buffer)
        return RPC_X_NULL_REF_POINTER;

    return PwrpWriteSettingData(SubGroupOfPowerSettingsGuid,
                                PowerSettingGuid,
                                szValueUnits,
                                REG_EXPAND_SZ,
                                Buffer,
                                BufferSize);
}

DWORD
WINAPI
PowerWriteACDefaultIndex(
    _In_opt_ HKEY RootSystemPowerKey,
    _In_ const GUID *SchemePersonalityGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_ const GUID *PowerSettingGuid,
    _In_ DWORD DefaultAcIndex)
{
    return PwrpWriteDefaultIndex(SchemePersonalityGuid,
                                 SubGroupOfPowerSettingsGuid,
                                 PowerSettingGuid,
                                 TRUE,
                                 DefaultAcIndex);
}

DWORD
WINAPI
PowerWriteDCDefaultIndex(
    _In_opt_ HKEY RootSystemPowerKey,
    _In_ const GUID *SchemePersonalityGuid,
    _In_opt_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_ const GUID *PowerSettingGuid,
    _In_ DWORD DefaultDcIndex)
{
    return PwrpWriteDefaultIndex(SchemePersonalityGuid,
                                 SubGroupOfPowerSettingsGuid,
                                 PowerSettingGuid,
                                 FALSE,
                                 DefaultDcIndex);
}

DWORD
WINAPI
PowerWriteSettingAttributes(
    _In_opt_ const GUID *SubGroupGuid,
    _In_opt_ const GUID *PowerSettingGuid,
    _In_ DWORD Attributes)
{
    return PwrpWriteSettingData(SubGroupGuid,
                                PowerSettingGuid,
                                szAttributes,
                                REG_DWORD,
                                &Attributes,
                                sizeof(Attributes));
}

/**
 * @brief
 * Creates a setting under its subgroup, creating the subgroup if needed.
 *
 * @return
 * ERROR_ALREADY_EXISTS when the setting is already defined.
 */
DWORD
WINAPI
PowerCreateSetting(
    _In_opt_ HKEY RootSystemPowerKey,
    _In_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_ const GUID *PowerSettingGuid)
{
    WCHAR Name[PWRP_GUID_CCH];
    HKEY GroupKey, Key;
    DWORD Disposition;
    LONG Error;

    if (!PwrpIsUserAdmin())
        return ERROR_ACCESS_DENIED;
    if (!SubGroupOfPowerSettingsGuid || !PowerSettingGuid)
        return ERROR_INVALID_PARAMETER;

    Error = PwrpOpenSettingKey(SubGroupOfPowerSettingsGuid, NULL, TRUE, KEY_WRITE, &GroupKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    PwrpGuidToKeyName(PowerSettingGuid, Name);
    Error = RegCreateKeyExW(GroupKey, Name, 0, NULL, 0, KEY_WRITE, NULL, &Key, &Disposition);
    RegCloseKey(GroupKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    RegCloseKey(Key);
    return (Disposition == REG_OPENED_EXISTING_KEY) ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS;
}

DWORD
WINAPI
PowerCreatePossibleSetting(
    _In_opt_ HKEY RootSystemPowerKey,
    _In_ const GUID *SubGroupOfPowerSettingsGuid,
    _In_ const GUID *PowerSettingGuid,
    _In_ ULONG PossibleSettingIndex)
{
    HKEY SettingKey, IndexKey;
    LONG Error;

    if (!PwrpIsUserAdmin())
        return ERROR_ACCESS_DENIED;
    if (!SubGroupOfPowerSettingsGuid || !PowerSettingGuid)
        return ERROR_INVALID_PARAMETER;

    Error = PwrpOpenSettingKey(SubGroupOfPowerSettingsGuid, PowerSettingGuid, TRUE, KEY_WRITE, &SettingKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = PwrpOpenIndexKey(SettingKey, PossibleSettingIndex, TRUE, KEY_WRITE, &IndexKey);
    RegCloseKey(SettingKey);
    if (Error == ERROR_SUCCESS)
        RegCloseKey(IndexKey);

    return Error;
}

static LONG
PwrpDeleteKeyTree(
    _In_ HKEY Parent,
    _In_ PCWSTR Name)
{
    WCHAR ChildName[MAX_PATH];
    DWORD NameLength;
    HKEY Key;
    LONG Error;

    Error = RegOpenKeyExW(Parent, Name, 0, KEY_READ | DELETE, &Key);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* Always take the first child, since each one is removed in turn */
    for (;;)
    {
        NameLength = _countof(ChildName);
        Error = RegEnumKeyExW(Key, 0, ChildName, &NameLength, NULL, NULL, NULL, NULL);
        if (Error != ERROR_SUCCESS)
            break;

        Error = PwrpDeleteKeyTree(Key, ChildName);
        if (Error != ERROR_SUCCESS)
            break;
    }

    RegCloseKey(Key);
    if (Error != ERROR_NO_MORE_ITEMS)
        return Error;

    return RegDeleteKeyW(Parent, Name);
}

DWORD
WINAPI
PowerRemovePowerSetting(
    _In_ const GUID *PowerSettingSubKeyGuid,
    _In_ const GUID *PowerSettingGuid)
{
    WCHAR Name[PWRP_GUID_CCH];
    HKEY GroupKey;
    LONG Error;

    if (!PwrpIsUserAdmin())
        return ERROR_ACCESS_DENIED;
    if (!PowerSettingSubKeyGuid || !PowerSettingGuid)
        return ERROR_INVALID_PARAMETER;

    Error = PwrpOpenSettingKey(PowerSettingSubKeyGuid, NULL, FALSE, KEY_READ | KEY_WRITE, &GroupKey);
    if (Error != ERROR_SUCCESS)
        return Error;

    PwrpGuidToKeyName(PowerSettingGuid, Name);
    Error = PwrpDeleteKeyTree(GroupKey, Name);
    RegCloseKey(GroupKey);
    return Error;
}

/**
 * @brief
 * Checks whether the caller may change the given kind of power data.
 * Remote sessions need administrator rights.
 */
DWORD
WINAPI
PowerSettingAccessCheck(
    _In_ POWER_DATA_ACCESSOR AccessFlags,
    _In_opt_ const GUID *PowerGuid)
{
    if (!PwrpIsConsoleSession() && !PwrpIsUserAdmin())
        return ERROR_INSTALL_REMOTE_DISALLOWED;

    switch (AccessFlags)
    {
        case ACCESS_AC_POWER_SETTING_INDEX:
        case ACCESS_DC_POWER_SETTING_INDEX:
        case ACCESS_SCHEME:
            if (!PowerGuid)
                return ERROR_INVALID_PARAMETER;
            break;

        case ACCESS_FRIENDLY_NAME:
        case ACCESS_DESCRIPTION:
        case ACCESS_DEFAULT_SECURITY_DESCRIPTOR:
        case ACCESS_ACTIVE_SCHEME:
        case ACCESS_CREATE_SCHEME:
        case ACCESS_ACTIVE_OVERLAY_SCHEME:
            break;

        default:
            return ERROR_INVALID_PARAMETER;
    }

    /* There is no power group policy that could override the setting */
    return ERROR_SUCCESS;
}

#define PWRP_MODE_PENDING   0
#define PWRP_MODE_RUNNING   1
#define PWRP_MODE_FINISHED  2
#define PWRP_MODE_CANCELLED 3
#define PWRP_MODE_ORPHANED  4

typedef struct _PWRP_MODE_NOTIFY
{
    EFFECTIVE_POWER_MODE_CALLBACK *Callback;
    PVOID Context;
    LONG State;
    DWORD CallbackThreadId;
    HANDLE Finished;
} PWRP_MODE_NOTIFY, *PPWRP_MODE_NOTIFY;

static VOID
PwrpFreeModeNotify(
    _In_ PPWRP_MODE_NOTIFY Notify)
{
    CloseHandle(Notify->Finished);
    HeapFree(GetProcessHeap(), 0, Notify);
}

static BOOLEAN
PwrpIsActiveScheme(
    _In_ PCWSTR ValueName,
    _In_ const GUID *Scheme)
{
    WCHAR Active[MAX_PATH], Name[PWRP_GUID_CCH];
    DWORD Size = sizeof(Active);

    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     szPowerSchemesKey,
                     ValueName,
                     RRF_RT_REG_SZ,
                     NULL,
                     Active,
                     &Size) != ERROR_SUCCESS)
    {
        return FALSE;
    }

    PwrpGuidToKeyName(Scheme, Name);
    return !_wcsicmp(Active, Name);
}

/**
 * @brief
 * Derives the effective mode from the power mode overlay of the current
 * power source, or from the active scheme when no overlay is set.
 */
static EFFECTIVE_POWER_MODE
PwrpQueryEffectivePowerMode(VOID)
{
    PCWSTR Overlay = L"ActiveOverlayAcPowerScheme";
    SYSTEM_POWER_STATUS PowerStatus;

    if (GetSystemPowerStatus(&PowerStatus) && PowerStatus.ACLineStatus == 0)
        Overlay = L"ActiveOverlayDcPowerScheme";

    if (PwrpIsActiveScheme(Overlay, &PwrpBestPerformanceOverlay))
        return EffectivePowerModeMaxPerformance;
    if (PwrpIsActiveScheme(Overlay, &PwrpBetterPerformanceOverlay))
        return EffectivePowerModeHighPerformance;
    if (PwrpIsActiveScheme(Overlay, &PwrpBestEfficiencyOverlay))
        return EffectivePowerModeBetterBattery;

    if (PwrpIsActiveScheme(L"ActivePowerScheme", &PwrpHighPerformanceScheme))
        return EffectivePowerModeHighPerformance;
    if (PwrpIsActiveScheme(L"ActivePowerScheme", &PwrpPowerSaverScheme))
        return EffectivePowerModeBetterBattery;

    return EffectivePowerModeBalanced;
}

static DWORD
WINAPI
PwrpEffectivePowerModeWorker(
    _In_ PVOID Parameter)
{
    PPWRP_MODE_NOTIFY Notify = Parameter;

    /* Unregistered before delivery, the cleanup is ours */
    if (InterlockedCompareExchange(&Notify->State, PWRP_MODE_RUNNING, PWRP_MODE_PENDING) != PWRP_MODE_PENDING)
    {
        PwrpFreeModeNotify(Notify);
        return 0;
    }

    Notify->CallbackThreadId = GetCurrentThreadId();
    Notify->Callback(PwrpQueryEffectivePowerMode(), Notify->Context);

    /* The callback unregistered itself */
    if (InterlockedCompareExchange(&Notify->State, PWRP_MODE_FINISHED, PWRP_MODE_RUNNING) != PWRP_MODE_RUNNING)
    {
        PwrpFreeModeNotify(Notify);
        return 0;
    }

    SetEvent(Notify->Finished);
    return 0;
}

/**
 * @brief
 * Registers for effective power mode changes. The callback receives the
 * current mode once, from a worker thread.
 */
HRESULT
WINAPI
PowerRegisterForEffectivePowerModeNotifications(
    _In_ ULONG Version,
    _In_ EFFECTIVE_POWER_MODE_CALLBACK *Callback,
    _In_opt_ PVOID Context,
    _Outptr_ PVOID *RegistrationHandle)
{
    PPWRP_MODE_NOTIFY Notify;
    DWORD Error;

    if (Version > EFFECTIVE_POWER_MODE_V2 || !Callback || !RegistrationHandle)
        return E_INVALIDARG;

    Notify = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Notify));
    if (!Notify)
        return E_OUTOFMEMORY;

    Notify->Finished = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Notify->Finished)
    {
        Error = GetLastError();
        HeapFree(GetProcessHeap(), 0, Notify);
        return HRESULT_FROM_WIN32(Error);
    }

    Notify->Callback = Callback;
    Notify->Context = Context;
    Notify->State = PWRP_MODE_PENDING;

    if (!QueueUserWorkItem(PwrpEffectivePowerModeWorker, Notify, WT_EXECUTEDEFAULT))
    {
        Error = GetLastError();
        PwrpFreeModeNotify(Notify);
        return HRESULT_FROM_WIN32(Error);
    }

    *RegistrationHandle = Notify;
    return S_OK;
}

/**
 * @brief
 * Cancels a pending callback, or waits for a running one unless it is the
 * callback itself unregistering.
 */
HRESULT
WINAPI
PowerUnregisterFromEffectivePowerModeNotifications(
    _In_ PVOID RegistrationHandle)
{
    PPWRP_MODE_NOTIFY Notify = RegistrationHandle;
    LONG State;

    if (!Notify)
        return E_INVALIDARG;

    State = InterlockedCompareExchange(&Notify->State, PWRP_MODE_CANCELLED, PWRP_MODE_PENDING);
    if (State == PWRP_MODE_PENDING)
        return S_OK;

    if (State == PWRP_MODE_RUNNING)
    {
        if (Notify->CallbackThreadId == GetCurrentThreadId())
        {
            InterlockedExchange(&Notify->State, PWRP_MODE_ORPHANED);
            return S_OK;
        }

        WaitForSingleObject(Notify->Finished, INFINITE);
    }

    PwrpFreeModeNotify(Notify);
    return S_OK;
}

BOOL WINAPI
DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch(fdwReason)
    {
        case DLL_PROCESS_ATTACH:
        {
            HKEY hKey;
            LONG Err;

            DisableThreadLibraryCalls(hinstDLL);

            Err = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szPowerCfgSubKey, 0, KEY_READ, &hKey);

            if (Err != ERROR_SUCCESS)
            {
                TRACE("Couldn't open registry key HKLM\\%s, using some sane(?) defaults\n", debugstr_w(szPowerCfgSubKey));
            }
            else
            {
                WCHAR lpValue[MAX_PATH];
                DWORD cbValue = sizeof(lpValue);

                Err = RegQueryValueExW(hKey, szLastID, 0, 0, (BYTE*)lpValue, &cbValue);
                if (Err == ERROR_SUCCESS)
                {
                    g_LastID = _wtoi(lpValue);
                }
                else
                {
                    TRACE("Couldn't open registry entry HKLM\\%s\\LastID, using some sane(?) defaults\n", debugstr_w(szPowerCfgSubKey));
                }
                RegCloseKey(hKey);
            }

            break;
        }
        case DLL_PROCESS_DETACH:
            if (lpvReserved) break;
            CloseHandle(PPRegSemaphore);
            break;
    }
    return TRUE;
}
