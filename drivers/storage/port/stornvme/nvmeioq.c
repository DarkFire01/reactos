/*
 * PROJECT:     ReactOS NVM Express Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Input and output queue creation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "stornvme.h"

#define NDEBUG
#include <debug.h>


/* FUNCTIONS ******************************************************************/

/**
 * @brief Asks the controller for the queue pairs this driver wants.
 *
 * Both the request and the answer are zero based counts, and a controller is
 * free to grant fewer than were asked for. It may not be asked twice without
 * a reset in between.
 */
static
BOOLEAN
NvmpRequestQueueCount(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ USHORT Wanted,
    _Out_ PUSHORT Granted)
{
    NVME_COMPLETION_ENTRY Completion;
    NVME_CDW11_FEATURE_NUMBER_OF_QUEUES Answer;
    NVME_COMMAND Command;

    *Granted = 0;

    RtlZeroMemory(&Command, sizeof(Command));

    Command.CDW0.OPC = NVME_ADMIN_COMMAND_SET_FEATURES;
    Command.u.SETFEATURES.CDW10.FID = NVME_FEATURE_NUMBER_OF_QUEUES;
    Command.u.SETFEATURES.CDW11.NumberOfQueues.NSQ = Wanted - 1;
    Command.u.SETFEATURES.CDW11.NumberOfQueues.NCQ = Wanted - 1;

    if (!NvmpIssueAdminCommand(Adapter, &Command, &Completion))
        return FALSE;

    Answer.AsUlong = Completion.DW0;

    /* Take the smaller of the two, since a pair needs one of each */
    *Granted = Answer.NSQ;
    if (Answer.NCQ < Answer.NSQ)
        *Granted = Answer.NCQ;
    (*Granted)++;

    DPRINT1("Controller granted %u submission and %u completion queues\n",
            (USHORT)(Answer.NSQ + 1), (USHORT)(Answer.NCQ + 1));

    return TRUE;
}


/**
 * @brief Creates the completion queue of a pair.
 *
 * The vector is an index into the messages the adapter was given. With a
 * single message everything lands on vector zero, which is also where the
 * admin queue reports.
 */
static
BOOLEAN
NvmpCreateCompletionQueue(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PNVME_QUEUE_PAIR Queue,
    _In_ USHORT Vector)
{
    NVME_COMMAND Command;

    RtlZeroMemory(&Command, sizeof(Command));

    Command.CDW0.OPC = NVME_ADMIN_COMMAND_CREATE_IO_CQ;
    Command.PRP1 = Queue->CompletionAddress.QuadPart;
    Command.u.CREATEIOCQ.CDW10.QID = Queue->QueueId;
    Command.u.CREATEIOCQ.CDW10.QSIZE = Queue->Depth - 1;

    /* The queue is one contiguous run of memory and it does raise interrupts */
    Command.u.CREATEIOCQ.CDW11.PC = 1;
    Command.u.CREATEIOCQ.CDW11.IEN = 1;
    Command.u.CREATEIOCQ.CDW11.IV = Vector;

    return NvmpIssueAdminCommand(Adapter, &Command, NULL);
}


/**
 * @brief Creates the submission queue of a pair, bound to its completion queue.
 */
static
BOOLEAN
NvmpCreateSubmissionQueue(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PNVME_QUEUE_PAIR Queue)
{
    NVME_COMMAND Command;

    RtlZeroMemory(&Command, sizeof(Command));

    Command.CDW0.OPC = NVME_ADMIN_COMMAND_CREATE_IO_SQ;
    Command.PRP1 = Queue->SubmissionAddress.QuadPart;
    Command.u.CREATEIOSQ.CDW10.QID = Queue->QueueId;
    Command.u.CREATEIOSQ.CDW10.QSIZE = Queue->Depth - 1;

    Command.u.CREATEIOSQ.CDW11.PC = 1;
    Command.u.CREATEIOSQ.CDW11.QPRIO = NVME_NVM_QUEUE_PRIORITY_MEDIUM;
    Command.u.CREATEIOSQ.CDW11.CQID = Queue->QueueId;

    return NvmpIssueAdminCommand(Adapter, &Command, NULL);
}


BOOLEAN
NvmpCreateIoQueues(
    _In_ PNVME_ADAPTER_EXTENSION Adapter)
{
    PNVME_QUEUE_PAIR Queue = &Adapter->IoQueue;
    USHORT Granted;

    if (!NvmpRequestQueueCount(Adapter, 1, &Granted))
    {
        DPRINT1("Controller would not say how many queues it has\n");
        return FALSE;
    }

    if (Granted == 0)
    {
        DPRINT1("Controller granted no queue pair\n");
        return FALSE;
    }

    /*
     * The completion queue has to exist before a submission queue can name
     * it, so the order here is not free.
     */
    if (!NvmpCreateCompletionQueue(Adapter, Queue, 0))
    {
        DPRINT1("Could not create the completion queue\n");
        return FALSE;
    }

    if (!NvmpCreateSubmissionQueue(Adapter, Queue))
    {
        DPRINT1("Could not create the submission queue\n");
        return FALSE;
    }

    DPRINT1("Queue pair %u of %lu entries is up\n", Queue->QueueId, Queue->Depth);

    return TRUE;
}
