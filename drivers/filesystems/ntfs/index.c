/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Directory index walks and the caches behind them
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/**
* @name NtfsCreateMetadataStream
* @implemented
*
* Creates an internal stream file object, and the FCB behind it, so metadata
* can be read through the cache.
*
* @param Vcb
* Volume the stream belongs to
*
* @param Flags
* FCB_IS_MFT_STREAM or FCB_IS_INDEX_STREAM; tells the paging path what to read
*
* @param MftIndex
* File record the stream's attribute lives in
*
* @param Size
* Size of the attribute, in bytes
*
* @param StreamFcb
* Receives the new FCB. Its FileObject is the stream.
*
* @return
* STATUS_SUCCESS, or the failure that stopped us.
*/
NTSTATUS
NTAPI
NtfsCreateMetadataStream(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ ULONG Flags,
    _In_ ULONGLONG MftIndex,
    _In_ ULONGLONG Size,
    _Out_ PNTFS_FCB *StreamFcb)
{
    PNTFS_FCB Fcb;
    PFILE_OBJECT FileObject = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    *StreamFcb = NULL;

    Fcb = NtfsCreateFCB(NULL, NULL, Vcb);
    if (Fcb == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Fcb->Flags = Flags;
    Fcb->MFTIndex = MftIndex;
    Fcb->RefCount = 1;
    Fcb->RFCB.AllocationSize.QuadPart = Size;
    Fcb->RFCB.FileSize.QuadPart = Size;
    Fcb->RFCB.ValidDataLength.QuadPart = Size;
    Fcb->RFCB.IsFastIoPossible = FastIoIsNotPossible;

    _SEH2_TRY
    {
        /* No handle, so no cleanup IRP, and no CCB for close to free */
        FileObject = IoCreateStreamFileObjectLite(NULL, Vcb->StorageDevice);

        /* Takes over the VPB reference the call made, so closing gives it back */
        FileObject->Vpb = Vcb->StorageDevice->Vpb;
        FileObject->FsContext = Fcb;
        FileObject->FsContext2 = NULL;
        FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;
        FileObject->ReadAccess = TRUE;
        FileObject->WriteAccess = TRUE;
        Fcb->FileObject = FileObject;

        CcInitializeCacheMap(FileObject,
                             (PCC_FILE_SIZES)&Fcb->RFCB.AllocationSize,
                             FALSE,
                             &NtfsGlobalData->CacheMgrCallbacks,
                             Fcb);
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
    {
        if (FileObject != NULL)
        {
            FileObject->FsContext = NULL;
            ObDereferenceObject(FileObject);
        }

        NtfsDestroyFCB(Fcb);
        return Status;
    }

    Fcb->Flags |= FCB_CACHE_INITIALIZED;
    *StreamFcb = Fcb;

    return STATUS_SUCCESS;
}

/**
* @name NtfsCloseMetadataStream
* @implemented
*
* Tears down a stream made by NtfsCreateMetadataStream().
*
* @param StreamFcb
* The stream's FCB. Nothing may be reading through it any more.
*
* @remarks The FCB is only freed once Mm let go of the section, the same rule
* NtfsReleaseFCB() follows.
*/
VOID
NTAPI
NtfsCloseMetadataStream(
    _In_ PNTFS_FCB StreamFcb)
{
    LARGE_INTEGER Zero;

    Zero.QuadPart = 0;

    if (StreamFcb->FileObject != NULL)
    {
        CcUninitializeCacheMap(StreamFcb->FileObject, &Zero, NULL);
        ObDereferenceObject(StreamFcb->FileObject);
        StreamFcb->FileObject = NULL;
    }

    if (StreamFcb->SectionObjectPointers.DataSectionObject != NULL)
    {
        DPRINT1("Keeping metadata stream FCB %p: still mapped\n", StreamFcb);
        return;
    }

    NtfsDestroyFCB(StreamFcb);
}

/* SawNonAscii is set when the outcome rested on anything outside ASCII */
static
VOID
NTAPI
NtfsCompareNamesWorker(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_reads_(Length1) PCWCH Name1,
    _In_ ULONG Length1,
    _In_reads_(Length2) PCWCH Name2,
    _In_ ULONG Length2,
    _In_ BOOLEAN Exact,
    _Inout_opt_ PBOOLEAN SawNonAscii,
    _Out_ PLONG Result)
{
    ULONG Shortest = min(Length1, Length2);
    ULONG i;
    WCHAR Upper1, Upper2;

    for (i = 0; i < Shortest; i++)
    {
        if (SawNonAscii != NULL && (Name1[i] >= 0x80 || Name2[i] >= 0x80))
            *SawNonAscii = TRUE;

        Upper1 = Vcb->UpCaseTable[Name1[i]];
        Upper2 = Vcb->UpCaseTable[Name2[i]];
        if (Upper1 != Upper2)
        {
            *Result = (Upper1 < Upper2) ? -1 : 1;
            return;
        }
    }

    if (Length1 != Length2)
    {
        *Result = (Length1 < Length2) ? -1 : 1;
        return;
    }

    *Result = 0;
    if (!Exact)
        return;

    /* Same name but for case: break the tie on the raw code points */
    for (i = 0; i < Shortest; i++)
    {
        if (Name1[i] != Name2[i])
        {
            *Result = (Name1[i] < Name2[i]) ? -1 : 1;
            return;
        }
    }
}

/**
* @name NtfsCompareIndexNames
* @implemented
*
* Orders two names the way a $I30 index keeps them.
*
* @param Vcb
* Volume whose $UpCase folds the names
*
* @param Exact
* When TRUE, names that differ only in case still compare unequal, so the
* result is a total order fit for resuming a scan.
*
* @return
* Less than, equal to or greater than zero as Name1 sorts before, with or
* after Name2.
*/
LONG
NTAPI
NtfsCompareIndexNames(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_reads_(Length1) PCWCH Name1,
    _In_ ULONG Length1,
    _In_reads_(Length2) PCWCH Name2,
    _In_ ULONG Length2,
    _In_ BOOLEAN Exact)
{
    LONG Result;

    NtfsCompareNamesWorker(Vcb, Name1, Length1, Name2, Length2, Exact, NULL, &Result);

    return Result;
}

/* An entry must lie inside its node and hold the key it claims to */
static
BOOLEAN
NTAPI
NtfsIndexEntryIsSane(
    _In_ PINDEX_ENTRY_ATTRIBUTE Entry,
    _In_ PINDEX_ENTRY_ATTRIBUTE End)
{
    ULONG_PTR Room;
    ULONG Needed;

    if ((ULONG_PTR)Entry >= (ULONG_PTR)End)
        return FALSE;

    Room = (ULONG_PTR)End - (ULONG_PTR)Entry;
    if (Room < FIELD_OFFSET(INDEX_ENTRY_ATTRIBUTE, FileName) ||
        Entry->Length < FIELD_OFFSET(INDEX_ENTRY_ATTRIBUTE, FileName) ||
        Entry->Length > Room)
    {
        return FALSE;
    }

    Needed = FIELD_OFFSET(INDEX_ENTRY_ATTRIBUTE, FileName);
    if (!(Entry->Flags & NTFS_INDEX_ENTRY_END))
    {
        if (Entry->KeyLength < FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) ||
            FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + Entry->FileName.NameLength * sizeof(WCHAR) > Entry->KeyLength)
        {
            return FALSE;
        }

        Needed += Entry->KeyLength;
    }

    /* The child's VCN is the entry's last ULONGLONG */
    if (Entry->Flags & NTFS_INDEX_ENTRY_NODE)
        Needed += sizeof(ULONGLONG);

    return (Entry->Length >= Needed);
}

/* Points a level at the entries described by Header, which must fit in Room bytes */
static
NTSTATUS
NTAPI
NtfsIndexSetLevel(
    _Out_ PNTFS_INDEX_LEVEL Level,
    _In_ PINDEX_HEADER_ATTRIBUTE Header,
    _In_ ULONG Room)
{
    if (Header->FirstEntryOffset < sizeof(INDEX_HEADER_ATTRIBUTE) ||
        Header->TotalSizeOfEntries > Room ||
        Header->FirstEntryOffset >= Header->TotalSizeOfEntries)
    {
        return STATUS_DATA_ERROR;
    }

    Level->Entry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)Header + Header->FirstEntryOffset);
    Level->End = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)Header + Header->TotalSizeOfEntries);

    return NtfsIndexEntryIsSane(Level->Entry, Level->End) ? STATUS_SUCCESS : STATUS_DATA_ERROR;
}

/* Reads the index block at Vcn into Block and undoes its fixups */
static
NTSTATUS
NTAPI
NtfsReadIndexBlock(
    _In_ PNTFS_INDEX_CURSOR Cursor,
    _In_ ULONGLONG Vcn,
    _Out_writes_bytes_(Cursor->BlockSize) PINDEX_BUFFER Block)
{
    PDEVICE_EXTENSION Vcb = Cursor->Vcb;
    ULONG BlockSize = Cursor->BlockSize;
    ULONGLONG Offset;

    Offset = GetAllocationOffsetFromVCN(Vcb, BlockSize, Vcn);

    if (ReadAttribute(Vcb, Cursor->AllocationContext, Offset, (PCHAR)Block, BlockSize) != BlockSize)
    {
        DPRINT1("Unable to read index block at VCN %I64u of MFT record %I64u\n", Vcn, Cursor->DirectoryMftIndex);
        return STATUS_UNSUCCESSFUL;
    }

    if (Block->Ntfs.Type != NRH_INDX_TYPE ||
        Block->Ntfs.UsaCount == 0 ||
        (ULONG)(Block->Ntfs.UsaCount - 1) * Vcb->NtfsInfo.BytesPerSector > BlockSize ||
        Block->Ntfs.UsaOffset + Block->Ntfs.UsaCount * sizeof(USHORT) > BlockSize)
    {
        DPRINT1("No index block at VCN %I64u of MFT record %I64u\n", Vcn, Cursor->DirectoryMftIndex);
        return STATUS_DATA_ERROR;
    }

    return FixupUpdateSequenceArray(Vcb, &Block->Ntfs);
}

/**
* @name NtfsIndexCursorOpen
* @implemented
*
* Prepares a walk over the $I30 index of a directory. The cursor stands
* nowhere until NtfsIndexCursorFirst() or NtfsIndexCursorSeekAfter().
*
* @param Cursor
* Cursor to set up
*
* @param Vcb
* Volume the directory lives on
*
* @param DirectoryFcb
* FCB of the directory, if the caller has one
*
* @param DirectoryMftIndex
* File record of the directory
*
* @return
* STATUS_SUCCESS, or the failure that stopped us. The cursor must be closed
* either way.
*/
NTSTATUS
NTAPI
NtfsIndexCursorOpen(
    _Out_ PNTFS_INDEX_CURSOR Cursor,
    _In_ PDEVICE_EXTENSION Vcb,
    _In_opt_ PNTFS_FCB DirectoryFcb,
    _In_ ULONGLONG DirectoryMftIndex)
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT RootContext;
    ULONGLONG RootLength;
    NTSTATUS Status;

    RtlZeroMemory(Cursor, sizeof(*Cursor));
    Cursor->Vcb = Vcb;
    Cursor->DirectoryFcb = DirectoryFcb;
    Cursor->DirectoryMftIndex = DirectoryMftIndex;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, DirectoryMftIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Quit;

    Status = FindAttribute(Vcb, FileRecord, AttributeIndexRoot, L"$I30", 4, &RootContext, NULL);
    if (!NT_SUCCESS(Status))
        goto Quit;

    RootLength = AttributeDataLength(RootContext->pRecord);
    if (RootContext->pRecord->IsNonResident ||
        RootLength < sizeof(INDEX_ROOT_ATTRIBUTE) ||
        RootLength > Vcb->NtfsInfo.BytesPerFileRecord)
    {
        ReleaseAttributeContext(RootContext);
        Status = STATUS_DATA_ERROR;
        goto Quit;
    }

    Cursor->Root = ExAllocatePoolWithTag(NonPagedPool, (ULONG)RootLength, TAG_NTFS);
    if (Cursor->Root == NULL)
    {
        ReleaseAttributeContext(RootContext);
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Quit;
    }

    if (ReadAttribute(Vcb, RootContext, 0, (PCHAR)Cursor->Root, (ULONG)RootLength) != RootLength)
    {
        ReleaseAttributeContext(RootContext);
        Status = STATUS_UNSUCCESSFUL;
        goto Quit;
    }

    ReleaseAttributeContext(RootContext);

    /* Only file name collation is understood here */
    if (Cursor->Root->AttributeType != AttributeFileName ||
        Cursor->Root->CollationRule != COLLATION_FILE_NAME)
    {
        Status = STATUS_NOT_SUPPORTED;
        goto Quit;
    }

    Status = NtfsIndexSetLevel(&Cursor->Levels[0],
                               &Cursor->Root->Header,
                               (ULONG)RootLength - FIELD_OFFSET(INDEX_ROOT_ATTRIBUTE, Header));
    if (!NT_SUCCESS(Status))
        goto Quit;

    Status = FindAttribute(Vcb, FileRecord, AttributeIndexAllocation, L"$I30", 4, &Cursor->AllocationContext, NULL);
    if (NT_SUCCESS(Status))
    {
        Cursor->BlockSize = Cursor->Root->SizeOfEntry;
        if (!Cursor->AllocationContext->pRecord->IsNonResident ||
            Cursor->BlockSize < Vcb->NtfsInfo.BytesPerSector ||
            Cursor->BlockSize > 0x10000 ||
            (Cursor->BlockSize % Vcb->NtfsInfo.BytesPerSector) != 0)
        {
            Status = STATUS_DATA_ERROR;
            goto Quit;
        }
    }
    else if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        /* A small directory keeps all of its entries in the root */
        Cursor->AllocationContext = NULL;
        Status = STATUS_SUCCESS;
    }

Quit:
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return Status;
}

VOID
NTAPI
NtfsIndexCursorClose(
    _Inout_ PNTFS_INDEX_CURSOR Cursor)
{
    ULONG i;

    for (i = 0; i < NTFS_MAX_INDEX_DEPTH; i++)
    {
        if (Cursor->Levels[i].Block != NULL)
            ExFreePoolWithTag(Cursor->Levels[i].Block, TAG_NTFS);
    }

    if (Cursor->AllocationContext != NULL)
        ReleaseAttributeContext(Cursor->AllocationContext);

    if (Cursor->Root != NULL)
        ExFreePoolWithTag(Cursor->Root, TAG_NTFS);

    RtlZeroMemory(Cursor, sizeof(*Cursor));
}

/* Goes down into the subnode of the entry the cursor stands on */
static
NTSTATUS
NTAPI
NtfsIndexCursorPush(
    _Inout_ PNTFS_INDEX_CURSOR Cursor)
{
    PNTFS_INDEX_LEVEL Parent = &Cursor->Levels[Cursor->Depth - 1];
    PNTFS_INDEX_LEVEL Child;
    NTSTATUS Status;

    ASSERT(Parent->Entry->Flags & NTFS_INDEX_ENTRY_NODE);

    if (Cursor->AllocationContext == NULL || Cursor->Depth >= NTFS_MAX_INDEX_DEPTH)
    {
        DPRINT1("Index of MFT record %I64u is corrupt\n", Cursor->DirectoryMftIndex);
        return STATUS_DATA_ERROR;
    }

    Child = &Cursor->Levels[Cursor->Depth];
    if (Child->Block == NULL)
    {
        Child->Block = ExAllocatePoolWithTag(NonPagedPool, Cursor->BlockSize, TAG_NTFS);
        if (Child->Block == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = NtfsReadIndexBlock(Cursor, GetIndexEntryVCN(Parent->Entry), Child->Block);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsIndexSetLevel(Child,
                               &Child->Block->Header,
                               Cursor->BlockSize - FIELD_OFFSET(INDEX_BUFFER, Header));
    if (!NT_SUCCESS(Status))
        return Status;

    Cursor->Depth++;

    return STATUS_SUCCESS;
}

/* Moves to the following entry in the same node */
static
NTSTATUS
NTAPI
NtfsIndexCursorStep(
    _Inout_ PNTFS_INDEX_LEVEL Level)
{
    ASSERT(!(Level->Entry->Flags & NTFS_INDEX_ENTRY_END));

    Level->Entry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)Level->Entry + Level->Entry->Length);

    return NtfsIndexEntryIsSane(Level->Entry, Level->End) ? STATUS_SUCCESS : STATUS_DATA_ERROR;
}

/* Settles on a real entry: first down the left of any subnode, then back up past ends */
static
NTSTATUS
NTAPI
NtfsIndexCursorSettle(
    _Inout_ PNTFS_INDEX_CURSOR Cursor,
    _In_ BOOLEAN DescendFirst)
{
    NTSTATUS Status;

    if (DescendFirst)
    {
        while (Cursor->Levels[Cursor->Depth - 1].Entry->Flags & NTFS_INDEX_ENTRY_NODE)
        {
            Status = NtfsIndexCursorPush(Cursor);
            if (!NT_SUCCESS(Status))
                return Status;
        }
    }

    /* A parent's entry sorts right after the subnode we finished */
    while (Cursor->Depth != 0 &&
           (Cursor->Levels[Cursor->Depth - 1].Entry->Flags & NTFS_INDEX_ENTRY_END))
    {
        Cursor->Depth--;
    }

    return (Cursor->Depth != 0) ? STATUS_SUCCESS : STATUS_NO_MORE_ENTRIES;
}

/* Back to the first entry of the root */
static
VOID
NTAPI
NtfsIndexCursorRewind(
    _Inout_ PNTFS_INDEX_CURSOR Cursor)
{
    Cursor->Depth = 1;
    Cursor->Levels[0].Entry = (PINDEX_ENTRY_ATTRIBUTE)((ULONG_PTR)&Cursor->Root->Header +
                                                        Cursor->Root->Header.FirstEntryOffset);
}

/**
* @name NtfsIndexCursorFirst
* @implemented
*
* Puts the cursor on the first entry of the index.
*
* @return
* STATUS_SUCCESS, STATUS_NO_MORE_ENTRIES for an empty index, or an error.
*/
NTSTATUS
NTAPI
NtfsIndexCursorFirst(
    _Inout_ PNTFS_INDEX_CURSOR Cursor)
{
    NtfsIndexCursorRewind(Cursor);

    return NtfsIndexCursorSettle(Cursor, TRUE);
}

/**
* @name NtfsIndexCursorSeekAfter
* @implemented
*
* Puts the cursor on the first entry that sorts after Name, using the exact
* order of NtfsCompareIndexNames().
*
* @return
* STATUS_SUCCESS, STATUS_NO_MORE_ENTRIES if nothing follows Name, or an error.
*/
NTSTATUS
NTAPI
NtfsIndexCursorSeekAfter(
    _Inout_ PNTFS_INDEX_CURSOR Cursor,
    _In_reads_(NameLength) PCWCH Name,
    _In_ ULONG NameLength)
{
    PNTFS_INDEX_LEVEL Level;
    NTSTATUS Status;

    NtfsIndexCursorRewind(Cursor);

    for (;;)
    {
        Level = &Cursor->Levels[Cursor->Depth - 1];

        while (!(Level->Entry->Flags & NTFS_INDEX_ENTRY_END) &&
               NtfsCompareIndexNames(Cursor->Vcb,
                                     Level->Entry->FileName.Name,
                                     Level->Entry->FileName.NameLength,
                                     Name,
                                     NameLength,
                                     TRUE) <= 0)
        {
            Status = NtfsIndexCursorStep(Level);
            if (!NT_SUCCESS(Status))
                return Status;
        }

        /* Whatever sorts between the previous entry and this one is below it */
        if (!(Level->Entry->Flags & NTFS_INDEX_ENTRY_NODE))
            break;

        Status = NtfsIndexCursorPush(Cursor);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return NtfsIndexCursorSettle(Cursor, FALSE);
}

/**
* @name NtfsIndexCursorNext
* @implemented
*
* Moves the cursor to the entry that sorts right after the current one.
*
* @return
* STATUS_SUCCESS, STATUS_NO_MORE_ENTRIES at the end of the index, or an error.
*/
NTSTATUS
NTAPI
NtfsIndexCursorNext(
    _Inout_ PNTFS_INDEX_CURSOR Cursor)
{
    NTSTATUS Status;

    if (Cursor->Depth == 0)
        return STATUS_NO_MORE_ENTRIES;

    Status = NtfsIndexCursorStep(&Cursor->Levels[Cursor->Depth - 1]);
    if (!NT_SUCCESS(Status))
        return Status;

    return NtfsIndexCursorSettle(Cursor, TRUE);
}

/**
* @name NtfsIndexFindName
* @implemented
*
* Looks a name up in a directory by walking down its index, instead of
* scanning every entry.
*
* @param Vcb
* Volume the directory lives on
*
* @param DirectoryMftIndex
* File record of the directory
*
* @param Name
* Name to find, a single path component
*
* @param CaseSensitive
* Whether the name has to match case too
*
* @param MftIndex
* Receives the file record the name refers to
*
* @return
* STATUS_SUCCESS if found, STATUS_OBJECT_PATH_NOT_FOUND if the directory has
* no such name, or STATUS_MORE_PROCESSING_REQUIRED when the walk can't be sure
* and the caller has to scan the index instead.
*
* @remarks This driver files names with the system upcase table while the walk
* folds through $UpCase. The two agree on ASCII, so only a miss that compared
* other characters, or a case sensitive one, is handed back for a scan. A hit
* is checked the same way the scan would check it.
*/
NTSTATUS
NTAPI
NtfsIndexFindName(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ ULONGLONG DirectoryMftIndex,
    _In_ PUNICODE_STRING Name,
    _In_ BOOLEAN CaseSensitive,
    _Out_ PULONGLONG MftIndex)
{
    PNTFS_INDEX_CURSOR Cursor;
    PNTFS_INDEX_LEVEL Level;
    PINDEX_ENTRY_ATTRIBUTE Entry;
    UNICODE_STRING EntryName;
    BOOLEAN SawNonAscii = FALSE;
    ULONG NameLength = Name->Length / sizeof(WCHAR);
    LONG Order = 1;
    NTSTATUS Status;

    *MftIndex = 0;

    Cursor = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Cursor), TAG_NTFS);
    if (Cursor == NULL)
        return STATUS_MORE_PROCESSING_REQUIRED;

    Status = NtfsIndexCursorOpen(Cursor, Vcb, NULL, DirectoryMftIndex);
    if (!NT_SUCCESS(Status))
    {
        Status = STATUS_MORE_PROCESSING_REQUIRED;
        goto Quit;
    }

    NtfsIndexCursorRewind(Cursor);

    for (;;)
    {
        Level = &Cursor->Levels[Cursor->Depth - 1];
        Entry = Level->Entry;

        while (!(Entry->Flags & NTFS_INDEX_ENTRY_END))
        {
            NtfsCompareNamesWorker(Vcb,
                                   Name->Buffer,
                                   NameLength,
                                   Entry->FileName.Name,
                                   Entry->FileName.NameLength,
                                   FALSE,
                                   &SawNonAscii,
                                   &Order);
            if (Order <= 0)
                break;

            Status = NtfsIndexCursorStep(Level);
            if (!NT_SUCCESS(Status))
            {
                Status = STATUS_MORE_PROCESSING_REQUIRED;
                goto Quit;
            }

            Entry = Level->Entry;
        }

        if (!(Entry->Flags & NTFS_INDEX_ENTRY_END) && Order == 0)
            break;

        if (!(Entry->Flags & NTFS_INDEX_ENTRY_NODE))
        {
            Status = (CaseSensitive || SawNonAscii) ? STATUS_MORE_PROCESSING_REQUIRED
                                                    : STATUS_OBJECT_PATH_NOT_FOUND;
            goto Quit;
        }

        Status = NtfsIndexCursorPush(Cursor);
        if (!NT_SUCCESS(Status))
        {
            Status = STATUS_MORE_PROCESSING_REQUIRED;
            goto Quit;
        }
    }

    /* Same rules as BrowseIndexEntries(); anything else is for it to sort out */
    EntryName.Buffer = Entry->FileName.Name;
    EntryName.Length = EntryName.MaximumLength = Entry->FileName.NameLength * sizeof(WCHAR);

    if ((Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK) >= NTFS_FILE_FIRST_USER_FILE &&
        Entry->FileName.NameType != NTFS_FILE_NAME_DOS &&
        RtlCompareUnicodeString(Name, &EntryName, !CaseSensitive) == 0)
    {
        *MftIndex = Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK;
        Status = STATUS_SUCCESS;
    }
    else
    {
        Status = STATUS_MORE_PROCESSING_REQUIRED;
    }

Quit:
    NtfsIndexCursorClose(Cursor);
    ExFreePoolWithTag(Cursor, TAG_NTFS);

    return Status;
}

/* EOF */
