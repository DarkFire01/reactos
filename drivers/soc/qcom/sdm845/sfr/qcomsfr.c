/*
 * PROJECT:     ReactOS Qualcomm SDM845 drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Prints the failure reasons the remote processors leave in shared memory
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>

/* Shared memory carve out, as the PEP describes it */
#define SFR_SMEM_BASE               0x86000000ULL
#define SFR_SMEM_SIZE               0x00200000

#define SFR_POLL_MS                 1000
#define SFR_TEXT_MAX                256

/* Header layout: four proc comm slots, then the version words, the heap info and the table */
#define SFR_VERSION_OFFSET          0x40
#define SFR_VERSION_BOOT_INDEX      7
#define SFR_TOC_OFFSET              0xD0
#define SFR_TOC_ENTRY_SIZE          16
#define SFR_TOC_ITEMS               512

/* From version 12 on the items live in a global partition named by the table at the end */
#define SFR_VERSION_PARTITIONED     12
#define SFR_PTABLE_SIZE             0x1000
#define SFR_PTABLE_MAGIC            0x434F5424
#define SFR_PTABLE_HEADER_SIZE      32
#define SFR_PTABLE_ENTRY_SIZE       48
#define SFR_PARTITION_MAGIC         0x54525024
#define SFR_PARTITION_HEADER_SIZE   32
#define SFR_GLOBAL_HOST             0xFFFE
#define SFR_ENTRY_CANARY            0xA5A5
#define SFR_ENTRY_HEADER_SIZE       16

typedef struct _SFR_SOURCE
{
    ULONG Item;
    PCSTR Name;
    CHAR Last[SFR_TEXT_MAX];
} SFR_SOURCE, *PSFR_SOURCE;

static SFR_SOURCE SfrSources[] =
{
    { 421, "modem" },
    { 423, "adsp" },
    { 601, "cdsp" },
    { 422, "wlan" },
};

static PUCHAR SfrSmem;

DRIVER_INITIALIZE DriverEntry;

static
ULONG
SfrRead32(
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(SfrSmem + Offset));
}

static
USHORT
SfrRead16(
    _In_ ULONG Offset)
{
    return READ_REGISTER_USHORT((PUSHORT)(SfrSmem + Offset));
}

/* Walks the uncached list of the global partition */
static
BOOLEAN
SfrFindPartitioned(
    _In_ ULONG Item,
    _Out_ PULONG Data,
    _Out_ PULONG Size)
{
    ULONG Table = SFR_SMEM_SIZE - SFR_PTABLE_SIZE;
    ULONG Count, Index, Entry;
    ULONG Partition = 0, PartitionSize = 0, End, Cursor;
    ULONG EntrySize;
    USHORT PadData, PadHeader;

    if (SfrRead32(Table) != SFR_PTABLE_MAGIC)
        return FALSE;

    Count = SfrRead32(Table + 8);
    for (Index = 0; Index < Count; Index++)
    {
        Entry = Table + SFR_PTABLE_HEADER_SIZE + Index * SFR_PTABLE_ENTRY_SIZE;
        if (Entry + SFR_PTABLE_ENTRY_SIZE > SFR_SMEM_SIZE)
            break;

        if (SfrRead16(Entry + 12) == SFR_GLOBAL_HOST && SfrRead16(Entry + 14) == SFR_GLOBAL_HOST)
        {
            Partition = SfrRead32(Entry);
            PartitionSize = SfrRead32(Entry + 4);
            break;
        }
    }

    if (!PartitionSize || Partition >= SFR_SMEM_SIZE || PartitionSize > SFR_SMEM_SIZE - Partition)
        return FALSE;
    if (SfrRead32(Partition) != SFR_PARTITION_MAGIC)
        return FALSE;

    End = Partition + SfrRead32(Partition + 12);
    if (End > Partition + PartitionSize)
        End = Partition + PartitionSize;

    for (Cursor = Partition + SFR_PARTITION_HEADER_SIZE; Cursor + SFR_ENTRY_HEADER_SIZE <= End;)
    {
        if (SfrRead16(Cursor) != SFR_ENTRY_CANARY)
            return FALSE;

        EntrySize = SfrRead32(Cursor + 4);
        PadData = SfrRead16(Cursor + 8);
        PadHeader = SfrRead16(Cursor + 10);

        if (SfrRead16(Cursor + 2) == Item)
        {
            *Data = Cursor + SFR_ENTRY_HEADER_SIZE + PadHeader;
            *Size = (EntrySize > PadData) ? EntrySize - PadData : 0;
            return *Data + *Size <= End;
        }

        Cursor += SFR_ENTRY_HEADER_SIZE + PadHeader + EntrySize;
    }

    return FALSE;
}

/* Older layouts keep every item in the table of contents of the global heap */
static
BOOLEAN
SfrFindHeap(
    _In_ ULONG Item,
    _Out_ PULONG Data,
    _Out_ PULONG Size)
{
    ULONG Entry = SFR_TOC_OFFSET + Item * SFR_TOC_ENTRY_SIZE;

    if (Item >= SFR_TOC_ITEMS || SfrRead32(Entry) != 1 || SfrRead32(Entry + 12) != 0)
        return FALSE;

    *Data = SfrRead32(Entry + 4);
    *Size = SfrRead32(Entry + 8);
    return *Data < SFR_SMEM_SIZE && *Size <= SFR_SMEM_SIZE - *Data;
}

static
VOID
SfrCheck(
    _Inout_ PSFR_SOURCE Source,
    _In_ ULONG Version)
{
    CHAR Text[SFR_TEXT_MAX];
    ULONG Data, Size, Index;
    BOOLEAN Found;

    if (Version >= SFR_VERSION_PARTITIONED)
        Found = SfrFindPartitioned(Source->Item, &Data, &Size);
    else
        Found = SfrFindHeap(Source->Item, &Data, &Size);

    if (!Found)
        return;

    for (Index = 0; Index < Size && Index < sizeof(Text) - 1; Index++)
    {
        Text[Index] = (CHAR)READ_REGISTER_UCHAR(SfrSmem + Data + Index);
        if (Text[Index] == ANSI_NULL)
            break;
    }
    Text[Index] = ANSI_NULL;

    if (Text[0] == ANSI_NULL || strcmp(Text, Source->Last) == 0)
        return;

    RtlCopyMemory(Source->Last, Text, sizeof(Text));
    DbgPrint("[qcomsfr] %s failure reason: %s\n", Source->Name, Text);
}

static
VOID
NTAPI
SfrThread(
    _In_ PVOID Context)
{
    LARGE_INTEGER Delay;
    ULONG Version;
    ULONG Index;

    UNREFERENCED_PARAMETER(Context);

    Version = SfrRead32(SFR_VERSION_OFFSET + SFR_VERSION_BOOT_INDEX * sizeof(ULONG)) >> 16;
    DbgPrint("[qcomsfr] shared memory version %lu, watching the failure reasons\n", Version);

    Delay.QuadPart = -(LONGLONG)SFR_POLL_MS * 10000;
    for (;;)
    {
        for (Index = 0; Index < RTL_NUMBER_OF(SfrSources); Index++)
            SfrCheck(&SfrSources[Index], Version);

        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    PHYSICAL_ADDRESS Base;
    HANDLE Thread;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    Base.QuadPart = SFR_SMEM_BASE;
    SfrSmem = MmMapIoSpace(Base, SFR_SMEM_SIZE, MmNonCached);
    if (!SfrSmem)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, SfrThread, NULL);
    if (!NT_SUCCESS(Status))
    {
        MmUnmapIoSpace(SfrSmem, SFR_SMEM_SIZE);
        return Status;
    }

    ZwClose(Thread);
    return STATUS_SUCCESS;
}
