/*
 * PROJECT:     ReactOS Quality Windows Audio/Video Experience
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Putting one socket's traffic ahead of the rest
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A program asks this to have the network stack carry one socket ahead of
 * everything else it is sending. Nothing here shapes traffic, so no flow is
 * ever taken on and the caller keeps sending at the rate it would have.
 */

#include <windef.h>
#include <winbase.h>
#include <winerror.h>

BOOL
WINAPI
DllMain(
    _In_ HINSTANCE Instance,
    _In_ ULONG Reason,
    _In_opt_ PVOID Reserved)
{
    UNREFERENCED_PARAMETER(Reserved);

    if (Reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(Instance);

    return TRUE;
}

/**
 * @brief
 * Opens the handle every other call here is made through.
 *
 * @return
 * FALSE, because no traffic is shaped. A caller that is told so sends the
 * same way it would have without asking.
 */
BOOL
WINAPI
QOSCreateHandle(
    _In_ PVOID Version,
    _Out_ PHANDLE QOSHandle)
{
    UNREFERENCED_PARAMETER(Version);

    if (QOSHandle != NULL)
        *QOSHandle = NULL;

    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

/**
 * @brief
 * Gives back a handle from QOSCreateHandle.
 *
 * @return
 * FALSE, since no handle was ever given out to close.
 */
BOOL
WINAPI
QOSCloseHandle(
    _In_ HANDLE QOSHandle)
{
    UNREFERENCED_PARAMETER(QOSHandle);

    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

/**
 * @brief
 * Puts a socket on a flow, making a new one when no flow is named.
 *
 * @return
 * FALSE, because there are no flows to be put on.
 */
BOOL
WINAPI
QOSAddSocketToFlow(
    _In_ HANDLE QOSHandle,
    _In_ UINT_PTR Socket,
    _In_opt_ PVOID DestAddr,
    _In_ ULONG TrafficType,
    _In_ ULONG Flags,
    _Inout_ PULONG FlowId)
{
    UNREFERENCED_PARAMETER(QOSHandle);
    UNREFERENCED_PARAMETER(Socket);
    UNREFERENCED_PARAMETER(DestAddr);
    UNREFERENCED_PARAMETER(TrafficType);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(FlowId);

    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

/**
 * @brief
 * Takes a socket back off a flow.
 *
 * @return
 * FALSE, because it was never on one.
 */
BOOL
WINAPI
QOSRemoveSocketFromFlow(
    _In_ HANDLE QOSHandle,
    _In_ UINT_PTR Socket,
    _In_ ULONG FlowId,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(QOSHandle);
    UNREFERENCED_PARAMETER(Socket);
    UNREFERENCED_PARAMETER(FlowId);
    UNREFERENCED_PARAMETER(Flags);

    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

/* EOF */
