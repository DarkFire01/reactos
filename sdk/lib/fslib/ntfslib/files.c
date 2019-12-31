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
    AddStandardInformationAttribute(FileRecord, NextAttribute);

    // Calculate pointer to the next attribute
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute + (ULONG_PTR)NextAttribute->Length);

    // Add the $FILE_NAME attribute
    AddFileNameAttribute(FileRecord, NextAttribute, MftRecordNumber);

    // Calculate pointer to the next attribute
    NextAttribute = (PNTFS_ATTR_RECORD)((ULONG_PTR)NextAttribute + (ULONG_PTR)NextAttribute->Length);

    // Add the $DATA attribute
    AddDataAttribute(FileRecord, NextAttribute);

    return FileRecord;
}

NTSTATUS
WriteMetafiles(IN HANDLE h)
{
    // FIXME

    NTSTATUS Status;

    NtfsCreateBlankFileRecord(0);

    Status = STATUS_SUCCESS;

    return Status;
}