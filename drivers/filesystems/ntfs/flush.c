/*
 *  ReactOS kernel
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
 * FILE:             drivers/filesystems/ntfs/flush.c
 * PURPOSE:          NTFS filesystem driver
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
* @name NtfsFlushBuffers
* @implemented
*
* Handles IRP_MJ_FLUSH_BUFFERS.
*
* @param IrpContext
* Points to an NTFS_IRP_CONTEXT which describes the request
*
* @return
* STATUS_SUCCESS if everything reached the media, otherwise the status the
* storage stack returned.
*
* @remarks Writes go straight to the disk as they come in, so there is nothing
* of ours to flush yet; all this does is pass the request on so the storage
* stack empties its own caches. Once the cache manager is wired up this will
* also have to flush the file's data and the volume metadata.
*
*/
NTSTATUS
NtfsFlushBuffers(PNTFS_IRP_CONTEXT IrpContext)
{
    PDEVICE_EXTENSION DeviceExt;

    DPRINT("NtfsFlushBuffers(%p)\n", IrpContext);

    IrpContext->Irp->IoStatus.Information = 0;

    /* Nothing is mounted on the main device object */
    if (IrpContext->DeviceObject == NtfsGlobalData->DeviceObject)
    {
        return STATUS_SUCCESS;
    }

    DeviceExt = IrpContext->DeviceObject->DeviceExtension;

    if (DeviceExt->Flags & VCB_VOLUME_DISMOUNTED)
    {
        return STATUS_VOLUME_DISMOUNTED;
    }

    return NtfsFlushDevice(DeviceExt->StorageDevice);
}

/* EOF */
