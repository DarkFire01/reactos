/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     API definitions for api-ms-win-core-processthreads-l1
 * COPYRIGHT:   Copyright 2024 Timo Kreuzer (timo.kreuzer@reactos.org)
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _STARTUPINFOA
{
    DWORD cb;
    LPSTR lpReserved;
    LPSTR lpDesktop;
    LPSTR lpTitle;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwXCountChars;
    DWORD dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    PBYTE lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;

typedef struct _STARTUPINFOW
{
    DWORD cb;
    LPWSTR lpReserved;
    LPWSTR lpDesktop;
    LPWSTR lpTitle;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwXCountChars;
    DWORD dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    PBYTE lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOW,*LPSTARTUPINFOW;

#ifdef UNICODE
typedef STARTUPINFOW STARTUPINFO, *LPSTARTUPINFO;
#else
typedef STARTUPINFOA STARTUPINFO, *LPSTARTUPINFO;
#endif // UNICODE

typedef struct _PROCESS_INFORMATION
{
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
} PROCESS_INFORMATION,*PPROCESS_INFORMATION,*LPPROCESS_INFORMATION;

typedef struct _PROC_THREAD_ATTRIBUTE_LIST *PPROC_THREAD_ATTRIBUTE_LIST, *LPPROC_THREAD_ATTRIBUTE_LIST;

WINBASEAPI
HRESULT
WINAPI
GetThreadDescription(
    _In_ HANDLE hThread,
    _Outptr_result_z_ PWSTR* ppszThreadDescription);

WINBASEAPI
HRESULT
WINAPI
SetThreadDescription(
    _In_ HANDLE hThread,
    _In_ PCWSTR lpThreadDescription);

WINBASEAPI
BOOL
WINAPI
SetThreadStackGuarantee(
    _Inout_ PULONG StackSizeInBytes);

#if (_WIN32_WINNT >= 0x0600)

WINBASEAPI
VOID
WINAPI
FlushProcessWriteBuffers(
    VOID);

WINBASEAPI
_Success_(return != FALSE)
BOOL
WINAPI
InitializeProcThreadAttributeList(
    _Out_writes_bytes_to_opt_(*lpSize,*lpSize) LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList,
    _In_ DWORD dwAttributeCount,
    _Reserved_ DWORD dwFlags,
    _When_(lpAttributeList == nullptr,_Out_) _When_(lpAttributeList != nullptr,_Inout_) PSIZE_T lpSize);

WINBASEAPI
BOOL
WINAPI
UpdateProcThreadAttribute(
    _Inout_ LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList,
    _In_ DWORD dwFlags,
    _In_ DWORD_PTR Attribute,
    _In_reads_bytes_opt_(cbSize) PVOID lpValue,
    _In_ SIZE_T cbSize,
    _Out_writes_bytes_opt_(cbSize) PVOID lpPreviousValue,
    _In_opt_ PSIZE_T lpReturnSize);

WINBASEAPI
VOID
WINAPI
DeleteProcThreadAttributeList(
    _Inout_ LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList);

#endif // (_WIN32_WINNT >= 0x0600) || defined(__REACTOS__)

#if (_WIN32_WINNT >= 0x602) || defined(__REACTOS__)

FORCEINLINE
HANDLE
GetCurrentProcessToken(
    VOID)
{
    return (HANDLE)(LONG_PTR)-4;
}

FORCEINLINE
HANDLE
GetCurrentThreadToken(
    VOID)
{
    return (HANDLE)(LONG_PTR)-5;
}

FORCEINLINE
HANDLE
GetCurrentThreadEffectiveToken(
    VOID)
{
    return (HANDLE)(LONG_PTR)-6;
}

#endif // (_WIN32_WINNT >= 0x602) || defined(__REACTOS__)

typedef enum _PROCESS_INFORMATION_CLASS
{
    ProcessMemoryPriority,
    ProcessMemoryExhaustionInfo,
    ProcessAppMemoryInfo,
    ProcessInPrivateInfo,
    ProcessPowerThrottling,
    ProcessReservedValue1,  // Formerly ProcessActivityThrottlePolicyInfo
    ProcessTelemetryCoverageInfo,
    ProcessProtectionLevelInfo,
    ProcessLeapSecondInfo,
    ProcessMachineTypeInfo,
    ProcessOverrideSubsequentPrefetchParameter,
    ProcessMaxOverridePrefetchParameter,
    ProcessInformationClassMax
} PROCESS_INFORMATION_CLASS;

#define PROCESS_MACHINE_ATTRIBUTE_KERNEL_ENABLED 0x00000001
#define PROCESS_MACHINE_ATTRIBUTE_USER_ENABLED   0x00000002
#define PROCESS_MACHINE_ATTRIBUTE_NATIVE_OS        0x00000004
#define PROCESS_MACHINE_ATTRIBUTE_WOW64_CONTAINER  0x00000008

typedef enum _PROCESS_MITIGATION_POLICY
{
    ProcessDEPPolicy,
    ProcessASLRPolicy,
    ProcessDynamicCodePolicy,
    ProcessStrictHandleCheckPolicy,
    ProcessSystemCallDisablePolicy,
    ProcessMitigationOptionsMask,
    ProcessExtensionPointDisablePolicy,
    ProcessControlFlowGuardPolicy,
    ProcessSignaturePolicy,
    ProcessFontDisablePolicy,
    ProcessImageLoadPolicy,
    ProcessSystemCallFilterPolicy,
    ProcessPayloadRestrictionPolicy,
    ProcessChildProcessPolicy,
    ProcessSideChannelIsolationPolicy,
    ProcessUserShadowStackPolicy,
    ProcessRedirectionTrustPolicy,
    ProcessUserPointerAuthPolicy,
    ProcessSEHOPPolicy,
    MaxProcessMitigationPolicy
} PROCESS_MITIGATION_POLICY, *PPROCESS_MITIGATION_POLICY;

typedef struct _PROCESS_MITIGATION_ASLR_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD EnableBottomUpRandomization : 1;
            DWORD EnableForceRelocateImages : 1;
            DWORD EnableHighEntropy : 1;
            DWORD DisallowStrippedImages : 1;
            DWORD ReservedFlags : 28;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_ASLR_POLICY, *PPROCESS_MITIGATION_ASLR_POLICY;

typedef struct _PROCESS_MITIGATION_DEP_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD Enable : 1;
            DWORD DisableAtlThunkEmulation : 1;
            DWORD ReservedFlags : 30;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
    BOOLEAN Permanent;
} PROCESS_MITIGATION_DEP_POLICY, *PPROCESS_MITIGATION_DEP_POLICY;

typedef struct _PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD RaiseExceptionOnInvalidHandleReference : 1;
            DWORD HandleExceptionsPermanentlyEnabled : 1;
            DWORD ReservedFlags : 30;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY, *PPROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY;

typedef struct _PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD DisallowWin32kSystemCalls : 1;
            DWORD AuditDisallowWin32kSystemCalls : 1;
            DWORD ReservedFlags : 30;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY, *PPROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY;

typedef struct _PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD DisableExtensionPoints : 1;
            DWORD ReservedFlags : 31;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY, *PPROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY;

typedef struct _PROCESS_MITIGATION_DYNAMIC_CODE_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD ProhibitDynamicCode : 1;
            DWORD AllowThreadOptOut : 1;
            DWORD AllowRemoteDowngrade : 1;
            DWORD AuditProhibitDynamicCode : 1;
            DWORD ReservedFlags : 28;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_DYNAMIC_CODE_POLICY, *PPROCESS_MITIGATION_DYNAMIC_CODE_POLICY;

typedef struct _PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD EnableControlFlowGuard : 1;
            DWORD EnableExportSuppression : 1;
            DWORD StrictMode : 1;
            DWORD ReservedFlags : 29;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY, *PPROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY;

typedef struct _PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD MicrosoftSignedOnly : 1;
            DWORD StoreSignedOnly : 1;
            DWORD MitigationOptIn : 1;
            DWORD AuditMicrosoftSignedOnly : 1;
            DWORD AuditStoreSignedOnly : 1;
            DWORD ReservedFlags : 27;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY, *PPROCESS_MITIGATION_BINARY_SIGNATURE_POLICY;

typedef struct _PROCESS_MITIGATION_FONT_DISABLE_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD DisableNonSystemFonts : 1;
            DWORD AuditNonSystemFontLoading : 1;
            DWORD ReservedFlags : 30;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_FONT_DISABLE_POLICY, *PPROCESS_MITIGATION_FONT_DISABLE_POLICY;

typedef struct _PROCESS_MITIGATION_IMAGE_LOAD_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD NoRemoteImages : 1;
            DWORD NoLowMandatoryLabelImages : 1;
            DWORD PreferSystem32Images : 1;
            DWORD AuditNoRemoteImages : 1;
            DWORD AuditNoLowMandatoryLabelImages : 1;
            DWORD ReservedFlags : 27;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_IMAGE_LOAD_POLICY, *PPROCESS_MITIGATION_IMAGE_LOAD_POLICY;

typedef struct _PROCESS_MITIGATION_SYSTEM_CALL_FILTER_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD FilterId : 4;
            DWORD ReservedFlags : 28;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_SYSTEM_CALL_FILTER_POLICY, *PPROCESS_MITIGATION_SYSTEM_CALL_FILTER_POLICY;

typedef struct _PROCESS_MITIGATION_PAYLOAD_RESTRICTION_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD EnableExportAddressFilter : 1;
            DWORD AuditExportAddressFilter : 1;
            DWORD EnableExportAddressFilterPlus : 1;
            DWORD AuditExportAddressFilterPlus : 1;
            DWORD EnableImportAddressFilter : 1;
            DWORD AuditImportAddressFilter : 1;
            DWORD EnableRopStackPivot : 1;
            DWORD AuditRopStackPivot : 1;
            DWORD EnableRopCallerCheck : 1;
            DWORD AuditRopCallerCheck : 1;
            DWORD EnableRopSimExec : 1;
            DWORD AuditRopSimExec : 1;
            DWORD ReservedFlags : 20;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_PAYLOAD_RESTRICTION_POLICY, *PPROCESS_MITIGATION_PAYLOAD_RESTRICTION_POLICY;

typedef struct _PROCESS_MITIGATION_CHILD_PROCESS_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD NoChildProcessCreation : 1;
            DWORD AuditNoChildProcessCreation : 1;
            DWORD AllowSecureProcessCreation : 1;
            DWORD ReservedFlags : 29;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_CHILD_PROCESS_POLICY, *PPROCESS_MITIGATION_CHILD_PROCESS_POLICY;

typedef struct _PROCESS_MITIGATION_SIDE_CHANNEL_ISOLATION_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD SmtBranchTargetIsolation : 1;
            DWORD IsolateSecurityDomain : 1;
            DWORD DisablePageCombine : 1;
            DWORD SpeculativeStoreBypassDisable : 1;
            DWORD RestrictCoreSharing : 1;
            DWORD ReservedFlags : 27;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_SIDE_CHANNEL_ISOLATION_POLICY, *PPROCESS_MITIGATION_SIDE_CHANNEL_ISOLATION_POLICY;

typedef struct _PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD EnableUserShadowStack : 1;
            DWORD AuditUserShadowStack : 1;
            DWORD SetContextIpValidation : 1;
            DWORD AuditSetContextIpValidation : 1;
            DWORD EnableUserShadowStackStrictMode : 1;
            DWORD BlockNonCetBinaries : 1;
            DWORD BlockNonCetBinariesNonEhcont : 1;
            DWORD AuditBlockNonCetBinaries : 1;
            DWORD CetDynamicApisOutOfProcOnly : 1;
            DWORD SetContextIpValidationRelaxedMode : 1;
            DWORD ReservedFlags : 22;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY, *PPROCESS_MITIGATION_USER_SHADOW_STACK_POLICY;

typedef struct _PROCESS_MITIGATION_REDIRECTION_TRUST_POLICY
{
    union
    {
        DWORD Flags;
        struct
        {
            DWORD EnforceRedirectionTrust : 1;
            DWORD AuditRedirectionTrust : 1;
            DWORD ReservedFlags : 30;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME;
} PROCESS_MITIGATION_REDIRECTION_TRUST_POLICY, *PPROCESS_MITIGATION_REDIRECTION_TRUST_POLICY;

#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4

typedef struct _PROCESS_POWER_THROTTLING_STATE
{
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} PROCESS_POWER_THROTTLING_STATE, *PPROCESS_POWER_THROTTLING_STATE;

#define MEMORY_PRIORITY_LOWEST       0
#define MEMORY_PRIORITY_VERY_LOW     1
#define MEMORY_PRIORITY_LOW          2
#define MEMORY_PRIORITY_MEDIUM       3
#define MEMORY_PRIORITY_BELOW_NORMAL 4
#define MEMORY_PRIORITY_NORMAL       5

typedef struct _MEMORY_PRIORITY_INFORMATION
{
    ULONG MemoryPriority;
} MEMORY_PRIORITY_INFORMATION, *PMEMORY_PRIORITY_INFORMATION;

typedef struct _APP_MEMORY_INFORMATION
{
    ULONG64 AvailableCommit;
    ULONG64 PrivateCommitUsage;
    ULONG64 PeakPrivateCommitUsage;
    ULONG64 TotalCommitUsage;
} APP_MEMORY_INFORMATION, *PAPP_MEMORY_INFORMATION;

typedef enum _THREAD_INFORMATION_CLASS
{
    ThreadMemoryPriority,
    ThreadAbsoluteCpuPriority,
    ThreadDynamicCodePolicy,
    ThreadPowerThrottling,
    ThreadInformationClassMax
} THREAD_INFORMATION_CLASS;

#define THREAD_POWER_THROTTLING_CURRENT_VERSION 1
#define THREAD_POWER_THROTTLING_EXECUTION_SPEED 0x1
#define THREAD_POWER_THROTTLING_VALID_FLAGS     THREAD_POWER_THROTTLING_EXECUTION_SPEED

typedef struct _THREAD_POWER_THROTTLING_STATE
{
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} THREAD_POWER_THROTTLING_STATE;

BOOL
WINAPI
GetThreadInformation(
  _In_ HANDLE hThread,
  _In_ THREAD_INFORMATION_CLASS ThreadInformationClass,
  _Out_writes_bytes_(ThreadInformationSize) LPVOID ThreadInformation,
  _In_ DWORD ThreadInformationSize);

BOOL
WINAPI
SetThreadInformation(
  _In_ HANDLE hThread,
  _In_ THREAD_INFORMATION_CLASS ThreadInformationClass,
  _In_reads_bytes_(ThreadInformationSize) LPVOID ThreadInformation,
  _In_ DWORD ThreadInformationSize);

BOOL
WINAPI
GetProcessInformation(
  _In_ HANDLE hProcess,
  _In_ PROCESS_INFORMATION_CLASS ProcessInformationClass,
  _Out_writes_bytes_(ProcessInformationSize) LPVOID ProcessInformation,
  _In_ DWORD ProcessInformationSize);

BOOL
WINAPI
SetProcessInformation(
  _In_ HANDLE hProcess,
  _In_ PROCESS_INFORMATION_CLASS ProcessInformationClass,
  _In_reads_bytes_(ProcessInformationSize) LPVOID ProcessInformation,
  _In_ DWORD ProcessInformationSize);

BOOL
WINAPI
GetProcessMitigationPolicy(
  _In_ HANDLE hProcess,
  _In_ PROCESS_MITIGATION_POLICY MitigationPolicy,
  _Out_writes_bytes_(dwLength) PVOID lpBuffer,
  _In_ SIZE_T dwLength);

BOOL
WINAPI
SetProcessMitigationPolicy(
  _In_ PROCESS_MITIGATION_POLICY MitigationPolicy,
  _In_reads_bytes_(dwLength) PVOID lpBuffer,
  _In_ SIZE_T dwLength);

#ifdef __cplusplus
} // extern "C"
#endif
