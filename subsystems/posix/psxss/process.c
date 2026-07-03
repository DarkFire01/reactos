/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Global POSIX process table
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Records are created when a POSIX image is spawned and looked up by ClientId when
 * that process connects to \PSXSS\ApiPort.
 */

#include "psxss.h"

LIST_ENTRY g_PsxProcessList;
RTL_CRITICAL_SECTION g_PsxProcessLock;

VOID
PsxInitProcessTable(VOID)
{
    InitializeListHead(&g_PsxProcessList);
    RtlInitializeCriticalSection(&g_PsxProcessLock);
}

PPSX_PROCESS
PsxAllocateProcess(VOID)
{
    PPSX_PROCESS Process;

    Process = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Process));
    return Process;
}

VOID
PsxInsertProcess(
    _Inout_ PPSX_PROCESS Process)
{
    RtlEnterCriticalSection(&g_PsxProcessLock);
    InsertTailList(&g_PsxProcessList, &Process->Entry);
    RtlLeaveCriticalSection(&g_PsxProcessLock);
}

VOID
PsxRemoveProcess(
    _Inout_ PPSX_PROCESS Process)
{
    RtlEnterCriticalSection(&g_PsxProcessLock);
    RemoveEntryList(&Process->Entry);
    RtlLeaveCriticalSection(&g_PsxProcessLock);
}

PPSX_PROCESS
PsxFindProcessByClientId(
    _In_ PCLIENT_ID ClientId)
{
    PLIST_ENTRY Entry;
    PPSX_PROCESS Process;
    PPSX_PROCESS Found = NULL;

    RtlEnterCriticalSection(&g_PsxProcessLock);
    for (Entry = g_PsxProcessList.Flink; Entry != &g_PsxProcessList; Entry = Entry->Flink)
    {
        Process = CONTAINING_RECORD(Entry, PSX_PROCESS, Entry);
        if (Process->ClientId.UniqueProcess == ClientId->UniqueProcess)
        {
            Found = Process;
            break;
        }
    }
    RtlLeaveCriticalSection(&g_PsxProcessLock);
    return Found;
}

PPSX_PROCESS
PsxFindProcessByPid(
    _In_ ULONG Pid)
{
    PLIST_ENTRY Entry;
    PPSX_PROCESS Process;
    PPSX_PROCESS Found = NULL;

    RtlEnterCriticalSection(&g_PsxProcessLock);
    for (Entry = g_PsxProcessList.Flink; Entry != &g_PsxProcessList; Entry = Entry->Flink)
    {
        Process = CONTAINING_RECORD(Entry, PSX_PROCESS, Entry);
        if (Process->Pid == Pid)
        {
            Found = Process;
            break;
        }
    }
    RtlLeaveCriticalSection(&g_PsxProcessLock);
    return Found;
}
