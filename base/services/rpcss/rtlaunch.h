/*
 * PROJECT:     ReactOS RPC Subsystem Service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Starting Windows Runtime servers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define RT_PACKAGE_NAME_LENGTH 128

/* IdentityType values of a server registration */
#define RT_IDENTITY_ACTIVATE_AS_PACKAGE 2

/* What rpcss read from a server registration */
typedef struct _RT_SERVER_INFO
{
    WCHAR PackageFullName[RT_PACKAGE_NAME_LENGTH];
    PWSTR ServerName;
    PWSTR ExePath;
    PWSTR CommandLine;
    PWSTR AppUserModelId;
    BOOL PackageRelative;
    DWORD Instancing;
    DWORD IdentityType;
} RT_SERVER_INFO, *PRT_SERVER_INFO;

HRESULT
RtLaunchServer(
    _In_ HANDLE ClientToken,
    _In_ PRT_SERVER_INFO Server,
    _In_ ULONGLONG HostId,
    _Out_ PROCESS_INFORMATION *Process);

VOID
RtFreeServerInfo(
    _Inout_ PRT_SERVER_INFO Server);

FARPROC
RtGetAppCoreExport(
    _In_ PCSTR Name);
