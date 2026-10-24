/*
 * PROJECT:     ReactOS RPC Subsystem Service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Asking DcomLaunch to start Windows Runtime servers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <stdio.h>
#include <stdlib.h>

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <rpc.h>

#include "rtlaunch.h"
#include "rtlauncher_c.h"

#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(ole);

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

/**
 * @brief
 * Starts a server for its activating user. rpcss runs as NetworkService, so the
 * launch itself happens in DcomLaunch, which duplicates the client token from here
 * and hands back a handle to the new process.
 */
HRESULT
RtLaunchServer(
    _In_ HANDLE ClientToken,
    _In_ PRT_SERVER_INFO Server,
    _In_ ULONGLONG HostId,
    _Out_ PROCESS_INFORMATION *Process)
{
    RPC_BINDING_HANDLE Binding = NULL;
    RPC_WSTR StringBinding = NULL;
    RT_LAUNCH_REQUEST Request;
    ULONGLONG Started = 0;
    DWORD ProcessId = 0;
    RPC_STATUS RpcStatus;
    HRESULT Status;

    RtlZeroMemory(Process, sizeof(*Process));

    RpcStatus = RpcStringBindingComposeW(NULL, (RPC_WSTR)RT_LAUNCH_PROTSEQ, NULL, (RPC_WSTR)RT_LAUNCH_ENDPOINT,
                                         NULL, &StringBinding);
    if (RpcStatus == RPC_S_OK)
        RpcStatus = RpcBindingFromStringBindingW(StringBinding, &Binding);
    if (StringBinding)
        RpcStringFreeW(&StringBinding);
    if (RpcStatus != RPC_S_OK)
        return HRESULT_FROM_WIN32(RpcStatus);

    Request.ClientToken = (ULONGLONG)(ULONG_PTR)ClientToken;
    Request.PackageFullName = Server->PackageFullName;
    Request.ServerName = Server->ServerName;
    Request.ExePath = Server->ExePath;
    Request.CommandLine = Server->CommandLine;
    Request.AppUserModelId = Server->AppUserModelId;
    Request.PackageRelative = Server->PackageRelative;
    Request.Instancing = Server->Instancing;
    Request.IdentityType = Server->IdentityType;
    Request.HostId = HostId;

    RpcTryExcept
    {
        Status = RtLaunchStartServer(Binding, &Request, &Started, &ProcessId);
    }
    RpcExcept(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = HRESULT_FROM_WIN32(RpcExceptionCode());
    }
    RpcEndExcept;

    RpcBindingFree(&Binding);

    if (FAILED(Status))
    {
        ERR("DcomLaunch could not start %s, %#lx\n", debugstr_w(Server->ServerName), Status);
        return Status;
    }

    Process->hProcess = (HANDLE)(ULONG_PTR)Started;
    Process->dwProcessId = ProcessId;
    return S_OK;
}
