/*
 *  ReactOS kernel
 *  Copyright (C) 2008 ReactOS Team
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
 * FILE:             drivers/filesystem/ntfs/fastio.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMER:       Pierre Schweitzer
 */

/* INCLUDES *****************************************************************/

#include <ntddk.h>

#define NDEBUG
#include <debug.h>

#include "ntfs.h"

/* FUNCTIONS ****************************************************************/

/*
 * FUNCTION: Acquires a file for the cache manager's lazy writer
 *
 * Context is the FCB handed to CcInitializeCacheMap(). The flush that follows arrives as
 * paging writes, which take PagingIoResource, so that is the one acquired here; ERESOURCE
 * allows the recursive acquisition that results.
 */
BOOLEAN
NTAPI
NtfsAcqLazyWrite(PVOID Context,
                 BOOLEAN Wait)
{
    PNTFS_FCB Fcb = (PNTFS_FCB)Context;

    ASSERT(Fcb != NULL);

    if (!ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, Wait))
        return FALSE;

    /* Mark the writes that follow as the cache manager's rather than a caller's */
    IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);

    return TRUE;
}


VOID
NTAPI
NtfsRelLazyWrite(PVOID Context)
{
    PNTFS_FCB Fcb = (PNTFS_FCB)Context;

    ASSERT(Fcb != NULL);

    IoSetTopLevelIrp(NULL);

    ExReleaseResourceLite(&Fcb->PagingIoResource);
}


/*
 * FUNCTION: Acquires a file for the cache manager's read-ahead
 *
 * Read-ahead only reads, so the file is taken shared. The page-ins it issues arrive as paging
 * reads, which take no resource of their own.
 */
BOOLEAN
NTAPI
NtfsAcqReadAhead(PVOID Context,
                 BOOLEAN Wait)
{
    PNTFS_FCB Fcb = (PNTFS_FCB)Context;

    ASSERT(Fcb != NULL);

    return ExAcquireResourceSharedLite(&Fcb->MainResource, Wait);
}


VOID
NTAPI
NtfsRelReadAhead(PVOID Context)
{
    PNTFS_FCB Fcb = (PNTFS_FCB)Context;

    ASSERT(Fcb != NULL);

    ExReleaseResourceLite(&Fcb->MainResource);
}

BOOLEAN
NTAPI
NtfsFastIoCheckIfPossible(
    _In_ PFILE_OBJECT FileObject,
    _In_ PLARGE_INTEGER FileOffset,
    _In_ ULONG Length,
    _In_ BOOLEAN Wait,
    _In_ ULONG LockKey,
    _In_ BOOLEAN CheckForReadOperation,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    /* Deny FastIo */
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(LockKey);
    UNREFERENCED_PARAMETER(CheckForReadOperation);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);
    return FALSE;
}

BOOLEAN
NTAPI
NtfsFastIoRead(
    _In_ PFILE_OBJECT FileObject,
    _In_ PLARGE_INTEGER FileOffset,
    _In_ ULONG Length,
    _In_ BOOLEAN Wait,
    _In_ ULONG LockKey,
    _Out_ PVOID Buffer,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    DBG_UNREFERENCED_PARAMETER(FileObject);
    DBG_UNREFERENCED_PARAMETER(FileOffset);
    DBG_UNREFERENCED_PARAMETER(Length);
    DBG_UNREFERENCED_PARAMETER(Wait);
    DBG_UNREFERENCED_PARAMETER(LockKey);
    DBG_UNREFERENCED_PARAMETER(Buffer);
    DBG_UNREFERENCED_PARAMETER(IoStatus);
    DBG_UNREFERENCED_PARAMETER(DeviceObject);
    return FALSE;
}

BOOLEAN
NTAPI
NtfsFastIoWrite(
    _In_ PFILE_OBJECT FileObject,
    _In_ PLARGE_INTEGER FileOffset,
    _In_ ULONG Length,
    _In_ BOOLEAN Wait,
    _In_ ULONG LockKey,
    _In_ PVOID Buffer,
    _Out_ PIO_STATUS_BLOCK IoStatus,
    _In_ PDEVICE_OBJECT DeviceObject)
{
    DBG_UNREFERENCED_PARAMETER(FileObject);
    DBG_UNREFERENCED_PARAMETER(FileOffset);
    DBG_UNREFERENCED_PARAMETER(Length);
    DBG_UNREFERENCED_PARAMETER(Wait);
    DBG_UNREFERENCED_PARAMETER(LockKey);
    DBG_UNREFERENCED_PARAMETER(Buffer);
    DBG_UNREFERENCED_PARAMETER(IoStatus);
    DBG_UNREFERENCED_PARAMETER(DeviceObject);
    return FALSE;
}

/* EOF */
