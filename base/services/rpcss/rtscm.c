/*
 * PROJECT:     ReactOS RPC Subsystem Service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Windows Runtime server table and activation requests
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* Process attribute lists and the package claims need the Windows 10 declarations */
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00

#include <stdio.h>
#include <stdlib.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <sddl.h>
#include <objbase.h>

#include "rtscm_s.h"
#include "rtlaunch.h"

#include <wine/debug.h>
#include <wine/list.h>

WINE_DEFAULT_DEBUG_CHANNEL(ole);

#define RT_RUNTIME_ROOT L"Software\\Microsoft\\WindowsRuntime"
#define RT_PACKAGED_ROOT L"Software\\Classes\\ActivatableClasses\\Package"
#define RT_REGISTRY_READ (KEY_READ | KEY_WOW64_64KEY)

/* How long an activation waits for the server it started to register, as DCOM does */
#define RT_SERVER_START_TIMEOUT 120000
#define RT_SERVER_POLL_INTERVAL 50

#define RT_MAX_CLASSES 0x10000
#define RT_MAX_PROVIDER (64 * 1024)

/* InstancingType values */
#define RT_INSTANCING_SINGLE 0

#ifndef APPMODEL_ERROR_NO_PACKAGE
#define APPMODEL_ERROR_NO_PACKAGE 15700L
#endif

typedef LONG (WINAPI *PFN_GET_PACKAGE_FULL_NAME_FROM_TOKEN)(HANDLE, UINT32 *, PWSTR);

/* Who is calling: the RPC client process and its token */
typedef struct _RT_CALLER
{
    DWORD ProcessId;
    HANDLE Token;
    PSID UserSid;
    DWORD SessionId;
    WCHAR PackageFullName[RT_PACKAGE_NAME_LENGTH];
} RT_CALLER, *PRT_CALLER;

/* One RoRegisterActivationFactories call of a server process */
typedef struct _RT_REGISTRATION
{
    struct list Entry;
    DWORD Cookie;
    DWORD ProcessId;
    HANDLE Process;
    PSID UserSid;
    DWORD SessionId;
    WCHAR PackageFullName[RT_PACKAGE_NAME_LENGTH];
    DWORD ClassCount;
    PWSTR *ClassIds;
    GUID *Keys;
    DWORD ProviderLength;
    BYTE *Provider;
} RT_REGISTRATION, *PRT_REGISTRATION;

/* A server process rpcss started and that may not have registered yet */
typedef struct _RT_LAUNCH
{
    struct list Entry;
    PSID UserSid;
    DWORD SessionId;
    WCHAR PackageFullName[RT_PACKAGE_NAME_LENGTH];
    PWSTR ServerName;
    DWORD ProcessId;
    HANDLE Process;
} RT_LAUNCH, *PRT_LAUNCH;

static struct list Registrations = LIST_INIT(Registrations);
static struct list Launches = LIST_INIT(Launches);
static CRITICAL_SECTION TableLock = { NULL, -1, 0, 0, 0, 0 };
static LONG NextCookie;

/* CALLERS *******************************************************************/

static
VOID
RtFreeCaller(
    _Inout_ PRT_CALLER Caller)
{
    if (Caller->Token)
        CloseHandle(Caller->Token);
    free(Caller->UserSid);
    RtlZeroMemory(Caller, sizeof(*Caller));
}

/**
 * @brief
 * Identifies the RPC client by its process and the token it calls with.
 */
static
HRESULT
RtGetCaller(
    _In_ handle_t Binding,
    _Out_ PRT_CALLER Caller)
{
    PFN_GET_PACKAGE_FULL_NAME_FROM_TOKEN GetFullName;
    BYTE Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    PTOKEN_USER User = (PTOKEN_USER)Buffer;
    UINT32 NameLength = RT_PACKAGE_NAME_LENGTH;
    DWORD Length;
    ULONG ProcessId = 0;
    RPC_STATUS RpcStatus;
    HRESULT Status = S_OK;
    LONG Error;

    RtlZeroMemory(Caller, sizeof(*Caller));

    RpcStatus = I_RpcBindingInqLocalClientPID(Binding, &ProcessId);
    if (RpcStatus != RPC_S_OK)
        return HRESULT_FROM_WIN32(RpcStatus);
    Caller->ProcessId = ProcessId;

    RpcStatus = RpcImpersonateClient(Binding);
    if (RpcStatus != RPC_S_OK)
        return HRESULT_FROM_WIN32(RpcStatus);
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE, TRUE, &Caller->Token))
        Status = HRESULT_FROM_WIN32(GetLastError());
    RpcRevertToSelf();
    if (FAILED(Status))
        return Status;

    if (!GetTokenInformation(Caller->Token, TokenUser, User, sizeof(Buffer), &Length) ||
        !GetTokenInformation(Caller->Token, TokenSessionId, &Caller->SessionId, sizeof(Caller->SessionId), &Length))
    {
        Status = HRESULT_FROM_WIN32(GetLastError());
        RtFreeCaller(Caller);
        return Status;
    }

    Caller->UserSid = malloc(GetLengthSid(User->User.Sid));
    if (!Caller->UserSid)
    {
        RtFreeCaller(Caller);
        return E_OUTOFMEMORY;
    }
    CopySid(GetLengthSid(User->User.Sid), Caller->UserSid, User->User.Sid);

    GetFullName = (PFN_GET_PACKAGE_FULL_NAME_FROM_TOKEN)RtGetAppCoreExport("GetPackageFullNameFromToken");
    if (GetFullName)
    {
        Error = GetFullName(Caller->Token, &NameLength, Caller->PackageFullName);
        if (Error != ERROR_SUCCESS)
            Caller->PackageFullName[0] = UNICODE_NULL;
    }
    return S_OK;
}

/* REGISTRY ******************************************************************/

/* The packaged registrations of a user's hive */
static
HRESULT
RtOpenUserPackages(
    _In_ PSID UserSid,
    _Out_ HKEY *Key)
{
    WCHAR Path[MAX_PATH];
    PWSTR SidText;
    LONG Error;

    *Key = NULL;
    if (!ConvertSidToStringSidW(UserSid, &SidText))
        return HRESULT_FROM_WIN32(GetLastError());

    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\%s", SidText, RT_PACKAGED_ROOT);
    Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;
    Error = RegOpenKeyExW(HKEY_USERS, Path, 0, RT_REGISTRY_READ, Key);
    if (Error == ERROR_FILE_NOT_FOUND)
    {
        /* Windows keeps per user classes in a hive of their own */
        _snwprintf(Path, ARRAYSIZE(Path), L"%s_Classes\\ActivatableClasses\\Package", SidText);
        Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;
        Error = RegOpenKeyExW(HKEY_USERS, Path, 0, RT_REGISTRY_READ, Key);
    }
    LocalFree(SidText);
    return HRESULT_FROM_WIN32(Error);
}

/**
 * @brief
 * Opens ActivatableClassId\<name> or Server\<name> of a package for a user, or of
 * the machine wide runtime registrations when no package is given.
 */
static
HRESULT
RtOpenEntry(
    _In_ PSID UserSid,
    _In_ PCWSTR PackageFullName,
    _In_ PCWSTR Collection,
    _In_ PCWSTR Name,
    _Out_ HKEY *Key)
{
    WCHAR Path[MAX_PATH * 2];
    HKEY Packages;
    LONG Error;

    *Key = NULL;
    if (!PackageFullName[0])
    {
        _snwprintf(Path, ARRAYSIZE(Path), L"%s\\%s\\%s", RT_RUNTIME_ROOT, Collection, Name);
        Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;
        return HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, RT_REGISTRY_READ, Key));
    }

    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\%s\\%s", PackageFullName, Collection, Name);
    Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;
    if (SUCCEEDED(RtOpenUserPackages(UserSid, &Packages)))
    {
        Error = RegOpenKeyExW(Packages, Path, 0, RT_REGISTRY_READ, Key);
        RegCloseKey(Packages);
        if (Error == ERROR_SUCCESS)
            return S_OK;
    }

    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\%s\\%s\\%s", RT_PACKAGED_ROOT, PackageFullName, Collection, Name);
    Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;
    return HRESULT_FROM_WIN32(RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, RT_REGISTRY_READ, Key));
}

static
DWORD
RtReadDword(
    _In_ HKEY Key,
    _In_ PCWSTR Value,
    _In_ DWORD Default)
{
    DWORD Data, Size = sizeof(Data);

    if (RegGetValueW(Key, NULL, Value, RRF_RT_REG_DWORD, NULL, &Data, &Size) != ERROR_SUCCESS)
        return Default;
    return Data;
}

/* A string value, environment strings expanded; NULL when missing */
static
PWSTR
RtReadString(
    _In_ HKEY Key,
    _In_ PCWSTR Value)
{
    DWORD Size = 0;
    PWSTR Text;

    if (RegGetValueW(Key, NULL, Value, RRF_RT_REG_SZ, NULL, NULL, &Size) != ERROR_SUCCESS)
        return NULL;
    Text = malloc(Size + sizeof(WCHAR));
    if (!Text)
        return NULL;
    if (RegGetValueW(Key, NULL, Value, RRF_RT_REG_SZ, NULL, Text, &Size) != ERROR_SUCCESS)
    {
        free(Text);
        return NULL;
    }
    return Text;
}

/**
 * @brief
 * Reads the out of process registration of a class and of its server.
 */
static
HRESULT
RtReadServer(
    _In_ PRT_CALLER Caller,
    _In_ PCWSTR PackageFullName,
    _In_ PCWSTR ClassId,
    _Out_ PRT_SERVER_INFO Server)
{
    HKEY Key;
    HRESULT Status;

    RtlZeroMemory(Server, sizeof(*Server));
    wcsncpy(Server->PackageFullName, PackageFullName, RT_PACKAGE_NAME_LENGTH - 1);

    Status = RtOpenEntry(Caller->UserSid, PackageFullName, L"ActivatableClassId", ClassId, &Key);
    if (FAILED(Status))
        return REGDB_E_CLASSNOTREG;

    if (RtReadDword(Key, L"ActivationType", 0) != 1)
    {
        RegCloseKey(Key);
        return REGDB_E_CLASSNOTREG;
    }
    Server->ServerName = RtReadString(Key, L"Server");
    RegCloseKey(Key);
    if (!Server->ServerName)
        return REGDB_E_INVALIDVALUE;

    Status = RtOpenEntry(Caller->UserSid, PackageFullName, L"Server", Server->ServerName, &Key);
    if (FAILED(Status))
    {
        RtFreeServerInfo(Server);
        return REGDB_E_INVALIDVALUE;
    }

    Server->ExePath = RtReadString(Key, L"ExePath");
    Server->CommandLine = RtReadString(Key, L"CommandLine");
    Server->AppUserModelId = RtReadString(Key, L"AppUserModelId");
    Server->PackageRelative = RtReadDword(Key, L"IsPackageRelativePath", 0) != 0;
    Server->Instancing = RtReadDword(Key, L"Instancing", RT_INSTANCING_SINGLE);
    Server->IdentityType = RtReadDword(Key, L"IdentityType", 0);
    RegCloseKey(Key);

    if (!Server->ExePath && !Server->CommandLine)
    {
        RtFreeServerInfo(Server);
        return REGDB_E_INVALIDVALUE;
    }
    return S_OK;
}

/* TABLE *********************************************************************/

static
VOID
RtFreeRegistration(
    _In_ PRT_REGISTRATION Registration)
{
    DWORD Index;

    for (Index = 0; Index < Registration->ClassCount; Index++)
        free(Registration->ClassIds[Index]);
    free(Registration->ClassIds);
    free(Registration->Keys);
    free(Registration->Provider);
    free(Registration->UserSid);
    if (Registration->Process)
        CloseHandle(Registration->Process);
    free(Registration);
}

static
VOID
RtFreeLaunch(
    _In_ PRT_LAUNCH Launch)
{
    free(Launch->UserSid);
    free(Launch->ServerName);
    if (Launch->Process)
        CloseHandle(Launch->Process);
    free(Launch);
}

static
BOOL
RtIsAlive(
    _In_opt_ HANDLE Process)
{
    return Process && WaitForSingleObject(Process, 0) == WAIT_TIMEOUT;
}

/* Forgets servers that exited; called under TableLock */
static
VOID
RtPruneTable(VOID)
{
    PRT_REGISTRATION Registration, NextRegistration;
    PRT_LAUNCH Launch, NextLaunch;

    LIST_FOR_EACH_ENTRY_SAFE(Registration, NextRegistration, &Registrations, RT_REGISTRATION, Entry)
    {
        if (!RtIsAlive(Registration->Process))
        {
            list_remove(&Registration->Entry);
            RtFreeRegistration(Registration);
        }
    }
    LIST_FOR_EACH_ENTRY_SAFE(Launch, NextLaunch, &Launches, RT_LAUNCH, Entry)
    {
        if (!RtIsAlive(Launch->Process))
        {
            list_remove(&Launch->Entry);
            RtFreeLaunch(Launch);
        }
    }
}

static
BOOL
RtSameOwner(
    _In_ PSID UserSid,
    _In_ DWORD SessionId,
    _In_ PCWSTR PackageFullName,
    _In_ PRT_CALLER Caller,
    _In_ PCWSTR WantedPackage)
{
    return SessionId == Caller->SessionId &&
           EqualSid(UserSid, Caller->UserSid) &&
           !_wcsicmp(PackageFullName, WantedPackage);
}

/**
 * @brief
 * Finds a live registration of a class and copies what the client needs.
 * With a process id only that process counts. Called under TableLock.
 */
static
BOOL
RtFindRegistration(
    _In_ PRT_CALLER Caller,
    _In_ PCWSTR PackageFullName,
    _In_ PCWSTR ClassId,
    _In_ DWORD ProcessId,
    _Out_ RT_ACTIVATION_RESULT *Result)
{
    PRT_REGISTRATION Registration;
    DWORD Index;

    LIST_FOR_EACH_ENTRY(Registration, &Registrations, RT_REGISTRATION, Entry)
    {
        if ((ProcessId && Registration->ProcessId != ProcessId) ||
            !RtSameOwner(Registration->UserSid, Registration->SessionId, Registration->PackageFullName,
                         Caller, PackageFullName) ||
            !RtIsAlive(Registration->Process))
        {
            continue;
        }

        for (Index = 0; Index < Registration->ClassCount; Index++)
        {
            if (wcscmp(Registration->ClassIds[Index], ClassId))
                continue;

            Result->Provider = MIDL_user_allocate(Registration->ProviderLength);
            if (!Result->Provider)
                return FALSE;
            memcpy(Result->Provider, Registration->Provider, Registration->ProviderLength);
            Result->ProviderLength = Registration->ProviderLength;
            Result->Key = Registration->Keys[Index];
            Result->ProcessId = Registration->ProcessId;
            return TRUE;
        }
    }
    return FALSE;
}

/* A started single instance server of the caller that is still running; under TableLock */
static
PRT_LAUNCH
RtFindLaunch(
    _In_ PRT_CALLER Caller,
    _In_ PCWSTR PackageFullName,
    _In_ PCWSTR ServerName)
{
    PRT_LAUNCH Launch;

    LIST_FOR_EACH_ENTRY(Launch, &Launches, RT_LAUNCH, Entry)
    {
        if (RtSameOwner(Launch->UserSid, Launch->SessionId, Launch->PackageFullName, Caller, PackageFullName) &&
            !wcscmp(Launch->ServerName, ServerName) &&
            RtIsAlive(Launch->Process))
        {
            return Launch;
        }
    }
    return NULL;
}

/**
 * @brief
 * Waits until a server process registers the class, exits or runs out of time.
 */
static
HRESULT
RtWaitForClass(
    _In_ PRT_CALLER Caller,
    _In_ PCWSTR PackageFullName,
    _In_ PCWSTR ClassId,
    _In_ DWORD ProcessId,
    _In_ HANDLE Process,
    _In_ ULONGLONG Deadline,
    _Out_ RT_ACTIVATION_RESULT *Result)
{
    BOOL Found;

    for (;;)
    {
        EnterCriticalSection(&TableLock);
        Found = RtFindRegistration(Caller, PackageFullName, ClassId, ProcessId, Result);
        LeaveCriticalSection(&TableLock);
        if (Found)
            return S_OK;

        if (GetTickCount64() >= Deadline)
        {
            ERR("%s did not register %s in time\n", debugstr_w(PackageFullName), debugstr_w(ClassId));
            return CO_E_SERVER_EXEC_FAILURE;
        }
        if (WaitForSingleObject(Process, RT_SERVER_POLL_INTERVAL) == WAIT_OBJECT_0)
        {
            EnterCriticalSection(&TableLock);
            Found = RtFindRegistration(Caller, PackageFullName, ClassId, ProcessId, Result);
            LeaveCriticalSection(&TableLock);
            if (Found)
                return S_OK;

            ERR("The server of %s exited before registering it\n", debugstr_w(ClassId));
            return CO_E_SERVER_EXEC_FAILURE;
        }
    }
}

/* RPC ***********************************************************************/

HRESULT
__stdcall
RtScmRegisterServer(
    _In_ handle_t Binding,
    _In_ DWORD ClassCount,
    _In_reads_(ClassCount) RT_SERVER_CLASS *Classes,
    _In_ DWORD ProviderLength,
    _In_reads_bytes_(ProviderLength) BYTE *Provider,
    _Out_ DWORD *Cookie)
{
    PRT_REGISTRATION Registration;
    RT_CALLER Caller;
    HRESULT Status;
    DWORD Index;
    HKEY Key;

    *Cookie = 0;
    if (!ClassCount || ClassCount > RT_MAX_CLASSES || !ProviderLength || ProviderLength > RT_MAX_PROVIDER)
        return E_INVALIDARG;

    Status = RtGetCaller(Binding, &Caller);
    if (FAILED(Status))
        return Status;

    /* A server only serves classes its own package or the machine registered */
    for (Index = 0; Index < ClassCount; Index++)
    {
        if (!Classes[Index].ClassId ||
            FAILED(RtOpenEntry(Caller.UserSid, Caller.PackageFullName, L"ActivatableClassId", Classes[Index].ClassId, &Key)))
        {
            WARN("Process %lu may not serve %s\n", Caller.ProcessId, debugstr_w(Classes[Index].ClassId));
            RtFreeCaller(&Caller);
            return E_ACCESSDENIED;
        }
        RegCloseKey(Key);
    }

    Registration = calloc(1, sizeof(*Registration));
    if (!Registration)
    {
        RtFreeCaller(&Caller);
        return E_OUTOFMEMORY;
    }

    Registration->ProcessId = Caller.ProcessId;
    Registration->Process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Caller.ProcessId);
    Registration->UserSid = Caller.UserSid;
    Caller.UserSid = NULL;
    Registration->SessionId = Caller.SessionId;
    wcscpy(Registration->PackageFullName, Caller.PackageFullName);
    RtFreeCaller(&Caller);

    Registration->ClassIds = calloc(ClassCount, sizeof(*Registration->ClassIds));
    Registration->Keys = calloc(ClassCount, sizeof(*Registration->Keys));
    Registration->Provider = malloc(ProviderLength);
    if (!Registration->Process || !Registration->ClassIds || !Registration->Keys || !Registration->Provider)
    {
        Status = Registration->Process ? E_OUTOFMEMORY : HRESULT_FROM_WIN32(GetLastError());
        RtFreeRegistration(Registration);
        return Status;
    }

    for (Index = 0; Index < ClassCount; Index++)
    {
        Registration->ClassIds[Index] = _wcsdup(Classes[Index].ClassId);
        if (!Registration->ClassIds[Index])
        {
            Registration->ClassCount = Index;
            RtFreeRegistration(Registration);
            return E_OUTOFMEMORY;
        }
        Registration->Keys[Index] = Classes[Index].Key;
    }
    Registration->ClassCount = ClassCount;
    Registration->ProviderLength = ProviderLength;
    memcpy(Registration->Provider, Provider, ProviderLength);

    EnterCriticalSection(&TableLock);
    RtPruneTable();
    Registration->Cookie = (DWORD)InterlockedIncrement(&NextCookie);
    list_add_tail(&Registrations, &Registration->Entry);
    *Cookie = Registration->Cookie;
    LeaveCriticalSection(&TableLock);

    TRACE("process %lu registered %lu classes of %s, cookie %lu\n", Registration->ProcessId, ClassCount,
          debugstr_w(Registration->PackageFullName), *Cookie);
    return S_OK;
}

HRESULT
__stdcall
RtScmRevokeServer(
    _In_ handle_t Binding,
    _In_ DWORD Cookie)
{
    PRT_REGISTRATION Registration;
    ULONG ProcessId = 0;
    RPC_STATUS RpcStatus;

    RpcStatus = I_RpcBindingInqLocalClientPID(Binding, &ProcessId);
    if (RpcStatus != RPC_S_OK)
        return HRESULT_FROM_WIN32(RpcStatus);

    EnterCriticalSection(&TableLock);
    LIST_FOR_EACH_ENTRY(Registration, &Registrations, RT_REGISTRATION, Entry)
    {
        if (Registration->Cookie == Cookie && Registration->ProcessId == ProcessId)
        {
            list_remove(&Registration->Entry);
            RtFreeRegistration(Registration);
            break;
        }
    }
    LeaveCriticalSection(&TableLock);
    return S_OK;
}

/**
 * @brief
 * Hands out the provider of a class: from a running single instance server, or
 * from a server started for the caller once it registered.
 */
HRESULT
__stdcall
RtScmGetActivationFactory(
    _In_ handle_t Binding,
    _In_ RT_ACTIVATION_REQUEST *Request,
    _Out_ RT_ACTIVATION_RESULT *Result)
{
    WCHAR Package[RT_PACKAGE_NAME_LENGTH];
    ULONGLONG Deadline = GetTickCount64() + RT_SERVER_START_TIMEOUT;
    PROCESS_INFORMATION Process;
    RT_SERVER_INFO Server;
    PRT_LAUNCH Launch;
    HANDLE Waiting = NULL;
    DWORD WaitingId = 0;
    RT_CALLER Caller;
    HRESULT Status;
    BOOL Found;

    RtlZeroMemory(Result, sizeof(*Result));
    if (!Request->ClassId || !Request->ClassId[0] ||
        (Request->PackageFullName && wcslen(Request->PackageFullName) >= RT_PACKAGE_NAME_LENGTH))
    {
        return E_INVALIDARG;
    }
    Package[0] = UNICODE_NULL;
    if (Request->PackageFullName)
        wcscpy(Package, Request->PackageFullName);

    Status = RtGetCaller(Binding, &Caller);
    if (FAILED(Status))
        return Status;

    Status = RtReadServer(&Caller, Package, Request->ClassId, &Server);
    if (FAILED(Status))
    {
        WARN("%s of %s is not an out of process class, %#lx\n", debugstr_w(Request->ClassId), debugstr_w(Package), Status);
        RtFreeCaller(&Caller);
        return Status;
    }

    TRACE("activating %s of %s through %s\n", debugstr_w(Request->ClassId), debugstr_w(Package),
          debugstr_w(Server.ServerName));

    EnterCriticalSection(&TableLock);
    RtPruneTable();
    if (Server.Instancing == RT_INSTANCING_SINGLE)
    {
        Found = RtFindRegistration(&Caller, Package, Request->ClassId, 0, Result);
        if (Found)
        {
            LeaveCriticalSection(&TableLock);
            goto Quit;
        }

        /* A server started for an earlier activation may still be coming up */
        Launch = RtFindLaunch(&Caller, Package, Server.ServerName);
        if (Launch && DuplicateHandle(GetCurrentProcess(), Launch->Process, GetCurrentProcess(), &Waiting,
                                      SYNCHRONIZE, FALSE, 0))
        {
            WaitingId = Launch->ProcessId;
        }
    }
    LeaveCriticalSection(&TableLock);

    if (!Waiting)
    {
        Status = RtLaunchServer(Caller.Token, &Server, Request->HostId, &Process);
        if (FAILED(Status))
            goto Quit;

        Launch = calloc(1, sizeof(*Launch));
        if (Launch)
        {
            Launch->UserSid = malloc(GetLengthSid(Caller.UserSid));
            Launch->ServerName = _wcsdup(Server.ServerName);
        }
        if (!Launch || !Launch->UserSid || !Launch->ServerName)
        {
            if (Launch)
                RtFreeLaunch(Launch);
            CloseHandle(Process.hProcess);
            Status = E_OUTOFMEMORY;
            goto Quit;
        }
        CopySid(GetLengthSid(Caller.UserSid), Launch->UserSid, Caller.UserSid);
        Launch->SessionId = Caller.SessionId;
        wcscpy(Launch->PackageFullName, Package);
        Launch->ProcessId = Process.dwProcessId;
        Launch->Process = Process.hProcess;

        if (!DuplicateHandle(GetCurrentProcess(), Process.hProcess, GetCurrentProcess(), &Waiting,
                             SYNCHRONIZE, FALSE, 0))
        {
            Status = HRESULT_FROM_WIN32(GetLastError());
            RtFreeLaunch(Launch);
            goto Quit;
        }
        WaitingId = Process.dwProcessId;

        EnterCriticalSection(&TableLock);
        list_add_tail(&Launches, &Launch->Entry);
        LeaveCriticalSection(&TableLock);
    }

    Status = RtWaitForClass(&Caller, Package, Request->ClassId, WaitingId, Waiting, Deadline, Result);

Quit:
    TRACE("%s: %#lx, process %lu\n", debugstr_w(Request->ClassId), Status, Result->ProcessId);
    if (Waiting)
        CloseHandle(Waiting);
    RtFreeServerInfo(&Server);
    RtFreeCaller(&Caller);
    return Status;
}

VOID
RtScmInitialize(VOID)
{
    InitializeCriticalSection(&TableLock);
}
