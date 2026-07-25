/*
 *  ReactOS kernel
 *  Copyright (C) 2016 ReactOS Team
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
 * FILE:             drivers/filesystem/ntfs/cleanup.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMER:       Pierre Schweitzer (pierre@reactos.org)
 * UPDATE HISTORY:
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/*
 * FUNCTION: Cleans up a file
 */
NTSTATUS
NtfsCleanupFile(PDEVICE_EXTENSION DeviceExt,
                PFILE_OBJECT FileObject,
                BOOLEAN CanWait)
{
    PNTFS_FCB Fcb;

    DPRINT("NtfsCleanupFile(DeviceExt %p, FileObject %p, CanWait %u)\n",
           DeviceExt,
           FileObject,
           CanWait);

    Fcb = (PNTFS_FCB)(FileObject->FsContext);
    if (!Fcb)
        return STATUS_SUCCESS;

    if (Fcb->Flags & FCB_IS_VOLUME)
    {
        Fcb->OpenHandleCount--;

        if (Fcb->OpenHandleCount != 0)
        {
            // Remove share access when handled
        }
    }
    else
    {
        if (!ExAcquireResourceExclusiveLite(&Fcb->MainResource, CanWait))
        {
            return STATUS_PENDING;
        }

        Fcb->OpenHandleCount--;

        /* The last handle to a file marked for deletion is where it actually goes. Truncate it
         * first so the cache manager isn't left holding pages for a file that no longer has
         * clusters, then take the file itself apart. */
        if (Fcb->OpenHandleCount == 0 &&
            BooleanFlagOn(Fcb->Flags, FCB_DELETE_PENDING))
        {
            LARGE_INTEGER Zero;
            NTSTATUS Status;

            Zero.QuadPart = 0;

            if (!NtfsFCBIsDirectory(Fcb))
            {
                Fcb->RFCB.FileSize = Zero;
                Fcb->RFCB.ValidDataLength = Zero;
                CcSetFileSizes(FileObject, (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize);
            }

            Status = NtfsDeleteFileRecord(DeviceExt, Fcb->MFTIndex, FALSE);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("ERROR: Failed to delete '%wS' (MFT record %I64u), Status %lx\n",
                        Fcb->ObjectName, Fcb->MFTIndex, Status);
            }

            Fcb->Flags &= ~FCB_DELETE_PENDING;
        }

        CcUninitializeCacheMap(FileObject, &Fcb->RFCB.FileSize, NULL);

        if (Fcb->OpenHandleCount != 0)
        {
            // Remove share access when handled
        }

        FileObject->Flags |= FO_CLEANUP_COMPLETE;

        ExReleaseResourceLite(&Fcb->MainResource);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NtfsCleanup(PNTFS_IRP_CONTEXT IrpContext)
{
    PDEVICE_EXTENSION DeviceExtension;
    PFILE_OBJECT FileObject;
    NTSTATUS Status;
    PDEVICE_OBJECT DeviceObject;

    DPRINT("NtfsCleanup() called\n");

    DeviceObject = IrpContext->DeviceObject;
    if (DeviceObject == NtfsGlobalData->DeviceObject)
    {
        DPRINT("Cleaning up file system\n");
        IrpContext->Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    FileObject = IrpContext->FileObject;
    DeviceExtension = DeviceObject->DeviceExtension;

    if (!ExAcquireResourceExclusiveLite(&DeviceExtension->DirResource,
                                        BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    Status = NtfsCleanupFile(DeviceExtension, FileObject, BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT));

    ExReleaseResourceLite(&DeviceExtension->DirResource);

    if (Status == STATUS_PENDING)
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    IrpContext->Irp->IoStatus.Information = 0;
    return Status;
}
