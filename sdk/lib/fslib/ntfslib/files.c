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


/* CONSTS ********************************************************************/

static const WCHAR* METAFILES_NAMES[] = {
    L"$MFT",
    L"$MFTMirr",
    L"$LogFile",
    L"$Volume",
    L"$AttrDef",
    L".",
    L"$Bitmap",
    L"$Boot",
    L"$BadClus",
    L"$Secure",
    L"$UpCase"
};


/* FUNCTIONS *****************************************************************/

// Create empty file record
static
PFILE_RECORD_HEADER
NtfsCreateEmptyFileRecord(IN DWORD32 MftRecordNumber)
{
    PFILE_RECORD_HEADER FileRecord;
    PATTR_RECORD NextAttribute;

    // Allocate memory for file record
    FileRecord = RtlAllocateHeap(RtlGetProcessHeap(), 0, MFT_RECORD_SIZE);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for file record!\n");
        return NULL;
    }

    RtlZeroMemory(FileRecord, MFT_RECORD_SIZE);

    FileRecord->Header.Magic    = FILE_RECORD_MAGIC;
    FileRecord->MFTRecordNumber = MftRecordNumber;

    // Calculate USA offset and count
    FileRecord->Header.UsaOffset = FIELD_OFFSET(FILE_RECORD_HEADER, MFTRecordNumber) + sizeof(ULONG);

    // Size of USA (in ULONG's) will be 1 (for USA number) + 1 for every sector the file record uses
    FileRecord->BytesAllocated = MFT_RECORD_SIZE;
    FileRecord->Header.UsaCount = (FileRecord->BytesAllocated / BYTES_PER_SECTOR) + 1;

    // Setup other file record fields
    FileRecord->SequenceNumber  = 1;
    FileRecord->FirstAttributeOffset = FileRecord->Header.UsaOffset + (2 * FileRecord->Header.UsaCount);
    FileRecord->FirstAttributeOffset = ALIGN_UP_BY(FileRecord->FirstAttributeOffset, ATTR_RECORD_ALIGNMENT);
    FileRecord->Flags      = MFT_RECORD_IN_USE;
    FileRecord->BytesInUse = FileRecord->FirstAttributeOffset + sizeof(ULONG) * 2;

    // Find where the first attribute will be added
    NextAttribute = (PATTR_RECORD)((ULONG_PTR)FileRecord + FileRecord->FirstAttributeOffset);

    // Temporary mark the end of the file-record
    NextAttribute->Type   = AttributeEnd;
    NextAttribute->Length = FILE_RECORD_END;

    return FileRecord;
}

// Create file record with $STANDARD_INFORMATION and $FILE_NAME attributes
static
PFILE_RECORD_HEADER
NtfsCreateBlankFileRecord(IN  LPCWSTR       FileName,
                          IN  DWORD32       MftRecordNumber,
                          OUT PATTR_RECORD* NextAttribute)
{
    PFILE_RECORD_HEADER FileRecord;

    // Create empty file record
    FileRecord = NtfsCreateEmptyFileRecord(MftRecordNumber);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for file record!\n");
        return NULL;
    }

    // $STANDARD_INFORMATION
    (*NextAttribute) = FIRST_ATTRIBUTE(FileRecord);
    AddStandardInformationAttribute(FileRecord, *NextAttribute);

    // $FILE_NAME
    (*NextAttribute) = NEXT_ATTRIBUTE(*NextAttribute);
    AddFileNameAttribute(FileRecord, *NextAttribute, FileName, MftRecordNumber);

    (*NextAttribute) = NEXT_ATTRIBUTE(*NextAttribute);
   
    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateMetaFileRecord(IN  DWORD32       MftRecordNumber,
                     OUT PATTR_RECORD* Attribute)
{
    PFILE_RECORD_HEADER FileRecord;

    FileRecord = NtfsCreateBlankFileRecord(METAFILES_NAMES[MftRecordNumber], MftRecordNumber, Attribute);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for %S file record!\n", METAFILES_NAMES[MftRecordNumber]);
        return NULL;
    }

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateMft()
{
    PFILE_RECORD_HEADER FileRecord;
    PATTR_RECORD        Attribute = NULL;
    
    // Create file record
    FileRecord = CreateMetaFileRecord(METAFILE_MFT, &Attribute);
    if (!FileRecord)
    {
        return NULL;
    }

    // $DATA
    AddNonResidentSingleRunDataAttribute(FileRecord, 
                                         Attribute,
                                         MFT_LOCATION,
                                         MFT_DEFAULT_CLUSTERS_SIZE);

    // $BITMAP
    Attribute = NEXT_ATTRIBUTE(Attribute);
    AddMftBitmapAttribute(FileRecord,
                          Attribute);

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateMFTMirr()
{
    PFILE_RECORD_HEADER FileRecord;
    PATTR_RECORD        Attribute = NULL;

    // Create file record
    FileRecord = CreateMetaFileRecord(METAFILE_MFTMIRR, &Attribute);
    if (!FileRecord)
    {
        return NULL;
    }

    // TODO: $DATA

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateLogFile()
{
    PFILE_RECORD_HEADER FileRecord;
    PATTR_RECORD        Attribute = NULL;

    // Create file record
    FileRecord = CreateMetaFileRecord(METAFILE_LOGFILE, &Attribute);
    if (!FileRecord)
    {
        return NULL;
    }

    // TODO: $DATA

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateVolume()
{
    PFILE_RECORD_HEADER FileRecord;
    PATTR_RECORD        Attribute = NULL;

    // Create file record
    FileRecord = CreateMetaFileRecord(METAFILE_VOLUME, &Attribute);
    if (!FileRecord)
    {
        return NULL;
    }

    // $VOLUME_NAME
    AddEmptyVolumeNameAttribute(FileRecord, Attribute);

    // $VOLUME_INFORMATION
    Attribute = NEXT_ATTRIBUTE(Attribute);
    AddVolumeInformationAttribute(FileRecord, 
                                  Attribute, 
                                  NTFS_MAJOR_VERSION, 
                                  NTFS_MINOR_VERSION);

    // $DATA
    Attribute = NEXT_ATTRIBUTE(Attribute);
    AddEmptyDataAttribute(FileRecord, Attribute);

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateAttrDef()
{
    PFILE_RECORD_HEADER FileRecord;
    PATTR_RECORD        Attribute = NULL;

    // Create file record
    FileRecord = CreateMetaFileRecord(METAFILE_ATTRDEF, &Attribute);
    if (!FileRecord)
    {
        return NULL;
    }
    
    // TODO: $DATA

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateRoot()
{
    // FIXME!

    PFILE_RECORD_HEADER FileRecord;
    PATTR_RECORD   Attribute = NULL;

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L".", METAFILE_ROOT, &Attribute);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for . file record!\n");
        return NULL;
    }

    // TODO: $INDEX_ROOT

    // TODO: $INDEX_ALLOCATION

    // TODO: $BITMAP

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateBitmap()
{
    PFILE_RECORD_HEADER FileRecord;
    PATTR_RECORD        Attribute = NULL;

    // Create file record
    FileRecord = CreateMetaFileRecord(METAFILE_BITMAP, &Attribute);
    if (!FileRecord)
    {
        return NULL;
    }

    // TODO: $DATA

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateBoot()
{
    PFILE_RECORD_HEADER FileRecord;
    PATTR_RECORD        Attribute = NULL;

    // Create file record
    FileRecord = CreateMetaFileRecord(METAFILE_BOOT, &Attribute);
    if (!FileRecord)
    {
        return NULL;
    }

    // TODO: $DATA

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateUpCase()
{
    PFILE_RECORD_HEADER FileRecord;
    PATTR_RECORD        Attribute = NULL;

    // Create file record
    FileRecord = CreateMetaFileRecord(METAFILE_UPCASE, &Attribute);
    if (!FileRecord)
    {
        return NULL;
    }

    // TODO: $DATA

    // TODO: $Info DATA

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateStub(IN DWORD32 MftRecordNumber)
{
    PFILE_RECORD_HEADER FileRecord;
    PATTR_RECORD   Attribute = NULL;

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"", MftRecordNumber, &Attribute);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for stub #%d file record!\n", MftRecordNumber);
        return NULL;
    }

    // Create empty resident $DATA attribute
    AddEmptyDataAttribute(FileRecord, Attribute);

    return FileRecord;
}

static
NTSTATUS
WriteZerosToClusters(IN  HANDLE                  Handle,
                     IN  LONGLONG                Address,
                     IN  ULONG                   ClustersCount,
                     OUT PIO_STATUS_BLOCK        IoStatusBlock)
{
    PBYTE         Zeros;
    ULONG         Size;
    LARGE_INTEGER Offset;
    NTSTATUS      Status;

    Size = BYTES_PER_CLUSTER * ClustersCount;

    Zeros = RtlAllocateHeap(RtlGetProcessHeap(), 0, Size);
    if (!Zeros)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Zeros, Size);

    Offset.QuadPart = Address * BYTES_PER_CLUSTER;

    Status = NtWriteFile(Handle,
                         NULL,
                         NULL,
                         NULL,
                         IoStatusBlock,
                         Zeros,
                         Size,
                         &Offset,
                         NULL);

    FREE(Zeros);

    return Status;
}

static
NTSTATUS
WriteMetafile(IN  HANDLE                   Handle,
              IN  PFILE_RECORD_HEADER      FileRecord, 
              OUT PIO_STATUS_BLOCK         IoStatusBlock)
{
    LARGE_INTEGER Offset;

    // Offset to $MFT + offset to record
    Offset.QuadPart = 
        ((LONGLONG)MFT_LOCATION * BYTES_PER_CLUSTER) +
        (LONGLONG)(FileRecord->MFTRecordNumber * MFT_RECORD_SIZE);

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

static
NTSTATUS
WriteMftBitmap(IN  HANDLE                   Handle,
               OUT PIO_STATUS_BLOCK         IoStatusBlock)
{
    PBYTE         Data;
    LARGE_INTEGER Offset;
    NTSTATUS      Status;

    Data = RtlAllocateHeap(RtlGetProcessHeap(), 0, BYTES_PER_SECTOR);
    if (!Data)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Data, BYTES_PER_SECTOR);

    // Every bit is cluster
    // First 16 clusters used by metafiles
    Data[0] = 0xFF;
    Data[1] = 0xFF;

    Offset.QuadPart = MFT_BITMAP_ADDRESS * BYTES_PER_CLUSTER;

    Status = NtWriteFile(Handle,
                         NULL,
                         NULL,
                         NULL,
                         IoStatusBlock,
                         Data,
                         BYTES_PER_SECTOR,
                         &Offset,
                         NULL);

    FREE(Data);

    return Status;
}

NTSTATUS
WriteMetafiles(IN HANDLE Handle)
{
    // FIXME: Rework structure of this

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

    // Clear first clusters
    Status = WriteZerosToClusters(Handle,
                                  MFT_LOCATION,
                                  MFT_DEFAULT_CLUSTERS_SIZE,
                                  &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteZerosToClusters(). Failed to clear sectors. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }

    // Write $MFT
    Status = WriteMetafile(Handle, MFT, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteMetafile(). $MFT write failed. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }
    
    // Clear bitmap cluster for $MFT
    Status = WriteZerosToClusters(Handle,
                                  MFT_BITMAP_ADDRESS,
                                  1,
                                  &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteZerosToClusters(). Failed to clear sectors for bitmap. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }

    // Write bitmap for $MFT
    Status = WriteMftBitmap(Handle, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Write bitmap unsuccessful. $MFT not completed. NtWriteFile() failed (Status %lx)\n", Status);
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
    Stub = CreateStub(METAFILE_BADCLUS);
    Status = WriteMetafile(Handle, Stub, &IoStatusBlock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WriteMetafile(). Stub for $BadClus write failed. NtWriteFile() failed (Status %lx)\n", Status);
        goto end;
    }

    FREE(Stub);

    // Write stub for $Secure
    Stub = CreateStub(METAFILE_SECURE);
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
    for (MftIndex = METAFILE_UPCASE+1; MftIndex < METAFILE_FIRST_USER_FILE; MftIndex++)
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