/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL heap exports (ordinals 1-5). The real NT 4.0 psxdll
 *              re-exports the Win32 heap names the reskit CRT/utilities use for
 *              their allocator; they are thin wrappers over the ntdll Rtl heap
 *              on the process's default heap.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

//
// GetProcessHeap() -- the loader-created default heap in the PEB.
//
PVOID __stdcall
GetProcessHeap(void)
{
    return NtCurrentPeb()->ProcessHeap;
}

//
// HeapAlloc(heap, flags, size)
//
PVOID __stdcall
HeapAlloc(PVOID Heap, ULONG Flags, ULONG Size)
{
    return RtlAllocateHeap(Heap, Flags, Size);
}

//
// HeapFree(heap, flags, mem)
//
BOOLEAN __stdcall
HeapFree(PVOID Heap, ULONG Flags, PVOID Memory)
{
    return RtlFreeHeap(Heap, Flags, Memory);
}

//
// HeapReAlloc(heap, flags, mem, size)
//
PVOID __stdcall
HeapReAlloc(PVOID Heap, ULONG Flags, PVOID Memory, ULONG Size)
{
    return RtlReAllocateHeap(Heap, Flags, Memory, Size);
}

//
// HeapSize(heap, flags, mem)
//
ULONG __stdcall
HeapSize(PVOID Heap, ULONG Flags, PVOID Memory)
{
    return (ULONG)RtlSizeHeap(Heap, Flags, Memory);
}
