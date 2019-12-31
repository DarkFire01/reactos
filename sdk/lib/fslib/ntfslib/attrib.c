/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NTFS FS library
 * FILE:        lib/fslib/ntfslib/attrib.c
 * PURPOSE:     NTFS lib
 * PROGRAMMERS: Pierre Schweitzer, Klachkov Valery
 */

/* INCLUDES ******************************************************************/

#include <ntfslib.h>

#define NDEBUG
#include <debug.h>


/* FUNCTIONS *****************************************************************/

static
VOID
SetFileRecordEnd(OUT PFILE_RECORD_HEADER FileRecord,
                 OUT PNTFS_ATTR_RECORD   AttrEnd,
                 IN  ULONG               EndMarker)
{
    // Ensure AttrEnd is aligned on an 8-byte boundary, relative to FileRecord
    ASSERT(((ULONG_PTR)AttrEnd - (ULONG_PTR)FileRecord) % ATTR_RECORD_ALIGNMENT == 0);

    // Mark the end of attributes
    AttrEnd->Type = AttributeEnd;

    // Restore the "file-record-end marker." The value is never checked but this behavior is consistent with Win2k3.
    AttrEnd->Length = EndMarker;

    // Recalculate bytes in use
    FileRecord->BytesInUse = (ULONG_PTR)AttrEnd - (ULONG_PTR)FileRecord + sizeof(ULONG) * 2;
}

VOID
AddStandardInformationAttribute(OUT PFILE_RECORD_HEADER FileRecord,
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

VOID
AddFileNameAttribute(OUT PFILE_RECORD_HEADER FileRecord,
                     OUT PNTFS_ATTR_RECORD   AttributeAddress,
                     IN  DWORD32             MftRecordNumber)
{
    ULONG ResidentHeaderLength = FIELD_OFFSET(NTFS_ATTR_RECORD, Resident.Reserved) + sizeof(UCHAR);
    PFILENAME_ATTRIBUTE FileNameAttribute;
    ULONG FileRecordEnd = AttributeAddress->Length;
    UNICODE_STRING FilenameNoPath;
    LARGE_INTEGER SystemTime;

    AttributeAddress->Type = AttributeFileName;
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

    FileNameAttribute->DirectoryFileReferenceNumber |= (ULONGLONG)NTFS_FILE_ROOT << 48;

    FileNameAttribute->NameLength = FilenameNoPath.Length / sizeof(WCHAR);
    RtlCopyMemory(FileNameAttribute->Name, FilenameNoPath.Buffer, FilenameNoPath.Length);

    // For now, we're emulating the way Windows behaves when 8.3 name generation is disabled
    if (RtlIsNameLegalDOS8Dot3(&FilenameNoPath, NULL, NULL))
    {
        FileNameAttribute->NameType = NTFS_FILE_NAME_WIN32_AND_DOS;
    }
    else
    {
        FileNameAttribute->NameType = NTFS_FILE_NAME_POSIX;
    }

    FileRecord->LinkCount++;

    AttributeAddress->Length =
        ResidentHeaderLength +
        FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) +
        FilenameNoPath.Length;

    AttributeAddress->Length = ALIGN_UP_BY(AttributeAddress->Length, ATTR_RECORD_ALIGNMENT);

    AttributeAddress->Resident.ValueLength = FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + FilenameNoPath.Length;
    AttributeAddress->Resident.ValueOffset = ResidentHeaderLength;
    AttributeAddress->Resident.Flags = RA_INDEXED;

    // Move the attribute-end and file-record-end markers to the end of the file record
    AttributeAddress = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttributeAddress + AttributeAddress->Length);
    SetFileRecordEnd(FileRecord, AttributeAddress, FileRecordEnd);
}

VOID
AddDataAttribute(OUT PFILE_RECORD_HEADER FileRecord,
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
    AttributeAddress->Instance   = FileRecord->NextAttributeNumber++;

    // Move the attribute-end and file-record-end markers to the end of the file record
    AttributeAddress = (PNTFS_ATTR_RECORD)((ULONG_PTR)AttributeAddress + AttributeAddress->Length);
    SetFileRecordEnd(FileRecord, AttributeAddress, FileRecordEnd);
}
