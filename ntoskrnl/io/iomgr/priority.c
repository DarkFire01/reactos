/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     How urgent a request is, and how urgent the thread behind it is
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * A driver that hands work to a thread of its own wants that thread to run as
 * the thread that asked would have. It takes what the requester was running
 * under, puts it on the worker for the length of the work, and puts back what
 * the worker had.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* PRIVATE DEFINITIONS ********************************************************/

/*
 * An IRP keeps its hint one above the hint itself, so that a packet nobody
 * has spoken for reads as no hint rather than as the lowest one.
 */
#define IOP_PRIORITY_HINT_SHIFT 17
#define IOP_PRIORITY_HINT_MASK  0x000E0000

/* Said of a priority the caller does not want carried or put back */
#define IOP_PRIORITY_UNSPOKEN_FOR MAXULONG

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
IO_PRIORITY_HINT
NTAPI
IoGetIoPriorityHint(
    _In_ PIRP Irp)
{
    ULONG Hint = (Irp->Flags & IOP_PRIORITY_HINT_MASK) >> IOP_PRIORITY_HINT_SHIFT;

    if (Hint == 0)
        return IoPriorityNormal;

    return (IO_PRIORITY_HINT)(Hint - 1);
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
IoSetIoPriorityHint(
    _In_ PIRP Irp,
    _In_ IO_PRIORITY_HINT PriorityHint)
{
    if (PriorityHint >= MaxIoPriorityTypes)
        return STATUS_INVALID_PARAMETER;

    Irp->Flags &= ~IOP_PRIORITY_HINT_MASK;
    Irp->Flags |= ((ULONG)PriorityHint + 1) << IOP_PRIORITY_HINT_SHIFT;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reads back what a request, or the thread that made it, is running under.
 *
 * @remarks
 * Nothing here keeps a page priority per thread, so that one is answered as
 * nothing to put back rather than as a number that would mean something it
 * does not.
 */
NTSTATUS
NTAPI
IoRetrievePriorityInfo(
    _In_opt_ PIRP Irp,
    _In_opt_ PFILE_OBJECT FileObject,
    _In_opt_ PETHREAD Thread,
    _Inout_ PIO_PRIORITY_INFO PriorityInfo)
{
    /* A file nobody set a priority on says nothing about the request */
    UNREFERENCED_PARAMETER(FileObject);

    PriorityInfo->IoPriority = IoPriorityNormal;
    PriorityInfo->PagePriority = IOP_PRIORITY_UNSPOKEN_FOR;

    if (Irp != NULL && (Irp->Flags & IOP_PRIORITY_HINT_MASK) != 0)
        PriorityInfo->IoPriority = IoGetIoPriorityHint(Irp);

    if (Thread == NULL)
    {
        PriorityInfo->ThreadPriority = IOP_PRIORITY_UNSPOKEN_FOR;
        return STATUS_SUCCESS;
    }

    PriorityInfo->ThreadPriority = (ULONG)Thread->Tcb.BasePriority;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Runs a thread under what a request was made with, giving back what that
 * thread was running under.
 *
 * @return
 * STATUS_INVALID_PARAMETER_1 for priorities that were only initialized and
 * never read from anything, since putting those on a thread would be putting
 * on nothing.
 */
NTSTATUS
NTAPI
IoApplyPriorityInfoThread(
    _In_ PIO_PRIORITY_INFO InputPriorityInfo,
    _Out_opt_ PIO_PRIORITY_INFO OutputPriorityInfo,
    _Inout_ PETHREAD Thread)
{
    IO_PRIORITY_INFO Previous;

    if (InputPriorityInfo->ThreadPriority == 0xFFFF)
        return STATUS_INVALID_PARAMETER_1;

    Previous.Size = sizeof(Previous);
    Previous.IoPriority = IoPriorityNormal;
    Previous.PagePriority = IOP_PRIORITY_UNSPOKEN_FOR;
    Previous.ThreadPriority = IOP_PRIORITY_UNSPOKEN_FOR;

    if (InputPriorityInfo->ThreadPriority != IOP_PRIORITY_UNSPOKEN_FOR)
    {
        KPRIORITY Old;

        Old = KeSetActualBasePriorityThread(&Thread->Tcb,
                                            (KPRIORITY)InputPriorityInfo->ThreadPriority);
        Previous.ThreadPriority = (ULONG)Old;
    }

    if (OutputPriorityInfo != NULL)
        *OutputPriorityInfo = Previous;

    return STATUS_SUCCESS;
}

/* EOF */
