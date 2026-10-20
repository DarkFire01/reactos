/*
 * PROJECT:     ReactOS 'Layers' Shim library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Version lie implementation helper
 * COPYRIGHT:   Copyright 2016,2017 Mark Jansen <mark.jansen@reactos.org>
 */

#include <setup_shim.inl>

DWORD WINAPI SHIM_OBJ_NAME(APIHook_GetVersion)()
{
    return VERSION_INFO.FullVersion;
}

BOOL WINAPI SHIM_OBJ_NAME(APIHook_GetVersionExA)(LPOSVERSIONINFOEXA lpOsVersionInfo)
{
    if (CALL_SHIM(0, GETVERSIONEXAPROC)(lpOsVersionInfo))
    {
        return FakeVersion(lpOsVersionInfo, &VERSION_INFO);
    }
    return FALSE;
}

/* We do not care about the actual type, FakeVersion will correctly handle it either way */
BOOL WINAPI SHIM_OBJ_NAME(APIHook_GetVersionExW)(LPOSVERSIONINFOEXA lpOsVersionInfo)
{
    if (CALL_SHIM(1, GETVERSIONEXAPROC)(lpOsVersionInfo))
    {
        return FakeVersion(lpOsVersionInfo, &VERSION_INFO);
    }
    return FALSE;
}

#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER 0xC000000DL
#define STATUS_SUCCESS 0
#endif

/* We do not care about the actual type, FakeVersion will correctly handle it either way */
DWORD WINAPI SHIM_OBJ_NAME(APIHook_RtlGetVersion)(LPOSVERSIONINFOEXA lpOsVersionInfo)
{
    if (CALL_SHIM(3, GETVERSIONEXAPROC)(lpOsVersionInfo) == STATUS_SUCCESS)
    {
        return FakeVersion(lpOsVersionInfo, &VERSION_INFO) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    }
    return STATUS_INVALID_PARAMETER;
}

#ifndef STATUS_REVISION_MISMATCH
#define STATUS_REVISION_MISMATCH 0xC0000059L
#endif

/* Product type and suite are not faked, so only those parts go to the real check */
#define FAKE_REAL_VERIFY_MASK (VER_PRODUCT_TYPE | VER_SUITENAME)

BOOL WINAPI SHIM_OBJ_NAME(APIHook_VerifyVersionInfoA)(LPOSVERSIONINFOEXA lpVersionInformation, DWORD dwTypeMask, DWORDLONG dwlConditionMask)
{
    FAKE_VERIFY_RESULT Result;

    if (!lpVersionInformation)
        return CALL_SHIM(4, VERIFYVERSIONINFOPROC)(lpVersionInformation, dwTypeMask, dwlConditionMask);

    Result = VerifyFakeVersion(&VERSION_INFO, lpVersionInformation->dwMajorVersion, lpVersionInformation->dwMinorVersion,
                               lpVersionInformation->dwBuildNumber, lpVersionInformation->dwPlatformId,
                               lpVersionInformation->wServicePackMajor, lpVersionInformation->wServicePackMinor,
                               dwTypeMask, dwlConditionMask);
    if (Result == FakeVerifyInvalid)
    {
        SetLastError(ERROR_BAD_ARGUMENTS);
        return FALSE;
    }
    if (Result == FakeVerifyMismatch)
    {
        SetLastError(ERROR_OLD_WIN_VERSION);
        return FALSE;
    }
    if (dwTypeMask & FAKE_REAL_VERIFY_MASK)
        return CALL_SHIM(4, VERIFYVERSIONINFOPROC)(lpVersionInformation, dwTypeMask & FAKE_REAL_VERIFY_MASK, dwlConditionMask);
    return TRUE;
}

BOOL WINAPI SHIM_OBJ_NAME(APIHook_VerifyVersionInfoW)(LPOSVERSIONINFOEXW lpVersionInformation, DWORD dwTypeMask, DWORDLONG dwlConditionMask)
{
    FAKE_VERIFY_RESULT Result;

    if (!lpVersionInformation)
        return CALL_SHIM(5, VERIFYVERSIONINFOPROC)(lpVersionInformation, dwTypeMask, dwlConditionMask);

    Result = VerifyFakeVersion(&VERSION_INFO, lpVersionInformation->dwMajorVersion, lpVersionInformation->dwMinorVersion,
                               lpVersionInformation->dwBuildNumber, lpVersionInformation->dwPlatformId,
                               lpVersionInformation->wServicePackMajor, lpVersionInformation->wServicePackMinor,
                               dwTypeMask, dwlConditionMask);
    if (Result == FakeVerifyInvalid)
    {
        SetLastError(ERROR_BAD_ARGUMENTS);
        return FALSE;
    }
    if (Result == FakeVerifyMismatch)
    {
        SetLastError(ERROR_OLD_WIN_VERSION);
        return FALSE;
    }
    if (dwTypeMask & FAKE_REAL_VERIFY_MASK)
        return CALL_SHIM(5, VERIFYVERSIONINFOPROC)(lpVersionInformation, dwTypeMask & FAKE_REAL_VERIFY_MASK, dwlConditionMask);
    return TRUE;
}

LONG NTAPI SHIM_OBJ_NAME(APIHook_RtlVerifyVersionInfo)(LPOSVERSIONINFOEXW lpVersionInformation, ULONG TypeMask, ULONGLONG ConditionMask)
{
    FAKE_VERIFY_RESULT Result;

    if (!lpVersionInformation)
        return CALL_SHIM(6, RTLVERIFYVERSIONINFOPROC)(lpVersionInformation, TypeMask, ConditionMask);

    Result = VerifyFakeVersion(&VERSION_INFO, lpVersionInformation->dwMajorVersion, lpVersionInformation->dwMinorVersion,
                               lpVersionInformation->dwBuildNumber, lpVersionInformation->dwPlatformId,
                               lpVersionInformation->wServicePackMajor, lpVersionInformation->wServicePackMinor,
                               TypeMask, ConditionMask);
    if (Result == FakeVerifyInvalid)
        return STATUS_INVALID_PARAMETER;
    if (Result == FakeVerifyMismatch)
        return STATUS_REVISION_MISMATCH;
    if (TypeMask & FAKE_REAL_VERIFY_MASK)
        return CALL_SHIM(6, RTLVERIFYVERSIONINFOPROC)(lpVersionInformation, TypeMask & FAKE_REAL_VERIFY_MASK, ConditionMask);
    return STATUS_SUCCESS;
}

BOOL WINAPI SHIM_OBJ_NAME(Notify)(DWORD fdwReason, PVOID ptr)
{
    if (fdwReason == SHIM_NOTIFY_ATTACH && VERSION_INFO.wServicePackMajor)
    {
        static CONST WCHAR szServicePack[] = {'S','e','r','v','i','c','e',' ','P','a','c','k',' ','%','u',0};
        HRESULT hr = StringCbPrintfA(VERSION_INFO.szCSDVersionA, sizeof(VERSION_INFO.szCSDVersionA),
            "Service Pack %u", VERSION_INFO.wServicePackMajor);
        if (FAILED(hr))
            return FALSE;
        hr = StringCbPrintfW(VERSION_INFO.szCSDVersionW, sizeof(VERSION_INFO.szCSDVersionW),
            szServicePack, VERSION_INFO.wServicePackMajor);
        if (FAILED(hr))
            return FALSE;
    }
    return TRUE;
}


#define SHIM_NUM_HOOKS  7
#define SHIM_SETUP_HOOKS \
    SHIM_HOOK(0, "KERNEL32.DLL", "GetVersionExA", SHIM_OBJ_NAME(APIHook_GetVersionExA)) \
    SHIM_HOOK(1, "KERNEL32.DLL", "GetVersionExW", SHIM_OBJ_NAME(APIHook_GetVersionExW)) \
    SHIM_HOOK(2, "KERNEL32.DLL", "GetVersion", SHIM_OBJ_NAME(APIHook_GetVersion)) \
    SHIM_HOOK(3, "NTDLL.DLL", "RtlGetVersion", SHIM_OBJ_NAME(APIHook_RtlGetVersion)) \
    SHIM_HOOK(4, "KERNEL32.DLL", "VerifyVersionInfoA", SHIM_OBJ_NAME(APIHook_VerifyVersionInfoA)) \
    SHIM_HOOK(5, "KERNEL32.DLL", "VerifyVersionInfoW", SHIM_OBJ_NAME(APIHook_VerifyVersionInfoW)) \
    SHIM_HOOK(6, "NTDLL.DLL", "RtlVerifyVersionInfo", SHIM_OBJ_NAME(APIHook_RtlVerifyVersionInfo))
#define SHIM_NOTIFY_FN SHIM_OBJ_NAME(Notify)

#include <implement_shim.inl>

#undef VERSION_INFO

