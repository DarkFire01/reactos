/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Windows AppModel definitions
 * COPYRIGHT:   Copyright 2024 Timo Kreuzer (timo.kreuzer@reactos.org)
 */

#pragma once

#include <minappmodel.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AppPolicyProcessTerminationMethod
{
    AppPolicyProcessTerminationMethod_ExitProcess = 0,
    AppPolicyProcessTerminationMethod_TerminateProcess = 1,
} AppPolicyProcessTerminationMethod;

typedef enum AppPolicyThreadInitializationType
{
    AppPolicyThreadInitializationType_None = 0,
    AppPolicyThreadInitializationType_InitializeWinRT = 1,
} AppPolicyThreadInitializationType;

typedef enum AppPolicyShowDeveloperDiagnostic
{
    AppPolicyShowDeveloperDiagnostic_None = 0,
    AppPolicyShowDeveloperDiagnostic_ShowUI = 1,
} AppPolicyShowDeveloperDiagnostic;

typedef enum AppPolicyWindowingModel
{
    AppPolicyWindowingModel_None = 0,
    AppPolicyWindowingModel_Universal = 1,
    AppPolicyWindowingModel_ClassicDesktop = 2,
    AppPolicyWindowingModel_ClassicPhone = 3
} AppPolicyWindowingModel;

typedef enum AppPolicyMediaFoundationCodecLoading
{
    AppPolicyMediaFoundationCodecLoading_All = 0,
} AppPolicyMediaFoundationCodecLoading;

typedef struct PACKAGE_VERSION
{
    union
    {
        UINT64 Version;
        struct
        {
            USHORT Revision;
            USHORT Build;
            USHORT Minor;
            USHORT Major;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PACKAGE_VERSION;

typedef struct PACKAGE_ID
{
    UINT32 reserved;
    UINT32 processorArchitecture;
    PACKAGE_VERSION version;
    PWSTR name;
    PWSTR publisher;
    PWSTR resourceId;
    PWSTR publisherId;
} PACKAGE_ID;

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
AppPolicyGetProcessTerminationMethod(
    _In_ HANDLE processToken,
    _Out_ AppPolicyProcessTerminationMethod* policy);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
AppPolicyGetThreadInitializationType(
    _In_ HANDLE processToken,
    _Out_ AppPolicyThreadInitializationType* policy);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
AppPolicyGetShowDeveloperDiagnostic(
    _In_ HANDLE processToken,
    _Out_ AppPolicyShowDeveloperDiagnostic* policy);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
AppPolicyGetWindowingModel(
    _In_ HANDLE processToken,
    _Out_ AppPolicyWindowingModel* policy);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
AppPolicyGetMediaFoundationCodecLoading(
    _In_ HANDLE processToken,
    _Out_ AppPolicyMediaFoundationCodecLoading* policy);

#define PACKAGE_PROPERTY_FRAMEWORK          0x00000001
#define PACKAGE_PROPERTY_RESOURCE           0x00000002
#define PACKAGE_PROPERTY_BUNDLE             0x00000004
#define PACKAGE_PROPERTY_OPTIONAL           0x00000008
#define PACKAGE_FILTER_HEAD     0x00000010
#define PACKAGE_FILTER_DIRECT   0x00000020
#define PACKAGE_FILTER_RESOURCE             0x00000040
#define PACKAGE_FILTER_BUNDLE               0x00000080
#define PACKAGE_INFORMATION_BASIC           0x00000000
#define PACKAGE_INFORMATION_FULL            0x00000100
#define PACKAGE_PROPERTY_DEVELOPMENT_MODE   0x00010000
#define PACKAGE_FILTER_OPTIONAL             0x00020000
#define PACKAGE_PROPERTY_IS_IN_RELATED_SET  0x00040000
#define PACKAGE_FILTER_IS_IN_RELATED_SET    0x00040000
#define PACKAGE_PROPERTY_STATIC             0x00080000
#define PACKAGE_FILTER_STATIC               0x00080000
#define PACKAGE_PROPERTY_DYNAMIC            0x00100000
#define PACKAGE_FILTER_DYNAMIC              0x00100000
#define PACKAGE_PROPERTY_HOSTRUNTIME        0x00200000
#define PACKAGE_FILTER_HOSTRUNTIME          0x00200000

typedef struct _PACKAGE_INFO_REFERENCE
{
    void *reserved;
} *PACKAGE_INFO_REFERENCE;

typedef enum PackageOrigin
{
    PackageOrigin_Unknown = 0,
    PackageOrigin_Unsigned = 1,
    PackageOrigin_Inbox = 2,
    PackageOrigin_Store = 3,
    PackageOrigin_DeveloperUnsigned = 4,
    PackageOrigin_DeveloperSigned = 5,
    PackageOrigin_LineOfBusiness = 6,
} PackageOrigin;

typedef struct PACKAGE_INFO
{
    UINT32 reserved;
    UINT32 flags;
    PWSTR path;
    PWSTR packageFullName;
    PWSTR packageFamilyName;
    PACKAGE_ID packageId;
} PACKAGE_INFO;

typedef enum PackagePathType
{
    PackagePathType_Install = 0,
    PackagePathType_Mutable = 1,
    PackagePathType_Effective = 2,
    PackagePathType_MachineExternal = 3,
    PackagePathType_UserExternal = 4,
    PackagePathType_EffectiveExternal = 5,
} PackagePathType;

#if (_WIN32_WINNT >= 0x0602)

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetCurrentPackageId(
    _Inout_ UINT32 *bufferLength,
    _Out_writes_bytes_opt_(*bufferLength) BYTE *buffer);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetCurrentPackageFullName(
    _Inout_ UINT32 *packageFullNameLength,
    _Out_writes_opt_(*packageFullNameLength) PWSTR packageFullName);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetCurrentPackageFamilyName(
    _Inout_ UINT32 *packageFamilyNameLength,
    _Out_writes_opt_(*packageFamilyNameLength) PWSTR packageFamilyName);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetCurrentPackagePath(
    _Inout_ UINT32 *pathLength,
    _Out_writes_opt_(*pathLength) PWSTR path);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetPackageId(
    _In_ HANDLE hProcess,
    _Inout_ UINT32 *bufferLength,
    _Out_writes_bytes_opt_(*bufferLength) BYTE *buffer);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetPackageFullName(
    _In_ HANDLE hProcess,
    _Inout_ UINT32 *packageFullNameLength,
    _Out_writes_opt_(*packageFullNameLength) PWSTR packageFullName);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetPackageFullNameFromToken(
    _In_ HANDLE token,
    _Inout_ UINT32 *packageFullNameLength,
    _Out_writes_opt_(*packageFullNameLength) PWSTR packageFullName);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetPackageFamilyName(
    _In_ HANDLE hProcess,
    _Inout_ UINT32 *packageFamilyNameLength,
    _Out_writes_opt_(*packageFamilyNameLength) PWSTR packageFamilyName);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetPackageFamilyNameFromToken(
    _In_ HANDLE token,
    _Inout_ UINT32 *packageFamilyNameLength,
    _Out_writes_opt_(*packageFamilyNameLength) PWSTR packageFamilyName);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetPackagePath(
    _In_ const PACKAGE_ID *packageId,
    _Reserved_ const UINT32 reserved,
    _Inout_ UINT32 *pathLength,
    _Out_writes_opt_(*pathLength) PWSTR path);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetPackagePathByFullName(
    _In_ PCWSTR packageFullName,
    _Inout_ UINT32 *pathLength,
    _Out_writes_opt_(*pathLength) PWSTR path);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetStagedPackagePathByFullName(
    _In_ PCWSTR packageFullName,
    _Inout_ UINT32 *pathLength,
    _Out_writes_opt_(*pathLength) PWSTR path);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetCurrentApplicationUserModelId(
    _Inout_ UINT32 *applicationUserModelIdLength,
    _Out_writes_opt_(*applicationUserModelIdLength) PWSTR applicationUserModelId);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetApplicationUserModelIdFromToken(
    _In_ HANDLE token,
    _Inout_ UINT32 *applicationUserModelIdLength,
    _Out_writes_opt_(*applicationUserModelIdLength) PWSTR applicationUserModelId);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
VerifyPackageFullName(
    _In_ PCWSTR packageFullName);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
VerifyPackageFamilyName(
    _In_ PCWSTR packageFamilyName);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
VerifyPackageId(
    _In_ const PACKAGE_ID *packageId);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
VerifyApplicationUserModelId(
    _In_ PCWSTR applicationUserModelId);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
VerifyPackageRelativeApplicationId(
    _In_ PCWSTR packageRelativeApplicationId);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
PackageIdFromFullName(
    _In_ PCWSTR packageFullName,
    _In_ const UINT32 flags,
    _Inout_ UINT32 *bufferLength,
    _Out_writes_bytes_opt_(*bufferLength) BYTE *buffer);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
PackageFullNameFromId(
    _In_ const PACKAGE_ID *packageId,
    _Inout_ UINT32 *packageFullNameLength,
    _Out_writes_opt_(*packageFullNameLength) PWSTR packageFullName);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
PackageFamilyNameFromId(
    _In_ const PACKAGE_ID *packageId,
    _Inout_ UINT32 *packageFamilyNameLength,
    _Out_writes_opt_(*packageFamilyNameLength) PWSTR packageFamilyName);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
PackageFamilyNameFromFullName(
    _In_ PCWSTR packageFullName,
    _Inout_ UINT32 *packageFamilyNameLength,
    _Out_writes_opt_(*packageFamilyNameLength) PWSTR packageFamilyName);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
PackageNameAndPublisherIdFromFamilyName(
    _In_ PCWSTR packageFamilyName,
    _Inout_ UINT32 *packageNameLength,
    _Out_writes_opt_(*packageNameLength) PWSTR packageName,
    _Inout_ UINT32 *packagePublisherIdLength,
    _Out_writes_opt_(*packagePublisherIdLength) PWSTR packagePublisherId);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
FormatApplicationUserModelId(
    _In_ PCWSTR packageFamilyName,
    _In_ PCWSTR packageRelativeApplicationId,
    _Inout_ UINT32 *applicationUserModelIdLength,
    _Out_writes_opt_(*applicationUserModelIdLength) PWSTR applicationUserModelId);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
ParseApplicationUserModelId(
    _In_ PCWSTR applicationUserModelId,
    _Inout_ UINT32 *packageFamilyNameLength,
    _Out_writes_opt_(*packageFamilyNameLength) PWSTR packageFamilyName,
    _Inout_ UINT32 *packageRelativeApplicationIdLength,
    _Out_writes_opt_(*packageRelativeApplicationIdLength) PWSTR packageRelativeApplicationId);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetPackagesByPackageFamily(
    _In_ PCWSTR packageFamilyName,
    _Inout_ UINT32 *count,
    _Out_writes_opt_(*count) PWSTR *packageFullNames,
    _Inout_ UINT32 *bufferLength,
    _Out_writes_opt_(*bufferLength) WCHAR *buffer);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
OpenPackageInfoByFullName(
    _In_ PCWSTR packageFullName,
    _Reserved_ const UINT32 reserved,
    _Out_ PACKAGE_INFO_REFERENCE *packageInfoReference);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
OpenPackageInfoByFullNameForUser(
    _In_opt_ PSID userSid,
    _In_ PCWSTR packageFullName,
    _Reserved_ const UINT32 reserved,
    _Out_ PACKAGE_INFO_REFERENCE *packageInfoReference);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
ClosePackageInfo(
    _In_ PACKAGE_INFO_REFERENCE packageInfoReference);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetPackageInfo(
    _In_ PACKAGE_INFO_REFERENCE packageInfoReference,
    _In_ const UINT32 flags,
    _Inout_ UINT32 *bufferLength,
    _Out_writes_bytes_opt_(*bufferLength) BYTE *buffer,
    _Out_opt_ UINT32 *count);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetPackageApplicationIds(
    _In_ PACKAGE_INFO_REFERENCE packageInfoReference,
    _Inout_ UINT32 *bufferLength,
    _Out_writes_bytes_opt_(*bufferLength) BYTE *buffer,
    _Out_opt_ UINT32 *count);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetCurrentPackageInfo(
    _In_ const UINT32 flags,
    _Inout_ UINT32 *bufferLength,
    _Out_writes_bytes_opt_(*bufferLength) BYTE *buffer,
    _Out_opt_ UINT32 *count);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetApplicationUserModelId(
    _In_ HANDLE hProcess,
    _Inout_ UINT32 *applicationUserModelIdLength,
    _Out_writes_opt_(*applicationUserModelIdLength) PWSTR applicationUserModelId);

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
FindPackagesByPackageFamily(
    _In_ PCWSTR packageFamilyName,
    _In_ UINT32 packageFilters,
    _Inout_ UINT32 *count,
    _Out_writes_opt_(*count) PWSTR *packageFullNames,
    _Inout_ UINT32 *bufferLength,
    _Out_writes_opt_(*bufferLength) WCHAR *buffer,
    _Out_writes_opt_(*count) UINT32 *packageProperties);

#endif /* _WIN32_WINNT >= 0x0602 */

#if (_WIN32_WINNT >= 0x0A00)

WINBASEAPI
_Check_return_
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetPackagePathByFullName2(
    _In_ PCWSTR packageFullName,
    _In_ PackagePathType packagePathType,
    _Inout_ UINT32 *pathLength,
    _Out_writes_opt_(*pathLength) PWSTR path);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetStagedPackagePathByFullName2(
    _In_ PCWSTR packageFullName,
    _In_ PackagePathType packagePathType,
    _Inout_ UINT32 *pathLength,
    _Out_writes_opt_(*pathLength) PWSTR path);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetCurrentPackageInfo2(
    _In_ const UINT32 flags,
    _In_ PackagePathType packagePathType,
    _Inout_ UINT32 *bufferLength,
    _Out_writes_bytes_opt_(*bufferLength) BYTE *buffer,
    _Out_opt_ UINT32 *count);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetCurrentPackagePath2(
    _In_ PackagePathType packagePathType,
    _Inout_ UINT32 *pathLength,
    _Out_writes_opt_(*pathLength) PWSTR path);

WINBASEAPI
_Success_(return == ERROR_SUCCESS)
LONG
WINAPI
GetPackageInfo2(
    _In_ PACKAGE_INFO_REFERENCE packageInfoReference,
    _In_ const UINT32 flags,
    _In_ PackagePathType packagePathType,
    _Inout_ UINT32 *bufferLength,
    _Out_writes_bytes_opt_(*bufferLength) BYTE *buffer,
    _Out_opt_ UINT32 *count);

#endif /* _WIN32_WINNT >= 0x0A00 */

#ifdef __cplusplus
} // extern "C"
#endif
