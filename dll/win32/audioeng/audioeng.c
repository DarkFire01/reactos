/*
 * PROJECT:     ReactOS Audio Engine
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Real-time memory helpers for audio processing objects
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <windef.h>
#include <winbase.h>
#include <winerror.h>

HRESULT
WINAPI
AERT_Allocate(
    _In_ SIZE_T Size,
    _Outptr_result_bytebuffer_(Size) PVOID *Memory);

HRESULT
WINAPI
AERT_Free(
    _In_opt_ PVOID Memory);

/**
 * @brief Allocates memory an APO may touch from its real-time processing path.
 *
 * @return S_OK with Memory set, or E_OUTOFMEMORY with Memory untouched.
 */
HRESULT
WINAPI
AERT_Allocate(
    _In_ SIZE_T Size,
    _Outptr_result_bytebuffer_(Size) PVOID *Memory)
{
    PVOID Block;

    Block = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Block)
        return E_OUTOFMEMORY;

    *Memory = Block;
    return S_OK;
}

/**
 * @brief Frees memory returned by AERT_Allocate. NULL is accepted.
 *
 * @return Always S_OK.
 */
HRESULT
WINAPI
AERT_Free(
    _In_opt_ PVOID Memory)
{
    if (Memory)
        HeapFree(GetProcessHeap(), 0, Memory);

    return S_OK;
}
