/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Ex functions of Vista+
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ntoskrnl_vista.h"
#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

/**
 * @brief
 * Queues an executive work item without waiting on the queue.
 *
 * @param[in,out] WorkItem
 * The work item to queue.
 *
 * @param[in] QueueType
 * The system work queue to use.
 *
 * @return
 * FALSE, as the work item is never queued.
 *
 * @unimplemented
 */
BOOLEAN
NTAPI
ExTryQueueWorkItem(
    _Inout_ PWORK_QUEUE_ITEM WorkItem,
    _In_ WORK_QUEUE_TYPE QueueType)
{
    UNREFERENCED_PARAMETER(WorkItem);
    UNREFERENCED_PARAMETER(QueueType);

    return FALSE;
}

/* EOF */
