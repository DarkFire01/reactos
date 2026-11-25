/*
 * PROJECT:     ReactOS Driver Install Frameworks API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#ifndef _DIFXAPI_PRECOMP_H_
#define _DIFXAPI_PRECOMP_H_

#include <stdarg.h>

#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <winuser.h>
#include <winnls.h>
#include <regstr.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <strsafe.h>
#include <difxapi.h>

#include <wine/debug.h>

/* Package type a [Version] DriverPackageType entry names, plug and play when absent */
#define DIFX_TYPE_KERNEL_SERVICE            1
#define DIFX_TYPE_FILE_SYSTEM               2
#define DIFX_TYPE_FILE_SYSTEM_FILTER        3
#define DIFX_TYPE_FILE_SYSTEM_MINIFILTER    4
#define DIFX_TYPE_CLASS_FILTER              5
#define DIFX_TYPE_PLUG_AND_PLAY             6
#define DIFX_TYPE_NETWORK                   7
#define DIFX_TYPE_NDIS_IM_MINIPORT          8
#define DIFX_TYPE_DEVICE_FILTER             9
#define DIFX_TYPE_KERNEL_MODULE             10
#define DIFX_TYPE_CLASS_COINSTALLER         11
#define DIFX_TYPE_UNKNOWN                   12

/* Longest INF path the entry points accept */
#define DIFX_MAX_INF_PATH                   0x7FFF

/* log.c */

VOID
__cdecl
DifxLog(
    _In_ DIFXAPI_LOG Event,
    _In_ _Printf_format_string_ PCWSTR Format,
    ...);

VOID
__cdecl
DifxLogError(
    _In_ DIFXAPI_LOG Event,
    _In_ DWORD Error,
    _In_ _Printf_format_string_ PCWSTR Format,
    ...);

_Ret_maybenull_
PWSTR
WINAPI
DifxAnsiToWide(
    _In_ PCSTR String);

_Ret_maybenull_
PSTR
WINAPI
DifxWideToAnsi(
    _In_ PCWSTR String);

VOID
WINAPI
DifxFree(
    _In_opt_ PVOID Buffer);

/* store.c */

SIZE_T
WINAPI
DifxListLength(
    _In_opt_ PCWSTR List);

BOOL
WINAPI
DifxListContains(
    _In_opt_ PCWSTR List,
    _In_ PCWSTR String);

DWORD
WINAPI
DifxListAppend(
    _Inout_ PWSTR *List,
    _In_ PCWSTR String);

BOOL
WINAPI
DifxIsFromInfDirectory(
    _In_ PCWSTR InfPath);

BOOL
WINAPI
DifxSplitPath(
    _In_ PCWSTR Path,
    _Out_writes_(cchDirectory) PWSTR Directory,
    _In_ DWORD cchDirectory,
    _Outptr_ PCWSTR *FileName);

DWORD
WINAPI
DifxStoreFind(
    _In_ PCWSTR InfPath,
    _Out_writes_(MAX_PATH) PWSTR StoreInf);

DWORD
WINAPI
DifxStoreAdd(
    _In_ PCWSTR InfPath,
    _In_ BOOL Replace,
    _Out_writes_(MAX_PATH) PWSTR StoreInf);

DWORD
WINAPI
DifxStoreDelete(
    _In_ PCWSTR StoreInf);

DWORD
WINAPI
DifxCatalogAdd(
    _In_ PCWSTR StoreInf);

BOOL
WINAPI
DifxGetOemInf(
    _In_ PCWSTR StoreInf,
    _Out_writes_(MAX_PATH) PWSTR OemInf);

DWORD
WINAPI
DifxAddReference(
    _In_ PCWSTR StoreInf,
    _In_ PCWSTR ApplicationId,
    _In_ PCWSTR DisplayName,
    _Out_ PBOOL AlreadyReferenced);

DWORD
WINAPI
DifxRemoveReference(
    _In_ PCWSTR StoreInf,
    _In_ PCWSTR ApplicationId,
    _In_ PCWSTR DisplayName,
    _Out_ PBOOL NotReferenced);

DWORD
WINAPI
DifxHasReferences(
    _In_ PCWSTR StoreInf,
    _Out_ PBOOL Referenced);

DWORD
WINAPI
DifxDeleteReferences(
    _In_ PCWSTR StoreInf);

/* pnp.c */

DWORD
WINAPI
DifxProbeDevices(
    _In_ PCWSTR InfPath);

DWORD
WINAPI
DifxInstallPnp(
    _In_ PCWSTR StoreInf,
    _In_ BOOL Force,
    _In_ BOOL Repair,
    _Out_ PBOOL NeedReboot);

DWORD
WINAPI
DifxUninstallPnp(
    _In_ PCWSTR StoreInf,
    _In_ BOOL DeleteFiles,
    _Out_ PBOOL NeedReboot);

#endif /* _DIFXAPI_PRECOMP_H_ */
