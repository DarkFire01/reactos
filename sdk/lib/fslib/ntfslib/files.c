/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NTFS FS library
 * FILE:        lib/fslib/ntfslib/files.c
 * PURPOSE:     NTFS lib
 * PROGRAMMERS: Pierre Schweitzer, Klachkov Valery
 */

/* INCLUDES ******************************************************************/

#include <ntfslib.h>

#define NDEBUG
#include <debug.h>


/* FUNCTIONS *****************************************************************/

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
    FileRecord->BytesAllocated = MFT_RECORD_SIZE;
    FileRecord->Header.UsaCount = (FileRecord->BytesAllocated / BPB_BYTES_PER_SECTOR) + 1;  // Check this!

    // Setup other file record fields
    FileRecord->SequenceNumber  = 1;
    FileRecord->AttributeOffset = FileRecord->Header.UsaOffset + (2 * FileRecord->Header.UsaCount);
    FileRecord->AttributeOffset = ALIGN_UP_BY(FileRecord->AttributeOffset, ATTR_RECORD_ALIGNMENT);
    FileRecord->Flags      = MFT_RECORD_IN_USE;
    FileRecord->BytesInUse = FileRecord->AttributeOffset + sizeof(ULONG) * 2;

    // Find where the first attribute will be added
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);  // Check this!

    // Temporary mark the end of the file-record
    NextAttribute->Type   = AttributeEnd;
    NextAttribute->Length = FILE_RECORD_END;

    return FileRecord;
}

// Create file record with $STANDARD_INFORMATION and $FILE_NAME attributes
static
PFILE_RECORD_HEADER
NtfsCreateBlankFileRecord(IN  LPCWSTR FileName,
                          IN  DWORD32 MftRecordNumber,
                          OUT PNTFS_ATTR_RECORD* NextAttribute)
{
    PFILE_RECORD_HEADER FileRecord;

    // Create empty file record
    FileRecord = NtfsCreateEmptyFileRecord(MftRecordNumber);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for file record!\n");
        return NULL;
    }

    // Find where the first attribute will be added
    (*NextAttribute) = (PNTFS_ATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->AttributeOffset);

    // Add $STANDARD_INFORMATION attribute
    AddStandardInformationAttribute(FileRecord, (*NextAttribute));

    // Calculate pointer to the next attribute
    (*NextAttribute) = (PNTFS_ATTR_RECORD)((ULONG_PTR)(*NextAttribute) + (ULONG_PTR)(*NextAttribute)->Length);

    // Add the $FILE_NAME attribute
    AddFileNameAttribute(FileRecord, (*NextAttribute), FileName, MftRecordNumber);

    (*NextAttribute) = (PNTFS_ATTR_RECORD)((ULONG_PTR)(*NextAttribute) + (ULONG_PTR)(*NextAttribute)->Length);
   
    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateMft()
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD   Attribute = NULL;
    
    // Create MFT file record
    FileRecord = NtfsCreateBlankFileRecord(L"$MFT", NTFS_FILE_MFT, &Attribute);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for $MFT file record!\n");
        return NULL;
    }

    // Create DATA attribute

    // Create BITMAP attribute

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateMFTMirr()
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD   Attribute = NULL;

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"$MFTMirr", NTFS_FILE_MFTMIRR, &Attribute);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for $MFTMirr file record!\n");
        return NULL;
    }

    // Create DATA attribute

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateLogFile()
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD   Attribute = NULL;

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"$LogFile", NTFS_FILE_LOGFILE, &Attribute);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for $LogFile file record!\n");
        return NULL;
    }

    // Create DATA attribute

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateVolume()
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD   Attribute = NULL;

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"$Volume", NTFS_FILE_VOLUME, &Attribute);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for $Volume file record!\n");
        return NULL;
    }

    // Add $VOLUME_NAME
    AddEmptyVolumeNameAttribute(FileRecord, Attribute);

    // Add $VOLUME_INFORMATION
    Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + (ULONG_PTR)Attribute->Length);
    AddVolumeInformationAttribute(FileRecord, Attribute, 3, 1);
    
    // Add $DATA
    Attribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)Attribute + (ULONG_PTR)Attribute->Length);
    AddEmptyDataAttribute(FileRecord, Attribute);

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateAttrDef()
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD   Attribute = NULL;

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"$AttrDef", NTFS_FILE_ATTRDEF, &Attribute);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for $AttrDef file record!\n");
        return NULL;
    }
    
    // Create DATA attribute

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateRoot()
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD   Attribute = NULL;

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L".", NTFS_FILE_ROOT, &Attribute);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for . file record!\n");
        return NULL;
    }

    // Create DATA attribute

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateBitmap()
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD   Attribute = NULL;

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"$Bitmap", NTFS_FILE_BITMAP, &Attribute);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for $Bitmap file record!\n");
        return NULL;
    }

    // Create DATA attribute

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateBoot()
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD   Attribute = NULL;

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"$Boot", NTFS_FILE_BOOT, &Attribute);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for $Boot file record!\n");
        return NULL;
    }

    // Create DATA attribute

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateUpCase()
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD   Attribute = NULL;

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"$UpCase", NTFS_FILE_ATTRDEF, &Attribute);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for $UpCase file record!\n");
        return NULL;
    }

    // Create unnamed DATA attribute

    // Create $Info DATA attribute

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateStub(IN DWORD32 MftRecordNumber)
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_RECORD   Attribute = NULL;

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"", MftRecordNumber, &Attribute);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for stub #%d file record!\n", MftRecordNumber);
        return NULL;
    }

    // Empty resident $DATA attribute
    AddEmptyDataAttribute(FileRecord, Attribute);

    return FileRecord;
}

static
NTSTATUS
WriteMetafile(IN HANDLE              Handle,
              IN PFILE_RECORD_HEADER FileRecord, 
              OUT PIO_STATUS_BLOCK   IoStatusBlock)
{
    LARGE_INTEGER Offset;

    Offset.QuadPart = 
        ((LONGLONG)MFT_FIRST_SECTOR * (LONGLONG)BPB_BYTES_PER_SECTOR) + // Offset to $MFT
        (LONGLONG)(FileRecord->MFTRecordNumber * MFT_RECORD_SIZE);      // Offset to record

    return NtWriteFile(Handle,
                       NULL,
                       NULL,
                       NULL,
                       IoStatusBlock,
                       FileRecord,
                       MFT_RECORD_SIZE,
                       &Offset,
                       NULL);
}

NTSTATUS
WriteMetafiles(IN HANDLE                  Handle, 
               IN GET_LENGTH_INFORMATION* LengthInformation,
               IN PDISK_GEOMETRY          DiskGeometry,
               IN PBOOT_SECTOR            BootSector)
{
    NTSTATUS Status = STATUS_SUCCESS;
    IO_STATUS_BLOCK IoStatusBlock;

    PFILE_RECORD_HEADER MFT     = CreateMft();
    PFILE_RECORD_HEADER MFTMirr = CreateMFTMirr();
    PFILE_RECORD_HEADER LogFile = CreateLogFile();
    PFILE_RECORD_HEADER Volume  = CreateVolume();
    PFILE_RECORD_HEADER AttrDef = CreateAttrDef();
    PFILE_RECORD_HEADER Root    = CreateRoot();
    PFILE_RECORD_HEADER Bitmap  = CreateBitmap();
    PFILE_RECORD_HEADER Boot    = CreateBoot();  
    PFILE_RECORD_HEADER UpCase  = CreateUpCase();
    PFILE_RECORD_HEADER Stub;

    DWORD32 MftIndex;

    if (!MFT    || !MFTMirr || !LogFile ||
        !Volume || !AttrDef || !Root    ||
        !Bitmap || !Boot    || !UpCase)
    {
        DPRINT1("ERROR: Unable to allocate memory for file records!\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    // Write $MFT
    Status = WriteMetafile(Handle, MFT, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteMetafile(). $MFT write failed. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }

    // Write $MFTMirr
    Status = WriteMetafile(Handle, MFTMirr, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteMetafile(). $MFTMirr write failed. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }

    // Write $LogFile
    Status = WriteMetafile(Handle, LogFile, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteMetafile(). $LogFile write failed. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }

    // Write $Volume
    Status = WriteMetafile(Handle, Volume, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteMetafile(). $Volume write failed. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }

    // Write $AttrDef
    Status = WriteMetafile(Handle, AttrDef, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteMetafile(). $AttrDef write failed. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }

    // Write $Root
    Status = WriteMetafile(Handle, Root, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteMetafile(). $Root write failed. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }

    // Write $Bitmap
    Status = WriteMetafile(Handle, Bitmap, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteMetafile(). $Bitmap write failed. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }

    // Write $Boot
    Status = WriteMetafile(Handle, Boot, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteMetafile(). $Boot write failed. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }

    // Write stub for $BadClus
    Stub = CreateStub(NTFS_FILE_BADCLUS);
    Status = WriteMetafile(Handle, Stub, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteMetafile(). Stub for $BadClus write failed. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }

    FREE(Stub);

    // Write stub for $Secure
    Stub = CreateStub(NTFS_FILE_SECURE);
    Status = WriteMetafile(Handle, Stub, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteMetafile(). Stub for $Secure write failed. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }

    FREE(Stub);

    // Write $UpCase
    Status = WriteMetafile(Handle, UpCase, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteMetafile(). $UpCase write failed. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }

    // Create stubs
    for (MftIndex = NTFS_FILE_UPCASE; MftIndex < NTFS_FILE_FIRST_USER_FILE; MftIndex++)
    {
        Stub = CreateStub(MftIndex);
        Status = WriteMetafile(Handle, Stub, &IoStatusBlock);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("WriteMetafile(). Stub #%d write failed. NtWriteFile() failed (Status %lx)\n", MftIndex, Status);
            goto end;
        }

        FREE(Stub);
    }

    // TODO: Write Mft Mirror

end:
    FREE(MFT);
    FREE(MFTMirr);
    FREE(LogFile);
    FREE(Volume);
    FREE(AttrDef);
    FREE(Root);
    FREE(Bitmap);
    FREE(Boot);
    FREE(UpCase);
    FREE(Stub);

    return Status;
}