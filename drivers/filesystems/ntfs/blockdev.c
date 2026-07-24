/*
 *  ReactOS kernel
 *  Copyright (C) 2002 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/blockdev.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMERS:      Eric Kohl
 *                   Trevor Thompson
 */

/*
 * This is the volume I/O layer. Everything the driver reads from or writes to
 * the underlying storage device goes through NtfsPerformIoRuns().
 *
 * A request is described as a list of runs (NTFS_IO_RUN_LIST), each run being
 * one physically contiguous piece of the transfer. All the runs of a request
 * are issued to the storage stack at once and the caller waits a single time
 * for all of them, rather than paying a full round trip per run. This is the
 * same shape fastfat (FatNonCachedIo -> FatSingleAsync/FatMultipleAsync) and
 * Windows' own ntfs.sys (NtfsNonCachedIo -> NtfsSingleAsync/NtfsMultipleAsync)
 * use.
 *
 * The IRPs are built with IoAllocateIrp() rather than IoBuildSynchronousFsdRequest().
 * The latter binds the IRP to the calling thread's IRP list and signals
 * completion through a thread APC, which is wrong for a file system: we issue
 * these from worker threads, from the cache manager and from the paging path,
 * where the issuing thread is not necessarily the one that should own or wait
 * on the IRP. Owning the IRP ourselves also lets the completion routine return
 * STATUS_MORE_PROCESSING_REQUIRED, so nothing above us can complete or cancel
 * an IRP we are still waiting on.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/*
 * Run list management. The run list starts out using storage embedded in the
 * caller's stack frame and only spills into pool for heavily fragmented files.
 */

VOID
NtfsInitIoRunList(OUT PNTFS_IO_RUN_LIST RunList)
{
    RunList->Runs = RunList->StackRuns;
    RunList->Capacity = NTFS_MAX_IO_RUNS_ON_STACK;
    RunList->Count = 0;
    RunList->TotalLength = 0;
}

/**
* @name NtfsAddIoRun
* @implemented
*
* Appends one contiguous piece to a request's run list.
*
* @param RunList
* The list being built, previously initialized by NtfsInitIoRunList()
*
* @param Lbo
* Byte offset on the volume of this piece, or NTFS_SPARSE_LBO if the piece has
* no backing storage
*
* @param ByteCount
* Size of this piece, in bytes
*
* @return
* STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES if the list had to grow and
* the allocation failed.
*
* @remarks Runs tile the caller's buffer in the order they are added, so the
* buffer offset is implied and must not be passed in. A piece that continues
* the previous one is merged into it, which keeps physically contiguous files
* down to a single IRP.
*
*/
NTSTATUS
NtfsAddIoRun(IN OUT PNTFS_IO_RUN_LIST RunList,
             IN LONGLONG Lbo,
             IN ULONG ByteCount)
{
    PNTFS_IO_RUN LastRun;
    PNTFS_IO_RUN NewRuns;
    ULONG NewCapacity;

    if (ByteCount == 0)
        return STATUS_SUCCESS;

    /* Does this piece simply continue the previous one? */
    if (RunList->Count != 0)
    {
        LastRun = &RunList->Runs[RunList->Count - 1];

        if (LastRun->ByteCount <= MAXULONG - ByteCount &&
            ((Lbo == NTFS_SPARSE_LBO && LastRun->Lbo == NTFS_SPARSE_LBO) ||
             (Lbo != NTFS_SPARSE_LBO && LastRun->Lbo != NTFS_SPARSE_LBO &&
              LastRun->Lbo + LastRun->ByteCount == Lbo)))
        {
            LastRun->ByteCount += ByteCount;
            RunList->TotalLength += ByteCount;
            return STATUS_SUCCESS;
        }
    }

    if (RunList->Count == RunList->Capacity)
    {
        NewCapacity = RunList->Capacity * 2;

        /* Non-paged: a run list stays live across the transfer, and a request
         * on the paging path must not fault while walking it. */
        NewRuns = ExAllocatePoolWithTag(NonPagedPool,
                                        NewCapacity * sizeof(NTFS_IO_RUN),
                                        TAG_IO_RUNS);
        if (NewRuns == NULL)
        {
            DPRINT1("Not enough memory to grow the I/O run list!\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlCopyMemory(NewRuns, RunList->Runs, RunList->Count * sizeof(NTFS_IO_RUN));

        if (RunList->Runs != RunList->StackRuns)
        {
            ExFreePoolWithTag(RunList->Runs, TAG_IO_RUNS);
        }

        RunList->Runs = NewRuns;
        RunList->Capacity = NewCapacity;
    }

    LastRun = &RunList->Runs[RunList->Count];
    LastRun->Lbo = Lbo;
    LastRun->BufferOffset = RunList->TotalLength;
    LastRun->ByteCount = ByteCount;
    LastRun->SavedIrp = NULL;

    RunList->Count++;
    RunList->TotalLength += ByteCount;

    return STATUS_SUCCESS;
}

VOID
NtfsFreeIoRunList(IN OUT PNTFS_IO_RUN_LIST RunList)
{
    if (RunList->Runs != RunList->StackRuns && RunList->Runs != NULL)
    {
        ExFreePoolWithTag(RunList->Runs, TAG_IO_RUNS);
    }

    RunList->Runs = NULL;
    RunList->Count = 0;
    RunList->Capacity = 0;
    RunList->TotalLength = 0;
}

/*
 * Completion routine shared by every run of a request.
 *
 * We allocated the IRP and (usually) its MDL, and nothing above us holds a
 * reference to either, so we tear them down here and return
 * STATUS_MORE_PROCESSING_REQUIRED to stop the I/O manager from completing the
 * IRP behind us. The last run to complete wakes the issuer.
 *
 * Runs at up to DISPATCH_LEVEL, in an arbitrary thread.
 */
static
NTSTATUS
NTAPI
NtfsIoRunCompletionRoutine(IN PDEVICE_OBJECT DeviceObject,
                           IN PIRP Irp,
                           IN PVOID Context)
{
    PNTFS_IO_CONTEXT IoContext = (PNTFS_IO_CONTEXT)Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (NT_SUCCESS(Irp->IoStatus.Status))
    {
        InterlockedExchangeAdd(&IoContext->BytesTransferred,
                               (LONG)Irp->IoStatus.Information);
    }
    else
    {
        /* First error wins; STATUS_SUCCESS is the "nothing failed yet" sentinel. */
        InterlockedCompareExchange(&IoContext->Status,
                                   Irp->IoStatus.Status,
                                   STATUS_SUCCESS);
    }

    /* The master MDL belongs to the issuer and outlives every run built from it. */
    if (Irp->MdlAddress != NULL && Irp->MdlAddress != IoContext->MasterMdl)
    {
        IoFreeMdl(Irp->MdlAddress);
    }

    IoFreeIrp(Irp);

    if (InterlockedDecrement(&IoContext->IrpCount) == 0)
    {
        KeSetEvent(&IoContext->SyncEvent, IO_NO_INCREMENT, FALSE);
    }

    return STATUS_MORE_PROCESSING_REQUIRED;
}

/*
 * Builds, but does not issue, the IRP for a single run. Leaves it in
 * Run->SavedIrp. Every IRP of a request is built before any of them is issued,
 * so that a failure part-way through can be unwound without any I/O in flight.
 */
static
NTSTATUS
NtfsBuildIoRunIrp(IN PDEVICE_OBJECT DeviceObject,
                  IN PNTFS_IO_CONTEXT IoContext,
                  IN UCHAR MajorFunction,
                  IN PUCHAR Buffer,
                  IN ULONG BufferLength,
                  IN OUT PNTFS_IO_RUN Run,
                  IN BOOLEAN Override)
{
    PIO_STACK_LOCATION Stack;
    PIRP Irp;
    PMDL Mdl;

    Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (Irp == NULL)
    {
        DPRINT1("IoAllocateIrp failed!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* A run that spans the whole buffer can use the master MDL as-is. Anything
     * else needs a partial MDL describing just its slice of it. */
    if (Run->BufferOffset == 0 && Run->ByteCount == BufferLength)
    {
        Mdl = IoContext->MasterMdl;
    }
    else
    {
        Mdl = IoAllocateMdl(Buffer + Run->BufferOffset,
                            Run->ByteCount,
                            FALSE,
                            FALSE,
                            NULL);
        if (Mdl == NULL)
        {
            DPRINT1("IoAllocateMdl failed!\n");
            IoFreeIrp(Irp);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        IoBuildPartialMdl(IoContext->MasterMdl,
                          Mdl,
                          Buffer + Run->BufferOffset,
                          Run->ByteCount);
    }

    Irp->MdlAddress = Mdl;
    Irp->Flags |= IRP_NOCACHE;

    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = MajorFunction;
    Stack->Parameters.Read.Length = Run->ByteCount;
    Stack->Parameters.Read.ByteOffset.QuadPart = Run->Lbo;

    if (Override)
    {
        Stack->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    IoSetCompletionRoutine(Irp,
                           NtfsIoRunCompletionRoutine,
                           IoContext,
                           TRUE,
                           TRUE,
                           TRUE);

    Run->SavedIrp = Irp;

    return STATUS_SUCCESS;
}

/*
 * Issues every non-sparse run of RunList in parallel against Buffer and waits
 * for all of them. Sparse runs are satisfied by zeroing, without touching the
 * storage stack.
 *
 * Buffer must be a system-space address covering RunList->TotalLength bytes,
 * and every run's Lbo and ByteCount must already be sector-aligned; the
 * alignment fixups live in NtfsPerformIoRuns().
 */
static
NTSTATUS
NtfsIssueIoRuns(IN PDEVICE_OBJECT DeviceObject,
                IN UCHAR MajorFunction,
                IN OUT PUCHAR Buffer,
                IN PNTFS_IO_RUN_LIST RunList,
                IN BOOLEAN Override,
                OUT PULONG BytesTransferred)
{
    NTFS_IO_CONTEXT IoContext;
    NTSTATUS Status = STATUS_SUCCESS;
    PNTFS_IO_RUN Run;
    ULONG DiskRunCount = 0;
    ULONG Index;
    ULONG BuiltCount = 0;
    PIRP Irp;

    *BytesTransferred = 0;

    RtlZeroMemory(&IoContext, sizeof(IoContext));
    KeInitializeEvent(&IoContext.SyncEvent, NotificationEvent, FALSE);
    IoContext.Status = STATUS_SUCCESS;

    /* Satisfy the sparse runs up front, and find out whether there is any real
     * I/O left to do. */
    for (Index = 0; Index < RunList->Count; Index++)
    {
        Run = &RunList->Runs[Index];

        if (Run->Lbo != NTFS_SPARSE_LBO)
        {
            DiskRunCount++;
            continue;
        }

        if (MajorFunction != IRP_MJ_READ)
        {
            DPRINT1("FIXME: Writing to sparse files is not supported yet!\n");
            return STATUS_NOT_IMPLEMENTED;
        }

        RtlZeroMemory(Buffer + Run->BufferOffset, Run->ByteCount);
        *BytesTransferred += Run->ByteCount;
    }

    if (DiskRunCount == 0)
    {
        return STATUS_SUCCESS;
    }

    /* Lock the buffer down once. Every run either uses this MDL directly or
     * carries a partial MDL cut from it. */
    IoContext.MasterMdl = IoAllocateMdl(Buffer, RunList->TotalLength, FALSE, FALSE, NULL);
    if (IoContext.MasterMdl == NULL)
    {
        DPRINT1("IoAllocateMdl failed!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    _SEH2_TRY
    {
        MmProbeAndLockPages(IoContext.MasterMdl,
                            KernelMode,
                            (MajorFunction == IRP_MJ_READ) ? IoWriteAccess : IoReadAccess);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("MmProbeAndLockPages failed (Status %lx)\n", Status);
        IoFreeMdl(IoContext.MasterMdl);
        return Status;
    }

    /* Build everything before issuing anything, so an allocation failure here
     * never leaves us tearing down an IRP that is already in flight. */
    for (Index = 0; Index < RunList->Count; Index++)
    {
        Run = &RunList->Runs[Index];

        if (Run->Lbo == NTFS_SPARSE_LBO)
            continue;

        Status = NtfsBuildIoRunIrp(DeviceObject,
                                   &IoContext,
                                   MajorFunction,
                                   Buffer,
                                   RunList->TotalLength,
                                   Run,
                                   Override);
        if (!NT_SUCCESS(Status))
            break;

        BuiltCount++;
    }

    if (!NT_SUCCESS(Status))
    {
        for (Index = 0; Index < RunList->Count; Index++)
        {
            Run = &RunList->Runs[Index];

            if (Run->SavedIrp == NULL)
                continue;

            if (Run->SavedIrp->MdlAddress != NULL &&
                Run->SavedIrp->MdlAddress != IoContext.MasterMdl)
            {
                IoFreeMdl(Run->SavedIrp->MdlAddress);
            }

            IoFreeIrp(Run->SavedIrp);
            Run->SavedIrp = NULL;
        }

        MmUnlockPages(IoContext.MasterMdl);
        IoFreeMdl(IoContext.MasterMdl);
        return Status;
    }

    /* Publish the count before the first completion routine can run. */
    IoContext.IrpCount = BuiltCount;

    for (Index = 0; Index < RunList->Count; Index++)
    {
        Run = &RunList->Runs[Index];

        if (Run->SavedIrp == NULL)
            continue;

        /* The completion routine frees the IRP, so let go of our pointer to it
         * before handing it down. */
        Irp = Run->SavedIrp;
        Run->SavedIrp = NULL;

        /* If the lower driver fails the IRP it completes it, and our completion
         * routine picks the error up like any other I/O failure. */
        (VOID)IoCallDriver(DeviceObject, Irp);
    }

    KeWaitForSingleObject(&IoContext.SyncEvent, Executive, KernelMode, FALSE, NULL);

    MmUnlockPages(IoContext.MasterMdl);
    IoFreeMdl(IoContext.MasterMdl);

    Status = (NTSTATUS)IoContext.Status;
    if (NT_SUCCESS(Status))
    {
        *BytesTransferred += (ULONG)IoContext.BytesTransferred;
    }

    return Status;
}

/**
* @name NtfsPerformIoRuns
* @implemented
*
* Reads or writes the pieces described by RunList, all in parallel.
*
* @param DeviceObject
* Storage device to transfer to or from
*
* @param MajorFunction
* IRP_MJ_READ or IRP_MJ_WRITE
*
* @param SectorSize
* Sector size the storage device requires transfers to be aligned to
*
* @param Buffer
* System-space buffer of RunList->TotalLength bytes, tiled by the runs
*
* @param RunList
* The pieces to transfer. Consumed by this call: the runs are rewritten in
* place to satisfy the device's alignment requirements.
*
* @param Override
* Whether to set SL_OVERRIDE_VERIFY_VOLUME on the requests
*
* @param BytesTransferred
* Optionally receives how much of the request was satisfied
*
* @return
* STATUS_SUCCESS on success, STATUS_INSUFFICIENT_RESOURCES if an allocation
* failed, STATUS_NOT_IMPLEMENTED for a write to a sparse run, or whatever
* status the storage stack returned.
*
* @remarks If the request does not start and end on a sector boundary it is
* rounded outwards and performed through a bounce buffer, which for a write
* means a read-modify-write of the two edge sectors. Because data runs are
* cluster-aligned and clusters are a whole number of sectors, rounding a
* request outwards can never reach past the run it belongs to.
*
*/
NTSTATUS
NtfsPerformIoRuns(IN PDEVICE_OBJECT DeviceObject,
                  IN UCHAR MajorFunction,
                  IN ULONG SectorSize,
                  IN OUT PUCHAR Buffer,
                  IN PNTFS_IO_RUN_LIST RunList,
                  IN BOOLEAN Override,
                  OUT PULONG BytesTransferred OPTIONAL)
{
    NTSTATUS Status;
    PNTFS_IO_RUN FirstRun;
    PNTFS_IO_RUN LastRun;
    PUCHAR BounceBuffer;
    LONGLONG EndLbo;
    ULONG FrontPad = 0;
    ULONG BackPad = 0;
    ULONG BounceLength;
    ULONG Transferred = 0;
    ULONG Index;
    ULONG Length;

    if (BytesTransferred != NULL)
        *BytesTransferred = 0;

    if (RunList->Count == 0 || RunList->TotalLength == 0)
        return STATUS_SUCCESS;

    ASSERT(SectorSize != 0);

    Length = RunList->TotalLength;
    FirstRun = &RunList->Runs[0];
    LastRun = &RunList->Runs[RunList->Count - 1];

    /* Only the first run can start unaligned, and only the last can end
     * unaligned: every run in between begins and ends on a cluster boundary. */
    if (FirstRun->Lbo != NTFS_SPARSE_LBO)
    {
        FrontPad = (ULONG)(FirstRun->Lbo % SectorSize);
    }

    if (LastRun->Lbo != NTFS_SPARSE_LBO)
    {
        EndLbo = LastRun->Lbo + LastRun->ByteCount;
        BackPad = (ULONG)(ROUND_UP(EndLbo, (LONGLONG)SectorSize) - EndLbo);
    }

    if (FrontPad == 0 && BackPad == 0)
    {
        Status = NtfsIssueIoRuns(DeviceObject,
                                 MajorFunction,
                                 Buffer,
                                 RunList,
                                 Override,
                                 &Transferred);

        if (BytesTransferred != NULL)
            *BytesTransferred = min(Transferred, Length);

        return Status;
    }

    BounceLength = FrontPad + Length + BackPad;

    BounceBuffer = ExAllocatePoolWithTag(NonPagedPool, BounceLength, TAG_NTFS);
    if (BounceBuffer == NULL)
    {
        DPRINT1("Not enough memory for an unaligned transfer!\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Grow the request outwards to cover whole sectors. Everything after the
     * first run slides along by the amount we prepended. */
    for (Index = 1; Index < RunList->Count; Index++)
    {
        RunList->Runs[Index].BufferOffset += FrontPad;
    }

    FirstRun->BufferOffset = 0;
    FirstRun->ByteCount += FrontPad;
    if (FirstRun->Lbo != NTFS_SPARSE_LBO)
        FirstRun->Lbo -= FrontPad;

    LastRun->ByteCount += BackPad;
    RunList->TotalLength = BounceLength;

    Status = STATUS_SUCCESS;

    if (MajorFunction != IRP_MJ_READ)
    {
        /* Read-modify-write: we are about to write whole sectors, so the bytes
         * of the edge sectors that the caller is not replacing have to be read
         * back first or we would put pool garbage on the disk. */
        if (FrontPad != 0)
        {
            Status = NtfsReadDisk(DeviceObject,
                                  FirstRun->Lbo,
                                  SectorSize,
                                  SectorSize,
                                  BounceBuffer,
                                  Override);
        }

        /* Skip this only when the read above already fetched the very same
         * sector -- that is, when there was a front read at all and the
         * whole transfer fits inside one sector. */
        if (NT_SUCCESS(Status) &&
            BackPad != 0 &&
            (FrontPad == 0 || BounceLength > SectorSize))
        {
            Status = NtfsReadDisk(DeviceObject,
                                  LastRun->Lbo + LastRun->ByteCount - SectorSize,
                                  SectorSize,
                                  SectorSize,
                                  BounceBuffer + BounceLength - SectorSize,
                                  Override);
        }

        if (NT_SUCCESS(Status))
        {
            RtlCopyMemory(BounceBuffer + FrontPad, Buffer, Length);
        }
    }

    if (NT_SUCCESS(Status))
    {
        Status = NtfsIssueIoRuns(DeviceObject,
                                 MajorFunction,
                                 BounceBuffer,
                                 RunList,
                                 Override,
                                 &Transferred);

        if (NT_SUCCESS(Status))
        {
            if (MajorFunction == IRP_MJ_READ)
            {
                RtlCopyMemory(Buffer, BounceBuffer + FrontPad, Length);
            }

            if (BytesTransferred != NULL)
            {
                /* The padding is ours, not the caller's. */
                Transferred = (Transferred > FrontPad) ? Transferred - FrontPad : 0;
                *BytesTransferred = min(Transferred, Length);
            }
        }
    }

    /* The bounce buffer can hold user data; do not leave it in pool. */
    RtlSecureZeroMemory(BounceBuffer, BounceLength);
    ExFreePoolWithTag(BounceBuffer, TAG_NTFS);

    return Status;
}

/*
 * Completion routine for an IRP we did not allocate and must not free: the
 * caller's own request, handed straight to the storage stack.
 *
 * Returning STATUS_MORE_PROCESSING_REQUIRED stops the I/O manager from
 * completing it, leaving ownership with the file system so that the dispatch
 * path can finish it in the usual way.
 */
static
NTSTATUS
NTAPI
NtfsForwardedIrpCompletionRoutine(IN PDEVICE_OBJECT DeviceObject,
                                  IN PIRP Irp,
                                  IN PVOID Context)
{
    PNTFS_IO_CONTEXT IoContext = (PNTFS_IO_CONTEXT)Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    IoContext->Status = Irp->IoStatus.Status;
    IoContext->BytesTransferred = (LONG)Irp->IoStatus.Information;

    KeSetEvent(&IoContext->SyncEvent, IO_NO_INCREMENT, FALSE);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

/*
 * Hands the caller's IRP directly to the storage stack for one contiguous run.
 *
 * Nothing is allocated and nothing is copied: the disk transfers straight into
 * (or out of) the pages the caller's MDL already describes. This is what
 * fastfat's FatSingleAsync() and ntfs.sys' NtfsSingleAsync() do, and it is the
 * common case for any file that is not fragmented.
 */
static
NTSTATUS
NtfsForwardIrpToRun(IN PDEVICE_OBJECT DeviceObject,
                    IN UCHAR MajorFunction,
                    IN PIRP Irp,
                    IN PNTFS_IO_RUN Run,
                    IN BOOLEAN Override,
                    OUT PULONG BytesTransferred)
{
    NTFS_IO_CONTEXT IoContext;
    PIO_STACK_LOCATION Stack;

    DPRINT("Forwarding IRP %p for a single run at %I64x, %lu bytes\n",
           Irp, Run->Lbo, Run->ByteCount);

    RtlZeroMemory(&IoContext, sizeof(IoContext));
    KeInitializeEvent(&IoContext.SyncEvent, NotificationEvent, FALSE);
    IoContext.Status = STATUS_SUCCESS;

    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MajorFunction = MajorFunction;
    Stack->MinorFunction = 0;
    Stack->FileObject = NULL;
    Stack->Parameters.Read.Length = Run->ByteCount;
    Stack->Parameters.Read.ByteOffset.QuadPart = Run->Lbo;
    Stack->Flags = Override ? SL_OVERRIDE_VERIFY_VOLUME : 0;

    /* Must come last: it takes over Control on this same stack location. */
    IoSetCompletionRoutine(Irp,
                           NtfsForwardedIrpCompletionRoutine,
                           &IoContext,
                           TRUE,
                           TRUE,
                           TRUE);

    (VOID)IoCallDriver(DeviceObject, Irp);

    KeWaitForSingleObject(&IoContext.SyncEvent, Executive, KernelMode, FALSE, NULL);

    *BytesTransferred = (ULONG)IoContext.BytesTransferred;

    return (NTSTATUS)IoContext.Status;
}

/**
* @name NtfsPerformIrpIoRuns
* @implemented
*
* Transfers the runs of a request that originated from an IRP, forwarding that
* IRP straight to the storage stack when the request allows it.
*
* @param DeviceObject
* Storage device to transfer to or from
*
* @param MajorFunction
* IRP_MJ_READ or IRP_MJ_WRITE
*
* @param SectorSize
* Sector size the storage device requires transfers to be aligned to
*
* @param Irp
* The request being served. May be NULL, in which case this is exactly
* NtfsPerformIoRuns().
*
* @param Buffer
* System-space mapping of the IRP's buffer, used whenever the request cannot
* simply be forwarded
*
* @param RunList
* The pieces to transfer
*
* @param Override
* Whether to set SL_OVERRIDE_VERIFY_VOLUME on the requests
*
* @param BytesTransferred
* Receives how much of the request was satisfied
*
* @return
* STATUS_SUCCESS on success, otherwise the status of the failing transfer.
*
* @remarks Forwarding is only possible when the whole request is one aligned,
* non-sparse run that fits the IRP's MDL. Anything else -- a fragmented file, a
* hole, an unaligned edge -- falls back to the gather/scatter path, which
* allocates its own IRPs but still issues every run in parallel.
*
*/
NTSTATUS
NtfsPerformIrpIoRuns(IN PDEVICE_OBJECT DeviceObject,
                     IN UCHAR MajorFunction,
                     IN ULONG SectorSize,
                     IN PIRP Irp,
                     IN OUT PUCHAR Buffer,
                     IN PNTFS_IO_RUN_LIST RunList,
                     IN BOOLEAN Override,
                     OUT PULONG BytesTransferred)
{
    if (Irp != NULL &&
        Irp->MdlAddress != NULL &&
        RunList->Count == 1 &&
        RunList->Runs[0].Lbo != NTFS_SPARSE_LBO &&
        (RunList->Runs[0].Lbo % SectorSize) == 0 &&
        (RunList->Runs[0].ByteCount % SectorSize) == 0 &&
        RunList->Runs[0].ByteCount <= MmGetMdlByteCount(Irp->MdlAddress))
    {
        return NtfsForwardIrpToRun(DeviceObject,
                                   MajorFunction,
                                   Irp,
                                   &RunList->Runs[0],
                                   Override,
                                   BytesTransferred);
    }

    return NtfsPerformIoRuns(DeviceObject,
                             MajorFunction,
                             SectorSize,
                             Buffer,
                             RunList,
                             Override,
                             BytesTransferred);
}

/**
* @name NtfsReadDisk
* @implemented
*
* Reads a single contiguous range from the given DeviceObject.
*
* @param DeviceObject
* Device to read from
*
* @param StartingOffset
* Offset, in bytes, from the start of the device object
*
* @param Length
* How much data to read, in bytes
*
* @param SectorSize
* Size of the sector on the disk that the read must be aligned to
*
* @param Buffer
* System-space buffer receiving the data
*
* @param Override
* Whether to set SL_OVERRIDE_VERIFY_VOLUME on the request
*
* @return
* STATUS_SUCCESS in case of success, STATUS_INSUFFICIENT_RESOURCES if a memory
* allocation failed, or whatever status the storage stack returned.
*
*/
NTSTATUS
NtfsReadDisk(IN PDEVICE_OBJECT DeviceObject,
             IN LONGLONG StartingOffset,
             IN ULONG Length,
             IN ULONG SectorSize,
             IN OUT PUCHAR Buffer,
             IN BOOLEAN Override)
{
    NTFS_IO_RUN_LIST RunList;
    NTSTATUS Status;
    ULONG Transferred;

    DPRINT("NtfsReadDisk(%p, %I64x, %lu, %lu, %p, %d)\n",
           DeviceObject, StartingOffset, Length, SectorSize, Buffer, Override);

    if (Length == 0)
        return STATUS_SUCCESS;

    NtfsInitIoRunList(&RunList);

    Status = NtfsAddIoRun(&RunList, StartingOffset, Length);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsPerformIoRuns(DeviceObject,
                               IRP_MJ_READ,
                               SectorSize,
                               Buffer,
                               &RunList,
                               Override,
                               &Transferred);

    NtfsFreeIoRunList(&RunList);

    if (NT_SUCCESS(Status) && Transferred != Length)
    {
        DPRINT1("Short read: asked for %lu bytes, got %lu\n", Length, Transferred);
        Status = STATUS_UNEXPECTED_IO_ERROR;
    }

    DPRINT("NtfsReadDisk() done (Status %x)\n", Status);

    return Status;
}

/**
* @name NtfsWriteDisk
* @implemented
*
* Writes data from the given buffer to the given DeviceObject.
*
* @param DeviceObject
* Device to write to
*
* @param StartingOffset
* Offset, in bytes, from the start of the device object where the data will be written
*
* @param Length
* How much data will be written, in bytes
*
* @param SectorSize
* Size of the sector on the disk that the write must be aligned to
*
* @param Buffer
* The data that's being written to the device
*
* @return
* STATUS_SUCCESS in case of success, STATUS_INSUFFICIENT_RESOURCES if a memory
* allocation failed, or whatever status the storage stack returned.
*
* @remarks May perform a read-modify-write operation if the requested write is
* not sector-aligned.
*
*/
NTSTATUS
NtfsWriteDisk(IN PDEVICE_OBJECT DeviceObject,
              IN LONGLONG StartingOffset,
              IN ULONG Length,
              IN ULONG SectorSize,
              IN const PUCHAR Buffer)
{
    NTFS_IO_RUN_LIST RunList;
    NTSTATUS Status;
    ULONG Transferred;

    DPRINT("NtfsWriteDisk(%p, %I64x, %lu, %lu, %p)\n",
           DeviceObject, StartingOffset, Length, SectorSize, Buffer);

    if (Length == 0)
        return STATUS_SUCCESS;

    NtfsInitIoRunList(&RunList);

    Status = NtfsAddIoRun(&RunList, StartingOffset, Length);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsPerformIoRuns(DeviceObject,
                               IRP_MJ_WRITE,
                               SectorSize,
                               Buffer,
                               &RunList,
                               FALSE,
                               &Transferred);

    NtfsFreeIoRunList(&RunList);

    if (NT_SUCCESS(Status) && Transferred != Length)
    {
        DPRINT1("Short write: asked for %lu bytes, wrote %lu\n", Length, Transferred);
        Status = STATUS_UNEXPECTED_IO_ERROR;
    }

    DPRINT("NtfsWriteDisk() done (Status %x)\n", Status);

    return Status;
}

NTSTATUS
NtfsReadSectors(IN PDEVICE_OBJECT DeviceObject,
                IN ULONG DiskSector,
                IN ULONG SectorCount,
                IN ULONG SectorSize,
                IN OUT PUCHAR Buffer,
                IN BOOLEAN Override)
{
    LONGLONG Offset;
    ULONG BlockSize;

    Offset = (LONGLONG)DiskSector * (LONGLONG)SectorSize;
    BlockSize = SectorCount * SectorSize;

    return NtfsReadDisk(DeviceObject, Offset, BlockSize, SectorSize, Buffer, Override);
}


NTSTATUS
NtfsDeviceIoControl(IN PDEVICE_OBJECT DeviceObject,
                    IN ULONG ControlCode,
                    IN PVOID InputBuffer,
                    IN ULONG InputBufferSize,
                    IN OUT PVOID OutputBuffer,
                    IN OUT PULONG OutputBufferSize,
                    IN BOOLEAN Override)
{
    PIO_STACK_LOCATION Stack;
    IO_STATUS_BLOCK IoStatus;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    IoStatus.Status = STATUS_SUCCESS;
    IoStatus.Information = 0;

    DPRINT("Building device I/O control request ...\n");
    Irp = IoBuildDeviceIoControlRequest(ControlCode,
                                        DeviceObject,
                                        InputBuffer,
                                        InputBufferSize,
                                        OutputBuffer,
                                        (OutputBufferSize) ? *OutputBufferSize : 0,
                                        FALSE,
                                        &Event,
                                        &IoStatus);
    if (Irp == NULL)
    {
        DPRINT("IoBuildDeviceIoControlRequest() failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Override)
    {
        Stack = IoGetNextIrpStackLocation(Irp);
        Stack->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
    }

    DPRINT("Calling IO Driver... with irp %p\n", Irp);
    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (OutputBufferSize)
    {
        *OutputBufferSize = IoStatus.Information;
    }

    return Status;
}

/* EOF */
