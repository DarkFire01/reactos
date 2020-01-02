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
NtfsCreateBlankFileRecord(IN LPCWSTR FileName, IN DWORD32 MftRecordNumber)
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
    AddStandardInformationAttribute(FileRecord, NextAttribute);

    // Calculate pointer to the next attribute
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute + (ULONG_PTR)NextAttribute->Length);

    // Add the $FILE_NAME attribute
    AddFileNameAttribute(FileRecord, NextAttribute, FileName, MftRecordNumber);

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateMft()
{
    PFILE_RECORD_HEADER FileRecord;
    
    // Create MFT file record
    FileRecord = NtfsCreateBlankFileRecord(L"$MFT", NTFS_FILE_MFT);
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

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"$MFTMirr", NTFS_FILE_MFTMIRR);
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

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"$LogFile", NTFS_FILE_LOGFILE);
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

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"$Volume", NTFS_FILE_VOLUME);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for $Volume file record!\n");
        return NULL;
    }

    // Create VOLUME_NAME attribute

    // Create VOLUME_INFORMATION attribute

    // Create resident DATA attribute

    return FileRecord;
}

static
PFILE_RECORD_HEADER
CreateAttrDef()
{
    PFILE_RECORD_HEADER FileRecord;

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"$AttrDef", NTFS_FILE_ATTRDEF);
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

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L".", NTFS_FILE_ROOT);
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

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"$Bitmap", NTFS_FILE_BITMAP);
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

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"$Boot", NTFS_FILE_BOOT);
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

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"$UpCase", NTFS_FILE_ATTRDEF);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for $UpCase file record!\n");
        return NULL;
    }

    // Create unnamed DATA attribute

    // Create $Info DATA attribute

    return FileRecord;
}

/*static
PFILE_RECORD_HEADER
CreateStub(IN DWORD32 MftRecordNumber)
{
    PFILE_RECORD_HEADER FileRecord;

    // Create file record
    FileRecord = NtfsCreateBlankFileRecord(L"", MftRecordNumber);
    if (!FileRecord)
    {
        DPRINT1("ERROR: Unable to allocate memory for stub file record!\n");
        return NULL;
    }

    // Create empty resident DATA attribute

    return FileRecord;
}*/

NTSTATUS
WriteMetafiles(IN HANDLE h, IN GET_LENGTH_INFORMATION* gli)
{
    NTSTATUS Status = STATUS_SUCCESS;

    PFILE_RECORD_HEADER MFT     = CreateMft();
    PFILE_RECORD_HEADER MFTMirr = CreateMFTMirr();
    PFILE_RECORD_HEADER LogFile = CreateLogFile();
    PFILE_RECORD_HEADER Volume  = CreateVolume();
    PFILE_RECORD_HEADER AttrDef = CreateAttrDef();
    PFILE_RECORD_HEADER Root    = CreateRoot();
    PFILE_RECORD_HEADER Bitmap  = CreateBitmap();
    PFILE_RECORD_HEADER Boot    = CreateBoot();  
    PFILE_RECORD_HEADER UpCase  = CreateUpCase();
    //PFILE_RECORD_HEADER Stub; 
    
    if (!MFT    || !MFTMirr || !LogFile ||
        !Volume || !AttrDef || !Root    ||
        !Bitmap || !Boot    || !UpCase)
    {
        DPRINT1("ERROR: Unable to allocate memory for file records!\n");
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto end;
    }

    /* TODO:
     *   Write files
     *   Write MFT, MFTMirr, LogFile, Volume, AttrDef, Root, Bitmap, Boot
     *   Write 2 stubs
     *   Write UpCase
     *   Write 5 stubs
     *   Write mirror of mft
     */

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

    return Status;
}