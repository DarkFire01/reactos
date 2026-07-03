/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The global POSIX process table, keyed by NT ClientId. psxss
 *              pre-creates a PSX_PROCESS when it spawns a POSIX image (so it can
 *              tag it with a session id and identity); the process's psxdll then
 *              FINDS that record when it connects to \PSXSS\ApiPort, rather than
 *              the server creating one at connect. Faithful to the NT 4.0 model
 *              (psxss LookupProcessByClientId / sub_1F44B10).
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"

LIST_ENTRY           g_PsxProcessList;
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
    return RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PSX_PROCESS));
}

VOID
PsxInsertProcess(IN PPSX_PROCESS Process)
{
    RtlEnterCriticalSection(&g_PsxProcessLock);
    InsertTailList(&g_PsxProcessList, &Process->Entry);
    RtlLeaveCriticalSection(&g_PsxProcessLock);
}

VOID
PsxRemoveProcess(IN PPSX_PROCESS Process)
{
    RtlEnterCriticalSection(&g_PsxProcessLock);
    RemoveEntryList(&Process->Entry);
    RtlLeaveCriticalSection(&g_PsxProcessLock);
}

PPSX_PROCESS
PsxFindProcessByClientId(IN PCLIENT_ID ClientId)
{
    PLIST_ENTRY Entry;
    PPSX_PROCESS Found = NULL;

    RtlEnterCriticalSection(&g_PsxProcessLock);
    for (Entry = g_PsxProcessList.Flink; Entry != &g_PsxProcessList; Entry = Entry->Flink)
    {
        PPSX_PROCESS Process = CONTAINING_RECORD(Entry, PSX_PROCESS, Entry);
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
PsxFindProcessByPid(IN ULONG Pid)
{
    PLIST_ENTRY Entry;
    PPSX_PROCESS Found = NULL;

    RtlEnterCriticalSection(&g_PsxProcessLock);
    for (Entry = g_PsxProcessList.Flink; Entry != &g_PsxProcessList; Entry = Entry->Flink)
    {
        PPSX_PROCESS Process = CONTAINING_RECORD(Entry, PSX_PROCESS, Entry);
        if (Process->Pid == Pid)
        {
            Found = Process;
            break;
        }
    }
    RtlLeaveCriticalSection(&g_PsxProcessLock);
    return Found;
}
