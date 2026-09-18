/*
 * PROJECT:     ReactOS NVM Express Miniport Driver
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Submission and completion queue handling
 */

/* INCLUDES *******************************************************************/

#include "stornvme.h"

#define NDEBUG
#include <debug.h>


/* FUNCTIONS ******************************************************************/

/**
 * @brief Places a command in a submission queue and rings its doorbell.
 *
 * The caller owns the queue for the duration; nothing here serialises against
 * another submitter.
 */
VOID
NvmpSubmitCommand(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PNVME_QUEUE_PAIR Queue,
    _In_ PNVME_COMMAND Command)
{
    Queue->SubmissionQueue[Queue->SubmissionTail] = *Command;

    Queue->SubmissionTail++;
    if (Queue->SubmissionTail == Queue->Depth)
        Queue->SubmissionTail = 0;

    /* The command has to be in memory before the controller is told about it */
    KeMemoryBarrier();

    StorPortWriteRegisterUlong(Adapter,
                               NvmpSubmissionDoorbell(Adapter, Queue->QueueId),
                               Queue->SubmissionTail);
}


/**
 * @brief Takes the next completion off a queue, if the controller left one.
 *
 * @return TRUE when Completion was filled in.
 *
 * A completion belongs to this pass of the queue only when its phase matches
 * the one the queue is on, which is how a stale entry is told from a fresh
 * one without anybody having to clear the memory.
 */
BOOLEAN
NvmpNextCompletion(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PNVME_QUEUE_PAIR Queue,
    _Out_ PNVME_COMPLETION_ENTRY Completion)
{
    volatile NVME_COMPLETION_ENTRY *Entry;
    NVME_COMMAND_STATUS Status;

    Entry = &Queue->CompletionQueue[Queue->CompletionHead];

    /* Nothing the controller wrote is trustworthy until the phase is read */
    KeMemoryBarrier();

    Status.AsUshort = (USHORT)(Entry->DW3.AsUlong >> 16);
    if (Status.P != Queue->Phase)
        return FALSE;

    Completion->DW0 = Entry->DW0;
    Completion->DW1 = Entry->DW1;
    Completion->DW2.AsUlong = Entry->DW2.AsUlong;
    Completion->DW3.AsUlong = Entry->DW3.AsUlong;

    Queue->CompletionHead++;
    if (Queue->CompletionHead == Queue->Depth)
    {
        Queue->CompletionHead = 0;
        Queue->Phase ^= 1;
    }

    StorPortWriteRegisterUlong(Adapter,
                               NvmpCompletionDoorbell(Adapter, Queue->QueueId),
                               Queue->CompletionHead);

    return TRUE;
}


/**
 * @brief Runs one admin command to completion without interrupts.
 *
 * Only for bringing the controller up, where there is nothing else in flight
 * and no interrupt is connected yet.
 */
BOOLEAN
NvmpIssueAdminCommand(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PNVME_COMMAND Command,
    _Out_opt_ PNVME_COMPLETION_ENTRY Completion)
{
    NVME_COMPLETION_ENTRY Result;
    ULONG Attempts;
    ULONG Limit;

    /* Identify this command so its completion can be recognised */
    Command->CDW0.CID = Adapter->AdminCommandId++;

    NvmpSubmitCommand(Adapter, &Adapter->AdminQueue, Command);

    Limit = NVME_ADMIN_TIMEOUT_MS / (NVME_POLL_INTERVAL_US / 1000);

    for (Attempts = 0; ; Attempts++)
    {
        if (NvmpNextCompletion(Adapter, &Adapter->AdminQueue, &Result))
            break;

        if (Attempts >= Limit)
        {
            DPRINT1("Admin command 0x%02x timed out\n", Command->CDW0.OPC);
            return FALSE;
        }

        StorPortStallExecution(NVME_POLL_INTERVAL_US);
    }

    if (Result.DW3.CID != Command->CDW0.CID)
    {
        DPRINT1("Admin completion is for command %u, expected %u\n",
                Result.DW3.CID, Command->CDW0.CID);
        return FALSE;
    }

    if (Completion != NULL)
        *Completion = Result;

    if (Result.DW3.Status.SC != NVME_STATUS_SUCCESS_COMPLETION ||
        Result.DW3.Status.SCT != NVME_STATUS_TYPE_GENERIC_COMMAND)
    {
        DPRINT1("Admin command 0x%02x failed, type %u code 0x%02x\n",
                Command->CDW0.OPC, Result.DW3.Status.SCT, Result.DW3.Status.SC);
        return FALSE;
    }

    return TRUE;
}
