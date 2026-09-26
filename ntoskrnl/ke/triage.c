/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Triage dump data arrays
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Prepares a caller supplied buffer to collect address ranges that a triage
 * dump should carry.
 *
 * @param[out] KtriageDumpDataArray
 * The buffer to prepare.
 *
 * @param[in] Size
 * Length of the buffer in bytes, which has to hold the header and room for at
 * least one block.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_BUFFER_TOO_SMALL for a buffer that cannot hold a
 * single block.
 */
NTSTATUS
NTAPI
KeInitializeTriageDumpDataArray(
    _Out_writes_bytes_(Size) PKTRIAGE_DUMP_DATA_ARRAY KtriageDumpDataArray,
    _In_ ULONG Size)
{
    ULONG Blocks;

    if ((KtriageDumpDataArray == NULL) ||
        (Size < (FIELD_OFFSET(KTRIAGE_DUMP_DATA_ARRAY, Blocks) + sizeof(KADDRESS_RANGE))))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    Blocks = (Size - FIELD_OFFSET(KTRIAGE_DUMP_DATA_ARRAY, Blocks)) / sizeof(KADDRESS_RANGE);

    RtlZeroMemory(KtriageDumpDataArray, Size);
    InitializeListHead(&KtriageDumpDataArray->List);
    KtriageDumpDataArray->NumBlocksTotal = Blocks;
    KtriageDumpDataArray->MaxDataSize = KE_MAX_TRIAGE_DUMP_DATA_MEMORY_SIZE;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Adds one address range to a triage dump data array.
 *
 * @return
 * STATUS_SUCCESS, STATUS_INVALID_PARAMETER for an empty range, or
 * STATUS_INSUFFICIENT_RESOURCES when the array is full or the total would run
 * past what a triage dump may carry.
 */
NTSTATUS
NTAPI
KeAddTriageDumpDataBlock(
    _Inout_ PKTRIAGE_DUMP_DATA_ARRAY KtriageDumpDataArray,
    _In_reads_bytes_(Size) PVOID Address,
    _In_ SIZE_T Size)
{
    PKADDRESS_RANGE Block;

    if ((KtriageDumpDataArray == NULL) || (Address == NULL) || (Size == 0))
        return STATUS_INVALID_PARAMETER;

    if (KtriageDumpDataArray->NumBlocksUsed >= KtriageDumpDataArray->NumBlocksTotal)
        return STATUS_INSUFFICIENT_RESOURCES;

    if ((KtriageDumpDataArray->DataSize + Size) > KtriageDumpDataArray->MaxDataSize)
        return STATUS_INSUFFICIENT_RESOURCES;

    Block = &KtriageDumpDataArray->Blocks[KtriageDumpDataArray->NumBlocksUsed];
    Block->Address = Address;
    Block->Size = Size;

    KtriageDumpDataArray->NumBlocksUsed++;
    KtriageDumpDataArray->DataSize += (ULONG)Size;

    return STATUS_SUCCESS;
}

/* EOF */
