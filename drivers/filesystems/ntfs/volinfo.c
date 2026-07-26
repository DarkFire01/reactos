/*
 *  ReactOS kernel
 *  Copyright (C) 2002, 2014 ReactOS Team
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
 * FILE:             drivers/filesystem/ntfs/volume.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMERS:      Eric Kohl
 *                   Pierre Schweitzer (pierre@reactos.org)
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
* @name NtfsMapVolumeBitmap
* @implemented
*
* Makes the volume $Bitmap available in memory and opens its data attribute for writing.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION.
*
* @param BitmapRecord
* Receives the $Bitmap file record. Pass it back to NtfsUnmapVolumeBitmap() when done.
*
* @param DataContext
* Receives the context of $Bitmap's data attribute, for writing changes back.
*
* @return
* STATUS_SUCCESS on success.
*
* @remarks
* The bitmap itself is read once and kept on the VCB. Callers work on DeviceExt->VolumeBitmap
* directly and write their changes back with NtfsFlushVolumeBitmapRange().
*/
NTSTATUS
NtfsMapVolumeBitmap(PDEVICE_EXTENSION DeviceExt,
                    PFILE_RECORD_HEADER *BitmapRecord,
                    PNTFS_ATTR_CONTEXT *DataContext)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT Context;
    ULONGLONG BitmapDataSize;

    *BitmapRecord = NULL;
    *DataContext = NULL;

    Record = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (Record == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(DeviceExt, NTFS_FILE_BITMAP, Record);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, Record);
        return Status;
    }

    Status = FindAttribute(DeviceExt, Record, AttributeData, L"", 0, &Context, NULL);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, Record);
        return Status;
    }

    if (!DeviceExt->VolumeBitmapValid)
    {
        BitmapDataSize = AttributeDataLength(Context->pRecord);
        BitmapDataSize = min(BitmapDataSize, 0xffffffff);
        ASSERT((BitmapDataSize * 8) >= DeviceExt->NtfsInfo.ClusterCount);

        /* RtlInitializeBitMap() wants a ULONG-aligned pointer, so allow for the adjustment */
        DeviceExt->VolumeBitmapAllocation =
            ExAllocatePoolWithTag(NonPagedPool,
                                  ROUND_UP(BitmapDataSize, DeviceExt->NtfsInfo.BytesPerSector) +
                                      sizeof(ULONG),
                                  TAG_NTFS);
        if (DeviceExt->VolumeBitmapAllocation == NULL)
        {
            ReleaseAttributeContext(Context);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, Record);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        DeviceExt->VolumeBitmapData =
            (PUCHAR)ALIGN_UP_BY((ULONG_PTR)DeviceExt->VolumeBitmapAllocation, sizeof(ULONG));
        DeviceExt->VolumeBitmapSize = (ULONG)BitmapDataSize;

        if (ReadAttribute(DeviceExt, Context, 0, (PCHAR)DeviceExt->VolumeBitmapData,
                          DeviceExt->VolumeBitmapSize) != DeviceExt->VolumeBitmapSize)
        {
            DPRINT1("ERROR: Couldn't read the volume bitmap!\n");
            ExFreePoolWithTag(DeviceExt->VolumeBitmapAllocation, TAG_NTFS);
            DeviceExt->VolumeBitmapAllocation = NULL;
            DeviceExt->VolumeBitmapData = NULL;
            ReleaseAttributeContext(Context);
            ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, Record);
            return STATUS_UNSUCCESSFUL;
        }

        RtlInitializeBitMap(&DeviceExt->VolumeBitmap,
                            (PULONG)DeviceExt->VolumeBitmapData,
                            (ULONG)DeviceExt->NtfsInfo.ClusterCount);

        DeviceExt->VolumeBitmapValid = TRUE;
    }

    *BitmapRecord = Record;
    *DataContext = Context;

    return STATUS_SUCCESS;
}

/**
* @name NtfsUnmapVolumeBitmap
* @implemented
*
* Releases what NtfsMapVolumeBitmap() handed out. The cached bitmap itself stays.
*/
VOID
NtfsUnmapVolumeBitmap(PDEVICE_EXTENSION DeviceExt,
                      PFILE_RECORD_HEADER BitmapRecord,
                      PNTFS_ATTR_CONTEXT DataContext)
{
    if (DataContext != NULL)
        ReleaseAttributeContext(DataContext);

    if (BitmapRecord != NULL)
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
}

/**
* @name NtfsFlushVolumeBitmapRange
* @implemented
*
* Writes back the part of the volume bitmap covering a range of clusters.
*
* @param DeviceExt
* Points to the target disk's DEVICE_EXTENSION.
*
* @param DataContext
* Context of $Bitmap's data attribute, from NtfsMapVolumeBitmap().
*
* @param BitmapRecord
* The $Bitmap file record, needed by WriteAttribute().
*
* @param FirstCluster
* First cluster whose bit changed.
*
* @param ClusterCount
* How many clusters changed.
*
* @return
* STATUS_SUCCESS on success.
*
* @remarks
* The written range is widened to whole sectors so the write never turns into a
* read-modify-write of a partial sector.
*/
NTSTATUS
NtfsFlushVolumeBitmapRange(PDEVICE_EXTENSION DeviceExt,
                           PNTFS_ATTR_CONTEXT DataContext,
                           PFILE_RECORD_HEADER BitmapRecord,
                           ULONG FirstCluster,
                           ULONG ClusterCount)
{
    NTSTATUS Status;
    ULONG FirstByte;
    ULONG LastByte;
    ULONG Length;
    ULONG LengthWritten;

    if (ClusterCount == 0 || !DeviceExt->VolumeBitmapValid)
        return STATUS_SUCCESS;

    FirstByte = ROUND_DOWN(FirstCluster / 8, DeviceExt->NtfsInfo.BytesPerSector);
    LastByte = ROUND_UP((FirstCluster + ClusterCount + 7) / 8, DeviceExt->NtfsInfo.BytesPerSector);

    if (LastByte > DeviceExt->VolumeBitmapSize)
        LastByte = DeviceExt->VolumeBitmapSize;

    if (FirstByte >= LastByte)
        return STATUS_SUCCESS;

    Length = LastByte - FirstByte;

    Status = WriteAttribute(DeviceExt,
                            DataContext,
                            FirstByte,
                            DeviceExt->VolumeBitmapData + FirstByte,
                            Length,
                            &LengthWritten,
                            BitmapRecord);
    if (!NT_SUCCESS(Status))
    {
        /* The copy in memory no longer matches the disk */
        DPRINT1("ERROR: Couldn't write the volume bitmap back!\n");
        DeviceExt->VolumeBitmapValid = FALSE;
        DeviceExt->FreeClusterCountValid = FALSE;
    }

    return Status;
}

/**
* @name NtfsFreeVolumeBitmap
* @implemented
*
* Drops the cached volume bitmap. Called when the volume goes away.
*/
VOID
NtfsFreeVolumeBitmap(PDEVICE_EXTENSION DeviceExt)
{
    if (DeviceExt->VolumeBitmapAllocation != NULL)
    {
        ExFreePoolWithTag(DeviceExt->VolumeBitmapAllocation, TAG_NTFS);
        DeviceExt->VolumeBitmapAllocation = NULL;
    }

    DeviceExt->VolumeBitmapData = NULL;
    DeviceExt->VolumeBitmapSize = 0;
    DeviceExt->VolumeBitmapValid = FALSE;
}

ULONGLONG
NtfsGetFreeClusters(PDEVICE_EXTENSION DeviceExt)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER BitmapRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    ULONGLONG BitmapDataSize;
    PCHAR BitmapData;
    ULONGLONG FreeClusters = 0;
    RTL_BITMAP Bitmap;

    DPRINT("NtfsGetFreeClusters(%p)\n", DeviceExt);

    if (DeviceExt->FreeClusterCountValid)
        return DeviceExt->FreeClusterCount;

    BitmapRecord = ExAllocateFromNPagedLookasideList(&DeviceExt->FileRecLookasideList);
    if (BitmapRecord == NULL)
    {
        return 0;
    }

    Status = ReadFileRecord(DeviceExt, NTFS_FILE_BITMAP, BitmapRecord);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
        return 0;
    }

    Status = FindAttribute(DeviceExt, BitmapRecord, AttributeData, L"", 0, &DataContext, NULL);
    if (!NT_SUCCESS(Status))
    {
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
        return 0;
    }

    BitmapDataSize = AttributeDataLength(DataContext->pRecord);
    BitmapDataSize = min(BitmapDataSize, 0xffffffff);
    ASSERT((BitmapDataSize * 8) >= DeviceExt->NtfsInfo.ClusterCount);
    BitmapData = ExAllocatePoolWithTag(NonPagedPool, ROUND_UP(BitmapDataSize, DeviceExt->NtfsInfo.BytesPerSector), TAG_NTFS);
    if (BitmapData == NULL)
    {
        ReleaseAttributeContext(DataContext);
        ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);
        return 0;
    }

    /* One read, not one per sector: $Bitmap is hundreds of kilobytes on a large volume */
    ReadAttribute(DeviceExt, DataContext, 0, (PCHAR)BitmapData, (ULONG)BitmapDataSize);
    ReleaseAttributeContext(DataContext);

    /* $Bitmap is rounded up to a cluster, so it normally covers a few clusters more than the
     * volume has. The bitmap below is bounded by ClusterCount, so the surplus bits are ignored. */
    DPRINT("Total clusters: %I64x\n", DeviceExt->NtfsInfo.ClusterCount);
    DPRINT("Total clusters in bitmap: %I64x\n", BitmapDataSize * 8);
    DPRINT("Diff in size: %I64d B\n", ((BitmapDataSize * 8) - DeviceExt->NtfsInfo.ClusterCount) * DeviceExt->NtfsInfo.SectorsPerCluster * DeviceExt->NtfsInfo.BytesPerSector);

    RtlInitializeBitMap(&Bitmap, (PULONG)BitmapData, DeviceExt->NtfsInfo.ClusterCount);
    FreeClusters = RtlNumberOfClearBits(&Bitmap);

    DeviceExt->FreeClusterCount = FreeClusters;
    DeviceExt->FreeClusterCountValid = TRUE;

    ExFreePoolWithTag(BitmapData, TAG_NTFS);
    ExFreeToNPagedLookasideList(&DeviceExt->FileRecLookasideList, BitmapRecord);

    return FreeClusters;
}

/**
* NtfsAllocateClusters
* Allocates a run of clusters. The run allocated might be smaller than DesiredClusters.
*/
NTSTATUS
NtfsAllocateClusters(PDEVICE_EXTENSION DeviceExt,
                     ULONG FirstDesiredCluster,
                     ULONG DesiredClusters,
                     PULONG FirstAssignedCluster,
                     PULONG AssignedClusters)
{
    NTSTATUS Status;
    PFILE_RECORD_HEADER BitmapRecord;
    PNTFS_ATTR_CONTEXT DataContext;
    ULONGLONG FreeClusters = 0;
    PRTL_BITMAP Bitmap;
    ULONG AssignedRun;

    DPRINT("NtfsAllocateClusters(%p, %lu, %lu, %p, %p)\n", DeviceExt, FirstDesiredCluster, DesiredClusters, FirstAssignedCluster, AssignedClusters);

    Status = NtfsMapVolumeBitmap(DeviceExt, &BitmapRecord, &DataContext);
    if (!NT_SUCCESS(Status))
        return Status;

    Bitmap = &DeviceExt->VolumeBitmap;

    FreeClusters = DeviceExt->FreeClusterCountValid ? DeviceExt->FreeClusterCount
                                                    : RtlNumberOfClearBits(Bitmap);

    if (FreeClusters < DesiredClusters)
    {
        NtfsUnmapVolumeBitmap(DeviceExt, BitmapRecord, DataContext);
        return STATUS_DISK_FULL;
    }

    // TODO: Observe MFT reservation zone

    // Can we get one contiguous run?
    AssignedRun = RtlFindClearBitsAndSet(Bitmap, DesiredClusters, FirstDesiredCluster);

    if (AssignedRun != 0xFFFFFFFF)
    {
        *FirstAssignedCluster = AssignedRun;
        *AssignedClusters = DesiredClusters;
    }
    else
    {
        // we can't get one contiguous run
        *AssignedClusters = RtlFindNextForwardRunClear(Bitmap, FirstDesiredCluster, FirstAssignedCluster);

        if (*AssignedClusters == 0)
        {
            // we couldn't find any runs starting at DesiredFirstCluster
            *AssignedClusters = RtlFindLongestRunClear(Bitmap, FirstAssignedCluster);
        }

        // Never hand back more than was asked for
        *AssignedClusters = min(*AssignedClusters, DesiredClusters);

        // Unlike RtlFindClearBitsAndSet(), these two only report a run; the bits are still
        // clear and have to be marked in use here.
        if (*AssignedClusters != 0)
            RtlSetBits(Bitmap, *FirstAssignedCluster, *AssignedClusters);
    }

    // Only the sectors holding the bits just changed need to go back to disk
    Status = NtfsFlushVolumeBitmapRange(DeviceExt, DataContext, BitmapRecord,
                                        *FirstAssignedCluster, *AssignedClusters);
    if (NT_SUCCESS(Status) && DeviceExt->FreeClusterCountValid)
    {
        DeviceExt->FreeClusterCount -= *AssignedClusters;
    }

    NtfsUnmapVolumeBitmap(DeviceExt, BitmapRecord, DataContext);

    return Status;
}

static
NTSTATUS
NtfsGetFsVolumeInformation(PDEVICE_OBJECT DeviceObject,
                           PFILE_FS_VOLUME_INFORMATION FsVolumeInfo,
                           PULONG BufferLength)
{
    DPRINT("NtfsGetFsVolumeInformation() called\n");
    DPRINT("FsVolumeInfo = %p\n", FsVolumeInfo);
    DPRINT("BufferLength %lu\n", *BufferLength);

    DPRINT("Vpb %p\n", DeviceObject->Vpb);

    DPRINT("Required length %lu\n",
           sizeof(FILE_FS_VOLUME_INFORMATION) + DeviceObject->Vpb->VolumeLabelLength);
    DPRINT("LabelLength %hu\n",
           DeviceObject->Vpb->VolumeLabelLength);
    DPRINT("Label %.*S\n",
           DeviceObject->Vpb->VolumeLabelLength / sizeof(WCHAR),
           DeviceObject->Vpb->VolumeLabel);

    if (*BufferLength < sizeof(FILE_FS_VOLUME_INFORMATION))
        return STATUS_INFO_LENGTH_MISMATCH;

    if (*BufferLength < (sizeof(FILE_FS_VOLUME_INFORMATION) + DeviceObject->Vpb->VolumeLabelLength))
        return STATUS_BUFFER_OVERFLOW;

    /* valid entries */
    FsVolumeInfo->VolumeSerialNumber = DeviceObject->Vpb->SerialNumber;
    FsVolumeInfo->VolumeLabelLength = DeviceObject->Vpb->VolumeLabelLength;
    memcpy(FsVolumeInfo->VolumeLabel,
           DeviceObject->Vpb->VolumeLabel,
           DeviceObject->Vpb->VolumeLabelLength);

    /* dummy entries */
    FsVolumeInfo->VolumeCreationTime.QuadPart = 0;
    FsVolumeInfo->SupportsObjects = FALSE;

    *BufferLength -= (sizeof(FILE_FS_VOLUME_INFORMATION) + DeviceObject->Vpb->VolumeLabelLength);

    DPRINT("BufferLength %lu\n", *BufferLength);
    DPRINT("NtfsGetFsVolumeInformation() done\n");

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetFsAttributeInformation(PDEVICE_EXTENSION DeviceExt,
                              PFILE_FS_ATTRIBUTE_INFORMATION FsAttributeInfo,
                              PULONG BufferLength)
{
    UNREFERENCED_PARAMETER(DeviceExt);

    DPRINT("NtfsGetFsAttributeInformation()\n");
    DPRINT("FsAttributeInfo = %p\n", FsAttributeInfo);
    DPRINT("BufferLength %lu\n", *BufferLength);
    DPRINT("Required length %lu\n", (sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 8));

    if (*BufferLength < sizeof (FILE_FS_ATTRIBUTE_INFORMATION))
        return STATUS_INFO_LENGTH_MISMATCH;

    if (*BufferLength < (sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 8))
        return STATUS_BUFFER_OVERFLOW;

    FsAttributeInfo->FileSystemAttributes =
        FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK | FILE_READ_ONLY_VOLUME;
    FsAttributeInfo->MaximumComponentNameLength = 255;
    FsAttributeInfo->FileSystemNameLength = 8;

    memcpy(FsAttributeInfo->FileSystemName, L"NTFS", 8);

    DPRINT("Finished NtfsGetFsAttributeInformation()\n");

    *BufferLength -= (sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + 8);
    DPRINT("BufferLength %lu\n", *BufferLength);

    return STATUS_SUCCESS;
}


static
NTSTATUS
NtfsGetFsSizeInformation(PDEVICE_OBJECT DeviceObject,
                         PFILE_FS_SIZE_INFORMATION FsSizeInfo,
                         PULONG BufferLength)
{
    PDEVICE_EXTENSION DeviceExt;
    NTSTATUS Status = STATUS_SUCCESS;

    DPRINT("NtfsGetFsSizeInformation()\n");
    DPRINT("FsSizeInfo = %p\n", FsSizeInfo);

    if (*BufferLength < sizeof(FILE_FS_SIZE_INFORMATION))
        return STATUS_BUFFER_OVERFLOW;

    DeviceExt = DeviceObject->DeviceExtension;

    FsSizeInfo->AvailableAllocationUnits.QuadPart = NtfsGetFreeClusters(DeviceExt);
    FsSizeInfo->TotalAllocationUnits.QuadPart = DeviceExt->NtfsInfo.ClusterCount;
    FsSizeInfo->SectorsPerAllocationUnit = DeviceExt->NtfsInfo.SectorsPerCluster;
    FsSizeInfo->BytesPerSector = DeviceExt->NtfsInfo.BytesPerSector;

    DPRINT("Finished NtfsGetFsSizeInformation()\n");
    if (NT_SUCCESS(Status))
        *BufferLength -= sizeof(FILE_FS_SIZE_INFORMATION);

    return Status;
}


static
NTSTATUS
NtfsGetFsDeviceInformation(PDEVICE_OBJECT DeviceObject,
                           PFILE_FS_DEVICE_INFORMATION FsDeviceInfo,
                           PULONG BufferLength)
{
    DPRINT("NtfsGetFsDeviceInformation()\n");
    DPRINT("FsDeviceInfo = %p\n", FsDeviceInfo);
    DPRINT("BufferLength %lu\n", *BufferLength);
    DPRINT("Required length %lu\n", sizeof(FILE_FS_DEVICE_INFORMATION));

    if (*BufferLength < sizeof(FILE_FS_DEVICE_INFORMATION))
        return STATUS_BUFFER_OVERFLOW;

    FsDeviceInfo->DeviceType = FILE_DEVICE_DISK;
    FsDeviceInfo->Characteristics = DeviceObject->Characteristics;

    DPRINT("NtfsGetFsDeviceInformation() finished.\n");

    *BufferLength -= sizeof(FILE_FS_DEVICE_INFORMATION);
    DPRINT("BufferLength %lu\n", *BufferLength);

    return STATUS_SUCCESS;
}


NTSTATUS
NtfsQueryVolumeInformation(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;
    FS_INFORMATION_CLASS FsInformationClass;
    PIO_STACK_LOCATION Stack;
    NTSTATUS Status = STATUS_SUCCESS;
    PVOID SystemBuffer;
    ULONG BufferLength;
    PDEVICE_EXTENSION DeviceExt;

    DPRINT("NtfsQueryVolumeInformation() called\n");

    ASSERT(IrpContext);

    Irp = IrpContext->Irp;
    DeviceObject = IrpContext->DeviceObject;
    DeviceExt = DeviceObject->DeviceExtension;
    Stack = IrpContext->Stack;

    if (!ExAcquireResourceSharedLite(&DeviceExt->DirResource,
                                     BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    FsInformationClass = Stack->Parameters.QueryVolume.FsInformationClass;
    BufferLength = Stack->Parameters.QueryVolume.Length;
    SystemBuffer = Irp->AssociatedIrp.SystemBuffer;
    RtlZeroMemory(SystemBuffer, BufferLength);

    DPRINT("FsInformationClass %d\n", FsInformationClass);
    DPRINT("SystemBuffer %p\n", SystemBuffer);

    switch (FsInformationClass)
    {
        case FileFsVolumeInformation:
            Status = NtfsGetFsVolumeInformation(DeviceObject,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        case FileFsAttributeInformation:
            Status = NtfsGetFsAttributeInformation(DeviceObject->DeviceExtension,
                                                   SystemBuffer,
                                                   &BufferLength);
            break;

        case FileFsSizeInformation:
            Status = NtfsGetFsSizeInformation(DeviceObject,
                                              SystemBuffer,
                                              &BufferLength);
            break;

        case FileFsDeviceInformation:
            Status = NtfsGetFsDeviceInformation(DeviceObject,
                                                SystemBuffer,
                                                &BufferLength);
            break;

        default:
            Status = STATUS_NOT_SUPPORTED;
    }

    ExReleaseResourceLite(&DeviceExt->DirResource);

    if (NT_SUCCESS(Status))
        Irp->IoStatus.Information =
            Stack->Parameters.QueryVolume.Length - BufferLength;
    else
        Irp->IoStatus.Information = 0;

    return Status;
}


NTSTATUS
NtfsSetVolumeInformation(PNTFS_IRP_CONTEXT IrpContext)
{
    PIRP Irp;

    DPRINT("NtfsSetVolumeInformation() called\n");

    ASSERT(IrpContext);

    Irp = IrpContext->Irp;
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;

    return STATUS_NOT_SUPPORTED;
}

/* EOF */
