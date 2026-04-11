/*
 * PROJECT:     ReactOS Runtime Library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     RtlLookupFunctionTable / RtlLookupFunctionEntry for ARM64
 * COPYRIGHT:   Copyright 2010-2025 Timo Kreuzer <timo.kreuzer@reactos.org>
 */

#include <rtl.h>

#define NDEBUG
#include <debug.h>

static ULONG
RtlpRuntimeFunctionEndRva(_In_ PRUNTIME_FUNCTION Entry)
{
    ULONG u = Entry->UnwindData;

    if ((u & 3) == 0)
        return Entry->BeginAddress + ((u >> 2) & 0x7FF) * 4;

    /* Unpacked .xdata — minimal bring-up: treat as one instruction */
    return Entry->BeginAddress + 4;
}

PRUNTIME_FUNCTION
NTAPI
RtlLookupFunctionTable(
    _In_ DWORD64 ControlPc,
    _Out_ PDWORD64 ImageBase,
    _Out_ PULONG Length)
{
    PVOID Table;
    ULONG Size;

    if (!RtlPcToFileHeader((PVOID)ControlPc, (PVOID *)ImageBase))
        return NULL;

    Table = RtlImageDirectoryEntryToData((PVOID)*ImageBase,
                                         TRUE,
                                         IMAGE_DIRECTORY_ENTRY_EXCEPTION,
                                         &Size);

    *Length = Size / sizeof(RUNTIME_FUNCTION);
    return Table;
}

PRUNTIME_FUNCTION
NTAPI
RtlpLookupDynamicFunctionEntry(
    _In_ DWORD64 ControlPc,
    _Out_ PDWORD64 ImageBase,
    _In_opt_ PVOID HistoryTable);

PRUNTIME_FUNCTION
NTAPI
RtlLookupFunctionEntry(
    _In_ DWORD64 ControlPc,
    _Out_ PDWORD64 ImageBase,
    _In_opt_ PVOID HistoryTable)
{
    PRUNTIME_FUNCTION FunctionTable, FunctionEntry;
    ULONG TableLength;
    ULONG IndexLo, IndexHi, IndexMid;

    FunctionTable = RtlLookupFunctionTable(ControlPc, ImageBase, &TableLength);

    if (!FunctionTable)
        return RtlpLookupDynamicFunctionEntry(ControlPc, ImageBase, HistoryTable);

    ControlPc -= *ImageBase;

    IndexLo = 0;
    IndexHi = TableLength;
    while (IndexHi > IndexLo)
    {
        IndexMid = (IndexLo + IndexHi) / 2;
        FunctionEntry = &FunctionTable[IndexMid];

        if (ControlPc < FunctionEntry->BeginAddress)
        {
            IndexHi = IndexMid;
        }
        else if (ControlPc >= RtlpRuntimeFunctionEndRva(FunctionEntry))
        {
            IndexLo = IndexMid + 1;
        }
        else
        {
            return FunctionEntry;
        }
    }

    return NULL;
}
