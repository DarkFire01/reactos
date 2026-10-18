/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Performance counter providers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Nothing consumes performance counters, so registrations and instances are
 * accepted and kept only so they can be released again.
 */

/* INCLUDES ******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

#define TAG_PCW 'twcP'

/* What a registration or an instance hands back, never looked into by callers */
typedef struct _PCW_HANDLE_BLOCK
{
    ULONG Kind;
} PCW_HANDLE_BLOCK, *PPCW_HANDLE_BLOCK;

/* FUNCTIONS *****************************************************************/

/**
 * @brief Registers a counter set.
 *
 * @param[out] Registration
 * Receives the registration.
 *
 * @param[in] Info
 * Description of the counter set.
 */
NTSTATUS
NTAPI
PcwRegister(
    _Outptr_ PVOID *Registration,
    _In_ PVOID Info)
{
    PPCW_HANDLE_BLOCK Block;

    if (!Registration || !Info)
        return STATUS_INVALID_PARAMETER;

    Block = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Block), TAG_PCW);
    if (!Block)
        return STATUS_INSUFFICIENT_RESOURCES;

    Block->Kind = 1;
    *Registration = Block;
    return STATUS_SUCCESS;
}

/**
 * @brief Removes a counter set PcwRegister registered.
 */
VOID
NTAPI
PcwUnregister(
    _In_ PVOID Registration)
{
    if (Registration)
        ExFreePoolWithTag(Registration, TAG_PCW);
}

/**
 * @brief Adds an instance to a counter set.
 *
 * @param[out] Instance
 * Receives the instance.
 *
 * @param[in] Registration
 * Counter set from PcwRegister.
 *
 * @param[in] Name
 * Name of the instance.
 *
 * @param[in] Count
 * Number of data blocks.
 *
 * @param[in] Data
 * Data blocks the counters are read from.
 */
NTSTATUS
NTAPI
PcwCreateInstance(
    _Outptr_ PVOID *Instance,
    _In_ PVOID Registration,
    _In_ PCUNICODE_STRING Name,
    _In_ ULONG Count,
    _In_reads_(Count) PVOID Data)
{
    PPCW_HANDLE_BLOCK Block;

    UNREFERENCED_PARAMETER(Name);
    UNREFERENCED_PARAMETER(Count);
    UNREFERENCED_PARAMETER(Data);

    if (!Instance || !Registration)
        return STATUS_INVALID_PARAMETER;

    Block = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Block), TAG_PCW);
    if (!Block)
        return STATUS_INSUFFICIENT_RESOURCES;

    Block->Kind = 2;
    *Instance = Block;
    return STATUS_SUCCESS;
}

/**
 * @brief Removes an instance PcwCreateInstance added.
 */
VOID
NTAPI
PcwCloseInstance(
    _In_ PVOID Instance)
{
    if (Instance)
        ExFreePoolWithTag(Instance, TAG_PCW);
}
