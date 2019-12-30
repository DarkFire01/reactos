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

VOID
GetSystemTimeAsFileTime(OUT PFILETIME lpFileTime)
{
    LARGE_INTEGER SystemTime;

    do
    {
        SystemTime.HighPart = SharedUserData->SystemTime.High1Time;
        SystemTime.LowPart = SharedUserData->SystemTime.LowPart;
    } while (SystemTime.HighPart != SharedUserData->SystemTime.High2Time);

    lpFileTime->dwLowDateTime = SystemTime.LowPart;
    lpFileTime->dwHighDateTime = SystemTime.HighPart;
}

#define KeQuerySystemTime(t)     GetSystemTimeAsFileTime((LPFILETIME)(t));

/* NTFS BOOT SECTOR **********************************************************/

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

    // Clear memory
    RtlFreeHeap(RtlGetProcessHeap(), 0, BootSector);
    
    return Status;
}


/* ATTRIBUTES FUNCTIONS ******************************************************/

static
VOID
SetFileRecordEnd(PFILE_RECORD_HEADER FileRecord,
    PNTFS_ATTR_RECORD AttrEnd,
    ULONG EndMarker)
{
    // Ensure AttrEnd is aligned on an 8-byte boundary, relative to FileRecord
    ASSERT(((ULONG_PTR)AttrEnd - (ULONG_PTR)FileRecord) % ATTR_RECORD_ALIGNMENT == 0);

    // mark the end of attributes
    AttrEnd->Type = AttributeEnd;

    // Restore the "file-record-end marker." The value is never checked but this behavior is consistent with Win2k3.
    AttrEnd->Length = EndMarker;

    // recalculate bytes in use
    FileRecord->BytesInUse = (ULONG_PTR)AttrEnd - (ULONG_PTR)FileRecord + sizeof(ULONG) * 2;
}

static
VOID
AddStandardInformation(OUT PFILE_RECORD_HEADER FileRecord,
                       OUT PNTFS_ATTR_RECORD   AttributeAddress)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    PSTANDARD_INFORMATION StandardInfo = (PSTANDARD_INFORMATION)((LONG_PTR)AttributeAddress + ResidentHeaderLength);
    LARGE_INTEGER SystemTime;
    ULONG FileRecordEnd = AttributeAddress->Length;

    AttributeAddress->Type   = AttributeStandardInformation;
    AttributeAddress->Length = sizeof(STANDARD_INFORMATION) + ResidentHeaderLength;
    AttributeAddress->Length = ALIGN_UP_BY(AttributeAddress->Length, ATTR_RECORD_ALIGNMENT);
    AttributeAddress->Resident.ValueLength = sizeof(STANDARD_INFORMATION);
    AttributeAddress->Resident.ValueOffset = ResidentHeaderLength;
    AttributeAddress->Instance = FileRecord->NextAttributeNumber++;

    // Set dates and times
    KeQuerySystemTime(&SystemTime);
    StandardInfo->CreationTime   = SystemTime.QuadPart;
    StandardInfo->ChangeTime     = SystemTime.QuadPart;
    StandardInfo->LastWriteTime  = SystemTime.QuadPart;
    StandardInfo->LastAccessTime = SystemTime.QuadPart;
    StandardInfo->FileAttribute  = NTFS_FILE_TYPE_ARCHIVE;

    // Move the attribute-end and file-record-end markers to the end of the file record
    AttributeAddress = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttributeAddress + AttributeAddress->Length);
    SetFileRecordEnd(FileRecord, AttributeAddress, FileRecordEnd);
}

static
VOID
AddFileName(OUT PFILE_RECORD_HEADER FileRecord,
            OUT PNTFS_ATTR_RECORD   AttributeAddress,
            IN  DWORD32             MftRecordNumber)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    PFILENAME_ATTRIBUTE FileNameAttribute;
    ULONG FileRecordEnd = AttributeAddress->Length;
    UNICODE_STRING FilenameNoPath;
    LARGE_INTEGER SystemTime;

    AttributeAddress->Type     = AttributeFileName;
    AttributeAddress->Instance = FileRecord->NextAttributeNumber++;

    FileNameAttribute = (PFILENAME_ATTRIBUTE)((LONG_PTR)AttributeAddress + ResidentHeaderLength);

    // Set dates and times
    KeQuerySystemTime(&SystemTime);
    FileNameAttribute->CreationTime   = SystemTime.QuadPart;
    FileNameAttribute->ChangeTime     = SystemTime.QuadPart;
    FileNameAttribute->LastWriteTime  = SystemTime.QuadPart;
    FileNameAttribute->LastAccessTime = SystemTime.QuadPart;

    if (FileRecord->Flags & MFT_RECORD_IS_DIRECTORY)
    {
        FileNameAttribute->FileAttributes = NTFS_FILE_TYPE_DIRECTORY;
    }
    else
    {
        FileNameAttribute->FileAttributes = NTFS_FILE_TYPE_ARCHIVE;
    }

    FileNameAttribute->DirectoryFileReferenceNumber = MftRecordNumber;

    FileNameAttribute->DirectoryFileReferenceNumber |= (ULONGLONG) NTFS_FILE_ROOT << 48;

    FileNameAttribute->NameLength = FilenameNoPath.Length / sizeof(WCHAR);
    RtlCopyMemory(FileNameAttribute->Name, FilenameNoPath.Buffer, FilenameNoPath.Length);

    // For now, we're emulating the way Windows behaves when 8.3 name generation is disabled
    if (RtlIsNameLegalDOS8Dot3(&FilenameNoPath, NULL, NULL))
        FileNameAttribute->NameType = NTFS_FILE_NAME_WIN32_AND_DOS;
    else
        FileNameAttribute->NameType = NTFS_FILE_NAME_POSIX;

    FileRecord->LinkCount++;

    AttributeAddress->Length =
        ResidentHeaderLength +
        FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) +
        FilenameNoPath.Length;

    AttributeAddress->Length = ALIGN_UP_BY(AttributeAddress->Length, ATTR_RECORD_ALIGNMENT);

    AttributeAddress->Resident.ValueLength = FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + FilenameNoPath.Length;
    AttributeAddress->Resident.ValueOffset = ResidentHeaderLength;
    AttributeAddress->Resident.Flags       = RA_INDEXED;

    // Move the attribute-end and file-record-end markers to the end of the file record
    AttributeAddress = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttributeAddress + AttributeAddress->Length);
    SetFileRecordEnd(FileRecord, AttributeAddress, FileRecordEnd);
}

void
AddData(OUT PFILE_RECORD_HEADER FileRecord,
        OUT PNTFS_ATTR_RECORD AttributeAddress)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    ULONG FileRecordEnd = AttributeAddress->Length;

    AttributeAddress->Type   = AttributeData;
    AttributeAddress->Length = ResidentHeaderLength;
    AttributeAddress->Length = ALIGN_UP_BY(AttributeAddress->Length, ATTR_RECORD_ALIGNMENT);
    AttributeAddress->Resident.ValueLength = 0;
    AttributeAddress->Resident.ValueOffset = ResidentHeaderLength;

    // For unnamed $DATA attributes, NameOffset equals header length
    AttributeAddress->NameOffset = ResidentHeaderLength;
    AttributeAddress->Instance = FileRecord->NextAttributeNumber++;

    // Move the attribute-end and file-record-end markers to the end of the file record
    AttributeAddress = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttributeAddress + AttributeAddress->Length);
    SetFileRecordEnd(FileRecord, AttributeAddress, FileRecordEnd);
}


/* FILES FUNCTIONS ***********************************************************/

// Create empty file record
static
PFILE_RECORD_HEADER
NtfsCreateEmptyFileRecord(IN DWORD32 MftRecordNumber)
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD NextAttribute;

    DPRINT("NtfsCreateEmptyFileRecord(%d)\n", MftRecordNumber);

    // Allocate memory for file record
    FileRecord = RtlAllocateHeap(RtlGetProcessHeap(), 0, MFT_RECORD_SIZE);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for file record!\n");
        return NULL;
    }

    RtlZeroMemory(FileRecord, MFT_RECORD_SIZE);

    FileRecord->Header.Magic    = NRH_FILE_TYPE;
    FileRecord->MFTRecordNumber = MftRecordNumber;

    // Calculate USA offset and count
    FileRecord->Header.UsaOffset = FIELD_OFFSET(FILE_RECORD_HEADER, MFTRecordNumber) + sizeof(ULONG);  // Check this!

    // Size of USA (in ULONG's) will be 1 (for USA number) + 1 for every sector the file record uses
    FileRecord->BytesAllocated  = MFT_RECORD_SIZE;
    FileRecord->Header.UsaCount = (FileRecord->BytesAllocated / BPB_BYTES_PER_SECTOR) + 1;  // Check this!

    // Setup other file record fields
    FileRecord->SequenceNumber  = 1;
    FileRecord->AttributeOffset = FileRecord->Header.UsaOffset + (2 * FileRecord->Header.UsaCount);
    FileRecord->AttributeOffset = ALIGN_UP_BY(FileRecord->AttributeOffset, ATTR_RECORD_ALIGNMENT);
    FileRecord->Flags           = MFT_RECORD_IN_USE;
    FileRecord->BytesInUse      = FileRecord->AttributeOffset + sizeof(ULONG) * 2;

    // Find where the first attribute will be added
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);

    // Temporary mark the end of the file-record
    NextAttribute->Type = AttributeEnd;
    NextAttribute->Length = FILE_RECORD_END;

    return FileRecord;
}

// Create file record with default attributes
static
PFILE_RECORD_HEADER
NtfsCreateBlankFileRecord(IN DWORD32 MftRecordNumber)
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD   NextAttribute;

    // Create empty file record
    FileRecord = NtfsCreateEmptyFileRecord(MftRecordNumber);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for file record!\n");
        return NULL;
    }

    // Find where the first attribute will be added
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);

    // Add $STANDARD_INFORMATION attribute
    AddStandardInformation(FileRecord, NextAttribute);

    // Calculate pointer to the next attribute
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute + (ULONG_PTR)NextAttribute->Length);

    // Add the $FILE_NAME attribute
    AddFileName(FileRecord, NextAttribute, MftRecordNumber);

    // Calculate pointer to the next attribute
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute + (ULONG_PTR)NextAttribute->Length);

    // Add the $DATA attribute
    AddData(FileRecord, NextAttribute);

    return FileRecord;
}

static
NTSTATUS
WriteMetafiles(IN HANDLE h)
{
    NTSTATUS Status;

    NtfsCreateBlankFileRecord(0);

    Status = STATUS_SUCCESS;

    return Status;
}


/* COMMON ********************************************************************/

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

    // Create metafiles
    Status = WriteMetafiles(FileHandle);
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

#undef KeQuerySystemTime