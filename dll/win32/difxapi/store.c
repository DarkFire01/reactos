/*
 * PROJECT:     ReactOS Driver Install Frameworks API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver store entries and the applications referencing them
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES *******************************************************************/

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(difxapi);

/* GLOBALS ********************************************************************/

/* One subkey per driver store entry, named after its folder */
#define DIFX_REFERENCES_KEY     L"Software\\Microsoft\\Windows\\CurrentVersion\\DIFx\\DriverStore"

#define DIFX_VALUE_INSTALLERS   L"DependentInstaller"
#define DIFX_VALUE_NAMES        L"DependentInstallerName"

/* FUNCTIONS ******************************************************************/

BOOL
WINAPI
DifxSplitPath(
    _In_ PCWSTR Path,
    _Out_writes_(cchDirectory) PWSTR Directory,
    _In_ DWORD cchDirectory,
    _Outptr_ PCWSTR *FileName)
{
    PCWSTR Slash;

    Slash = wcsrchr(Path, L'\\');
    if (!Slash || (DWORD)(Slash - Path) >= cchDirectory)
        return FALSE;

    CopyMemory(Directory, Path, (Slash - Path) * sizeof(*Directory));
    Directory[Slash - Path] = UNICODE_NULL;
    *FileName = Slash + 1;
    return TRUE;
}

BOOL
WINAPI
DifxIsFromInfDirectory(
    _In_ PCWSTR InfPath)
{
    WCHAR FullPath[MAX_PATH];
    WCHAR InfDirectory[MAX_PATH];
    PWSTR FilePart = NULL;
    DWORD Length;

    Length = GetFullPathNameW(InfPath, ARRAYSIZE(FullPath), FullPath, &FilePart);
    if (!Length || Length >= ARRAYSIZE(FullPath) || !FilePart || FilePart == FullPath)
        return FALSE;

    Length = GetSystemWindowsDirectoryW(InfDirectory, ARRAYSIZE(InfDirectory));
    if (!Length || Length >= ARRAYSIZE(InfDirectory) ||
        FAILED(StringCchCatW(InfDirectory, ARRAYSIZE(InfDirectory), L"\\inf")))
    {
        return FALSE;
    }

    FilePart[-1] = UNICODE_NULL;
    return (_wcsicmp(FullPath, InfDirectory) == 0);
}

DWORD
WINAPI
DifxStoreFind(
    _In_ PCWSTR InfPath,
    _Out_writes_(MAX_PATH) PWSTR StoreInf)
{
    WCHAR OemInf[MAX_PATH];
    DWORD Error;

    *StoreInf = UNICODE_NULL;

    /* A published INF belongs to the entry it was imported from */
    if (DifxIsFromInfDirectory(InfPath))
    {
        if (!SetupGetInfDriverStoreLocationW(InfPath, NULL, NULL, StoreInf, MAX_PATH, NULL))
            return GetLastError();

        return ERROR_SUCCESS;
    }

    /* Any other INF is in the store when an identical copy of it was published */
    OemInf[0] = UNICODE_NULL;
    if (!SetupCopyOEMInfW(InfPath, NULL, SPOST_NONE, SP_COPY_REPLACEONLY, OemInf, ARRAYSIZE(OemInf), NULL, NULL))
    {
        Error = GetLastError();
        return (Error == ERROR_FILE_NOT_FOUND) ? ERROR_NOT_FOUND : Error;
    }

    if (!SetupGetInfDriverStoreLocationW(OemInf, NULL, NULL, StoreInf, MAX_PATH, NULL))
    {
        Error = GetLastError();
        return (Error == ERROR_FILE_NOT_FOUND) ? ERROR_NOT_FOUND : Error;
    }

    return ERROR_SUCCESS;
}

DWORD
WINAPI
DifxStoreAdd(
    _In_ PCWSTR InfPath,
    _In_ BOOL Replace,
    _Out_writes_(MAX_PATH) PWSTR StoreInf)
{
    WCHAR SourceDirectory[MAX_PATH];
    WCHAR OemInf[MAX_PATH];
    PCWSTR FileName;

    *StoreInf = UNICODE_NULL;

    if (!DifxSplitPath(InfPath, SourceDirectory, ARRAYSIZE(SourceDirectory), &FileName))
        return ERROR_INVALID_PARAMETER;

    /* Publishing the INF imports its package, an identical one keeps its entry */
    TRACE("Adding %s, replace %d\n", debugstr_w(InfPath), Replace);
    if (!SetupCopyOEMInfW(InfPath, SourceDirectory, SPOST_PATH, 0, OemInf, ARRAYSIZE(OemInf), NULL, NULL))
        return GetLastError();

    if (!SetupGetInfDriverStoreLocationW(OemInf, NULL, NULL, StoreInf, MAX_PATH, NULL))
    {
        ERR("%s was published as %s without a driver store entry\n", debugstr_w(InfPath), debugstr_w(OemInf));
        return ERROR_DRIVER_STORE_ADD_FAILED;
    }

    return ERROR_SUCCESS;
}

DWORD
WINAPI
DifxStoreDelete(
    _In_ PCWSTR StoreInf)
{
    WCHAR PublishedName[MAX_PATH];

    if (!SetupGetInfPublishedNameW(StoreInf, PublishedName, ARRAYSIZE(PublishedName), NULL))
        return GetLastError();

    if (!SetupUninstallOEMInfW(PublishedName, SUOI_FORCEDELETE, NULL))
        return GetLastError();

    return ERROR_SUCCESS;
}

DWORD
WINAPI
DifxCatalogAdd(
    _In_ PCWSTR StoreInf)
{
    WCHAR Directory[MAX_PATH];
    PCWSTR FileName;

    if (!DifxSplitPath(StoreInf, Directory, ARRAYSIZE(Directory), &FileName))
        return ERROR_INSTALL_FAILURE;

    if (!SetupCopyOEMInfW(StoreInf, Directory, SPOST_PATH, SP_COPY_OEMINF_CATALOG_ONLY, NULL, 0, NULL, NULL))
        return GetLastError();

    return ERROR_SUCCESS;
}

BOOL
WINAPI
DifxGetOemInf(
    _In_ PCWSTR StoreInf,
    _Out_writes_(MAX_PATH) PWSTR OemInf)
{
    WCHAR Directory[MAX_PATH];
    PCWSTR FileName;

    *OemInf = UNICODE_NULL;

    if (!DifxSplitPath(StoreInf, Directory, ARRAYSIZE(Directory), &FileName))
        return FALSE;

    SetupCopyOEMInfW(StoreInf, Directory, SPOST_PATH, SP_COPY_REPLACEONLY, OemInf, MAX_PATH, NULL, NULL);
    return (*OemInf != UNICODE_NULL);
}

/* The registry key of a store entry is named after its folder */
static
BOOL
WINAPI
DifxGetReferenceKeyName(
    _In_ PCWSTR StoreInf,
    _Out_writes_(cchKeyName) PWSTR KeyName,
    _In_ DWORD cchKeyName)
{
    WCHAR EntryDirectory[MAX_PATH];
    WCHAR Repository[MAX_PATH];
    PCWSTR FileName, Folder;

    if (!DifxSplitPath(StoreInf, EntryDirectory, ARRAYSIZE(EntryDirectory), &FileName) ||
        !DifxSplitPath(EntryDirectory, Repository, ARRAYSIZE(Repository), &Folder) ||
        !*Folder)
    {
        return FALSE;
    }

    return SUCCEEDED(StringCchPrintfW(KeyName, cchKeyName, L"%s\\%s", DIFX_REFERENCES_KEY, Folder));
}

static
DWORD
WINAPI
DifxReadList(
    _In_ PCWSTR StoreInf,
    _In_ PCWSTR ValueName,
    _Outptr_result_maybenull_ PWSTR *List)
{
    WCHAR KeyName[MAX_PATH];
    DWORD Size = 0;
    HKEY hKey;
    LONG Error;

    *List = NULL;

    if (!DifxGetReferenceKeyName(StoreInf, KeyName, ARRAYSIZE(KeyName)))
        return ERROR_FUNCTION_FAILED;

    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, KeyName, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &hKey, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = RegQueryValueExW(hKey, ValueName, NULL, NULL, NULL, &Size);
    if (Error == ERROR_FILE_NOT_FOUND)
    {
        Error = ERROR_NOT_FOUND;
    }
    else if (Error == ERROR_SUCCESS)
    {
        /* Room for the two terminators a damaged value may lack */
        Size += 2 * sizeof(WCHAR);
        *List = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
        if (!*List)
        {
            Error = ERROR_NOT_ENOUGH_MEMORY;
        }
        else
        {
            Size -= 2 * sizeof(WCHAR);
            Error = RegQueryValueExW(hKey, ValueName, NULL, NULL, (PBYTE)*List, &Size);
            if (Error != ERROR_SUCCESS)
            {
                DifxFree(*List);
                *List = NULL;
            }
        }
    }

    RegCloseKey(hKey);
    return Error;
}

SIZE_T
WINAPI
DifxListLength(
    _In_opt_ PCWSTR List)
{
    PCWSTR Entry;

    if (!List)
        return 0;

    for (Entry = List; *Entry; Entry += wcslen(Entry) + 1);
    return Entry - List;
}

static
DWORD
WINAPI
DifxWriteList(
    _In_ PCWSTR StoreInf,
    _In_ PCWSTR ValueName,
    _In_opt_ PCWSTR List)
{
    WCHAR KeyName[MAX_PATH];
    SIZE_T Length;
    HKEY hKey;
    LONG Error;

    if (!DifxGetReferenceKeyName(StoreInf, KeyName, ARRAYSIZE(KeyName)))
        return ERROR_FUNCTION_FAILED;

    Error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, KeyName, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &hKey, NULL);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* An empty list is no value at all */
    Length = DifxListLength(List);
    if (Length)
        Error = RegSetValueExW(hKey, ValueName, 0, REG_MULTI_SZ, (const BYTE *)List, (DWORD)(Length + 1) * sizeof(WCHAR));
    else
        Error = RegDeleteValueW(hKey, ValueName);

    RegCloseKey(hKey);
    return Error;
}

BOOL
WINAPI
DifxListContains(
    _In_opt_ PCWSTR List,
    _In_ PCWSTR String)
{
    PCWSTR Entry;

    if (!List)
        return FALSE;

    for (Entry = List; *Entry; Entry += wcslen(Entry) + 1)
    {
        if (!_wcsicmp(Entry, String))
            return TRUE;
    }

    return FALSE;
}

DWORD
WINAPI
DifxListAppend(
    _Inout_ PWSTR *List,
    _In_ PCWSTR String)
{
    SIZE_T Length = DifxListLength(*List);
    SIZE_T cchString = wcslen(String) + 1;
    PWSTR NewList;

    if (*List)
        NewList = HeapReAlloc(GetProcessHeap(), 0, *List, (Length + cchString + 1) * sizeof(*NewList));
    else
        NewList = HeapAlloc(GetProcessHeap(), 0, (cchString + 1) * sizeof(*NewList));

    if (!NewList)
        return ERROR_NOT_ENOUGH_MEMORY;

    CopyMemory(NewList + Length, String, cchString * sizeof(*NewList));
    NewList[Length + cchString] = UNICODE_NULL;
    *List = NewList;
    return ERROR_SUCCESS;
}

/* Drops every entry equal to String */
static
VOID
WINAPI
DifxListRemove(
    _Inout_opt_ PWSTR List,
    _In_ PCWSTR String)
{
    PWSTR Read, Write;
    SIZE_T cchEntry;

    if (!List)
        return;

    for (Read = Write = List; *Read; Read += cchEntry)
    {
        cchEntry = wcslen(Read) + 1;
        if (!_wcsicmp(Read, String))
            continue;

        MoveMemory(Write, Read, cchEntry * sizeof(*Write));
        Write += cchEntry;
    }

    *Write = UNICODE_NULL;
}

DWORD
WINAPI
DifxAddReference(
    _In_ PCWSTR StoreInf,
    _In_ PCWSTR ApplicationId,
    _In_ PCWSTR DisplayName,
    _Out_ PBOOL AlreadyReferenced)
{
    PWSTR List;
    DWORD Error;

    *AlreadyReferenced = FALSE;

    Error = DifxReadList(StoreInf, DIFX_VALUE_INSTALLERS, &List);
    if (Error != ERROR_SUCCESS && Error != ERROR_NOT_FOUND)
        return Error;

    if (DifxListContains(List, ApplicationId))
    {
        *AlreadyReferenced = TRUE;
        DifxFree(List);
        return ERROR_SUCCESS;
    }

    Error = DifxListAppend(&List, ApplicationId);
    if (Error == ERROR_SUCCESS)
        Error = DifxWriteList(StoreInf, DIFX_VALUE_INSTALLERS, List);

    DifxFree(List);
    if (Error != ERROR_SUCCESS)
        return Error;

    /* The display name only accompanies the reference, its failures do not count */
    Error = DifxReadList(StoreInf, DIFX_VALUE_NAMES, &List);
    if ((Error == ERROR_SUCCESS || Error == ERROR_NOT_FOUND) &&
        DifxListAppend(&List, DisplayName) == ERROR_SUCCESS)
    {
        DifxWriteList(StoreInf, DIFX_VALUE_NAMES, List);
    }

    DifxFree(List);
    return ERROR_SUCCESS;
}

DWORD
WINAPI
DifxRemoveReference(
    _In_ PCWSTR StoreInf,
    _In_ PCWSTR ApplicationId,
    _In_ PCWSTR DisplayName,
    _Out_ PBOOL NotReferenced)
{
    PWSTR List;
    DWORD Error;

    *NotReferenced = FALSE;

    Error = DifxReadList(StoreInf, DIFX_VALUE_INSTALLERS, &List);
    if (Error == ERROR_NOT_FOUND)
    {
        *NotReferenced = TRUE;
        return ERROR_SUCCESS;
    }
    else if (Error != ERROR_SUCCESS)
    {
        return Error;
    }

    if (!DifxListContains(List, ApplicationId))
    {
        *NotReferenced = TRUE;
        DifxFree(List);
        return ERROR_SUCCESS;
    }

    DifxListRemove(List, ApplicationId);
    Error = DifxWriteList(StoreInf, DIFX_VALUE_INSTALLERS, List);
    DifxFree(List);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (DifxReadList(StoreInf, DIFX_VALUE_NAMES, &List) == ERROR_SUCCESS)
    {
        DifxListRemove(List, DisplayName);
        DifxWriteList(StoreInf, DIFX_VALUE_NAMES, List);
        DifxFree(List);
    }

    return ERROR_SUCCESS;
}

DWORD
WINAPI
DifxHasReferences(
    _In_ PCWSTR StoreInf,
    _Out_ PBOOL Referenced)
{
    PWSTR List;
    DWORD Error;

    *Referenced = TRUE;

    Error = DifxReadList(StoreInf, DIFX_VALUE_INSTALLERS, &List);
    if (Error == ERROR_NOT_FOUND)
    {
        *Referenced = FALSE;
        return ERROR_SUCCESS;
    }
    else if (Error != ERROR_SUCCESS)
    {
        return Error;
    }

    *Referenced = (DifxListLength(List) != 0);
    DifxFree(List);
    return ERROR_SUCCESS;
}

DWORD
WINAPI
DifxDeleteReferences(
    _In_ PCWSTR StoreInf)
{
    WCHAR KeyName[MAX_PATH];

    if (!DifxGetReferenceKeyName(StoreInf, KeyName, ARRAYSIZE(KeyName)))
        return ERROR_FUNCTION_FAILED;

    return RegDeleteKeyW(HKEY_LOCAL_MACHINE, KeyName);
}
