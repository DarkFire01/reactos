/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NTFS FS library
 * FILE:        lib/fslib/ntfslib/ntfslib.c
 * PURPOSE:     NTFS lib
 * PROGRAMMERS: Pierre Schweitzer, Klachkov Valery
 */

/* INCLUDES ******************************************************************/

#define NDEBUG
#include <debug.h>

#include "ntfslib.h"


/* MACROSES ******************************************************************/

#define MB_TO_B(x) (x * 1024)


/* HELPERS FUNCTIONS *********************************************************/

static
ULONGLONG
CalcVolumeSerialNumber(VOID)
{
    // TODO: Ñheck the correctness of the generated serial number

    BYTE  i;
    ULONG r;
    ULONG seed;
    ULONGLONG serial;

    seed = NtGetTickCount();

    for (i = 0; i < 32; i += 2)
    {
        r = RtlRandom(&seed);

        serial |= ((r & 0xff00) >> 8) << (i * 8);
        serial |= ((r & 0xff)) << (i * 8 * 2);
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
WriteBiosParametersBlock(OUT PBIOS_PARAMETERS_BLOCK bpb,
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
WriteExBiosParametersBlock(OUT PEXTENDED_BIOS_PARAMETERS_BLOCK ebpb,
                           IN  GET_LENGTH_INFORMATION* gli,
                           IN  PDISK_GEOMETRY dg)
{
    ebpb->Header      = EBPB_HEADER;
    ebpb->SectorCount = 
        ((ULONGLONG) dg->SectorsPerTrack)   *
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

    // Allocate memory
    BootSector = RtlAllocateHeap(RtlGetProcessHeap(), 0, sizeof(BOOT_SECTOR));
    if (BootSector == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Clear memory
    RtlZeroMemory(BootSector, sizeof(BOOT_SECTOR));

    // Fill boot sector structure
    WriteJumpInstruction(BootSector);

    BootSector->OEMID.QuadPart = 0x202020205346544E;  // NTFS   

    WriteBiosParametersBlock(&(BootSector->BPB), gli, dg);
    WriteExBiosParametersBlock(&(BootSector->EBPB), gli, dg);

    BootSector->EndSector = BOOT_SECTOR_END;

    // Write to disk
    Status = NtWriteFile(h,
                         NULL,
                         NULL,
                         NULL,
                         &IoStatusBlock,
                         BootSector,
                         sizeof(BOOT_SECTOR),
                         0L,
                         NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("BootSector write failed. NtWriteFile() failed (Status %lx)\n", Status);
    }

done:
    // Clear memory
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
    Status = WriteBootSector(FileHandle, &LengthInformation, &DiskGeometry);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteBootSector() failed with status 0x%.08x\n", Status);
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


/* NTFS CHECK DISK FUNCTIONS *****************************************************/

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