/*
 * PROJECT:     ReactOS Setup Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver store, the folder every imported driver package is kept and run from
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "setupapi_private.h"
#include <strsafe.h>

/* GLOBALS ********************************************************************/

#if defined(_M_AMD64)
#define DRVSTORE_ARCH L"amd64"
#elif defined(_M_ARM64)
#define DRVSTORE_ARCH L"arm64"
#elif defined(_M_ARM)
#define DRVSTORE_ARCH L"arm"
#else
#define DRVSTORE_ARCH L"x86"
#endif

#define DRVSTORE_REPOSITORY     L"\\DriverStore\\FileRepository"
#define DRVSTORE_INF_FILES_KEY  L"SYSTEM\\DriverDatabase\\DriverInfFiles"
#define DRVSTORE_PACKAGES_KEY   L"SYSTEM\\DriverDatabase\\DriverPackages"

/* FNV-1a, 64 bit */
#define DRVSTORE_HASH_BASIS     0xCBF29CE484222325ULL
#define DRVSTORE_HASH_PRIME     0x00000100000001B3ULL

/* FUNCTIONS ******************************************************************/

static
BOOL
WINAPI
DrvStoreGetRepository(
    _Out_writes_(cchPath) PWSTR Path,
    _In_ DWORD cchPath)
{
    UINT Length = GetSystemDirectoryW(Path, cchPath);

    if (!Length || Length >= cchPath)
        return FALSE;

    return SUCCEEDED(StringCchCatW(Path, cchPath, DRVSTORE_REPOSITORY));
}

static
BOOL
WINAPI
DrvStoreCreatePath(
    _In_ PCWSTR Path)
{
    WCHAR Partial[MAX_PATH];
    PWSTR Next;

    if (FAILED(StringCchCopyW(Partial, ARRAYSIZE(Partial), Path)))
        return FALSE;

    /* Skip the drive, or the UNC server and share, before creating anything */
    Next = strchrW(Partial, '\\');
    if (Next == Partial && Next[1] == '\\')
    {
        Next = strchrW(Next + 2, '\\');
        if (Next)
            Next = strchrW(Next + 1, '\\');
    }

    while (Next)
    {
        Next = strchrW(Next + 1, '\\');
        if (Next)
            *Next = UNICODE_NULL;

        if (!CreateDirectoryW(Partial, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
            return FALSE;

        if (Next)
            *Next = '\\';
    }

    return TRUE;
}

/* The package name, <inf name>_<architecture>_<hash of the inf> in lower case */
static
BOOL
WINAPI
DrvStoreGetPackageId(
    _In_ PCWSTR InfPath,
    _Out_writes_(cchId) PWSTR PackageId,
    _In_ DWORD cchId)
{
    ULONGLONG Hash = DRVSTORE_HASH_BASIS;
    BYTE Buffer[4096];
    PCWSTR Name;
    HANDLE hFile;
    DWORD Read, i;
    BOOL Ok;

    hFile = CreateFileW(InfPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;

    while ((Ok = ReadFile(hFile, Buffer, sizeof(Buffer), &Read, NULL)) && Read)
    {
        for (i = 0; i < Read; i++)
            Hash = (Hash ^ Buffer[i]) * DRVSTORE_HASH_PRIME;
    }
    CloseHandle(hFile);

    if (!Ok)
        return FALSE;

    Name = strrchrW(InfPath, '\\');
    Name = Name ? Name + 1 : InfPath;

    if (FAILED(StringCchPrintfW(PackageId, cchId, L"%s_%s_%016I64x", Name, DRVSTORE_ARCH, Hash)))
        return FALSE;

    CharLowerW(PackageId);
    return TRUE;
}

/* The package an inf published to the inf folder was imported as */
static
BOOL
WINAPI
DrvStoreGetPublishedPackage(
    _In_ PCWSTR InfName,
    _Out_writes_(cchId) PWSTR PackageId,
    _In_ DWORD cchId)
{
    WCHAR KeyName[MAX_PATH];
    DWORD cbData = (cchId - 1) * sizeof(WCHAR);
    DWORD Type;
    HKEY hKey;
    LONG rc;

    if (FAILED(StringCchPrintfW(KeyName, ARRAYSIZE(KeyName), L"%s\\%s", DRVSTORE_INF_FILES_KEY, InfName)))
        return FALSE;

    rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, KeyName, 0, KEY_QUERY_VALUE, &hKey);
    if (rc != ERROR_SUCCESS)
        return FALSE;

    rc = RegQueryValueExW(hKey, L"Active", NULL, &Type, (PBYTE)PackageId, &cbData);
    RegCloseKey(hKey);

    if (rc != ERROR_SUCCESS || Type != REG_SZ || cbData < sizeof(WCHAR))
        return FALSE;

    PackageId[cbData / sizeof(WCHAR)] = UNICODE_NULL;
    return TRUE;
}

/**
 * @brief The driver store folder of the package an inf belongs to, what DIRID 13 stands for.
 *
 * @remarks An inf inside the store belongs to its own folder, a published oem inf to the
 *          package it was imported as and any other inf to the package it would import as.
 */
BOOL
WINAPI
DRVSTORE_GetPackageDirectory(
    _In_ PCWSTR InfPath,
    _Out_writes_(cchDir) PWSTR Directory,
    _In_ DWORD cchDir)
{
    WCHAR Repository[MAX_PATH];
    WCHAR InfDirectory[MAX_PATH];
    WCHAR PackageId[MAX_PATH];
    PCWSTR Name;
    SIZE_T cchRepository;
    UINT Length;

    if (!DrvStoreGetRepository(Repository, ARRAYSIZE(Repository)))
        return FALSE;

    Name = strrchrW(InfPath, '\\');
    if (!Name)
        return FALSE;
    Name++;

    cchRepository = strlenW(Repository);
    if (!strncmpiW(InfPath, Repository, cchRepository) && InfPath[cchRepository] == '\\')
    {
        if ((SIZE_T)(Name - InfPath) > cchDir)
            return FALSE;

        lstrcpynW(Directory, InfPath, (int)(Name - InfPath));
        return TRUE;
    }

    Length = GetSystemWindowsDirectoryW(InfDirectory, ARRAYSIZE(InfDirectory));
    if (!Length || Length >= ARRAYSIZE(InfDirectory) ||
        FAILED(StringCchCatW(InfDirectory, ARRAYSIZE(InfDirectory), L"\\inf\\")))
    {
        return FALSE;
    }

    if ((SIZE_T)(Name - InfPath) == strlenW(InfDirectory) &&
        !strncmpiW(InfPath, InfDirectory, Name - InfPath))
    {
        if (!DrvStoreGetPublishedPackage(Name, PackageId, ARRAYSIZE(PackageId)))
            return FALSE;
    }
    else if (!DrvStoreGetPackageId(InfPath, PackageId, ARRAYSIZE(PackageId)))
    {
        return FALSE;
    }

    return SUCCEEDED(StringCchPrintfW(Directory, cchDir, L"%s\\%s", Repository, PackageId));
}

/* Copies one file of the package, keeping its path relative to the inf */
static
BOOL
WINAPI
DrvStoreCopyFile(
    _In_ PCWSTR SourceDirectory,
    _In_ PCWSTR PackageDirectory,
    _In_ PCWSTR RelativePath)
{
    WCHAR Source[MAX_PATH];
    WCHAR Target[MAX_PATH];
    PWSTR Slash;
    SIZE_T Length;

    while (*RelativePath == '\\' || (RelativePath[0] == '.' && RelativePath[1] == '\\'))
        RelativePath += (*RelativePath == '\\') ? 1 : 2;

    if (FAILED(StringCchPrintfW(Source, ARRAYSIZE(Source), L"%s\\%s", SourceDirectory, RelativePath)) ||
        FAILED(StringCchPrintfW(Target, ARRAYSIZE(Target), L"%s\\%s", PackageDirectory, RelativePath)))
    {
        return FALSE;
    }

    /* Media may carry the file compressed, with the last letter of its name replaced */
    if (GetFileAttributesW(Source) == INVALID_FILE_ATTRIBUTES)
    {
        Length = strlenW(Source);
        if (Length < 2 || Source[Length - 1] == '_')
            return FALSE;

        Source[Length - 1] = '_';
        Target[strlenW(Target) - 1] = '_';
        if (GetFileAttributesW(Source) == INVALID_FILE_ATTRIBUTES)
            return FALSE;
    }

    Slash = strrchrW(Target, '\\');
    if (Slash)
    {
        *Slash = UNICODE_NULL;
        if (!DrvStoreCreatePath(Target))
            return FALSE;
        *Slash = '\\';
    }

    if (!CopyFileW(Source, Target, FALSE))
    {
        WARN("Cannot copy %s to the driver store, error %lu\n", debugstr_w(Source), GetLastError());
        return FALSE;
    }

    return TRUE;
}

/* Reads a field of the first line with that key, from the architecture section or the plain one */
static
BOOL
WINAPI
DrvStoreGetField(
    _In_ HINF hInf,
    _In_ PCWSTR Section,
    _In_ PCWSTR Key,
    _In_ DWORD Field,
    _Out_writes_(cchValue) PWSTR Value,
    _In_ DWORD cchValue)
{
    WCHAR Decorated[MAX_PATH];
    INFCONTEXT Context;

    *Value = UNICODE_NULL;

    if (SUCCEEDED(StringCchPrintfW(Decorated, ARRAYSIZE(Decorated), L"%s.%s", Section, DRVSTORE_ARCH)) &&
        SetupFindFirstLineW(hInf, Decorated, Key, &Context))
    {
        return SetupGetStringFieldW(&Context, Field, Value, cchValue, NULL);
    }

    if (SetupFindFirstLineW(hInf, Section, Key, &Context))
        return SetupGetStringFieldW(&Context, Field, Value, cchValue, NULL);

    return FALSE;
}

static
VOID
WINAPI
DrvStoreCopySourceFiles(
    _In_ HINF hInf,
    _In_ PCWSTR Section,
    _In_ PCWSTR SourceDirectory,
    _In_ PCWSTR PackageDirectory)
{
    WCHAR FileName[MAX_PATH];
    WCHAR DiskId[32];
    WCHAR DiskPath[MAX_PATH];
    WCHAR SubDirectory[MAX_PATH];
    WCHAR Cabinet[MAX_PATH];
    WCHAR Relative[MAX_PATH];
    INFCONTEXT Context;
    BOOL bLine;

    for (bLine = SetupFindFirstLineW(hInf, Section, NULL, &Context);
         bLine;
         bLine = SetupFindNextLine(&Context, &Context))
    {
        if (!SetupGetStringFieldW(&Context, 0, FileName, ARRAYSIZE(FileName), NULL) ||
            !SetupGetStringFieldW(&Context, 1, DiskId, ARRAYSIZE(DiskId), NULL))
        {
            continue;
        }

        if (!SetupGetStringFieldW(&Context, 2, SubDirectory, ARRAYSIZE(SubDirectory), NULL))
            SubDirectory[0] = UNICODE_NULL;

        /* The disk gives the path below the inf and maybe the cabinet the files come in */
        if (!DrvStoreGetField(hInf, L"SourceDisksNames", DiskId, 4, DiskPath, ARRAYSIZE(DiskPath)))
            DiskPath[0] = UNICODE_NULL;

        if (DrvStoreGetField(hInf, L"SourceDisksNames", DiskId, 2, Cabinet, ARRAYSIZE(Cabinet)) &&
            strlenW(Cabinet) > 4 && !strcmpiW(Cabinet + strlenW(Cabinet) - 4, L".cab") &&
            SUCCEEDED(StringCchPrintfW(Relative, ARRAYSIZE(Relative), L"%s\\%s", DiskPath, Cabinet)))
        {
            DrvStoreCopyFile(SourceDirectory, PackageDirectory, Relative);
        }

        if (SUCCEEDED(StringCchPrintfW(Relative,
                                       ARRAYSIZE(Relative),
                                       L"%s%s%s\\%s",
                                       DiskPath,
                                       SubDirectory[0] ? L"\\" : L"",
                                       SubDirectory,
                                       FileName)))
        {
            DrvStoreCopyFile(SourceDirectory, PackageDirectory, Relative);
        }
    }
}

static
VOID
WINAPI
DrvStoreRecordPackage(
    _In_ PCWSTR PackageId,
    _In_ PCWSTR PublishedName,
    _In_ PCWSTR InfName,
    _In_ PCWSTR SourceDirectory,
    _In_ PCWSTR Catalog)
{
    WCHAR KeyName[MAX_PATH];
    WCHAR MultiId[MAX_PATH + 1];
    SIZE_T cchId = strlenW(PackageId);
    HKEY hKey;

    if (cchId >= MAX_PATH)
        return;

    /* The default value is the list of packages the inf was imported as */
    memcpy(MultiId, PackageId, cchId * sizeof(WCHAR));
    MultiId[cchId] = UNICODE_NULL;
    MultiId[cchId + 1] = UNICODE_NULL;

    if (SUCCEEDED(StringCchPrintfW(KeyName, ARRAYSIZE(KeyName), L"%s\\%s", DRVSTORE_INF_FILES_KEY, PublishedName)) &&
        RegCreateKeyExW(HKEY_LOCAL_MACHINE, KeyName, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, NULL, 0, REG_MULTI_SZ, (PBYTE)MultiId, (DWORD)(cchId + 2) * sizeof(WCHAR));
        RegSetValueExW(hKey, L"Active", 0, REG_SZ, (PBYTE)PackageId, (DWORD)(cchId + 1) * sizeof(WCHAR));
        RegCloseKey(hKey);
    }

    if (SUCCEEDED(StringCchPrintfW(KeyName, ARRAYSIZE(KeyName), L"%s\\%s", DRVSTORE_PACKAGES_KEY, PackageId)) &&
        RegCreateKeyExW(HKEY_LOCAL_MACHINE, KeyName, 0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (PBYTE)PublishedName, (DWORD)(strlenW(PublishedName) + 1) * sizeof(WCHAR));
        RegSetValueExW(hKey, L"InfName", 0, REG_SZ, (PBYTE)InfName, (DWORD)(strlenW(InfName) + 1) * sizeof(WCHAR));
        RegSetValueExW(hKey, L"OemPath", 0, REG_SZ, (PBYTE)SourceDirectory, (DWORD)(strlenW(SourceDirectory) + 1) * sizeof(WCHAR));
        if (*Catalog)
            RegSetValueExW(hKey, L"Catalog", 0, REG_SZ, (PBYTE)Catalog, (DWORD)(strlenW(Catalog) + 1) * sizeof(WCHAR));
        RegCloseKey(hKey);
    }
}

/**
 * @brief Imports the package of an oem inf into the driver store: the inf, its catalog and
 *        every file its SourceDisksFiles list, at their paths relative to the inf.
 *
 * @param[in] SourceInfPath
 * Full path of the inf on the media.
 *
 * @param[in] PublishedName
 * The oem inf name the inf is published as in the inf folder.
 */
BOOL
WINAPI
DRVSTORE_ImportPackage(
    _In_ PCWSTR SourceInfPath,
    _In_ PCWSTR PublishedName)
{
    WCHAR SourceDirectory[MAX_PATH];
    WCHAR PackageDirectory[MAX_PATH];
    WCHAR PackageId[MAX_PATH];
    WCHAR Recorded[MAX_PATH];
    WCHAR StoredInf[MAX_PATH];
    WCHAR Catalog[MAX_PATH];
    WCHAR Section[64];
    PCWSTR InfName;
    HINF hInf;

    InfName = strrchrW(SourceInfPath, '\\');
    if (!InfName || (SIZE_T)(InfName - SourceInfPath) >= ARRAYSIZE(SourceDirectory))
        return FALSE;

    lstrcpynW(SourceDirectory, SourceInfPath, (int)(InfName - SourceInfPath) + 1);
    InfName++;

    if (!DrvStoreGetPackageId(SourceInfPath, PackageId, ARRAYSIZE(PackageId)) ||
        !DrvStoreGetRepository(PackageDirectory, ARRAYSIZE(PackageDirectory)) ||
        FAILED(StringCchCatW(PackageDirectory, ARRAYSIZE(PackageDirectory), L"\\")) ||
        FAILED(StringCchCatW(PackageDirectory, ARRAYSIZE(PackageDirectory), PackageId)) ||
        !DrvStoreCreatePath(PackageDirectory))
    {
        ERR("Cannot create the driver store folder for %s\n", debugstr_w(SourceInfPath));
        return FALSE;
    }

    /* A package recorded for this inf whose folder still has the inf is in the store already */
    if (DrvStoreGetPublishedPackage(PublishedName, Recorded, ARRAYSIZE(Recorded)) &&
        !strcmpiW(Recorded, PackageId) &&
        SUCCEEDED(StringCchPrintfW(StoredInf, ARRAYSIZE(StoredInf), L"%s\\%s", PackageDirectory, InfName)) &&
        GetFileAttributesW(StoredInf) != INVALID_FILE_ATTRIBUTES)
    {
        return TRUE;
    }

    if (!DrvStoreCopyFile(SourceDirectory, PackageDirectory, InfName))
        return FALSE;

    hInf = SetupOpenInfFileW(SourceInfPath, NULL, INF_STYLE_WIN4, NULL);
    if (hInf == INVALID_HANDLE_VALUE)
        return FALSE;

    /* The most specific catalog entry wins, like the signature check picks it */
    if (!DrvStoreGetField(hInf, L"Version", L"CatalogFile.NT" DRVSTORE_ARCH, 1, Catalog, ARRAYSIZE(Catalog)) &&
        !DrvStoreGetField(hInf, L"Version", L"CatalogFile.NT", 1, Catalog, ARRAYSIZE(Catalog)) &&
        !DrvStoreGetField(hInf, L"Version", L"CatalogFile", 1, Catalog, ARRAYSIZE(Catalog)))
    {
        Catalog[0] = UNICODE_NULL;
    }

    if (Catalog[0])
        DrvStoreCopyFile(SourceDirectory, PackageDirectory, Catalog);

    DrvStoreCopySourceFiles(hInf, L"SourceDisksFiles", SourceDirectory, PackageDirectory);
    if (SUCCEEDED(StringCchPrintfW(Section, ARRAYSIZE(Section), L"SourceDisksFiles.%s", DRVSTORE_ARCH)))
        DrvStoreCopySourceFiles(hInf, Section, SourceDirectory, PackageDirectory);

    SetupCloseInfFile(hInf);

    DrvStoreRecordPackage(PackageId, PublishedName, InfName, SourceDirectory, Catalog);
    TRACE("Imported %s as %s\n", debugstr_w(SourceInfPath), debugstr_w(PackageId));
    return TRUE;
}

/* Where an inf sits as far as the driver store is concerned */
typedef enum _DRVSTORE_INF_LOCATION
{
    DrvStoreInfElsewhere,
    DrvStoreInfInInfDirectory,
    DrvStoreInfInRepository
} DRVSTORE_INF_LOCATION;

/* A bare file name means the inf folder, anything else is a path */
static
BOOL
WINAPI
DrvStoreResolveInf(
    _In_ PCWSTR FileName,
    _Out_writes_(cchPath) PWSTR Path,
    _In_ DWORD cchPath)
{
    WCHAR Combined[MAX_PATH];
    UINT Length;

    if (!strchrW(FileName, '\\') && !strchrW(FileName, '/'))
    {
        Length = GetSystemWindowsDirectoryW(Combined, ARRAYSIZE(Combined));
        if (!Length || Length >= ARRAYSIZE(Combined) ||
            FAILED(StringCchCatW(Combined, ARRAYSIZE(Combined), L"\\inf\\")) ||
            FAILED(StringCchCatW(Combined, ARRAYSIZE(Combined), FileName)))
        {
            return FALSE;
        }

        FileName = Combined;
    }

    Length = GetFullPathNameW(FileName, cchPath, Path, NULL);
    return (Length && Length < cchPath);
}

static
DRVSTORE_INF_LOCATION
WINAPI
DrvStoreClassifyInf(
    _In_ PCWSTR FullPath)
{
    WCHAR Directory[MAX_PATH];
    PCWSTR Name, Rest;
    SIZE_T cchDirectory;
    UINT Length;

    Name = strrchrW(FullPath, '\\');
    if (!Name)
        return DrvStoreInfElsewhere;

    /* <repository>\<package>\<inf> */
    if (DrvStoreGetRepository(Directory, ARRAYSIZE(Directory)))
    {
        cchDirectory = strlenW(Directory);
        if (!strncmpiW(FullPath, Directory, cchDirectory) && FullPath[cchDirectory] == '\\')
        {
            Rest = strchrW(FullPath + cchDirectory + 1, '\\');
            if (Rest && Rest == Name && Rest > FullPath + cchDirectory + 1)
                return DrvStoreInfInRepository;
        }
    }

    Length = GetSystemWindowsDirectoryW(Directory, ARRAYSIZE(Directory));
    if (!Length || Length >= ARRAYSIZE(Directory) ||
        FAILED(StringCchCatW(Directory, ARRAYSIZE(Directory), L"\\inf")))
    {
        return DrvStoreInfElsewhere;
    }

    cchDirectory = strlenW(Directory);
    if ((SIZE_T)(Name - FullPath) == cchDirectory && !strncmpiW(FullPath, Directory, cchDirectory))
        return DrvStoreInfInInfDirectory;

    return DrvStoreInfElsewhere;
}

static
BOOL
WINAPI
DrvStoreQueryPackageValue(
    _In_ PCWSTR PackageId,
    _In_opt_ PCWSTR ValueName,
    _Out_writes_(cchValue) PWSTR Value,
    _In_ DWORD cchValue)
{
    WCHAR KeyName[MAX_PATH];
    DWORD cbData = (cchValue - 1) * sizeof(WCHAR);
    DWORD Type;
    HKEY hKey;
    LONG rc;

    if (FAILED(StringCchPrintfW(KeyName, ARRAYSIZE(KeyName), L"%s\\%s", DRVSTORE_PACKAGES_KEY, PackageId)))
        return FALSE;

    rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, KeyName, 0, KEY_QUERY_VALUE, &hKey);
    if (rc != ERROR_SUCCESS)
        return FALSE;

    rc = RegQueryValueExW(hKey, ValueName, NULL, &Type, (PBYTE)Value, &cbData);
    RegCloseKey(hKey);

    if (rc != ERROR_SUCCESS || Type != REG_SZ || cbData < sizeof(WCHAR))
        return FALSE;

    Value[cbData / sizeof(WCHAR)] = UNICODE_NULL;
    return TRUE;
}

/* Hands a string back the way the inf location functions do */
static
BOOL
WINAPI
DrvStoreReturnString(
    _In_ PCWSTR String,
    _Out_writes_opt_(ReturnBufferSize) PWSTR ReturnBuffer,
    _In_ DWORD ReturnBufferSize,
    _Out_opt_ PDWORD RequiredSize)
{
    DWORD Size = (DWORD)strlenW(String) + 1;

    if (RequiredSize)
        *RequiredSize = Size;

    if (!ReturnBuffer || ReturnBufferSize < Size)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    memcpy(ReturnBuffer, String, Size * sizeof(WCHAR));
    return TRUE;
}

/***********************************************************************
 *      SetupGetInfDriverStoreLocationW  (SETUPAPI.@)
 */
BOOL
WINAPI
SetupGetInfDriverStoreLocationW(
    _In_ PCWSTR FileName,
    _In_opt_ PSP_ALTPLATFORM_INFO AlternatePlatformInfo,
    _In_opt_ PCWSTR LocaleName,
    _Out_writes_(ReturnBufferSize) PWSTR ReturnBuffer,
    _In_ DWORD ReturnBufferSize,
    _Out_opt_ PDWORD RequiredSize)
{
    WCHAR FullPath[MAX_PATH];
    WCHAR PackageId[MAX_PATH];
    WCHAR InfName[MAX_PATH];
    WCHAR Location[MAX_PATH];

    TRACE("%s %p %s %p %lu %p\n", debugstr_w(FileName), AlternatePlatformInfo, debugstr_w(LocaleName),
          ReturnBuffer, ReturnBufferSize, RequiredSize);

    if (!FileName || (!ReturnBuffer && ReturnBufferSize))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (AlternatePlatformInfo)
        FIXME("Alternate platform %p ignored\n", AlternatePlatformInfo);

    if (!DrvStoreResolveInf(FileName, FullPath, ARRAYSIZE(FullPath)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    switch (DrvStoreClassifyInf(FullPath))
    {
        case DrvStoreInfInRepository:
            if (GetFileAttributesW(FullPath) == INVALID_FILE_ATTRIBUTES)
                return FALSE;

            return DrvStoreReturnString(FullPath, ReturnBuffer, ReturnBufferSize, RequiredSize);

        case DrvStoreInfInInfDirectory:
            if (GetFileAttributesW(FullPath) == INVALID_FILE_ATTRIBUTES)
            {
                SetLastError(ERROR_FILE_NOT_FOUND);
                return FALSE;
            }

            /* Only a published oem inf was imported into the store */
            if (!DrvStoreGetPublishedPackage(strrchrW(FullPath, '\\') + 1, PackageId, ARRAYSIZE(PackageId)) ||
                !DrvStoreQueryPackageValue(PackageId, L"InfName", InfName, ARRAYSIZE(InfName)) ||
                !DrvStoreGetRepository(Location, ARRAYSIZE(Location)) ||
                FAILED(StringCchPrintfW(Location + strlenW(Location),
                                        ARRAYSIZE(Location) - strlenW(Location),
                                        L"\\%s\\%s",
                                        PackageId,
                                        InfName)) ||
                GetFileAttributesW(Location) == INVALID_FILE_ATTRIBUTES)
            {
                break;
            }

            return DrvStoreReturnString(Location, ReturnBuffer, ReturnBufferSize, RequiredSize);

        default:
            break;
    }

    SetLastError(ERROR_NOT_FOUND);
    return FALSE;
}

/***********************************************************************
 *      SetupGetInfPublishedNameW  (SETUPAPI.@)
 */
BOOL
WINAPI
SetupGetInfPublishedNameW(
    _In_ PCWSTR DriverStoreLocation,
    _Out_writes_(ReturnBufferSize) PWSTR ReturnBuffer,
    _In_ DWORD ReturnBufferSize,
    _Out_opt_ PDWORD RequiredSize)
{
    WCHAR FullPath[MAX_PATH];
    WCHAR PublishedName[MAX_PATH];
    PWSTR Name, Package;

    TRACE("%s %p %lu %p\n", debugstr_w(DriverStoreLocation), ReturnBuffer, ReturnBufferSize, RequiredSize);

    if (!DriverStoreLocation ||
        !DrvStoreResolveInf(DriverStoreLocation, FullPath, ARRAYSIZE(FullPath)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Name = strrchrW(FullPath, '\\');

    switch (DrvStoreClassifyInf(FullPath))
    {
        case DrvStoreInfInRepository:
            if (GetFileAttributesW(FullPath) == INVALID_FILE_ATTRIBUTES)
            {
                SetLastError(ERROR_PATH_NOT_FOUND);
                return FALSE;
            }

            /* The package folder names the package the published copy came from */
            *Name = UNICODE_NULL;
            Package = strrchrW(FullPath, '\\') + 1;
            if (!DrvStoreQueryPackageValue(Package, NULL, PublishedName, ARRAYSIZE(PublishedName)))
            {
                SetLastError(ERROR_NOT_FOUND);
                return FALSE;
            }
            break;

        case DrvStoreInfInInfDirectory:
            if (GetFileAttributesW(FullPath) == INVALID_FILE_ATTRIBUTES)
            {
                SetLastError(ERROR_FILE_NOT_FOUND);
                return FALSE;
            }

            if (FAILED(StringCchCopyW(PublishedName, ARRAYSIZE(PublishedName), Name + 1)))
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            break;

        default:
            SetLastError(ERROR_NOT_FOUND);
            return FALSE;
    }

    /* Unlike the store location, no buffer at all is a bad parameter */
    if (!ReturnBuffer || !ReturnBufferSize)
    {
        if (RequiredSize)
            *RequiredSize = (DWORD)strlenW(PublishedName) + 1;

        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    return DrvStoreReturnString(PublishedName, ReturnBuffer, ReturnBufferSize, RequiredSize);
}

/* Converts a wide string result of the inf location functions for their ANSI forms */
static
BOOL
WINAPI
DrvStoreReturnStringA(
    _In_ PCWSTR String,
    _Out_writes_opt_(ReturnBufferSize) PSTR ReturnBuffer,
    _In_ DWORD ReturnBufferSize,
    _Out_opt_ PDWORD RequiredSize)
{
    int Size;

    Size = WideCharToMultiByte(CP_ACP, 0, String, -1, NULL, 0, NULL, NULL);
    if (!Size)
        return FALSE;

    if (RequiredSize)
        *RequiredSize = Size;

    if (!ReturnBuffer || ReturnBufferSize < (DWORD)Size)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    return WideCharToMultiByte(CP_ACP, 0, String, -1, ReturnBuffer, ReturnBufferSize, NULL, NULL) != 0;
}

/***********************************************************************
 *      SetupGetInfDriverStoreLocationA  (SETUPAPI.@)
 */
BOOL
WINAPI
SetupGetInfDriverStoreLocationA(
    _In_ PCSTR FileName,
    _In_opt_ PSP_ALTPLATFORM_INFO AlternatePlatformInfo,
    _In_opt_ PCSTR LocaleName,
    _Out_writes_(ReturnBufferSize) PSTR ReturnBuffer,
    _In_ DWORD ReturnBufferSize,
    _Out_opt_ PDWORD RequiredSize)
{
    WCHAR Location[MAX_PATH];
    PWSTR FileNameW = NULL;
    PWSTR LocaleNameW = NULL;
    BOOL Result = FALSE;

    if (!FileName || (!ReturnBuffer && ReturnBufferSize))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    FileNameW = pSetupMultiByteToUnicode(FileName, CP_ACP);
    if (!FileNameW)
        goto Quit;

    if (LocaleName)
    {
        LocaleNameW = pSetupMultiByteToUnicode(LocaleName, CP_ACP);
        if (!LocaleNameW)
            goto Quit;
    }

    if (SetupGetInfDriverStoreLocationW(FileNameW, AlternatePlatformInfo, LocaleNameW, Location, ARRAYSIZE(Location), NULL))
        Result = DrvStoreReturnStringA(Location, ReturnBuffer, ReturnBufferSize, RequiredSize);

Quit:
    MyFree(LocaleNameW);
    MyFree(FileNameW);
    return Result;
}

/***********************************************************************
 *      SetupGetInfPublishedNameA  (SETUPAPI.@)
 */
BOOL
WINAPI
SetupGetInfPublishedNameA(
    _In_ PCSTR DriverStoreLocation,
    _Out_writes_(ReturnBufferSize) PSTR ReturnBuffer,
    _In_ DWORD ReturnBufferSize,
    _Out_opt_ PDWORD RequiredSize)
{
    WCHAR PublishedName[MAX_PATH];
    PWSTR LocationW;
    BOOL Result = FALSE;
    DWORD Error;

    if (!DriverStoreLocation)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    LocationW = pSetupMultiByteToUnicode(DriverStoreLocation, CP_ACP);
    if (!LocationW)
        return FALSE;

    if (SetupGetInfPublishedNameW(LocationW, PublishedName, ARRAYSIZE(PublishedName), NULL))
    {
        if (!ReturnBuffer || !ReturnBufferSize)
        {
            if (RequiredSize)
                *RequiredSize = WideCharToMultiByte(CP_ACP, 0, PublishedName, -1, NULL, 0, NULL, NULL);

            SetLastError(ERROR_INVALID_PARAMETER);
        }
        else
        {
            Result = DrvStoreReturnStringA(PublishedName, ReturnBuffer, ReturnBufferSize, RequiredSize);
        }
    }

    Error = GetLastError();
    MyFree(LocationW);
    SetLastError(Error);
    return Result;
}

/* Deletes a package folder and everything below it */
static
VOID
WINAPI
DrvStoreDeleteTree(
    _In_ PCWSTR Directory)
{
    WIN32_FIND_DATAW FindData;
    WCHAR Path[MAX_PATH];
    HANDLE hFind;

    if (FAILED(StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\*", Directory)))
        return;

    hFind = FindFirstFileW(Path, &FindData);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!strcmpW(FindData.cFileName, L".") || !strcmpW(FindData.cFileName, L".."))
                continue;

            if (FAILED(StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\%s", Directory, FindData.cFileName)))
                continue;

            if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                DrvStoreDeleteTree(Path);
            }
            else
            {
                SetFileAttributesW(Path, FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(Path))
                    WARN("Cannot delete %s, error %lu\n", debugstr_w(Path), GetLastError());
            }
        } while (FindNextFileW(hFind, &FindData));

        FindClose(hFind);
    }

    RemoveDirectoryW(Directory);
}

/**
 * @brief Removes the package an oem inf was imported as from the driver store.
 *
 * @param[in] PublishedName
 * The oem inf name the package is published as in the inf folder.
 */
VOID
WINAPI
DRVSTORE_RemovePackage(
    _In_ PCWSTR PublishedName)
{
    WCHAR PackageId[MAX_PATH];
    WCHAR Path[MAX_PATH];

    if (DrvStoreGetPublishedPackage(PublishedName, PackageId, ARRAYSIZE(PackageId)))
    {
        if (DrvStoreGetRepository(Path, ARRAYSIZE(Path)) &&
            SUCCEEDED(StringCchCatW(Path, ARRAYSIZE(Path), L"\\")) &&
            SUCCEEDED(StringCchCatW(Path, ARRAYSIZE(Path), PackageId)))
        {
            DrvStoreDeleteTree(Path);
        }

        /* Both records only hold values */
        if (SUCCEEDED(StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\%s", DRVSTORE_PACKAGES_KEY, PackageId)))
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, Path);

        TRACE("Removed %s from the driver store\n", debugstr_w(PackageId));
    }

    if (SUCCEEDED(StringCchPrintfW(Path, ARRAYSIZE(Path), L"%s\\%s", DRVSTORE_INF_FILES_KEY, PublishedName)))
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, Path);
}
