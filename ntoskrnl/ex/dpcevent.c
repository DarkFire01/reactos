/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Events a deferred procedure call can wait on
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A DPC event lets code that cannot block ask to be called back once an event
 * is signalled: the wait is queued, and the DPC runs when it completes.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* TYPES **********************************************************************/

typedef struct _EX_DPC_EVENT
{
    KEVENT Event;
    KDPC Dpc;
    PKWAIT_BLOCK WaitBlock;
    BOOLEAN Queued;
} EX_DPC_EVENT, *PEX_DPC_EVENT;

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Creates an event a DPC can be attached to.
 *
 * @param[out] DpcEvent
 * Receives the object, which ExDeleteDpcEvent frees.
 *
 * @param[out] Event
 * Receives the event to signal.
 *
 * @param[out] Dpc
 * Receives the DPC that the caller fills in and that runs once the event is
 * signalled.
 */
NTSTATUS
NTAPI
ExCreateDpcEvent(
    _Out_ PVOID *DpcEvent,
    _Out_ PKEVENT *Event,
    _Out_ PKDPC *Dpc)
{
    PEX_DPC_EVENT Entry;

    Entry = ExAllocatePoolZero(NonPagedPool, sizeof(EX_DPC_EVENT), TAG_EX_DPC_EVENT);
    if (Entry == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    KeInitializeEvent(&Entry->Event, NotificationEvent, FALSE);

    *DpcEvent = Entry;
    *Event = &Entry->Event;
    *Dpc = &Entry->Dpc;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Frees an event created by ExCreateDpcEvent.
 */
VOID
NTAPI
ExDeleteDpcEvent(
    _In_ PVOID DpcEvent)
{
    PEX_DPC_EVENT Entry = DpcEvent;

    ASSERT(!Entry->Queued);

    ExFreePoolWithTag(Entry, TAG_EX_DPC_EVENT);
}

/**
 * @brief
 * Asks for the DPC to run once the event is signalled.
 *
 * @param[in] Timeout
 * How long to wait, or NULL to wait for as long as it takes.
 *
 * @remarks
 * ReactOS has no way to queue a wait without a thread to do the waiting, so
 * an event that is already signalled queues the DPC straight away and one
 * that is not is reported as not waitable. A caller that gets FALSE does the
 * work itself, which is the path this takes until a waiting thread exists.
 */
BOOLEAN
NTAPI
ExQueueDpcEventWait(
    _In_ PVOID DpcEvent,
    _In_opt_ PLARGE_INTEGER Timeout)
{
    PEX_DPC_EVENT Entry = DpcEvent;
    LARGE_INTEGER Immediate;

    UNREFERENCED_PARAMETER(Timeout);

    Immediate.QuadPart = 0;

    if (KeWaitForSingleObject(&Entry->Event,
                              Executive,
                              KernelMode,
                              FALSE,
                              &Immediate) != STATUS_SUCCESS)
    {
        return FALSE;
    }

    Entry->Queued = TRUE;
    KeInsertQueueDpc(&Entry->Dpc, NULL, NULL);

    return TRUE;
}

/**
 * @brief
 * Takes back a wait queued with ExQueueDpcEventWait.
 *
 * @return
 * TRUE when the wait was taken back before its DPC ran.
 */
BOOLEAN
NTAPI
ExCancelDpcEventWait(
    _In_ PVOID DpcEvent)
{
    PEX_DPC_EVENT Entry = DpcEvent;

    if (!Entry->Queued)
        return FALSE;

    Entry->Queued = FALSE;

    return KeRemoveQueueDpc(&Entry->Dpc);
}

/* EOF */
