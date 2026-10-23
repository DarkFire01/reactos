/*
 * PROJECT:     ReactOS RPC Subsystem Service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Starting Windows Runtime servers, packaged ones in their AppContainer
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
#include <userenv.h>
#include <sddl.h>
#define NTOS_MODE_USER
#include <ndk/rtlfuncs.h>
#include <ndk/sefuncs.h>
#include <ndk/setypes.h>

#include "rtlaunch.h"

#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(ole);

#define RT_SERVER_NAME_SWITCH L" -ServerName:"
#define RT_PACKAGE_CACHE L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModel\\StateRepository\\Cache\\Package"

/* WIN://PKG flags of a normal launch; bit 0x10 would select the registrar path */
#define RT_PACKAGE_CLAIM_FLAGS 0x1

#ifndef APPMODEL_ERROR_NO_PACKAGE
#define APPMODEL_ERROR_NO_PACKAGE 15700L
#endif

typedef LONG (WINAPI *PFN_GET_PACKAGE_PATH_BY_FULL_NAME)(PCWSTR, UINT32 *, PWSTR);
typedef LONG (WINAPI *PFN_PACKAGE_FAMILY_NAME_FROM_FULL_NAME)(PCWSTR, UINT32 *, PWSTR);
typedef HRESULT (WINAPI *PFN_GET_PACKAGE_CAPABILITIES)(PCWSTR, BOOL *, PSID_AND_ATTRIBUTES *, DWORD *);
typedef VOID (WINAPI *PFN_FREE_APPX_MEMORY)(PVOID);

/**
 * @brief
 * Looks up an export of kernel.appcore, which keeps the package store.
 */
FARPROC
RtGetAppCoreExport(
    _In_ PCSTR Name)
{
    static HMODULE AppCore;
    HMODULE Module;

    if (!AppCore)
    {
        Module = LoadLibraryW(L"kernel.appcore.dll");
        if (!Module)
            return NULL;
        if (InterlockedCompareExchangePointer((PVOID *)&AppCore, Module, NULL))
            FreeLibrary(Module);
    }
    return GetProcAddress(AppCore, Name);
}

VOID
RtFreeServerInfo(
    _Inout_ PRT_SERVER_INFO Server)
{
    free(Server->ServerName);
    free(Server->ExePath);
    free(Server->CommandLine);
    free(Server->AppUserModelId);
    RtlZeroMemory(Server, sizeof(*Server));
}

/* The install folder of a package, looked up as the activating user */
static
HRESULT
RtGetInstallPath(
    _In_ HANDLE ClientToken,
    _In_ PCWSTR PackageFullName,
    _Outptr_ PWSTR *Path)
{
    PFN_GET_PACKAGE_PATH_BY_FULL_NAME GetPath;
    UINT32 Length = 0;
    LONG Error;

    *Path = NULL;
    GetPath = (PFN_GET_PACKAGE_PATH_BY_FULL_NAME)RtGetAppCoreExport("GetPackagePathByFullName");
    if (!GetPath)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    if (!ImpersonateLoggedOnUser(ClientToken))
        return HRESULT_FROM_WIN32(GetLastError());

    Error = GetPath(PackageFullName, &Length, NULL);
    if (Error == ERROR_INSUFFICIENT_BUFFER)
    {
        *Path = malloc(Length * sizeof(WCHAR));
        Error = *Path ? GetPath(PackageFullName, &Length, *Path) : ERROR_OUTOFMEMORY;
    }
    else if (Error == ERROR_SUCCESS)
    {
        Error = ERROR_NOT_FOUND;
    }
    RevertToSelf();

    if (Error != ERROR_SUCCESS)
    {
        free(*Path);
        *Path = NULL;
        return HRESULT_FROM_WIN32(Error);
    }
    return S_OK;
}

/**
 * @brief
 * Builds "<server command>" -ServerName:<name>, the switch always last.
 */
static
HRESULT
RtBuildCommandLine(
    _In_ PRT_SERVER_INFO Server,
    _In_opt_ PCWSTR InstallPath,
    _Outptr_ PWSTR *CommandLine)
{
    PCWSTR Base, Folder = L"", Separator = L"", Quote = L"\"";
    SIZE_T Length;

    *CommandLine = NULL;
    if (Server->CommandLine)
    {
        Base = Server->CommandLine;
        Quote = L"";
    }
    else if (Server->ExePath)
    {
        Base = Server->ExePath;
        if (Server->PackageRelative && InstallPath)
        {
            Folder = InstallPath;
            Separator = L"\\";
        }
    }
    else
    {
        return REGDB_E_INVALIDVALUE;
    }

    Length = wcslen(Quote) * 2 + wcslen(Folder) + wcslen(Separator) + wcslen(Base) +
             wcslen(RT_SERVER_NAME_SWITCH) + wcslen(Server->ServerName) + 1;
    *CommandLine = malloc(Length * sizeof(WCHAR));
    if (!*CommandLine)
        return E_OUTOFMEMORY;

    _snwprintf(*CommandLine, Length, L"%s%s%s%s%s%s%s", Quote, Folder, Separator, Base, Quote,
               RT_SERVER_NAME_SWITCH, Server->ServerName);
    (*CommandLine)[Length - 1] = UNICODE_NULL;
    return S_OK;
}

/* PackageOrigin column of the package in the store cache */
static
DWORD
RtGetPackageOrigin(
    _In_ PCWSTR PackageFullName)
{
    WCHAR Path[MAX_PATH * 2], Id[32];
    DWORD Origin = 0, Size = sizeof(Origin), Length = ARRAYSIZE(Id);
    HKEY Key;

    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\Index\\PackageFullName\\%s", RT_PACKAGE_CACHE, PackageFullName);
    Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, Path, 0, KEY_READ, &Key) != ERROR_SUCCESS)
        return 0;
    if (RegEnumKeyExW(Key, 0, Id, &Length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
        Id[0] = UNICODE_NULL;
    RegCloseKey(Key);
    if (!Id[0])
        return 0;

    _snwprintf(Path, ARRAYSIZE(Path), L"%s\\Data\\%s", RT_PACKAGE_CACHE, Id);
    Path[ARRAYSIZE(Path) - 1] = UNICODE_NULL;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, Path, L"PackageOrigin", RRF_RT_REG_DWORD, NULL, &Origin, &Size) != ERROR_SUCCESS)
        return 0;
    return Origin;
}

/**
 * @brief
 * A host id that stays the same for a user's application, used when the
 * activation does not bring one.
 */
static
ULONGLONG
RtMakeHostId(
    _In_ HANDLE ClientToken,
    _In_ PCWSTR AppUserModelId)
{
    BYTE Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    PTOKEN_USER User = (PTOKEN_USER)Buffer;
    ULONGLONG Hash = 0xcbf29ce484222325ULL;
    DWORD Length, Index;
    PCWSTR Text;

    if (GetTokenInformation(ClientToken, TokenUser, User, sizeof(Buffer), &Length))
    {
        for (Index = 0; Index < GetLengthSid(User->User.Sid); Index++)
            Hash = (Hash ^ ((BYTE *)User->User.Sid)[Index]) * 0x100000001b3ULL;
    }
    for (Text = AppUserModelId; *Text; Text++)
        Hash = (Hash ^ towlower(*Text)) * 0x100000001b3ULL;

    return Hash ? Hash : 1;
}

static
VOID
RtInitString(
    _Out_ PUNICODE_STRING String,
    _In_ PCWSTR Text)
{
    SIZE_T Length = wcslen(Text) * sizeof(WCHAR);

    String->Buffer = (PWSTR)Text;
    String->Length = (USHORT)Length;
    String->MaximumLength = (USHORT)(Length + sizeof(WCHAR));
}

/**
 * @brief
 * Puts the package identity claims on the token of a new server process:
 * WIN://SYSAPPID {full name, application id, family name}, WIN://PKG and
 * WIN://PKGHOSTID. Needs SeTcbPrivilege.
 */
static
HRESULT
RtSetPackageClaims(
    _In_ HANDLE Token,
    _In_ PCWSTR PackageFullName,
    _In_ PCWSTR ApplicationId,
    _In_ PCWSTR FamilyName,
    _In_ DWORD Origin,
    _In_ ULONGLONG HostId)
{
    TOKEN_SECURITY_ATTRIBUTE_OPERATION Operations[3];
    TOKEN_SECURITY_ATTRIBUTES_AND_OPERATION_INFORMATION Information;
    TOKEN_SECURITY_ATTRIBUTES_INFORMATION Attributes;
    TOKEN_SECURITY_ATTRIBUTE_V1 Claims[3];
    UNICODE_STRING Identity[3];
    ULONG64 Package, Host = HostId;
    BOOLEAN WasEnabled;
    NTSTATUS Status;

    Package = ((ULONG64)Origin << 32) | RT_PACKAGE_CLAIM_FLAGS;

    RtInitString(&Identity[0], PackageFullName);
    RtInitString(&Identity[1], ApplicationId);
    RtInitString(&Identity[2], FamilyName);

    RtlZeroMemory(Claims, sizeof(Claims));
    RtInitString(&Claims[0].Name, L"WIN://SYSAPPID");
    Claims[0].ValueType = TOKEN_SECURITY_ATTRIBUTE_TYPE_STRING;
    Claims[0].ValueCount = ARRAYSIZE(Identity);
    Claims[0].Values.pString = Identity;

    RtInitString(&Claims[1].Name, L"WIN://PKG");
    Claims[1].ValueType = TOKEN_SECURITY_ATTRIBUTE_TYPE_UINT64;
    Claims[1].ValueCount = 1;
    Claims[1].Values.pUint64 = &Package;

    RtInitString(&Claims[2].Name, L"WIN://PKGHOSTID");
    Claims[2].ValueType = TOKEN_SECURITY_ATTRIBUTE_TYPE_UINT64;
    Claims[2].ValueCount = 1;
    Claims[2].Values.pUint64 = &Host;

    Operations[0] = TOKEN_SECURITY_ATTRIBUTE_OPERATION_REPLACE;
    Operations[1] = TOKEN_SECURITY_ATTRIBUTE_OPERATION_REPLACE;
    Operations[2] = TOKEN_SECURITY_ATTRIBUTE_OPERATION_REPLACE;

    Attributes.Version = TOKEN_SECURITY_ATTRIBUTES_INFORMATION_VERSION_V1;
    Attributes.Reserved = 0;
    Attributes.AttributeCount = ARRAYSIZE(Claims);
    Attributes.Attribute.pAttributeV1 = Claims;
    Information.Attributes = &Attributes;
    Information.Operations = Operations;

    Status = RtlAdjustPrivilege(SE_TCB_PRIVILEGE, TRUE, FALSE, &WasEnabled);
    if (!NT_SUCCESS(Status))
        return HRESULT_FROM_NT(Status);

    Status = NtSetInformationToken(Token, TokenSecurityAttributes, &Information, sizeof(Information));
    if (!WasEnabled)
        RtlAdjustPrivilege(SE_TCB_PRIVILEGE, FALSE, FALSE, &WasEnabled);
    return NT_SUCCESS(Status) ? S_OK : HRESULT_FROM_NT(Status);
}

/* Package SID and capability SIDs the AppContainer gets */
static
HRESULT
RtGetContainerIdentity(
    _In_ PCWSTR PackageFullName,
    _In_ PCWSTR FamilyName,
    _Out_ PSID *PackageSid,
    _Out_ PSID_AND_ATTRIBUTES *Capabilities,
    _Out_ DWORD *CapabilityCount)
{
    PFN_GET_PACKAGE_CAPABILITIES GetCapabilities;
    BOOL FullTrust = FALSE;
    HRESULT Status;

    *PackageSid = NULL;
    *Capabilities = NULL;
    *CapabilityCount = 0;

    Status = DeriveAppContainerSidFromAppContainerName(FamilyName, PackageSid);
    if (FAILED(Status))
        return Status;

    GetCapabilities = (PFN_GET_PACKAGE_CAPABILITIES)RtGetAppCoreExport("AppXGetPackageCapabilities");
    if (!GetCapabilities)
        return S_OK;

    /* A package without provisioned capabilities still runs, with none */
    Status = GetCapabilities(PackageFullName, &FullTrust, Capabilities, CapabilityCount);
    if (FAILED(Status))
    {
        WARN("No capabilities for %s, %#lx\n", debugstr_w(PackageFullName), Status);
        *Capabilities = NULL;
        *CapabilityCount = 0;
    }
    return S_OK;
}

/**
 * @brief
 * Starts a server for its activating user. A packaged server is created
 * suspended in its AppContainer, gets its package claims and is then resumed.
 */
HRESULT
RtLaunchServer(
    _In_ HANDLE ClientToken,
    _In_ PRT_SERVER_INFO Server,
    _In_ ULONGLONG HostId,
    _Out_ PROCESS_INFORMATION *Process)
{
    PFN_PACKAGE_FAMILY_NAME_FROM_FULL_NAME FamilyFromFullName;
    PFN_FREE_APPX_MEMORY FreeAppxMemory;
    WCHAR FamilyName[RT_PACKAGE_NAME_LENGTH];
    PCWSTR ApplicationId = L"";
    SECURITY_CAPABILITIES Security;
    STARTUPINFOEXW Startup;
    PSID_AND_ATTRIBUTES Capabilities = NULL;
    PSID PackageSid = NULL;
    PWSTR InstallPath = NULL, CommandLine = NULL;
    PVOID Environment = NULL;
    HANDLE Primary = NULL, ProcessToken = NULL;
    SIZE_T ListSize = 0;
    DWORD CapabilityCount = 0, Flags;
    UINT32 FamilyLength = ARRAYSIZE(FamilyName);
    BOOL Packaged = Server->PackageFullName[0] != UNICODE_NULL;
    BOOL Container;
    HRESULT Status = S_OK;
    LONG Error;

    RtlZeroMemory(Process, sizeof(*Process));
    RtlZeroMemory(&Startup, sizeof(Startup));
    Container = Packaged && Server->IdentityType == RT_IDENTITY_ACTIVATE_AS_PACKAGE;

    if (Packaged)
    {
        FamilyFromFullName = (PFN_PACKAGE_FAMILY_NAME_FROM_FULL_NAME)RtGetAppCoreExport("PackageFamilyNameFromFullName");
        Error = FamilyFromFullName ? FamilyFromFullName(Server->PackageFullName, &FamilyLength, FamilyName) : ERROR_PROC_NOT_FOUND;
        Status = HRESULT_FROM_WIN32(Error);
        if (SUCCEEDED(Status))
            Status = RtGetInstallPath(ClientToken, Server->PackageFullName, &InstallPath);
        if (FAILED(Status))
            goto Quit;

        if (Server->AppUserModelId && wcschr(Server->AppUserModelId, L'!'))
            ApplicationId = wcschr(Server->AppUserModelId, L'!') + 1;
        if (!HostId)
            HostId = RtMakeHostId(ClientToken, Server->AppUserModelId ? Server->AppUserModelId : Server->PackageFullName);
    }

    Status = RtBuildCommandLine(Server, InstallPath, &CommandLine);
    if (FAILED(Status))
        goto Quit;

    if (!DuplicateTokenEx(ClientToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &Primary))
    {
        Status = HRESULT_FROM_WIN32(GetLastError());
        goto Quit;
    }

    if (!CreateEnvironmentBlock(&Environment, Primary, FALSE))
        Environment = NULL;

    Startup.StartupInfo.cb = sizeof(Startup.StartupInfo);
    Startup.StartupInfo.lpDesktop = L"WinSta0\\Default";
    Flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;

    if (Container)
    {
        Status = RtGetContainerIdentity(Server->PackageFullName, FamilyName, &PackageSid, &Capabilities, &CapabilityCount);
        if (FAILED(Status))
            goto Quit;

        Security.AppContainerSid = PackageSid;
        Security.Capabilities = Capabilities;
        Security.CapabilityCount = CapabilityCount;
        Security.Reserved = 0;

        InitializeProcThreadAttributeList(NULL, 1, 0, &ListSize);
        Startup.lpAttributeList = malloc(ListSize);
        if (!Startup.lpAttributeList ||
            !InitializeProcThreadAttributeList(Startup.lpAttributeList, 1, 0, &ListSize))
        {
            free(Startup.lpAttributeList);
            Startup.lpAttributeList = NULL;
            Status = E_OUTOFMEMORY;
            goto Quit;
        }
        if (!UpdateProcThreadAttribute(Startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                       &Security, sizeof(Security), NULL, NULL))
        {
            Status = HRESULT_FROM_WIN32(GetLastError());
            goto Quit;
        }
        Startup.StartupInfo.cb = sizeof(Startup);
        Flags |= EXTENDED_STARTUPINFO_PRESENT;
    }

    TRACE("starting %s, container %d\n", debugstr_w(CommandLine), Container);

    if (!CreateProcessAsUserW(Primary, NULL, CommandLine, NULL, NULL, FALSE, Flags, Environment,
                              InstallPath, &Startup.StartupInfo, Process))
    {
        Status = HRESULT_FROM_WIN32(GetLastError());
        ERR("Starting %s failed, %#lx\n", debugstr_w(CommandLine), Status);
        RtlZeroMemory(Process, sizeof(*Process));
        goto Quit;
    }

    if (Packaged)
    {
        if (!OpenProcessToken(Process->hProcess, TOKEN_ADJUST_DEFAULT | TOKEN_QUERY, &ProcessToken))
            Status = HRESULT_FROM_WIN32(GetLastError());
        else
            Status = RtSetPackageClaims(ProcessToken, Server->PackageFullName, ApplicationId, FamilyName,
                                        RtGetPackageOrigin(Server->PackageFullName), HostId);
        if (FAILED(Status))
        {
            ERR("Setting the package claims of %s failed, %#lx\n", debugstr_w(Server->PackageFullName), Status);
            TerminateProcess(Process->hProcess, Status);
            CloseHandle(Process->hProcess);
            CloseHandle(Process->hThread);
            RtlZeroMemory(Process, sizeof(*Process));
            goto Quit;
        }
    }

    ResumeThread(Process->hThread);
    CloseHandle(Process->hThread);
    Process->hThread = NULL;

Quit:
    if (Startup.lpAttributeList)
    {
        DeleteProcThreadAttributeList(Startup.lpAttributeList);
        free(Startup.lpAttributeList);
    }
    if (Capabilities)
    {
        FreeAppxMemory = (PFN_FREE_APPX_MEMORY)RtGetAppCoreExport("AppXFreeMemory");
        if (FreeAppxMemory)
            FreeAppxMemory(Capabilities);
    }
    if (PackageSid)
        FreeSid(PackageSid);
    if (ProcessToken)
        CloseHandle(ProcessToken);
    if (Environment)
        DestroyEnvironmentBlock(Environment);
    if (Primary)
        CloseHandle(Primary);
    free(CommandLine);
    free(InstallPath);
    return Status;
}
