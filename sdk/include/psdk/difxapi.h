/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver Install Frameworks API (difxapi.dll)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#ifndef _INC_DIFXAPI_
#define _INC_DIFXAPI_

#if !defined(_DIFXAPI_)
#define WINDIFXAPI DECLSPEC_IMPORT
#else
#define WINDIFXAPI
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _DIFXAPI_VERSION_
#define _DIFXAPI_VERSION_ 0x0200
#endif

/* DIFx errors use the customer bit so they never collide with system codes */
#define ERROR_DEPENDENT_APPLICATIONS_EXIST  (APPLICATION_ERROR_MASK | ERROR_SEVERITY_ERROR | 0x300)
#define ERROR_NO_DEVICE_ID                  (APPLICATION_ERROR_MASK | ERROR_SEVERITY_ERROR | 0x301)
#define ERROR_DRIVER_PACKAGE_NOT_IN_STORE   (APPLICATION_ERROR_MASK | ERROR_SEVERITY_ERROR | 0x302)
#define ERROR_MISSING_FILE                  (APPLICATION_ERROR_MASK | ERROR_SEVERITY_ERROR | 0x303)
#define ERROR_INVALID_CATALOG_DATA          (APPLICATION_ERROR_MASK | ERROR_SEVERITY_ERROR | 0x304)

#ifndef ERROR_NO_SUCH_DEVINST
#define ERROR_NO_SUCH_DEVINST               0xE000020B
#endif

#ifndef ERROR_SIGNATURE_OSATTRIBUTE_MISMATCH
#define ERROR_SIGNATURE_OSATTRIBUTE_MISMATCH 0xE0000244
#endif

/* Preinstall replaces the driver store entry, install also reinstalls the devices using it */
#define DRIVER_PACKAGE_REPAIR                   0x00000001

/* Fail instead of showing any UI */
#define DRIVER_PACKAGE_SILENT                   0x00000002

/* Install even when the driver is not better, uninstall even when applications still reference it */
#define DRIVER_PACKAGE_FORCE                    0x00000004

/* Only preinstall or install when a matching device is present */
#define DRIVER_PACKAGE_ONLY_IF_DEVICE_PRESENT   0x00000008

/* Accept unsigned packages and packages with missing files */
#define DRIVER_PACKAGE_LEGACY_MODE              0x00000010

/* Uninstall makes a best effort to remove the files the package copied */
#define DRIVER_PACKAGE_DELETE_FILES             0x00000020

typedef struct
{
    PWSTR pApplicationId;
    PWSTR pDisplayName;
    PWSTR pProductName;
    PWSTR pMfgName;
} INSTALLERINFO_W, *PINSTALLERINFO_W;

typedef const PINSTALLERINFO_W PCINSTALLERINFO_W;

typedef struct
{
    PSTR pApplicationId;
    PSTR pDisplayName;
    PSTR pProductName;
    PSTR pMfgName;
} INSTALLERINFO_A, *PINSTALLERINFO_A;

typedef const PINSTALLERINFO_A PCINSTALLERINFO_A;

#ifdef UNICODE
typedef INSTALLERINFO_W INSTALLERINFO;
typedef PINSTALLERINFO_W PINSTALLERINFO;
typedef PCINSTALLERINFO_W PCINSTALLERINFO;
#else
typedef INSTALLERINFO_A INSTALLERINFO;
typedef PINSTALLERINFO_A PINSTALLERINFO;
typedef PCINSTALLERINFO_A PCINSTALLERINFO;
#endif

typedef enum
{
    DIFXAPI_SUCCESS = 0,
    DIFXAPI_INFO = 1,
    DIFXAPI_WARNING = 2,
    DIFXAPI_ERROR = 3
} DIFXAPI_LOG;

typedef VOID (WINAPI *DIFXLOGCALLBACK_W)(DIFXAPI_LOG Event, DWORD Error, PCWSTR EventDescription, PVOID CallbackContext);
typedef VOID (WINAPI *DIFXLOGCALLBACK_A)(DIFXAPI_LOG Event, DWORD Error, PCSTR EventDescription, PVOID CallbackContext);

typedef VOID (__cdecl *DIFXAPILOGCALLBACK_W)(DIFXAPI_LOG Event, DWORD Error, PCWSTR EventDescription, PVOID CallbackContext);
typedef VOID (__cdecl *DIFXAPILOGCALLBACK_A)(DIFXAPI_LOG Event, DWORD Error, PCSTR EventDescription, PVOID CallbackContext);

_Check_return_
WINDIFXAPI
DWORD
WINAPI
DriverPackagePreinstallW(
    _In_ PCWSTR DriverPackageInfPath,
    _In_ DWORD Flags);

_Check_return_
WINDIFXAPI
DWORD
WINAPI
DriverPackagePreinstallA(
    _In_ PCSTR DriverPackageInfPath,
    _In_ DWORD Flags);

_Check_return_
WINDIFXAPI
DWORD
WINAPI
DriverPackageInstallW(
    _In_ PCWSTR DriverPackageInfPath,
    _In_ DWORD Flags,
    _In_opt_ PCINSTALLERINFO_W pInstallerInfo,
    _Out_ BOOL *pNeedReboot);

_Check_return_
WINDIFXAPI
DWORD
WINAPI
DriverPackageInstallA(
    _In_ PCSTR DriverPackageInfPath,
    _In_ DWORD Flags,
    _In_opt_ PCINSTALLERINFO_A pInstallerInfo,
    _Out_ BOOL *pNeedReboot);

_Check_return_
WINDIFXAPI
DWORD
WINAPI
DriverPackageUninstallW(
    _In_ PCWSTR DriverPackageInfPath,
    _In_ DWORD Flags,
    _In_opt_ PCINSTALLERINFO_W pInstallerInfo,
    _Out_ BOOL *pNeedReboot);

_Check_return_
WINDIFXAPI
DWORD
WINAPI
DriverPackageUninstallA(
    _In_ PCSTR DriverPackageInfPath,
    _In_ DWORD Flags,
    _In_opt_ PCINSTALLERINFO_A pInstallerInfo,
    _Out_ BOOL *pNeedReboot);

_Check_return_
WINDIFXAPI
DWORD
WINAPI
DriverPackageGetPathW(
    _In_ PCWSTR DriverPackageInfPath,
    _Out_writes_opt_(*pNumOfChars) PWSTR pDestInfPath,
    _Inout_ DWORD *pNumOfChars);

_Check_return_
WINDIFXAPI
DWORD
WINAPI
DriverPackageGetPathA(
    _In_ PCSTR DriverPackageInfPath,
    _Out_writes_opt_(*pNumOfChars) PSTR pDestInfPath,
    _Inout_ DWORD *pNumOfChars);

WINDIFXAPI
VOID
WINAPI
SetDifxLogCallbackW(
    _In_ DIFXLOGCALLBACK_W LogCallback,
    _In_ PVOID CallbackContext);

WINDIFXAPI
VOID
WINAPI
SetDifxLogCallbackA(
    _In_ DIFXLOGCALLBACK_A LogCallback,
    _In_ PVOID CallbackContext);

WINDIFXAPI
VOID
WINAPI
DIFXAPISetLogCallbackW(
    _In_opt_ DIFXAPILOGCALLBACK_W LogCallback,
    _In_opt_ PVOID CallbackContext);

WINDIFXAPI
VOID
WINAPI
DIFXAPISetLogCallbackA(
    _In_opt_ DIFXAPILOGCALLBACK_A LogCallback,
    _In_opt_ PVOID CallbackContext);

#ifdef UNICODE
#define DriverPackagePreinstall DriverPackagePreinstallW
#define DriverPackageInstall DriverPackageInstallW
#define DriverPackageUninstall DriverPackageUninstallW
#define DriverPackageGetPath DriverPackageGetPathW
#define DIFLOGCALLBACK DIFXLOGCALLBACK_W
#define SetDifxLogCallback SetDifxLogCallbackW
#define DIFXAPILOGCALLBACK DIFXAPILOGCALLBACK_W
#define DIFXAPISetLogCallback DIFXAPISetLogCallbackW
#else
#define DriverPackagePreinstall DriverPackagePreinstallA
#define DriverPackageInstall DriverPackageInstallA
#define DriverPackageUninstall DriverPackageUninstallA
#define DriverPackageGetPath DriverPackageGetPathA
#define DIFLOGCALLBACK DIFXLOGCALLBACK_A
#define SetDifxLogCallback SetDifxLogCallbackA
#define DIFXAPILOGCALLBACK DIFXAPILOGCALLBACK_A
#define DIFXAPISetLogCallback DIFXAPISetLogCallbackA
#endif

#ifdef __cplusplus
}
#endif

#endif /* _INC_DIFXAPI_ */
