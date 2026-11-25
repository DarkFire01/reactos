/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hardware entropy sources feeding the kernel random number generator
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "cngp.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

#define CNG_ENTROPY_TAG 'nECK'

typedef
VOID
(NTAPI *PCNG_ENTROPY_CALLBACK)(
    _In_ PVOID SourceHandle,
    _In_opt_ PVOID Context);

typedef struct _CNG_ENTROPY_SOURCE
{
    ULONG Type;
    PCNG_ENTROPY_CALLBACK Callback;
    PVOID Context;
    WORK_QUEUE_ITEM WorkItem;
    KEVENT Idle;
} CNG_ENTROPY_SOURCE, *PCNG_ENTROPY_SOURCE;

/* FUNCTIONS ******************************************************************/

/**
 * @brief Asks a source for a block of entropy from a worker thread.
 */
static
VOID
NTAPI
CngEntropyWorker(
    _In_ PVOID Parameter)
{
    PCNG_ENTROPY_SOURCE Source = Parameter;

    Source->Callback(Source, Source->Context);
    KeSetEvent(&Source->Idle, IO_NO_INCREMENT, FALSE);
}

/**
 * @brief Registers a hardware entropy source.
 *
 * @param[out] SourceHandle
 * Receives the handle the source provides data and unregisters with.
 */
NTSTATUS
NTAPI
EntropyRegisterSource(
    _Out_ PVOID *SourceHandle,
    _In_ ULONG SourceType,
    _In_opt_ PCWSTR SourceName)
{
    PCNG_ENTROPY_SOURCE Source;

    if (!SourceHandle)
        return STATUS_INVALID_PARAMETER;

    Source = ExAllocatePoolZero(NonPagedPool, sizeof(*Source), CNG_ENTROPY_TAG);
    if (!Source)
        return STATUS_INSUFFICIENT_RESOURCES;

    Source->Type = SourceType;
    KeInitializeEvent(&Source->Idle, NotificationEvent, TRUE);
    ExInitializeWorkItem(&Source->WorkItem, CngEntropyWorker, Source);

    DPRINT1("Entropy source %p registered, type %lu, %S\n", Source, SourceType, SourceName ? SourceName : L"");
    *SourceHandle = Source;
    return STATUS_SUCCESS;
}

/**
 * @brief Sets the routine the RNG calls when it wants entropy from a source.
 *
 * @remarks The source is asked once right away to seed the generator.
 */
NTSTATUS
NTAPI
EntropyRegisterCallback(
    _In_ PVOID SourceHandle,
    _In_ PCNG_ENTROPY_CALLBACK Callback,
    _In_opt_ PVOID Context)
{
    PCNG_ENTROPY_SOURCE Source = SourceHandle;

    if (!Source || !Callback || Source->Callback)
        return STATUS_INVALID_PARAMETER;

    Source->Callback = Callback;
    Source->Context = Context;

    KeClearEvent(&Source->Idle);
    ExQueueWorkItem(&Source->WorkItem, DelayedWorkQueue);
    return STATUS_SUCCESS;
}

/**
 * @brief Takes a block of entropy from a source and reseeds the generator with it.
 */
NTSTATUS
NTAPI
EntropyProvideData(
    _In_ PVOID SourceHandle,
    _In_reads_bytes_(Length) const VOID *Data,
    _In_ SIZE_T Length,
    _In_ ULONG EntropyEstimateInMilliBits)
{
    UNREFERENCED_PARAMETER(EntropyEstimateInMilliBits);

    if (!SourceHandle || !Data || !Length)
        return STATUS_INVALID_PARAMETER;

    CngMixEntropy(Data, Length);
    return STATUS_SUCCESS;
}

/**
 * @brief Removes a source, waiting out a request still running on it.
 */
NTSTATUS
NTAPI
EntropyUnregisterSource(
    _In_ PVOID SourceHandle)
{
    PCNG_ENTROPY_SOURCE Source = SourceHandle;

    if (!Source)
        return STATUS_INVALID_PARAMETER;

    KeWaitForSingleObject(&Source->Idle, Executive, KernelMode, FALSE, NULL);
    ExFreePoolWithTag(Source, CNG_ENTROPY_TAG);
    return STATUS_SUCCESS;
}
