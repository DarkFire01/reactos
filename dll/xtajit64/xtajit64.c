/*
 * PROJECT:     ReactOS x64 emulator
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Process and thread setup of the x64 emulator
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include "xtajit64.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

#define XTA_STACK_SIZE (512 * 1024)

/* FUNCTIONS ****************************************************************/

/**
 * @brief
 * Returns the CPU area of the current thread, set up by the loader.
 */
PCHPE_V2_CPU_AREA_INFO
NTAPI
XtapGetCpuArea(VOID)
{
    return NtCurrentTeb()->ChpeV2CpuAreaInfo;
}

/**
 * @brief
 * Sets up the emulator for the process.
 */
NTSTATUS
NTAPI
ProcessInit(VOID)
{
    SIZE_T Size = PAGE_SIZE;
    PVOID Base = NULL;
    NTSTATUS Status;

    /* The x64 ret has to live outside any image, RtlIsEcCode would see image code as native */
    Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                     &Base,
                                     0,
                                     &Size,
                                     MEM_RESERVE | MEM_COMMIT,
                                     PAGE_EXECUTE_READWRITE);
    if (!NT_SUCCESS(Status))
        return Status;

    XtapX64Ret = Base;
    XtapX64Ret[0] = 0xC3;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Gives the current thread an emulator stack and its interpreter state.
 */
NTSTATUS
NTAPI
ThreadInit(VOID)
{
    PCHPE_V2_CPU_AREA_INFO CpuArea = XtapGetCpuArea();
    PXTA_THREAD Thread;
    SIZE_T Size = XTA_STACK_SIZE;
    PVOID Base = NULL;
    NTSTATUS Status;

    if (!CpuArea)
        return STATUS_INVALID_PARAMETER;

    Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                     &Base,
                                     0,
                                     &Size,
                                     MEM_RESERVE | MEM_COMMIT,
                                     PAGE_READWRITE);
    if (!NT_SUCCESS(Status))
        return Status;

    Thread = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Thread));
    if (!Thread)
        return STATUS_NO_MEMORY;

    Thread->CpuArea = CpuArea;
    Thread->Gs = (ULONG64)NtCurrentTeb();

    CpuArea->EmulatorStackLimit = (ULONG64)Base;
    CpuArea->EmulatorStackBase = (ULONG64)Base + Size;
    CpuArea->EmulatorData[0] = Thread;

    /* The loader fills in the stack and the entry point, the rest is the x64 start state */
    RtlZeroMemory(CpuArea->ContextAmd64, sizeof(*CpuArea->ContextAmd64));
    CpuArea->ContextAmd64->ContextFlags = 0x0010001F;
    CpuArea->ContextAmd64->AMD64_EFlags = 0x202;
    CpuArea->ContextAmd64->AMD64_SegCs = 0x33;
    CpuArea->ContextAmd64->AMD64_SegSs = 0x2B;
    CpuArea->ContextAmd64->AMD64_MxCsr = 0x1F80;
    CpuArea->ContextAmd64->AMD64_MxCsr_copy = 0x1F80;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Frees the interpreter state of the current thread.
 */
NTSTATUS
NTAPI
ThreadTerm(
    _In_ HANDLE ThreadHandle,
    _In_ LONG ExitCode)
{
    PCHPE_V2_CPU_AREA_INFO CpuArea = XtapGetCpuArea();
    PVOID Base;
    SIZE_T Size = 0;

    UNREFERENCED_PARAMETER(ThreadHandle);
    UNREFERENCED_PARAMETER(ExitCode);

    if (!CpuArea || !CpuArea->EmulatorData[0])
        return STATUS_SUCCESS;

    Base = (PVOID)CpuArea->EmulatorStackLimit;
    NtFreeVirtualMemory(NtCurrentProcess(), &Base, &Size, MEM_RELEASE);
    RtlFreeHeap(RtlGetProcessHeap(), 0, CpuArea->EmulatorData[0]);
    CpuArea->EmulatorData[0] = NULL;

    return STATUS_SUCCESS;
}

VOID
NTAPI
ProcessTerm(
    _In_ HANDLE ProcessHandle,
    _In_ BOOL After,
    _In_ NTSTATUS Status)
{
    UNREFERENCED_PARAMETER(ProcessHandle);
    UNREFERENCED_PARAMETER(After);
    UNREFERENCED_PARAMETER(Status);
}

/* The interpreter keeps no translations, there is nothing to throw away */
VOID
NTAPI
FlushInstructionCacheHeavy(
    _In_ PVOID Address,
    _In_ SIZE_T Size)
{
    UNREFERENCED_PARAMETER(Address);
    UNREFERENCED_PARAMETER(Size);
}

VOID
NTAPI
BTCpu64FlushInstructionCache(
    _In_ PVOID Address,
    _In_ SIZE_T Size)
{
    UNREFERENCED_PARAMETER(Address);
    UNREFERENCED_PARAMETER(Size);
}

VOID
NTAPI
BTCpu64NotifyMemoryDirty(
    _In_ PVOID Address,
    _In_ SIZE_T Size)
{
    UNREFERENCED_PARAMETER(Address);
    UNREFERENCED_PARAMETER(Size);
}

BOOLEAN
NTAPI
BTCpu64IsProcessorFeaturePresent(
    _In_ UINT Feature)
{
    /* Only what the interpreter does: plain x64 with SSE moves */
    return Feature == PF_XMMI_INSTRUCTIONS_AVAILABLE ||
           Feature == PF_XMMI64_INSTRUCTIONS_AVAILABLE ||
           Feature == PF_NX_ENABLED;
}

/* EOF */
