/*
 * PROJECT:     ReactOS Management Infrastructure
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Opening a management session
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A caller of this asks the management infrastructure for a session and then
 * queries or subscribes through it. Nothing here keeps one, so the session is
 * refused on the way in and the caller carries on without management.
 */

#include <windef.h>
#include <winbase.h>

#define NDEBUG
#include <debug.h>

/* MI_Result, of which only the two that matter here are named */
#define MI_RESULT_OK 0
#define MI_RESULT_NOT_SUPPORTED 7

/**
 * @brief
 * Opens a management session for an application.
 *
 * @param[out] ExtendedError
 * Where a description of the refusal would go. None is given, so this is
 * cleared.
 *
 * @return
 * MI_RESULT_NOT_SUPPORTED, because there is no infrastructure to talk to.
 */
INT
WINAPI
MI_Application_InitializeV1(
    _In_ ULONG Flags,
    _In_opt_ PCWSTR ApplicationId,
    _Outptr_opt_result_maybenull_ PVOID *ExtendedError,
    _Out_ PVOID Application)
{
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(ApplicationId);
    UNREFERENCED_PARAMETER(Application);

    if (ExtendedError != NULL)
        *ExtendedError = NULL;

    return MI_RESULT_NOT_SUPPORTED;
}

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

/* EOF */
