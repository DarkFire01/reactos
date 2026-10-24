/*
 * PROJECT:     ReactOS DCOM Server Process Launcher
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Serving Windows Runtime server launches to rpcss
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* QueryFullProcessImageNameW is a Vista function */
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600

#include <stdlib.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <rpc.h>
#include <rpcndr.h>

#include "launch.h"
#include "rtlauncher_s.h"

#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(ole);

/* rpcss only waits on the started process, and its id comes back separately */
#define RT_LAUNCH_PROCESS_ACCESS SYNCHRONIZE

void __RPC_FAR * __RPC_USER
MIDL_user_allocate(
    _In_ SIZE_T Size)
{
    return malloc(Size);
}

void __RPC_USER
MIDL_user_free(
    _In_opt_ void __RPC_FAR *Pointer)
{
    free(Pointer);
}

/*
 * The caller has to be rpcss: the system's rpcss.exe, running as LocalSystem or,
 * as it normally does, NetworkService. Anyone else could have servers started with
 * the package claims of a package of their choosing.
 */
static
BOOL
RtIsRpcss(
    _In_ HANDLE Caller)
{
    BYTE Buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
    PTOKEN_USER User = (PTOKEN_USER)Buffer;
    WCHAR Expected[MAX_PATH], Image[MAX_PATH];
    DWORD Length = ARRAYSIZE(Image);
    HANDLE Token;
    BOOL Allowed;
    UINT Used;

    Used = GetSystemDirectoryW(Expected, ARRAYSIZE(Expected) - ARRAYSIZE(L"\\rpcss.exe"));
    if (!Used || Used >= ARRAYSIZE(Expected) - ARRAYSIZE(L"\\rpcss.exe"))
        return FALSE;
    wcscat(Expected, L"\\rpcss.exe");

    if (!QueryFullProcessImageNameW(Caller, 0, Image, &Length))
    {
        ERR("the caller's image is unknown, %lu\n", GetLastError());
        return FALSE;
    }
    if (_wcsicmp(Image, Expected))
    {
        ERR("the caller is %s, not %s\n", debugstr_w(Image), debugstr_w(Expected));
        return FALSE;
    }

    if (!OpenProcessToken(Caller, TOKEN_QUERY, &Token))
    {
        ERR("opening the caller's token failed, %lu\n", GetLastError());
        return FALSE;
    }
    Allowed = GetTokenInformation(Token, TokenUser, User, sizeof(Buffer), &Length) &&
              (IsWellKnownSid(User->User.Sid, WinNetworkServiceSid) ||
               IsWellKnownSid(User->User.Sid, WinLocalSystemSid));
    CloseHandle(Token);
    if (!Allowed)
        ERR("the caller runs under an account other than NetworkService or LocalSystem\n");
    return Allowed;
}

HRESULT
__stdcall
RtLaunchStartServer(
    _In_ handle_t Binding,
    _In_ RT_LAUNCH_REQUEST *Request,
    _Out_ ULONGLONG *Process,
    _Out_ DWORD *ProcessId)
{
    PROCESS_INFORMATION Started;
    RT_SERVER_INFO Server;
    HANDLE Caller = NULL, ClientToken = NULL, Remote = NULL;
    ULONG CallerId = 0;
    RPC_STATUS RpcStatus;
    HRESULT Status;

    *Process = 0;
    *ProcessId = 0;

    RpcStatus = I_RpcBindingInqLocalClientPID(Binding, &CallerId);
    if (RpcStatus != RPC_S_OK)
    {
        ERR("the calling process is unknown, %lu\n", RpcStatus);
        return E_ACCESSDENIED;
    }

    /* ReactOS opens a process token with PROCESS_QUERY_INFORMATION, not the limited right */
    Caller = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_DUP_HANDLE, FALSE, CallerId);
    if (!Caller)
    {
        ERR("opening process %lu failed, %lu\n", CallerId, GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }
    if (!RtIsRpcss(Caller))
    {
        ERR("process %lu is not rpcss\n", CallerId);
        CloseHandle(Caller);
        return E_ACCESSDENIED;
    }

    if (!DuplicateHandle(Caller, (HANDLE)(ULONG_PTR)Request->ClientToken, GetCurrentProcess(), &ClientToken,
                         TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE, FALSE, 0))
    {
        Status = HRESULT_FROM_WIN32(GetLastError());
        ERR("taking the client token from rpcss failed, %#lx\n", Status);
        CloseHandle(Caller);
        return Status;
    }

    RtlZeroMemory(&Server, sizeof(Server));
    if (wcslen(Request->PackageFullName) >= ARRAYSIZE(Server.PackageFullName))
    {
        Status = E_INVALIDARG;
        goto Quit;
    }
    wcscpy(Server.PackageFullName, Request->PackageFullName);
    Server.ServerName = Request->ServerName;
    Server.ExePath = Request->ExePath;
    Server.CommandLine = Request->CommandLine;
    Server.AppUserModelId = Request->AppUserModelId;
    Server.PackageRelative = Request->PackageRelative != 0;
    Server.Instancing = Request->Instancing;
    Server.IdentityType = Request->IdentityType;

    Status = RtLaunchServer(ClientToken, &Server, Request->HostId, &Started);
    if (FAILED(Status))
        goto Quit;

    if (!DuplicateHandle(GetCurrentProcess(), Started.hProcess, Caller, &Remote, RT_LAUNCH_PROCESS_ACCESS, FALSE, 0))
    {
        Status = HRESULT_FROM_WIN32(GetLastError());
        ERR("handing process %lu to rpcss failed, %#lx\n", Started.dwProcessId, Status);
        TerminateProcess(Started.hProcess, Status);
    }
    else
    {
        *Process = (ULONGLONG)(ULONG_PTR)Remote;
        *ProcessId = Started.dwProcessId;
    }
    CloseHandle(Started.hProcess);

Quit:
    CloseHandle(ClientToken);
    CloseHandle(Caller);
    return Status;
}

/* Serves the launch interface on its own local endpoint for as long as the service runs */
VOID
RtLaunchStartListening(VOID)
{
    RPC_STATUS Status;

    Status = RpcServerUseProtseqEpW((RPC_WSTR)RT_LAUNCH_PROTSEQ, RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
                                    (RPC_WSTR)RT_LAUNCH_ENDPOINT, NULL);
    if (Status != RPC_S_OK && Status != RPC_S_DUPLICATE_ENDPOINT)
    {
        ERR("RpcServerUseProtseqEpW failed, %lu\n", Status);
        return;
    }

    Status = RpcServerRegisterIfEx(IRtLaunch_v1_0_s_ifspec, NULL, NULL, RPC_IF_AUTOLISTEN,
                                   RPC_C_LISTEN_MAX_CALLS_DEFAULT, NULL);
    if (Status != RPC_S_OK)
        ERR("RpcServerRegisterIfEx failed, %lu\n", Status);
}
