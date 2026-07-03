/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Win32 style heap exports backed by the process default heap
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

PVOID
__stdcall
GetProcessHeap(VOID)
{
    return NtCurrentPeb()->ProcessHeap;
}

PVOID
__stdcall
HeapAlloc(
    _In_ PVOID Heap,
    _In_ ULONG Flags,
    _In_ ULONG Size)
{
    return RtlAllocateHeap(Heap, Flags, Size);
}

BOOLEAN
__stdcall
HeapFree(
    _In_ PVOID Heap,
    _In_ ULONG Flags,
    _In_opt_ PVOID Memory)
{
    return RtlFreeHeap(Heap, Flags, Memory);
}

PVOID
__stdcall
HeapReAlloc(
    _In_ PVOID Heap,
    _In_ ULONG Flags,
    _In_opt_ PVOID Memory,
    _In_ ULONG Size)
{
    return RtlReAllocateHeap(Heap, Flags, Memory, Size);
}

ULONG
__stdcall
HeapSize(
    _In_ PVOID Heap,
    _In_ ULONG Flags,
    _In_ PVOID Memory)
{
    return (ULONG)RtlSizeHeap(Heap, Flags, Memory);
}
