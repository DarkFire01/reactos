/*
 *  ReactOS kernel
 *  Copyright (C) 2004 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * FILE:            dll/win32/userenv/registry.c
 * PURPOSE:         User profile code
 * PROGRAMMER:      Eric Kohl
 */

#include "precomp.h"

#include <sddl.h>

#define NDEBUG
#include <debug.h>

LSTATUS WINAPI RegCopyTreeW(_In_ HKEY, _In_opt_ LPCWSTR, _In_ HKEY);

/* FUNCTIONS ***************************************************************/

static
BOOL
CopyKey(HKEY hDstKey,
        HKEY hSrcKey)
{
    LONG Error;

    Error = RegCopyTreeW(hSrcKey,
                         NULL,
                         hDstKey);
    if (Error != ERROR_SUCCESS)
    {
        SetLastError((DWORD)Error);
        return FALSE;
    }

    return TRUE;
}

/* The user, SYSTEM and Administrators own a new user hive, restricted code may read it */
#define USER_HIVE_ACES(Flags) \
    L"(A;" Flags L";KA;;;%s)(A;" Flags L";KA;;;SY)(A;" Flags L";KA;;;BA)(A;" Flags L";KR;;;RC)"

/* The registryRead capability lets less privileged app containers read too */
#define REGISTRY_READ_CAPABILITY \
    L"S-1-15-3-1024-1065365936-1281604716-3511738428-1654721687-432734479-3232135806-4053264122-3456934681"

typedef struct _APP_READABLE_KEY
{
    PCWSTR Path;
    BOOLEAN Subtree;
    BOOLEAN Capability;
} APP_READABLE_KEY;

/*
 * Keys of a new user hive app containers may read, as on Windows 8 and later.
 * NULL is the hive root. Subtree also covers every key below, Capability also
 * grants the registryRead capability.
 */
static const APP_READABLE_KEY AppReadableKeys[] =
{
    { L"AppEvents", TRUE, FALSE },
    { L"Control Panel", TRUE, TRUE },
    { L"Environment", TRUE, TRUE },
    { L"EUDC", TRUE, TRUE },
    { L"Keyboard Layout", TRUE, TRUE },
    { L"Keyboard Layout\\Toggle", TRUE, TRUE },
    { L"Software\\Microsoft\\Command Processor", TRUE, FALSE },
    { L"Software\\Microsoft\\CTF", TRUE, TRUE },
    { L"Software\\Microsoft\\Internet Explorer", TRUE, TRUE },
    { L"Software\\Microsoft\\Windows\\CurrentVersion\\AppHost", TRUE, FALSE },
    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer", TRUE, TRUE },
    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", TRUE, TRUE },
    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies", TRUE, TRUE },
    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions", TRUE, TRUE },
    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes", TRUE, TRUE },
    { L"Software\\Microsoft\\Windows\\CurrentVersion\\WinTrust", TRUE, TRUE },
    { L"Software\\Microsoft\\Windows\\Windows Error Reporting", TRUE, TRUE },
    { L"Software\\Microsoft\\Windows NT", TRUE, FALSE },
    { L"Software\\Microsoft\\Windows NT\\CurrentVersion\\ICM", TRUE, TRUE },
    { L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Windows", TRUE, TRUE },
    { L"Software\\Microsoft\\Wisp", TRUE, FALSE },
    { L"Software\\Policies", TRUE, TRUE },
    { NULL, FALSE, TRUE },
    { L"Software", FALSE, TRUE },
    { L"Software\\Microsoft", FALSE, TRUE },
    { L"Software\\Microsoft\\Windows", FALSE, TRUE },
    { L"Software\\Microsoft\\Windows\\CurrentVersion", FALSE, TRUE },
    { L"Software\\Microsoft\\Windows\\CurrentVersion\\Holographic", FALSE, TRUE },

    /* Windows keeps these in a classes hive of their own, secured like this */
    { L"Software\\Classes", TRUE, TRUE },
};

/* Sets a descriptor on a key and, when asked, the inherited one on every key below it */
static
LONG
ApplyKeySecurity(
    _In_ HKEY hKey,
    _In_ PSECURITY_DESCRIPTOR Descriptor,
    _In_opt_ PSECURITY_DESCRIPTOR Inherited)
{
    WCHAR szName[256];
    DWORD dwIndex, dwLength;
    HKEY hSubKey;
    LONG Error;

    Error = RegSetKeySecurity(hKey, DACL_SECURITY_INFORMATION, Descriptor);
    if (Error != ERROR_SUCCESS || !Inherited)
        return Error;

    for (dwIndex = 0; ; dwIndex++)
    {
        dwLength = _countof(szName);
        Error = RegEnumKeyExW(hKey, dwIndex, szName, &dwLength, NULL, NULL, NULL, NULL);
        if (Error == ERROR_NO_MORE_ITEMS)
            return ERROR_SUCCESS;
        if (Error != ERROR_SUCCESS)
            return Error;

        Error = RegOpenKeyExW(hKey, szName, 0, KEY_ENUMERATE_SUB_KEYS | READ_CONTROL | WRITE_DAC, &hSubKey);
        if (Error != ERROR_SUCCESS)
            continue;

        Error = ApplyKeySecurity(hSubKey, Inherited, Inherited);
        RegCloseKey(hSubKey);
        if (Error != ERROR_SUCCESS)
            return Error;
    }
}

/* Builds the explicit and inherited descriptors for a user hive key, then applies them */
static
LONG
SetUserKeySecurity(
    _In_ HKEY hKey,
    _In_ LPCWSTR lpSidString,
    _In_ LPCWSTR lpAppAces,
    _In_ LPCWSTR lpInheritedAppAces,
    _In_ BOOL bSubtree)
{
    WCHAR szText[640];
    PSECURITY_DESCRIPTOR Descriptor = NULL, Inherited = NULL;
    LONG Error = ERROR_SUCCESS;

    StringCbPrintfW(szText, sizeof(szText), L"D:PAI" USER_HIVE_ACES(L"OICI") L"%s", lpSidString, lpAppAces);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(szText, SDDL_REVISION_1, &Descriptor, NULL))
        return GetLastError();

    if (bSubtree)
    {
        StringCbPrintfW(szText, sizeof(szText), L"D:AI" USER_HIVE_ACES(L"OICIID") L"%s", lpSidString, lpInheritedAppAces);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(szText, SDDL_REVISION_1, &Inherited, NULL))
            Error = GetLastError();
    }

    if (Error == ERROR_SUCCESS)
        Error = ApplyKeySecurity(hKey, Descriptor, Inherited);

    if (Inherited)
        LocalFree(Inherited);
    LocalFree(Descriptor);
    return Error;
}

/* Gives a new user hive the security Windows gives it, so app containers read only what they may */
static
BOOL
SetUserHiveSecurity(
    _In_ HKEY hUserKey,
    _In_ LPCWSTR lpSidString)
{
    const APP_READABLE_KEY *Key;
    WCHAR szAppAces[256], szInheritedAppAces[256];
    HKEY hKey;
    LONG Error;
    ULONG i;

    Error = SetUserKeySecurity(hUserKey, lpSidString, L"", L"", TRUE);
    if (Error != ERROR_SUCCESS)
    {
        DPRINT1("Failed to secure the user hive (Error %ld)\n", Error);
        SetLastError((DWORD)Error);
        return FALSE;
    }

    for (i = 0; i < _countof(AppReadableKeys); i++)
    {
        Key = &AppReadableKeys[i];

        StringCbPrintfW(szAppAces, sizeof(szAppAces), L"(A;%s;KR;;;AC)", Key->Subtree ? L"OICI" : L"");
        StringCbCopyW(szInheritedAppAces, sizeof(szInheritedAppAces), L"(A;OICIID;KR;;;AC)");
        if (Key->Capability)
        {
            StringCbPrintfW(szAppAces + wcslen(szAppAces),
                            sizeof(szAppAces) - wcslen(szAppAces) * sizeof(WCHAR),
                            L"(A;%s;KR;;;" REGISTRY_READ_CAPABILITY L")",
                            Key->Subtree ? L"OICI" : L"");
            StringCbCatW(szInheritedAppAces, sizeof(szInheritedAppAces), L"(A;OICIID;KR;;;" REGISTRY_READ_CAPABILITY L")");
        }

        if (Key->Path)
        {
            Error = RegOpenKeyExW(hUserKey, Key->Path, 0, KEY_ENUMERATE_SUB_KEYS | READ_CONTROL | WRITE_DAC, &hKey);
            if (Error != ERROR_SUCCESS)
                continue;
        }
        else
        {
            hKey = hUserKey;
        }

        Error = SetUserKeySecurity(hKey, lpSidString, szAppAces, szInheritedAppAces, Key->Subtree);
        if (Error != ERROR_SUCCESS)
            DPRINT1("Failed to secure %S (Error %ld)\n", Key->Path ? Key->Path : L"the user hive root", Error);

        if (hKey != hUserKey)
            RegCloseKey(hKey);
    }

    return TRUE;
}


BOOL
CreateUserHive(LPCWSTR lpKeyName,
               LPCWSTR lpProfilePath)
{
    HKEY hDefaultKey = NULL;
    HKEY hUserKey = NULL;
    LONG Error;
    BOOL Ret = FALSE;

    DPRINT("CreateUserHive(%S) called\n", lpKeyName);

    Error = RegOpenKeyExW(HKEY_USERS,
                          L".Default",
                          0,
                          KEY_READ,
                          &hDefaultKey);
    if (Error != ERROR_SUCCESS)
    {
        SetLastError((DWORD)Error);
        goto Cleanup;
    }

    Error = RegOpenKeyExW(HKEY_USERS,
                          lpKeyName,
                          0,
                          KEY_ALL_ACCESS,
                          &hUserKey);
    if (Error != ERROR_SUCCESS)
    {
        SetLastError((DWORD)Error);
        goto Cleanup;
    }

    if (!CopyKey(hUserKey, hDefaultKey))
    {
        goto Cleanup;
    }

    if (!UpdateUsersShellFolderSettings(lpProfilePath,
                                        hUserKey))
    {
        goto Cleanup;
    }

    if (!SetUserHiveSecurity(hUserKey, lpKeyName))
    {
        goto Cleanup;
    }

    RegFlushKey(hUserKey);
    Ret = TRUE;

Cleanup:
    if (hUserKey != NULL)
        RegCloseKey (hUserKey);

    if (hDefaultKey != NULL)
        RegCloseKey (hDefaultKey);

    return Ret;
}

/* EOF */
