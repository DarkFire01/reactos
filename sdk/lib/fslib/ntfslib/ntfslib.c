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


/* MACROSES ******************************************************************/

#define MB_TO_B(x) (x * 1024)


/* HELPERS FUNCTIONS *********************************************************/

static
ULONGLONG
CalcVolumeSerialNumber(VOID)
{
    //todo test that

    BYTE  i;
    ULONG r;
    ULONG seed;
    ULONGLONG serial;

    seed = NtGetTickCount();

    for (i = 0; i < 32; i += 2)
    {
        r = RtlRandom(&seed);

        serial |= ((r & 0xff00) >> 8) << (i * 8);
        serial |= ((r & 0xff)) << (i * 8 + 8);
    }

    return serial;
}


/* NTFS FORMAT FUNCTIONS *****************************************************/

static
VOID
WriteJumpInstruction(OUT PBOOT_SECTOR pbs)
{
    pbs->Jump[0] = 0xEB;  // jmp
    pbs->Jump[1] = 0x52;  // 82
    pbs->Jump[2] = 0x90;  // nop
}

static
VOID
WriteBiosParametrsBlock(OUT PBIOS_PARAMETERS_BLOCK bpb,
                        IN  GET_LENGTH_INFORMATION* gli,
                        IN  PDISK_GEOMETRY dg)
{
    bpb->BytesPerSector = BPB_BYTES_PER_SECTOR;

    if (gli->Length.QuadPart < MB_TO_B(512))
    {
        bpb->SectorsPerCluster = 1;
    }
    else if (gli->Length.QuadPart < MB_TO_B(1024))
    {
        bpb->SectorsPerCluster = 2;
    }
    else if (gli->Length.QuadPart < MB_TO_B(2048))
    {
        bpb->SectorsPerCluster = 4;
    }
    else
    {
        bpb->SectorsPerCluster = 8;
    }

    bpb->MediaId = (dg->MediaType == FixedMedia) ? 0xF8 : 0x00;

    bpb->SectorsPerTrack = dg->SectorsPerTrack;

    bpb->Heads = BPB_HEADS;

    bpb->HiddenSectorsCount = BPB_WINXP_HIDDEN_SECTORS;
}

static
VOID
WriteExBiosParametrsBlock(OUT PEXTENDED_BIOS_PARAMETERS_BLOCK ebpb,
                          IN  GET_LENGTH_INFORMATION* gli,
                          IN  PDISK_GEOMETRY dg)
{
    ebpb->Header      = EBPB_HEADER;
    ebpb->SectorCount = 
        ((ULONGLONG) dg->SectorsPerTrack) *
        ((ULONGLONG) dg->TracksPerCylinder) * 
        ((ULONGLONG) dg->Cylinders.QuadPart);

    ebpb->MftLocation     = MFT_LOCATION;
    ebpb->MftMirrLocation = gli->Length.QuadPart / 2;  // Only for Windows XP

    ebpb->ClustersPerMftRecord   = CLUSTER_PER_MFT_RECORD;
    ebpb->ClustersPerIndexRecord = 0x01;

    ebpb->SerialNumber = CalcVolumeSerialNumber();
}

static
NTSTATUS
WriteBootSector(IN HANDLE h,
                IN GET_LENGTH_INFORMATION* gli,
                IN PDISK_GEOMETRY dg)
{
    PBOOT_SECTOR    BootSector;
    NTSTATUS        Status;
    IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER   FileOffset;

    BootSector = RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(BOOT_SECTOR));

    if (BootSector == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(BootSector, sizeof(BOOT_SECTOR));

    WriteJumpInstruction(BootSector);

    BootSector->OEMID.QuadPart = 0x202020205346544E;

    WriteBiosParametrsBlock(&(BootSector->BPB), gli, dg);
    WriteExBiosParametrsBlock(&(BootSector->EBPB), gli, dg);

    BootSector->EndSector = BOOT_SECTOR_END;

    FileOffset.QuadPart = 0ULL;
    Status = NtWriteFile(h,
                         NULL,
                         NULL,
                         NULL,
                         &IoStatusBlock,
                         BootSector,
                         sizeof(BOOT_SECTOR),
                         &FileOffset,
                         NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("BootSector write failed. NtWriteFile() failed (Status %lx)\n", Status);
        goto done;
    }

done:
    RtlFreeHeap(RtlGetProcessHeap(), 0, BootSector);
    return Status;
}

NTSTATUS NTAPI
NtfsFormat(IN PUNICODE_STRING DriveRoot,
           IN FMIFS_MEDIA_FLAG MediaFlag,
           IN PUNICODE_STRING Label,
           IN BOOLEAN QuickFormat,
           IN ULONG ClusterSize,
           IN PFMIFSCALLBACK Callback)
{
    HANDLE                 FileHandle;
    OBJECT_ATTRIBUTES      Attributes;
    IO_STATUS_BLOCK        Iosb;
    GET_LENGTH_INFORMATION LengthInformation;
    DISK_GEOMETRY          DiskGeometry;
    NTSTATUS               Status;

    DPRINT1("NtfsFormat(DriveRoot '%wZ')\n", DriveRoot);

    InitializeObjectAttributes(&Attributes, DriveRoot, 0, NULL, NULL);

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

    if (Callback)
    {
        ULONG pc = 0;
        Callback(PROGRESS, 0, (PVOID)&pc);
    }

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

    Status = WriteBootSector(FileHandle, &LengthInformation, &DiskGeometry);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteBootSector() failed with status 0x%.08x\n", Status);
        NtClose(FileHandle);
        goto end;
    }

end:
    NtFsControlFile(FileHandle, NULL, NULL, NULL, &Iosb, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0);
    NtFsControlFile(FileHandle, NULL, NULL, NULL, &Iosb, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0);

    NtClose(FileHandle);

    if (Callback)
    {
        BOOL success = NT_SUCCESS(Status);
        Callback(DONE, 0, (PVOID)&success);
    }

    return Status;
}


NTSTATUS NTAPI
NtfsChkdsk(IN PUNICODE_STRING DriveRoot,
    IN BOOLEAN FixErrors,
    IN BOOLEAN Verbose,
    IN BOOLEAN CheckOnlyIfDirty,
    IN BOOLEAN ScanDrive,
    IN PFMIFSCALLBACK Callback)
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