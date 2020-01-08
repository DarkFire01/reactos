/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NTFS FS library
 * FILE:        lib/fslib/ntfslib/ntfslib.c
 * PURPOSE:     NTFS lib
 * PROGRAMMERS: Pierre Schweitzer, Klachkov Valery
 */

/* INCLUDES ******************************************************************/

#include "ntfslib.h"

#define NDEBUG
#include <debug.h>


/* FUNCTIONS *****************************************************************/

VOID
GetSystemTimeAsFileTime(OUT PFILETIME lpFileTime)
{
    LARGE_INTEGER SystemTime;

    do
    {
        SystemTime.HighPart = SharedUserData->SystemTime.High1Time;
        SystemTime.LowPart = SharedUserData->SystemTime.LowPart;
    }
    while (SystemTime.HighPart != SharedUserData->SystemTime.High2Time);

    lpFileTime->dwLowDateTime = SystemTime.LowPart;
    lpFileTime->dwHighDateTime = SystemTime.HighPart;
}

NTSTATUS NTAPI
NtfsFormat(IN PUNICODE_STRING  DriveRoot,
           IN FMIFS_MEDIA_FLAG MediaFlag,
           IN PUNICODE_STRING  Label,
           IN BOOLEAN          QuickFormat,
           IN ULONG            ClusterSize,
           IN PFMIFSCALLBACK   Callback)
{
    HANDLE                 FileHandle;
    OBJECT_ATTRIBUTES      Attributes;
    IO_STATUS_BLOCK        Iosb;
    GET_LENGTH_INFORMATION LengthInformation;
    DISK_GEOMETRY          DiskGeometry;
    NTSTATUS               Status;

    DPRINT1("NtfsFormat(DriveRoot '%wZ')\n", DriveRoot);

    InitializeObjectAttributes(&Attributes, DriveRoot, 0, NULL, NULL);

    // Open volume
    Status = NtOpenFile(&FileHandle,
                        FILE_GENERIC_READ | FILE_GENERIC_WRITE | SYNCHRONIZE,
                        &Attributes,
                        &Iosb,
                        FILE_SHARE_READ,
                        FILE_SYNCHRONOUS_IO_ALERT);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("NtOpenFile() failed with status 0x%.08x\n", Status);
        return Status;
    }

    // Get length info
    Status = NtDeviceIoControlFile(FileHandle, 
                                   NULL,
                                   NULL,
                                   NULL, 
                                   &Iosb, 
                                   IOCTL_DISK_GET_LENGTH_INFO,
                                   NULL, 
                                   0, 
                                   &LengthInformation, 
                                   sizeof(GET_LENGTH_INFORMATION));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IOCTL_DISK_GET_LENGTH_INFO failed with status 0x%.08x\n", Status);
        NtClose(FileHandle);
        return Status;
    }

    // Get disk geometry
    Status = NtDeviceIoControlFile(FileHandle, 
                                   NULL,
                                   NULL, 
                                   NULL, 
                                   &Iosb,
                                   IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                   NULL, 
                                   0,
                                   &DiskGeometry,
                                   sizeof(DiskGeometry));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("IOCTL_DISK_GET_DRIVE_GEOMETRY failed with status 0x%.08x\n", Status);
        NtClose(FileHandle);
        return Status;
    }

    // Initialize progress bar
    if (Callback)
    {
        ULONG pc = 0;
        Callback(PROGRESS, 0, (PVOID)&pc);
    }

    // Lock volume
    NtFsControlFile(FileHandle, 
                    NULL,
                    NULL,
                    NULL, 
                    &Iosb, 
                    FSCTL_LOCK_VOLUME,
                    NULL, 
                    0, 
                    NULL,
                    0);

    // Write boot sector
    Status = WriteBootSector(FileHandle, &LengthInformation, &DiskGeometry, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteBootSector() failed with status 0x%.08x\n", Status);
        NtClose(FileHandle);
        goto end;
    }

    // Create metafiles
    Status = WriteMetafiles(FileHandle, &LengthInformation, &DiskGeometry);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteMetafiles() failed with status 0x%.08x\n", Status);
        NtClose(FileHandle);
        goto end;
    }

end:

    // Dismount and unlock volume
    NtFsControlFile(FileHandle, NULL, NULL, NULL, &Iosb, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0);
    NtFsControlFile(FileHandle, NULL, NULL, NULL, &Iosb, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0);

    // Clear memory
    NtClose(FileHandle);

    // Update progress bar
    if (Callback)
    {
        BOOL success = NT_SUCCESS(Status);
        Callback(DONE, 0, (PVOID)&success);
    }

    return Status;
}


NTSTATUS NTAPI
NtfsChkdsk(IN PUNICODE_STRING DriveRoot,
           IN BOOLEAN         FixErrors,
           IN BOOLEAN         Verbose,
           IN BOOLEAN         CheckOnlyIfDirty,
           IN BOOLEAN         ScanDrive,
           IN PFMIFSCALLBACK  Callback)
{
    // STUB

    if (Callback)
    {
        TEXTOUTPUT TextOut;

        TextOut.Lines = 1;
        TextOut.Output = "stub, not implemented";

        Callback(OUTPUT, 0, &TextOut);
    }

    return STATUS_SUCCESS;
}