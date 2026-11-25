/*
 * PROJECT:     ReactOS Driver Install Frameworks API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver package preinstall, install, uninstall and lookup
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES *******************************************************************/

#include "precomp.h"

WINE_DEFAULT_DEBUG_CHANNEL(difxapi);

/* GLOBALS ********************************************************************/

#define DIFX_PREINSTALL_FLAGS   (DRIVER_PACKAGE_REPAIR | DRIVER_PACKAGE_SILENT | DRIVER_PACKAGE_FORCE | \
                                 DRIVER_PACKAGE_ONLY_IF_DEVICE_PRESENT | DRIVER_PACKAGE_LEGACY_MODE)

#define DIFX_INSTALL_FLAGS      DIFX_PREINSTALL_FLAGS

#define DIFX_UNINSTALL_FLAGS    (DRIVER_PACKAGE_SILENT | DRIVER_PACKAGE_FORCE | DRIVER_PACKAGE_DELETE_FILES)

/* The DriverPackageType names, the type is plug and play when the entry is missing */
static const struct
{
    PCWSTR Name;
    ULONG Type;
} DifxPackageTypes[] =
{
    { L"ClassCoinstaller",     DIFX_TYPE_CLASS_COINSTALLER },
    { L"ClassFilter",          DIFX_TYPE_CLASS_FILTER },
    { L"DeviceFilter",         DIFX_TYPE_DEVICE_FILTER },
    { L"FileSystem",           DIFX_TYPE_FILE_SYSTEM },
    { L"FileSystemFilter",     DIFX_TYPE_FILE_SYSTEM_FILTER },
    { L"FileSystemMinifilter", DIFX_TYPE_FILE_SYSTEM_MINIFILTER },
    { L"KernelModule",         DIFX_TYPE_KERNEL_MODULE },
    { L"KernelService",        DIFX_TYPE_KERNEL_SERVICE },
    { L"NdisImMiniport",       DIFX_TYPE_NDIS_IM_MINIPORT },
    { L"Network",              DIFX_TYPE_NETWORK },
    { L"PlugAndPlay",          DIFX_TYPE_PLUG_AND_PLAY },
};

/* FUNCTIONS ******************************************************************/

static
BOOL
WINAPI
DifxIsUserAdmin(VOID)
{
    SID_IDENTIFIER_AUTHORITY NtAuthority = { SECURITY_NT_AUTHORITY };
    PSID AdministratorsGroup;
    BOOL IsMember;

    if (!AllocateAndInitializeSid(&NtAuthority,
                                  2,
                                  SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0,
                                  &AdministratorsGroup))
    {
        return FALSE;
    }

    if (!CheckTokenMembership(NULL, AdministratorsGroup, &IsMember))
        IsMember = FALSE;

    FreeSid(AdministratorsGroup);
    return IsMember;
}

static
BOOL
WINAPI
DifxIsWow64(VOID)
{
    BOOL Wow64 = FALSE;

    if (!IsWow64Process(GetCurrentProcess(), &Wow64))
        return FALSE;

    return Wow64;
}

/**
 * @brief Switches setupapi between interactive and silent operation.
 *
 * @return Whether setupapi was in silent mode before.
 *
 * @remarks Callers restore by passing the returned value back in, so a process
 *          that was interactive is left in silent mode afterwards, like Windows.
 */
static
BOOL
WINAPI
DifxSetInteractive(
    _In_ BOOL Interactive)
{
    return (SetupSetNonInteractiveMode(!Interactive) != FALSE);
}

/* Checks the INF path argument and turns it into a full path of an existing file */
static
DWORD
WINAPI
DifxResolveInfPath(
    _In_ PCWSTR InfPath,
    _Outptr_result_maybenull_ PWSTR *FullPath)
{
    DWORD Length, Attributes, Error;
    PWSTR Path;

    *FullPath = NULL;

    if (wcsnlen(InfPath, DIFX_MAX_INF_PATH) >= DIFX_MAX_INF_PATH)
    {
        DifxLog(DIFXAPI_ERROR, L"Driver package file has invalid length.");
        return ERROR_FILENAME_EXCED_RANGE;
    }

    Length = GetFullPathNameW(InfPath, 0, NULL, NULL);
    if (!Length)
    {
        Error = GetLastError();
        DifxLogError(DIFXAPI_ERROR, Error, L"Invalid driver package path '%s'.", InfPath);
        return Error;
    }

    Path = HeapAlloc(GetProcessHeap(), 0, Length * sizeof(*Path));
    if (!Path)
        return ERROR_NOT_ENOUGH_MEMORY;

    Length = GetFullPathNameW(InfPath, Length, Path, NULL);
    if (!Length)
    {
        Error = GetLastError();
        DifxFree(Path);
        DifxLogError(DIFXAPI_ERROR, Error, L"Invalid driver package path '%s'.", InfPath);
        return Error;
    }

    /* A folder is no package */
    Attributes = GetFileAttributesW(Path);
    if (Attributes == INVALID_FILE_ATTRIBUTES)
        Error = GetLastError();
    else if (Attributes & FILE_ATTRIBUTE_DIRECTORY)
        Error = ERROR_FILE_NOT_FOUND;
    else
        Error = ERROR_SUCCESS;

    if (Error != ERROR_SUCCESS)
    {
        DifxLogError(DIFXAPI_ERROR, Error, L"Could not open file %s.", Path);
        DifxFree(Path);
        return Error;
    }

    *FullPath = Path;
    return ERROR_SUCCESS;
}

/* The caller either gives no installer information or all of it */
static
BOOL
WINAPI
DifxIsInstallerInfoValid(
    _In_opt_ PCINSTALLERINFO_W InstallerInfo)
{
    if (!InstallerInfo)
        return TRUE;

    if (!InstallerInfo->pApplicationId || !*InstallerInfo->pApplicationId ||
        !InstallerInfo->pDisplayName || !*InstallerInfo->pDisplayName ||
        !InstallerInfo->pProductName || !*InstallerInfo->pProductName ||
        !InstallerInfo->pMfgName || !*InstallerInfo->pMfgName)
    {
        DifxLog(DIFXAPI_ERROR,
                L"The INSTALLERINFO structure passed in by the caller was non-NULL, "
                L"but one or more fields of the structure was NULL or an empty string.");
        return FALSE;
    }

    return TRUE;
}

/* Administrator rights and a native process are needed to touch the driver store */
static
DWORD
WINAPI
DifxCheckCaller(
    _In_ PCWSTR AccessDeniedMessage)
{
    if (!DifxIsUserAdmin())
    {
        DifxLog(DIFXAPI_ERROR, AccessDeniedMessage);
        return ERROR_ACCESS_DENIED;
    }

    if (DifxIsWow64())
    {
        DifxLog(DIFXAPI_ERROR, L"System does not work under WOW64 and requires 64-bit version.");
        return ERROR_IN_WOW64;
    }

    return ERROR_SUCCESS;
}

/* Opens the package INF and reads the package type it declares */
static
DWORD
WINAPI
DifxGetPackageType(
    _In_ PCWSTR InfPath,
    _Out_ PULONG Type)
{
    WCHAR TypeName[64];
    INFCONTEXT Context;
    DWORD Error;
    HINF hInf;
    ULONG i;

    *Type = DIFX_TYPE_UNKNOWN;

    hInf = SetupOpenInfFileW(InfPath, NULL, INF_STYLE_WIN4, NULL);
    if (hInf == INVALID_HANDLE_VALUE)
    {
        Error = GetLastError();
        if (Error == ERROR_WRONG_INF_STYLE)
            Error = ERROR_INVALID_DATA;

        DifxLogError(DIFXAPI_ERROR, Error, L"Failed to open %s.", InfPath);
        return Error;
    }

    if (!SetupFindFirstLineW(hInf, L"Version", L"DriverPackageType", &Context))
    {
        *Type = DIFX_TYPE_PLUG_AND_PLAY;
    }
    else if (SetupGetStringFieldW(&Context, 1, TypeName, ARRAYSIZE(TypeName), NULL))
    {
        for (i = 0; i < ARRAYSIZE(DifxPackageTypes); i++)
        {
            if (!_wcsicmp(TypeName, DifxPackageTypes[i].Name))
            {
                *Type = DifxPackageTypes[i].Type;
                break;
            }
        }
    }

    SetupCloseInfFile(hInf);
    return ERROR_SUCCESS;
}

/* The adding of a package failed, tell why */
static
DWORD
WINAPI
DifxReportAddFailure(
    _In_ DWORD Error,
    _In_ PCWSTR InfPath)
{
    if (Error == (DWORD)CERT_E_WRONG_USAGE)
    {
        DifxLogError(DIFXAPI_ERROR, Error, L"Invalid signature. Possibly rejected by user.");
    }
    else if (Error == (DWORD)TRUST_E_NOSIGNATURE)
    {
        DifxLogError(DIFXAPI_ERROR, Error, L"Unsigned driver. Possibly rejected by user.");
    }
    else if (Error == ERROR_FILE_NOT_FOUND)
    {
        DifxLog(DIFXAPI_ERROR, L"One or more files referenced by '%s' cannot be found in the package.", InfPath);
        return ERROR_MISSING_FILE;
    }

    return Error;
}

/* Publishes the catalog of a freshly added entry, removing the entry if that fails */
static
DWORD
WINAPI
DifxFinishAdd(
    _In_ PCWSTR StoreInf)
{
    DWORD Error;

    Error = DifxCatalogAdd(StoreInf);
    if (Error == ERROR_SUCCESS)
        return ERROR_SUCCESS;

    DifxLogError(DIFXAPI_ERROR, Error, L"Failed to add catalog file for '%s'.", StoreInf);
    if (DifxStoreDelete(StoreInf) != ERROR_SUCCESS)
        DifxLog(DIFXAPI_ERROR, L"Could not delete driver store entry '%s'.", StoreInf);

    return Error;
}

static
DWORD
WINAPI
DifxPreinstall(
    _In_opt_ PCWSTR InfPath,
    _In_ DWORD Flags)
{
    WCHAR StoreInf[MAX_PATH];
    PWSTR FullPath = NULL;
    BOOL WasSilent;
    ULONG Type;
    DWORD Error;

    if (!InfPath)
    {
        DifxLog(DIFXAPI_ERROR, L"Driver Package Inf path parameter is NULL.");
        return ERROR_INVALID_PARAMETER;
    }

    Error = DifxResolveInfPath(InfPath, &FullPath);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (Flags & ~DIFX_PREINSTALL_FLAGS)
    {
        DifxLog(DIFXAPI_ERROR, L"Flags are invalid.");
        Error = ERROR_INVALID_PARAMETER;
        goto Quit;
    }

    Error = DifxCheckCaller(L"Access denied! Administrator account required.");
    if (Error != ERROR_SUCCESS)
        goto Quit;

    /* Repairing replaces an entry that may already be there */
    if (!(Flags & DRIVER_PACKAGE_REPAIR) && DifxStoreFind(FullPath, StoreInf) == ERROR_SUCCESS)
    {
        DifxLog(DIFXAPI_INFO, L"Driver package is already preinstalled '%s'.", FullPath);
        Error = ERROR_ALREADY_EXISTS;
        goto Quit;
    }

    if (DifxIsFromInfDirectory(FullPath))
    {
        DifxLog(DIFXAPI_ERROR, L"Can't preinstall driver packages from the INF directory.");
        Error = ERROR_CANT_ACCESS_FILE;
        goto Quit;
    }

    Error = DifxGetPackageType(FullPath, &Type);
    if (Error != ERROR_SUCCESS)
        goto Quit;

    if (Type != DIFX_TYPE_PLUG_AND_PLAY)
    {
        DifxLog(DIFXAPI_ERROR, L"Preinstall is not a supported operation for driver type %u", Type);
        Error = ERROR_INVALID_FUNCTION;
        goto Quit;
    }

    WasSilent = DifxSetInteractive(!(Flags & DRIVER_PACKAGE_SILENT));

    if (Flags & DRIVER_PACKAGE_ONLY_IF_DEVICE_PRESENT)
    {
        Error = DifxProbeDevices(FullPath);

        /* A present device that already has an equal or better driver needs force */
        if (Error == ERROR_NO_MORE_ITEMS && (Flags & DRIVER_PACKAGE_FORCE))
            Error = ERROR_SUCCESS;
    }

    if (Error == ERROR_SUCCESS)
    {
        Error = DifxStoreAdd(FullPath, (Flags & DRIVER_PACKAGE_REPAIR) != 0, StoreInf);
        if (Error == ERROR_SUCCESS)
            Error = DifxFinishAdd(StoreInf);
        else
            Error = DifxReportAddFailure(Error, FullPath);
    }

    DifxSetInteractive(WasSilent);

Quit:
    DifxFree(FullPath);
    return Error;
}

/* Takes an application reference back after an install failed */
static
VOID
WINAPI
DifxUndoReference(
    _In_ PCWSTR StoreInf,
    _In_ PCINSTALLERINFO_W InstallerInfo)
{
    BOOL NotReferenced;

    if (DifxRemoveReference(StoreInf,
                            InstallerInfo->pApplicationId,
                            InstallerInfo->pDisplayName,
                            &NotReferenced) != ERROR_SUCCESS)
    {
        DifxLog(DIFXAPI_ERROR,
                L"Could not remove '%s' from reference list of driver store entry '%s'",
                InstallerInfo->pApplicationId,
                StoreInf);
    }
    else
    {
        DifxLog(DIFXAPI_INFO,
                L"Successfully removed '%s' from reference list of driver store entry '%s'",
                InstallerInfo->pApplicationId,
                StoreInf);
    }
}

/* Removes an entry this install added before it failed */
static
VOID
WINAPI
DifxUndoAdd(
    _In_ PCWSTR StoreInf)
{
    if (DifxStoreDelete(StoreInf) != ERROR_SUCCESS)
        DifxLog(DIFXAPI_ERROR, L"Could not delete driver store entry '%s'.", StoreInf);
    else
        DifxLog(DIFXAPI_INFO, L"Successfully deleted driver store entry '%s'.", StoreInf);

    if (DifxDeleteReferences(StoreInf) != ERROR_SUCCESS)
        DifxLog(DIFXAPI_ERROR, L"Could not delete properties for driver store entry '%s'.", StoreInf);
    else
        DifxLog(DIFXAPI_INFO, L"Successfully deleted properties for driver store entry '%s'.", StoreInf);
}

static
DWORD
WINAPI
DifxInstall(
    _In_opt_ PCWSTR InfPath,
    _In_ DWORD Flags,
    _In_opt_ PCINSTALLERINFO_W InstallerInfo,
    _Out_opt_ BOOL *NeedReboot)
{
    WCHAR StoreInf[MAX_PATH];
    PWSTR FullPath = NULL;
    BOOL Force = (Flags & DRIVER_PACKAGE_FORCE) != 0;
    BOOL Repair = (Flags & DRIVER_PACKAGE_REPAIR) != 0;
    BOOL AddedEntry = FALSE, Referenced = FALSE, AlreadyReferenced = FALSE;
    BOOL InStore, AddEntry, WasSilent;
    DWORD Error, FindError;
    ULONG Type;

    if (!InfPath || !NeedReboot)
    {
        DifxLog(DIFXAPI_ERROR, L"Parameter is NULL.");
        return ERROR_INVALID_PARAMETER;
    }

    *NeedReboot = FALSE;
    StoreInf[0] = UNICODE_NULL;

    Error = DifxResolveInfPath(InfPath, &FullPath);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (Flags & ~DIFX_INSTALL_FLAGS)
    {
        DifxLog(DIFXAPI_ERROR, L"Flags are invalid.");
        Error = ERROR_INVALID_PARAMETER;
        goto Quit;
    }

    if (!DifxIsInstallerInfoValid(InstallerInfo))
    {
        Error = ERROR_INVALID_PARAMETER;
        goto Quit;
    }

    Error = DifxCheckCaller(L"Access denied! Administrator account required.");
    if (Error != ERROR_SUCCESS)
        goto Quit;

    Error = DifxGetPackageType(FullPath, &Type);
    if (Error != ERROR_SUCCESS)
        goto Quit;

    if (Type != DIFX_TYPE_PLUG_AND_PLAY)
    {
        FIXME("Driver package type %lu is not supported\n", Type);
        Error = ERROR_UNSUPPORTED_TYPE;
        goto Quit;
    }

    if (Flags & DRIVER_PACKAGE_LEGACY_MODE)
    {
        DifxLog(DIFXAPI_WARNING,
                L"DRIVER_PACKAGE_LEGACY_MODE flag set but not supported on Plug and Play driver on VISTA. "
                L"Flag will be ignored.");
    }

    if (Repair && DifxIsFromInfDirectory(FullPath))
    {
        DifxLog(DIFXAPI_ERROR, L"Can't repair driver packages from the INF directory.");
        Error = ERROR_INVALID_DATA;
        goto Quit;
    }

    WasSilent = DifxSetInteractive(!(Flags & DRIVER_PACKAGE_SILENT));

    /* The default result for a probe that sends the package nowhere */
    Error = ERROR_INSTALL_FAILURE;

    if (Flags & DRIVER_PACKAGE_ONLY_IF_DEVICE_PRESENT)
    {
        Error = DifxProbeDevices(FullPath);
        if (Error == ERROR_NO_MORE_ITEMS && !Force)
        {
            /* Nothing is installed, an entry already in the store still gets the reference */
            if (InstallerInfo)
            {
                FindError = DifxStoreFind(FullPath, StoreInf);
                if (FindError != ERROR_SUCCESS)
                {
                    Error = ERROR_INSTALL_FAILURE;
                }
                else
                {
                    Error = DifxAddReference(StoreInf,
                                             InstallerInfo->pApplicationId,
                                             InstallerInfo->pDisplayName,
                                             &AlreadyReferenced);
                    if (Error == ERROR_SUCCESS)
                        Error = ERROR_NO_MORE_ITEMS;
                }
            }

            goto Done;
        }
        else if (Error != ERROR_SUCCESS && Error != ERROR_NO_MORE_ITEMS)
        {
            goto Done;
        }
    }

    FindError = DifxStoreFind(FullPath, StoreInf);
    InStore = (FindError == ERROR_SUCCESS);

    /* A lookup that fails for another reason leaves the store alone */
    AddEntry = (!InStore && FindError == ERROR_NOT_FOUND) || (InStore && Repair);

    if (AddEntry)
    {
        if (DifxIsFromInfDirectory(FullPath))
        {
            DifxLog(DIFXAPI_ERROR, L"Can't preinstall and then install driver packages from the INF directory.");
            Error = ERROR_CANT_ACCESS_FILE;
            goto Done;
        }

        Error = DifxStoreAdd(FullPath, Repair, StoreInf);
        if (Error != ERROR_SUCCESS)
        {
            Error = DifxReportAddFailure(Error, FullPath);
            goto Failed;
        }

        Error = DifxCatalogAdd(StoreInf);
        if (Error != ERROR_SUCCESS)
        {
            DifxLogError(DIFXAPI_ERROR, Error, L"Failed to add catalog file for '%s'.", StoreInf);
            if (DifxStoreDelete(StoreInf) != ERROR_SUCCESS)
                DifxLog(DIFXAPI_ERROR, L"Could not delete driver store entry '%s'.", StoreInf);
            goto Failed;
        }

        AddedEntry = !InStore;
    }

    if (InstallerInfo)
    {
        Error = DifxAddReference(StoreInf,
                                 InstallerInfo->pApplicationId,
                                 InstallerInfo->pDisplayName,
                                 &AlreadyReferenced);
        if (Error != ERROR_SUCCESS)
            goto Failed;
    }

    Referenced = TRUE;

    DifxLog(DIFXAPI_INFO, L"Installing INF file '%s' (Plug and Play).", FullPath);
    Error = DifxInstallPnp(StoreInf, Force, Repair, NeedReboot);

Failed:
    /* No device or nothing better leaves the package in the store */
    if (Error != ERROR_SUCCESS && Error != ERROR_NO_MORE_ITEMS &&
        Error != ERROR_NO_SUCH_DEVINST && Error != ERROR_NO_DEVICE_ID)
    {
        if (Referenced && !AlreadyReferenced && InstallerInfo)
            DifxUndoReference(StoreInf, InstallerInfo);

        if (AddedEntry)
            DifxUndoAdd(StoreInf);
    }

Done:
    DifxSetInteractive(WasSilent);

Quit:
    DifxFree(FullPath);
    return Error;
}

static
DWORD
WINAPI
DifxUninstall(
    _In_opt_ PCWSTR InfPath,
    _In_ DWORD Flags,
    _In_opt_ PCINSTALLERINFO_W InstallerInfo,
    _Out_opt_ BOOL *NeedReboot)
{
    WCHAR StoreInf[MAX_PATH];
    PWSTR FullPath = NULL;
    BOOL WasSilent = TRUE, Removed = FALSE, NotReferenced = FALSE, Referenced, Readded;
    DWORD Error;
    ULONG Type;

    if (!InfPath || !NeedReboot)
    {
        DifxLog(DIFXAPI_ERROR, L"Parameter is NULL.");
        return ERROR_INVALID_PARAMETER;
    }

    *NeedReboot = FALSE;

    Error = DifxResolveInfPath(InfPath, &FullPath);
    if (Error != ERROR_SUCCESS)
        return Error;

    if (Flags & ~DIFX_UNINSTALL_FLAGS)
    {
        DifxLog(DIFXAPI_ERROR, L"Flags are invalid.");
        Error = ERROR_INVALID_PARAMETER;
        goto Quit;
    }

    if (!DifxIsInstallerInfoValid(InstallerInfo))
    {
        Error = ERROR_INVALID_PARAMETER;
        goto Quit;
    }

    Error = DifxCheckCaller(L"Access denied! Administrator account required.");
    if (Error != ERROR_SUCCESS)
        goto Quit;

    Error = DifxGetPackageType(FullPath, &Type);
    if (Error != ERROR_SUCCESS)
        goto Quit;

    if (Type != DIFX_TYPE_PLUG_AND_PLAY)
    {
        FIXME("Driver package type %lu is not supported\n", Type);
        Error = ERROR_UNSUPPORTED_TYPE;
        goto Quit;
    }

    /* Windows leaves setupapi interactive when the lookup fails */
    Error = DifxStoreFind(FullPath, StoreInf);
    if (Error != ERROR_SUCCESS)
    {
        if (Error == ERROR_NOT_FOUND)
        {
            DifxLogError(DIFXAPI_INFO, Error, L"No driver store entry for '%s' found.", FullPath);
            Error = ERROR_DRIVER_PACKAGE_NOT_IN_STORE;
        }

        goto Done;
    }

    WasSilent = DifxSetInteractive(!(Flags & DRIVER_PACKAGE_SILENT));

    /* Without force the package stays while other applications still reference it */
    if (!(Flags & DRIVER_PACKAGE_FORCE))
    {
        if (InstallerInfo)
        {
            Error = DifxRemoveReference(StoreInf,
                                        InstallerInfo->pApplicationId,
                                        InstallerInfo->pDisplayName,
                                        &NotReferenced);
            if (Error != ERROR_SUCCESS)
            {
                DifxLog(DIFXAPI_ERROR,
                        L"Could not remove the reference of application '%s' from driver '%s'",
                        InstallerInfo->pApplicationId,
                        FullPath);
                goto Failed;
            }
        }

        Removed = TRUE;

        Error = DifxHasReferences(StoreInf, &Referenced);
        if (Error != ERROR_SUCCESS)
        {
            DifxLog(DIFXAPI_ERROR,
                    L"Could not verify if there are any applications that are still dependent on driver '%s'.",
                    FullPath);
            goto Failed;
        }

        if (Referenced)
        {
            DifxLog(DIFXAPI_INFO, L"Will not uninstall because other Application depend on this package %s.", FullPath);
            Error = ERROR_DEPENDENT_APPLICATIONS_EXIST;
            goto Done;
        }
    }

    DifxLog(DIFXAPI_INFO, L"Uninstalling driver package %s...", FullPath);
    Error = DifxUninstallPnp(StoreInf, (Flags & DRIVER_PACKAGE_DELETE_FILES) != 0, NeedReboot);
    if (Error != ERROR_SUCCESS)
    {
        if (Error == ERROR_NOT_AN_INSTALLED_OEM_INF)
            DifxLog(DIFXAPI_ERROR, L"Cannot uninstall inbox driver package '%s'", FullPath);
        else
            DifxLogError(DIFXAPI_ERROR, Error, L"An error occurred while uninstalling driver package '%s'", FullPath);

        goto Failed;
    }

    if (DifxDeleteReferences(StoreInf) != ERROR_SUCCESS)
        DifxLog(DIFXAPI_ERROR, L"Could not delete properties for driver store entry '%s'.", StoreInf);
    else
        DifxLog(DIFXAPI_INFO, L"Successfully deleted properties for driver store entry '%s'.", StoreInf);

    DifxLog(DIFXAPI_SUCCESS, L"Uninstall completed.");
    goto Done;

Failed:
    /* Give the reference back when the package could not be removed */
    if (Removed && InstallerInfo && !NotReferenced)
    {
        Readded = FALSE;
        if (DifxAddReference(StoreInf,
                             InstallerInfo->pApplicationId,
                             InstallerInfo->pDisplayName,
                             &Readded) != ERROR_SUCCESS)
        {
            DifxLog(DIFXAPI_ERROR,
                    L"Could not re-add '%s' to reference list of driver store entry '%s'",
                    InstallerInfo->pApplicationId,
                    StoreInf);
        }
        else
        {
            DifxLog(DIFXAPI_INFO,
                    L"Successfully re-added '%s' to reference list of driver store entry '%s'",
                    InstallerInfo->pApplicationId,
                    StoreInf);
        }
    }

Done:
    DifxSetInteractive(WasSilent);

Quit:
    DifxFree(FullPath);
    return Error;
}

/* Looks up the driver store entry an INF belongs to */
static
DWORD
WINAPI
DifxGetPath(
    _In_ PCWSTR InfPath,
    _Out_writes_(MAX_PATH) PWSTR StoreInf)
{
    PWSTR FullPath = NULL;
    DWORD Error;
    ULONG Type;

    Error = DifxResolveInfPath(InfPath, &FullPath);
    if (Error != ERROR_SUCCESS)
        return Error;

    Error = DifxCheckCaller(L"Access denied! Administrator account is required.");
    if (Error != ERROR_SUCCESS)
        goto Quit;

    Error = DifxGetPackageType(FullPath, &Type);
    if (Error != ERROR_SUCCESS)
        goto Quit;

    if (Type != DIFX_TYPE_PLUG_AND_PLAY)
    {
        FIXME("Driver package type %lu is not supported\n", Type);
        Error = ERROR_UNSUPPORTED_TYPE;
        goto Quit;
    }

    Error = DifxStoreFind(FullPath, StoreInf);
    if (Error == ERROR_NOT_FOUND)
        Error = ERROR_DRIVER_PACKAGE_NOT_IN_STORE;

Quit:
    DifxFree(FullPath);
    return Error;
}

/* Converts the installer information for the ANSI entry points */
static
DWORD
WINAPI
DifxConvertInstallerInfo(
    _In_ PCINSTALLERINFO_A InstallerInfo,
    _Out_ PINSTALLERINFO_W Wide)
{
    ZeroMemory(Wide, sizeof(*Wide));

    if ((InstallerInfo->pApplicationId && !(Wide->pApplicationId = DifxAnsiToWide(InstallerInfo->pApplicationId))) ||
        (InstallerInfo->pDisplayName && !(Wide->pDisplayName = DifxAnsiToWide(InstallerInfo->pDisplayName))) ||
        (InstallerInfo->pProductName && !(Wide->pProductName = DifxAnsiToWide(InstallerInfo->pProductName))) ||
        (InstallerInfo->pMfgName && !(Wide->pMfgName = DifxAnsiToWide(InstallerInfo->pMfgName))))
    {
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    return ERROR_SUCCESS;
}

static
VOID
WINAPI
DifxFreeInstallerInfo(
    _In_ PINSTALLERINFO_W Wide)
{
    DifxFree(Wide->pApplicationId);
    DifxFree(Wide->pDisplayName);
    DifxFree(Wide->pProductName);
    DifxFree(Wide->pMfgName);
}

DWORD
WINAPI
DriverPackagePreinstallW(
    _In_ PCWSTR DriverPackageInfPath,
    _In_ DWORD Flags)
{
    DWORD Error;

    DifxLog(DIFXAPI_INFO, L"ENTER:  DriverPackagePreinstallW");

    Error = DifxPreinstall(DriverPackageInfPath, Flags);
    if (Error == ERROR_SUCCESS || Error == ERROR_ALREADY_EXISTS)
        DifxLog(DIFXAPI_SUCCESS, L"%s is preinstalled.", DriverPackageInfPath);

    DifxLog(DIFXAPI_INFO, L"RETURN: DriverPackagePreinstallW  (0x%X)", Error);
    return Error;
}

DWORD
WINAPI
DriverPackagePreinstallA(
    _In_ PCSTR DriverPackageInfPath,
    _In_ DWORD Flags)
{
    PWSTR InfPath;
    DWORD Error;

    DifxLog(DIFXAPI_INFO, L"ENTER:  DriverPackagePreinstallA");

    if (!DriverPackageInfPath)
    {
        Error = ERROR_INVALID_PARAMETER;
    }
    else if (!(InfPath = DifxAnsiToWide(DriverPackageInfPath)))
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
    }
    else
    {
        Error = DriverPackagePreinstallW(InfPath, Flags);
        DifxFree(InfPath);
    }

    DifxLog(DIFXAPI_INFO, L"RETURN: DriverPackagePreinstallA  (0x%X)", Error);
    return Error;
}

DWORD
WINAPI
DriverPackageInstallW(
    _In_ PCWSTR DriverPackageInfPath,
    _In_ DWORD Flags,
    _In_opt_ PCINSTALLERINFO_W pInstallerInfo,
    _Out_ BOOL *pNeedReboot)
{
    DWORD Error;

    DifxLog(DIFXAPI_INFO, L"ENTER:  DriverPackageInstallW");
    Error = DifxInstall(DriverPackageInfPath, Flags, pInstallerInfo, pNeedReboot);
    DifxLog(DIFXAPI_INFO, L"RETURN: DriverPackageInstallW  (0x%X)", Error);
    return Error;
}

DWORD
WINAPI
DriverPackageInstallA(
    _In_ PCSTR DriverPackageInfPath,
    _In_ DWORD Flags,
    _In_opt_ PCINSTALLERINFO_A pInstallerInfo,
    _Out_ BOOL *pNeedReboot)
{
    INSTALLERINFO_W InstallerInfo;
    PWSTR InfPath = NULL;
    DWORD Error;

    DifxLog(DIFXAPI_INFO, L"ENTER:  DriverPackageInstallA");
    ZeroMemory(&InstallerInfo, sizeof(InstallerInfo));

    if (!DriverPackageInfPath || !pNeedReboot)
    {
        Error = ERROR_INVALID_PARAMETER;
    }
    else if (!(InfPath = DifxAnsiToWide(DriverPackageInfPath)))
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
    }
    else if (pInstallerInfo &&
             DifxConvertInstallerInfo(pInstallerInfo, &InstallerInfo) != ERROR_SUCCESS)
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
    }
    else
    {
        Error = DriverPackageInstallW(InfPath, Flags, pInstallerInfo ? &InstallerInfo : NULL, pNeedReboot);
    }

    DifxFree(InfPath);
    DifxFreeInstallerInfo(&InstallerInfo);

    DifxLog(DIFXAPI_INFO, L"RETURN: DriverPackageInstallA  (0x%X)", Error);
    return Error;
}

DWORD
WINAPI
DriverPackageUninstallW(
    _In_ PCWSTR DriverPackageInfPath,
    _In_ DWORD Flags,
    _In_opt_ PCINSTALLERINFO_W pInstallerInfo,
    _Out_ BOOL *pNeedReboot)
{
    DWORD Error;

    DifxLog(DIFXAPI_INFO, L"ENTER:  DriverPackageUninstallW");
    Error = DifxUninstall(DriverPackageInfPath, Flags, pInstallerInfo, pNeedReboot);
    DifxLog(DIFXAPI_INFO, L"RETURN: DriverPackageUninstallW  (0x%X)", Error);
    return Error;
}

DWORD
WINAPI
DriverPackageUninstallA(
    _In_ PCSTR DriverPackageInfPath,
    _In_ DWORD Flags,
    _In_opt_ PCINSTALLERINFO_A pInstallerInfo,
    _Out_ BOOL *pNeedReboot)
{
    INSTALLERINFO_W InstallerInfo;
    PWSTR InfPath = NULL;
    DWORD Error;

    DifxLog(DIFXAPI_INFO, L"ENTER:  DriverPackageUninstallA");
    ZeroMemory(&InstallerInfo, sizeof(InstallerInfo));

    if (!DriverPackageInfPath || !pNeedReboot)
    {
        Error = ERROR_INVALID_PARAMETER;
    }
    else if (!(InfPath = DifxAnsiToWide(DriverPackageInfPath)))
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
    }
    else if (pInstallerInfo &&
             DifxConvertInstallerInfo(pInstallerInfo, &InstallerInfo) != ERROR_SUCCESS)
    {
        Error = ERROR_NOT_ENOUGH_MEMORY;
    }
    else
    {
        Error = DriverPackageUninstallW(InfPath, Flags, pInstallerInfo ? &InstallerInfo : NULL, pNeedReboot);
    }

    DifxFree(InfPath);
    DifxFreeInstallerInfo(&InstallerInfo);

    DifxLog(DIFXAPI_INFO, L"RETURN: DriverPackageUninstallA  (0x%X)", Error);
    return Error;
}

DWORD
WINAPI
DriverPackageGetPathW(
    _In_ PCWSTR DriverPackageInfPath,
    _Out_writes_opt_(*pNumOfChars) PWSTR pDestInfPath,
    _Inout_ DWORD *pNumOfChars)
{
    WCHAR StoreInf[MAX_PATH];
    DWORD Error, Length;

    DifxLog(DIFXAPI_INFO, L"ENTER:  DriverPackageGetPathW");

    if (!pNumOfChars || !DriverPackageInfPath)
    {
        DifxLog(DIFXAPI_ERROR, L"Parameter is NULL.");
        return ERROR_INVALID_PARAMETER;
    }

    Error = DifxGetPath(DriverPackageInfPath, StoreInf);
    if (Error == ERROR_SUCCESS)
    {
        /* The count covers the terminator when asking for room, not when reporting a copy */
        Length = (DWORD)wcslen(StoreInf);
        if (!pDestInfPath || *pNumOfChars < Length + 1)
        {
            *pNumOfChars = Length + 1;
            Error = ERROR_INSUFFICIENT_BUFFER;
        }
        else if (FAILED(StringCchCopyW(pDestInfPath, *pNumOfChars, StoreInf)))
        {
            *pNumOfChars = 0;
            Error = ERROR_CANNOT_COPY;
        }
        else
        {
            *pNumOfChars = Length;
        }
    }

    DifxLog(DIFXAPI_INFO, L"RETURN: DriverPackageGetPathW  (0x%X)", Error);
    return Error;
}

DWORD
WINAPI
DriverPackageGetPathA(
    _In_ PCSTR DriverPackageInfPath,
    _Out_writes_opt_(*pNumOfChars) PSTR pDestInfPath,
    _Inout_ DWORD *pNumOfChars)
{
    WCHAR StoreInf[MAX_PATH];
    PWSTR InfPath;
    PSTR AnsiPath;
    DWORD Error;
    size_t Length;

    DifxLog(DIFXAPI_INFO, L"ENTER:  DriverPackageGetPathA");

    if (!pNumOfChars || !DriverPackageInfPath)
    {
        DifxLog(DIFXAPI_ERROR, L"Parameter is NULL.");
        return ERROR_INVALID_PARAMETER;
    }

    InfPath = DifxAnsiToWide(DriverPackageInfPath);
    if (!InfPath)
        return ERROR_NOT_ENOUGH_MEMORY;

    Error = DifxGetPath(InfPath, StoreInf);
    DifxFree(InfPath);

    if (Error == ERROR_SUCCESS)
    {
        AnsiPath = DifxWideToAnsi(StoreInf);
        if (!AnsiPath)
        {
            *pNumOfChars = 0;
            Error = ERROR_NOT_ENOUGH_MEMORY;
        }
        else
        {
            if (FAILED(StringCchLengthA(AnsiPath, STRSAFE_MAX_CCH, &Length)))
            {
                *pNumOfChars = 0;
                Error = ERROR_CANNOT_COPY;
            }
            else if (!pDestInfPath || *pNumOfChars < Length + 1)
            {
                *pNumOfChars = (DWORD)Length + 1;
                Error = ERROR_INSUFFICIENT_BUFFER;
            }
            else if (FAILED(StringCchCopyA(pDestInfPath, *pNumOfChars, AnsiPath)))
            {
                *pNumOfChars = 0;
                Error = ERROR_CANNOT_COPY;
            }
            else
            {
                *pNumOfChars = (DWORD)Length;
            }

            DifxFree(AnsiPath);
        }
    }

    DifxLog(DIFXAPI_INFO, L"RETURN: DriverPackageGetPathA  (0x%X)", Error);
    return Error;
}
