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

/* Returns the index stream of a directory, sized to its allocation. With
 * Create clear, a directory that has none yet gets none. */
static
PNTFS_FCB
NTAPI
NtfsGetIndexStream(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_opt_ PNTFS_FCB DirectoryFcb,
    _In_ ULONGLONG DirectoryMftIndex,
    _In_ PNTFS_ATTR_CONTEXT AllocationContext,
    _In_ BOOLEAN Create)
{
    PNTFS_FCB IndexFcb;
    ULONGLONG Size;
    NTSTATUS Status = STATUS_SUCCESS;

    if (DirectoryFcb == NULL || DirectoryFcb->IndexUncached)
        return NULL;

    Size = AttributeDataLength(AllocationContext->pRecord);

    /* The usual case: made earlier and still the right size */
    IndexFcb = DirectoryFcb->IndexFcb;
    if (IndexFcb != NULL && (ULONGLONG)IndexFcb->RFCB.FileSize.QuadPart == Size)
        return IndexFcb;

    if (IndexFcb == NULL && !Create)
        return NULL;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->IndexStreamResource, TRUE);

    IndexFcb = DirectoryFcb->IndexFcb;
    if (IndexFcb == NULL)
    {
        Status = NtfsCreateMetadataStream(Vcb, FCB_IS_INDEX_STREAM, DirectoryMftIndex, Size, &IndexFcb);
        if (NT_SUCCESS(Status))
            InterlockedExchangePointer((PVOID *)&DirectoryFcb->IndexFcb, IndexFcb);
    }
    else if ((ULONGLONG)IndexFcb->RFCB.FileSize.QuadPart != Size)
    {
        IndexFcb->RFCB.AllocationSize.QuadPart = Size;
        IndexFcb->RFCB.FileSize.QuadPart = Size;
        IndexFcb->RFCB.ValidDataLength.QuadPart = Size;

        _SEH2_TRY
        {
            CcSetFileSizes(IndexFcb->FileObject, (PCC_FILE_SIZES)&IndexFcb->RFCB.AllocationSize);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Reading the index of MFT record %I64u uncached (Status %lx)\n", DirectoryMftIndex, Status);
        InterlockedExchange(&DirectoryFcb->IndexUncached, TRUE);
        IndexFcb = NULL;
    }

    ExReleaseResourceLite(&Vcb->IndexStreamResource);
    KeLeaveCriticalRegion();

    return IndexFcb;
}

/* Copies part of the $I30 allocation, from the directory's cache when it can */
static
ULONG
NTAPI
NtfsCopyIndexAllocation(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_opt_ PNTFS_FCB DirectoryFcb,
    _In_ ULONGLONG DirectoryMftIndex,
    _In_ PNTFS_ATTR_CONTEXT AllocationContext,
    _In_ ULONGLONG Offset,
    _Out_writes_bytes_(Length) PCHAR Buffer,
    _In_ ULONG Length)
{
    PNTFS_FCB IndexFcb = NULL;
    LARGE_INTEGER FileOffset;
    IO_STATUS_BLOCK IoStatus;
    BOOLEAN Copied = FALSE;

    if (Offset + Length <= AttributeDataLength(AllocationContext->pRecord))
        IndexFcb = NtfsGetIndexStream(Vcb, DirectoryFcb, DirectoryMftIndex, AllocationContext, TRUE);

    if (IndexFcb != NULL)
    {
        FileOffset.QuadPart = Offset;

        _SEH2_TRY
        {
            Copied = CcCopyRead(IndexFcb->FileObject, &FileOffset, Length, TRUE, Buffer, &IoStatus) &&
                     NT_SUCCESS(IoStatus.Status) &&
                     IoStatus.Information == Length;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Copied = FALSE;
        }
        _SEH2_END;

        if (Copied)
            return Length;
    }

    /* Whatever the cache couldn't do, the disk still can */
    return ReadAttribute(Vcb, AllocationContext, Offset, Buffer, Length);
}

/**
* @name NtfsReadIndexAllocation
* @implemented
*
* ReadAttribute() for a directory's $I30 allocation, served from the
* directory's index cache when it has an FCB.
*
* @return
* The number of bytes read, zero on failure.
*/
ULONG
NTAPI
NtfsReadIndexAllocation(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ PNTFS_ATTR_CONTEXT AllocationContext,
    _In_ ULONGLONG Offset,
    _Out_writes_bytes_(Length) PCHAR Buffer,
    _In_ ULONG Length)
{
    PNTFS_FCB DirectoryFcb;
    ULONG Read;

    DirectoryFcb = NtfsReferenceDirectoryFcb(Vcb, AllocationContext->FileOwnerMFTIndex);

    Read = NtfsCopyIndexAllocation(Vcb,
                                   DirectoryFcb,
                                   AllocationContext->FileOwnerMFTIndex,
                                   AllocationContext,
                                   Offset,
                                   Buffer,
                                   Length);

    if (DirectoryFcb != NULL)
        NtfsReleaseFCB(Vcb, DirectoryFcb);

    return Read;
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

    if (NtfsCopyIndexAllocation(Vcb,
                                Cursor->DirectoryFcb,
                                Cursor->DirectoryMftIndex,
                                Cursor->AllocationContext,
                                Offset,
                                (PCHAR)Block,
                                BlockSize) != BlockSize)
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
* FCB of the directory, if the caller has one. Index blocks are then read
* through its cache.
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
        /* Writes find the cache by these numbers; if they're off, it can't be kept in step */
        if (FileRecord->MFTRecordNumber != (ULONG)DirectoryMftIndex ||
            Cursor->AllocationContext->FileOwnerMFTIndex != DirectoryMftIndex)
        {
            Cursor->DirectoryFcb = NULL;
        }

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
    PNTFS_FCB DirectoryFcb;
    UNICODE_STRING EntryName;
    BOOLEAN SawNonAscii = FALSE;
    ULONG NameLength = Name->Length / sizeof(WCHAR);
    LONG Order = 1;
    NTSTATUS Status;

    *MftIndex = 0;

    Cursor = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Cursor), TAG_NTFS);
    if (Cursor == NULL)
        return STATUS_MORE_PROCESSING_REQUIRED;

    DirectoryFcb = NtfsReferenceDirectoryFcb(Vcb, DirectoryMftIndex);

    Status = NtfsIndexCursorOpen(Cursor, Vcb, DirectoryFcb, DirectoryMftIndex);
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

    if (DirectoryFcb != NULL)
        NtfsReleaseFCB(Vcb, DirectoryFcb);

    return Status;
}

/**
* @name NtfsPageInIndexStream
* @implemented
*
* Serves a paging read of a directory's index stream from its $I30 allocation.
*
* @param IndexFcb
* FCB of the index stream; its MFTIndex is the directory
*
* @param Irp
* The paging request; Buffer maps its MDL
*
* @param Transferred
* Receives Length on success; the part past the allocation reads as zeroes
*/
NTSTATUS
NTAPI
NtfsPageInIndexStream(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ PNTFS_FCB IndexFcb,
    _In_ PIRP Irp,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length,
    _In_ ULONGLONG Offset,
    _Out_ PULONG Transferred)
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT AllocationContext;
    ULONGLONG ValidLength;
    ULONG ToRead = 0;
    ULONG Read = 0;
    NTSTATUS Status;

    *Transferred = 0;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, IndexFcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Quit;

    Status = FindAttribute(Vcb, FileRecord, AttributeIndexAllocation, L"$I30", 4, &AllocationContext, NULL);
    if (!NT_SUCCESS(Status))
        goto Quit;

    ValidLength = AttributeDataLength(AllocationContext->pRecord);
    if (Offset < ValidLength)
    {
        ToRead = (ULONG)min(Length, ValidLength - Offset);
        ToRead = ROUND_UP(ToRead, Vcb->NtfsInfo.BytesPerSector);

        /* The IRP may only go down whole when nothing is left to zero after it */
        Read = ReadAttributeToIrp(Vcb,
                                  AllocationContext,
                                  Offset,
                                  (PCHAR)Buffer,
                                  ToRead,
                                  (ToRead == Length) ? Irp : NULL);
        if (Read == 0)
            Status = STATUS_UNEXPECTED_IO_ERROR;
    }

    ReleaseAttributeContext(AllocationContext);

    if (NT_SUCCESS(Status))
    {
        if (Read < Length)
            RtlZeroMemory(Buffer + Read, Length - Read);

        *Transferred = Length;
    }

Quit:
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsPageOutIndexStream
* @implemented
*
* Serves a paging write of a directory's index stream, which only the
* write-through of NtfsWriteIndexAllocation() produces.
*
* @param Transferred
* Receives Length on success; what lies past the allocation is dropped
*/
NTSTATUS
NTAPI
NtfsPageOutIndexStream(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ PNTFS_FCB IndexFcb,
    _In_ PIRP Irp,
    _In_reads_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length,
    _In_ ULONGLONG Offset,
    _Out_ PULONG Transferred)
{
    PFILE_RECORD_HEADER FileRecord;
    PNTFS_ATTR_CONTEXT AllocationContext;
    ULONGLONG ValidLength;
    ULONG Inside = 0;
    ULONG Written = 0;
    NTSTATUS Status;

    *Transferred = 0;

    FileRecord = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (FileRecord == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* The record went out before any node that uses new clusters, so its runs are current */
    Status = ReadFileRecord(Vcb, IndexFcb->MFTIndex, FileRecord);
    if (!NT_SUCCESS(Status))
        goto Quit;

    Status = FindAttribute(Vcb, FileRecord, AttributeIndexAllocation, L"$I30", 4, &AllocationContext, NULL);
    if (!NT_SUCCESS(Status))
        goto Quit;

    ValidLength = AttributeDataLength(AllocationContext->pRecord);
    if (Offset < ValidLength)
    {
        /* Always with the IRP: without one, the write would come back through this cache */
        Inside = (ULONG)min(Length, ValidLength - Offset);
        Status = WriteAttributeFromIrp(Vcb,
                                       AllocationContext,
                                       Offset,
                                       Buffer,
                                       Inside,
                                       &Written,
                                       NULL,
                                       Irp);
    }

    ReleaseAttributeContext(AllocationContext);

    if (NT_SUCCESS(Status))
        *Transferred = Length;

Quit:
    ExFreeToNPagedLookasideList(&Vcb->FileRecLookasideList, FileRecord);

    return Status;
}

/**
* @name NtfsWriteIndexAllocation
* @implemented
*
* Writes part of a directory's $I30 allocation through its index cache, so
* the cache stays valid instead of being dropped.
*
* @param AllocationContext
* The allocation being written
*
* @param Handled
* Set when the write went through the cache. When clear, nothing was
* written and the caller writes to disk itself.
*
* @return
* The status of the write when Handled is set.
*
* @remarks Like $MFT, the range is flushed before returning: nothing journals
* our metadata, so it has to be on the disk already.
*/
NTSTATUS
NTAPI
NtfsWriteIndexAllocation(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ PNTFS_ATTR_CONTEXT AllocationContext,
    _In_ ULONGLONG Offset,
    _In_reads_bytes_(Length) const PUCHAR Buffer,
    _In_ ULONG Length,
    _Out_ PBOOLEAN Handled)
{
    ULONGLONG MftIndex = AllocationContext->FileOwnerMFTIndex;
    PNTFS_FCB DirectoryFcb;
    PNTFS_FCB IndexFcb = NULL;
    LARGE_INTEGER FileOffset;
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status = STATUS_SUCCESS;

    *Handled = FALSE;

    DirectoryFcb = NtfsReferenceDirectoryFcb(Vcb, MftIndex);
    if (DirectoryFcb == NULL)
        return STATUS_SUCCESS;

    /* Nothing is cached for a directory without a stream, the disk alone will do */
    if (Offset + Length <= AttributeDataLength(AllocationContext->pRecord))
        IndexFcb = NtfsGetIndexStream(Vcb, DirectoryFcb, MftIndex, AllocationContext, FALSE);

    if (IndexFcb != NULL)
    {
        FileOffset.QuadPart = Offset;

        _SEH2_TRY
        {
            if (!CcCopyWrite(IndexFcb->FileObject, &FileOffset, Length, TRUE, Buffer))
                Status = STATUS_UNSUCCESSFUL;
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            Status = _SEH2_GetExceptionCode();
        }
        _SEH2_END;

        if (NT_SUCCESS(Status))
        {
            CcFlushCache(&IndexFcb->SectionObjectPointers, &FileOffset, Length, &IoStatus);
            Status = IoStatus.Status;
            *Handled = TRUE;
        }
        else
        {
            /* The cache may hold part of it now; it can't be trusted any more */
            DPRINT1("Writing the index of MFT record %I64u through the cache failed (Status %lx)\n", MftIndex, Status);
            InterlockedExchange(&DirectoryFcb->IndexUncached, TRUE);
            Status = STATUS_SUCCESS;
        }
    }

    /* Any other FCB of the directory still holds what was there before */
    if (*Handled)
        NtfsIndexAllocationWritten(Vcb, MftIndex, DirectoryFcb);

    NtfsReleaseFCB(Vcb, DirectoryFcb);

    return Status;
}

/**
* @name NtfsIndexAllocationWritten
* @implemented
*
* Keeps the cached index of a directory in step with a write that went
* straight to its $I30 allocation on disk.
*
* @param MftIndex
* The directory's file record
*
* @param Skip
* An FCB whose cache already holds the write, or NULL
*
* @remarks The disk is always written first, so dropping the cache is enough.
* All of it goes: Cc keeps a view that is only partly in the purged range, and
* Mm then refuses the pages that view still maps. Should Cc refuse anyway,
* because someone is reading through it right now, the directory goes back to
* reading its index from disk.
*/
VOID
NTAPI
NtfsIndexAllocationWritten(
    _In_ PDEVICE_EXTENSION Vcb,
    _In_ ULONGLONG MftIndex,
    _In_opt_ PNTFS_FCB Skip)
{
    PNTFS_FCB Cached[4];
    ULONG Count = 0;
    ULONG i;
    PLIST_ENTRY ListEntry;
    PNTFS_FCB Fcb;
    PNTFS_FCB IndexFcb;
    KIRQL OldIrql;

    /* One FCB per directory is the rule, but every one of them must hear of it */
    KeAcquireSpinLock(&Vcb->FcbListLock, &OldIrql);

    for (ListEntry = Vcb->FcbListHead.Flink;
         ListEntry != &Vcb->FcbListHead;
         ListEntry = ListEntry->Flink)
    {
        Fcb = CONTAINING_RECORD(ListEntry, NTFS_FCB, FcbListEntry);
        if (Fcb == Skip || Fcb->MFTIndex != MftIndex || !NtfsFCBIsDirectory(Fcb) || Fcb->IndexFcb == NULL)
            continue;

        if (Count < ARRAYSIZE(Cached))
        {
            Fcb->RefCount++;
            Cached[Count++] = Fcb;
        }
        else
        {
            InterlockedExchange(&Fcb->IndexUncached, TRUE);
        }
    }

    KeReleaseSpinLock(&Vcb->FcbListLock, OldIrql);

    if (Count == 0)
        return;

    KeEnterCriticalRegion();
    ExAcquireResourceExclusiveLite(&Vcb->IndexStreamResource, TRUE);

    for (i = 0; i < Count; i++)
    {
        IndexFcb = Cached[i]->IndexFcb;
        if (!Cached[i]->IndexUncached &&
            !CcPurgeCacheSection(&IndexFcb->SectionObjectPointers, NULL, 0, FALSE))
        {
            DPRINT1("Couldn't drop the cached index of MFT record %I64u, reading it uncached\n", MftIndex);
            InterlockedExchange(&Cached[i]->IndexUncached, TRUE);
        }
    }

    ExReleaseResourceLite(&Vcb->IndexStreamResource);
    KeLeaveCriticalRegion();

    for (i = 0; i < Count; i++)
        NtfsReleaseFCB(Vcb, Cached[i]);
}

/* EOF */
