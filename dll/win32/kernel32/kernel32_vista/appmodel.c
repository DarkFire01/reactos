/*
 * PROJECT:     ReactOS Kernel32
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Application model (packaged app) queries
 * COPYRIGHT:   Copyright 2026 ReactOS Contributors
 */

/*
 * Nothing here runs inside an application package, and there is no package
 * repository to look one up in. That is not a gap to paper over: "this
 * process has no package identity" is a real answer with a documented error
 * to carry it, APPMODEL_ERROR_NO_PACKAGE, and it is what an unpackaged
 * process gets on Windows too.
 *
 * The reason to answer at all rather than leave the entry points out is that
 * a caller asks these to decide which of two paths to take. Absent, the name
 * fails to resolve and the caller either cannot load or dies on a delay-load
 * failure; answering lets it take the desktop path, which is the correct one
 * here.
 */

#include "k32_vista.h"

#define NDEBUG
#include <debug.h>

#ifndef APPMODEL_ERROR_NO_PACKAGE
#define APPMODEL_ERROR_NO_PACKAGE 15700L
#endif

/*
 * @implemented
 */
LONG
WINAPI
GetCurrentPackageFullName(
    _Inout_ UINT32 *packageFullNameLength,
    _Out_writes_opt_(*packageFullNameLength) PWSTR packageFullName)
{
    if (packageFullNameLength == NULL)
        return ERROR_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(packageFullName);

    return APPMODEL_ERROR_NO_PACKAGE;
}

/*
 * @implemented
 */
LONG
WINAPI
GetCurrentPackageFamilyName(
    _Inout_ UINT32 *packageFamilyNameLength,
    _Out_writes_opt_(*packageFamilyNameLength) PWSTR packageFamilyName)
{
    if (packageFamilyNameLength == NULL)
        return ERROR_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(packageFamilyName);

    return APPMODEL_ERROR_NO_PACKAGE;
}

/*
 * @implemented
 */
LONG
WINAPI
GetPackageFullName(
    _In_ HANDLE hProcess,
    _Inout_ UINT32 *packageFullNameLength,
    _Out_writes_opt_(*packageFullNameLength) PWSTR packageFullName)
{
    UNREFERENCED_PARAMETER(hProcess);

    if (packageFullNameLength == NULL)
        return ERROR_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(packageFullName);

    return APPMODEL_ERROR_NO_PACKAGE;
}

/*
 * @implemented
 */
LONG
WINAPI
GetPackageFamilyName(
    _In_ HANDLE hProcess,
    _Inout_ UINT32 *packageFamilyNameLength,
    _Out_writes_opt_(*packageFamilyNameLength) PWSTR packageFamilyName)
{
    UNREFERENCED_PARAMETER(hProcess);

    if (packageFamilyNameLength == NULL)
        return ERROR_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(packageFamilyName);

    return APPMODEL_ERROR_NO_PACKAGE;
}

/*
 * @implemented
 */
LONG
WINAPI
GetPackagePathByFullName(
    _In_ PCWSTR packageFullName,
    _Inout_ UINT32 *pathLength,
    _Out_writes_opt_(*pathLength) PWSTR path)
{
    if (packageFullName == NULL || pathLength == NULL)
        return ERROR_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(path);

    /* There is no repository, so no name names a package that is installed */
    return ERROR_NOT_FOUND;
}

/*
 * @implemented
 */
LONG
WINAPI
GetPackagesByPackageFamily(
    _In_ PCWSTR packageFamilyName,
    _Inout_ UINT32 *count,
    _Out_writes_opt_(*count) PWSTR *packageFullNames,
    _Inout_ UINT32 *bufferLength,
    _Out_writes_opt_(*bufferLength) WCHAR *buffer)
{
    if (packageFamilyName == NULL || count == NULL || bufferLength == NULL)
        return ERROR_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(packageFullNames);
    UNREFERENCED_PARAMETER(buffer);

    /* No package belongs to any family here, which is an empty answer and
       not an error - a caller enumerates and finds nothing */
    *count = 0;
    *bufferLength = 0;

    return ERROR_SUCCESS;
}

/*
 * @implemented
 */
LONG
WINAPI
GetCurrentApplicationUserModelId(
    _Inout_ UINT32 *applicationUserModelIdLength,
    _Out_writes_opt_(*applicationUserModelIdLength) PWSTR applicationUserModelId)
{
    if (applicationUserModelIdLength == NULL)
        return ERROR_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(applicationUserModelId);

    /* Nothing here runs as a packaged application, so there is no AUMID to
       report. This is the answer Windows gives an unpackaged process, and
       callers test for it - it is not a failure to paper over. */
    return APPMODEL_ERROR_NO_APPLICATION;
}
