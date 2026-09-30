/*
 * PROJECT:     ReactOS Windows Event Log API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Reading and subscribing to the event log
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A program asks this to be told about events as a channel collects them, and
 * to turn one into text it can read. Nothing here serves channels, so no
 * subscription is ever taken out and no event is ever handed back.
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
 * Asks to be told about the events a channel collects from now on.
 *
 * @return
 * NULL, because no channel is served.
 */
HANDLE
WINAPI
EvtSubscribe(
    _In_opt_ HANDLE Session,
    _In_opt_ HANDLE SignalEvent,
    _In_opt_ LPCWSTR ChannelPath,
    _In_opt_ LPCWSTR Query,
    _In_opt_ HANDLE Bookmark,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID Callback,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(Session);
    UNREFERENCED_PARAMETER(SignalEvent);
    UNREFERENCED_PARAMETER(ChannelPath);
    UNREFERENCED_PARAMETER(Query);
    UNREFERENCED_PARAMETER(Bookmark);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Callback);
    UNREFERENCED_PARAMETER(Flags);

    SetLastError(ERROR_NOT_SUPPORTED);
    return NULL;
}

/**
 * @brief
 * Names the values that EvtRender is to pick out of an event.
 *
 * @return
 * NULL, because there are no events to pick anything out of.
 */
HANDLE
WINAPI
EvtCreateRenderContext(
    _In_ ULONG ValuePathsCount,
    _In_opt_ LPCWSTR *ValuePaths,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(ValuePathsCount);
    UNREFERENCED_PARAMETER(ValuePaths);
    UNREFERENCED_PARAMETER(Flags);

    SetLastError(ERROR_NOT_SUPPORTED);
    return NULL;
}

/**
 * @brief
 * Turns an event into the text or the values a caller asked for.
 *
 * @return
 * FALSE, because no event is ever handed out to be rendered.
 */
BOOL
WINAPI
EvtRender(
    _In_opt_ HANDLE Context,
    _In_ HANDLE Fragment,
    _In_ ULONG Flags,
    _In_ ULONG BufferSize,
    _Out_opt_ PVOID Buffer,
    _Out_ PULONG BufferUsed,
    _Out_ PULONG PropertyCount)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Fragment);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(BufferSize);
    UNREFERENCED_PARAMETER(Buffer);

    if (BufferUsed != NULL)
        *BufferUsed = 0;

    if (PropertyCount != NULL)
        *PropertyCount = 0;

    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

/**
 * @brief
 * Gives back anything the other calls here handed out.
 *
 * @return
 * FALSE, since nothing was ever handed out to give back.
 */
BOOL
WINAPI
EvtClose(
    _In_ HANDLE Object)
{
    UNREFERENCED_PARAMETER(Object);

    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

/* EOF */
