/*
 * PROJECT:     ReactOS Named Pipe FileSystem
 * LICENSE:     BSD - See COPYING.ARM in the top level directory
 * PURPOSE:     Directory control: listing the pipes and waiting for changes in the root
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "npfs.h"

// File ID number for NPFS bugchecking support
#define NPFS_BUGCHECK_FILE_ID   (NPFS_BUGCHECK_MAIN)

/* FUNCTIONS ******************************************************************/

/* Offset of the name in each directory information class the root answers */
static
ULONG
NpDirectoryEntrySize(
    _In_ FILE_INFORMATION_CLASS InformationClass)
{
    switch (InformationClass)
    {
        case FileDirectoryInformation:
            return FIELD_OFFSET(FILE_DIRECTORY_INFORMATION, FileName);

        case FileFullDirectoryInformation:
            return FIELD_OFFSET(FILE_FULL_DIR_INFORMATION, FileName);

        case FileBothDirectoryInformation:
            return FIELD_OFFSET(FILE_BOTH_DIR_INFORMATION, FileName);

        case FileNamesInformation:
            return FIELD_OFFSET(FILE_NAMES_INFORMATION, FileName);

        default:
            return 0;
    }
}

/**
 * @brief
 * Lists the pipes of the root matching the search expression of the handle.
 * Each entry reports the current instances as end of file and the maximum
 * instances as allocation size.
 */
static
NTSTATUS
NpQueryDirectory(
    _In_ PNP_DCB Dcb,
    _In_ PNP_ROOT_DCB_FCB Ccb,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    FILE_INFORMATION_CLASS InformationClass = IoStack->Parameters.QueryDirectory.FileInformationClass;
    PUNICODE_STRING FileName = IoStack->Parameters.QueryDirectory.FileName;
    ULONG Length = IoStack->Parameters.QueryDirectory.Length;
    PUCHAR Buffer = Irp->UserBuffer;
    ULONG EntrySize, Index, StartIndex, Offset, LastOffset, NameLength;
    PUNICODE_STRING Expression;
    UNICODE_STRING Source;
    PLIST_ENTRY Entry;
    PNP_FCB Fcb;
    NTSTATUS Status = STATUS_SUCCESS;
    BOOLEAN Wrote = FALSE;

    PAGED_CODE();

    if (FileName && (FileName->Length & 1))
        return STATUS_INVALID_PARAMETER;

    EntrySize = NpDirectoryEntrySize(InformationClass);
    if (!EntrySize)
        return STATUS_INVALID_INFO_CLASS;

    /* The first query of a handle fixes its search expression, upcased */
    if (!Ccb->SearchExpression)
    {
        static const WCHAR Star[] = L"*";

        if (FileName && FileName->Length)
        {
            Source = *FileName;
        }
        else
        {
            Source.Buffer = (PWCHAR)Star;
            Source.Length = Source.MaximumLength = sizeof(WCHAR);
        }

        Expression = ExAllocatePoolWithTag(PagedPool, sizeof(*Expression) + Source.Length, NPFS_QUERY_TEMPLATE_TAG);
        if (!Expression)
            return STATUS_INSUFFICIENT_RESOURCES;

        Expression->Buffer = (PWCHAR)(Expression + 1);
        Expression->Length = 0;
        Expression->MaximumLength = Source.Length;
        RtlUpcaseUnicodeString(Expression, &Source, FALSE);
        Ccb->SearchExpression = Expression;
    }

    if (IoStack->Flags & SL_RESTART_SCAN)
        StartIndex = 0;
    else if (IoStack->Flags & SL_INDEX_SPECIFIED)
        StartIndex = IoStack->Parameters.QueryDirectory.FileIndex;
    else
        StartIndex = Ccb->Index;

    Index = 0;
    Offset = 0;
    LastOffset = 0;

    _SEH2_TRY
    {
        for (Entry = Dcb->FcbList.Flink; Entry != &Dcb->FcbList; Entry = Entry->Flink)
        {
            Fcb = CONTAINING_RECORD(Entry, NP_FCB, DcbEntry);

            if (!FsRtlIsNameInExpression(Ccb->SearchExpression, &Fcb->ShortName, TRUE, NULL))
                continue;

            if (Index++ < StartIndex)
                continue;

            /* Stop at the first entry that does not fit, unless nothing fit yet */
            NameLength = Fcb->ShortName.Length;
            if (Offset + EntrySize + NameLength > Length)
            {
                if (Wrote || Offset + EntrySize > Length)
                    break;

                NameLength = Length - Offset - EntrySize;
                Status = STATUS_BUFFER_OVERFLOW;
            }

            RtlZeroMemory(Buffer + Offset, EntrySize);
            if (InformationClass == FileNamesInformation)
            {
                ((PFILE_NAMES_INFORMATION)(Buffer + Offset))->FileNameLength = Fcb->ShortName.Length;
            }
            else
            {
                PFILE_DIRECTORY_INFORMATION Info = (PFILE_DIRECTORY_INFORMATION)(Buffer + Offset);

                Info->EndOfFile.QuadPart = Fcb->CurrentInstances;
                Info->AllocationSize.QuadPart = Fcb->MaximumInstances;
                Info->FileAttributes = FILE_ATTRIBUTE_NORMAL;
                Info->FileNameLength = Fcb->ShortName.Length;
            }
            RtlCopyMemory(Buffer + Offset + EntrySize, Fcb->ShortName.Buffer, NameLength);

            if (Wrote)
                *(PULONG)(Buffer + LastOffset) = Offset - LastOffset;

            Wrote = TRUE;
            Irp->IoStatus.Information = Offset + EntrySize + NameLength;
            if (Status == STATUS_BUFFER_OVERFLOW)
                break;

            Ccb->Index = Index;
            if (IoStack->Flags & SL_RETURN_SINGLE_ENTRY)
                break;

            LastOffset = Offset;
            Offset = ALIGN_UP_BY(Offset + EntrySize + NameLength, sizeof(LONGLONG));
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return _SEH2_GetExceptionCode());
    }
    _SEH2_END;

    return Wrote ? Status : STATUS_NO_MORE_FILES;
}

static
VOID
NTAPI
NpCancelChangeNotifyIrp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    IoReleaseCancelSpinLock(Irp->CancelIrql);

    FsRtlEnterFileSystem();
    NpAcquireExclusiveVcb();
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    NpReleaseVcb();
    FsRtlExitFileSystem();

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NAMED_PIPE_INCREMENT);
}

/* Waits for a pipe to be created or to go away; completes with no data */
static
NTSTATUS
NpNotifyChangeDirectory(
    _In_ PNP_DCB Dcb,
    _In_ PIRP Irp)
{
    PAGED_CODE();

    IoSetCancelRoutine(Irp, NpCancelChangeNotifyIrp);
    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL))
        return STATUS_CANCELLED;

    IoMarkIrpPending(Irp);
    InsertTailList(&Dcb->NotifyList, &Irp->Tail.Overlay.ListEntry);
    return STATUS_PENDING;
}

/**
 * @brief
 * IRP_MJ_DIRECTORY_CONTROL. Only a handle to the root can be listed or watched.
 */
NTSTATUS
NTAPI
NpFsdDirectoryControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    NODE_TYPE_CODE NodeType;
    PNP_ROOT_DCB_FCB Ccb;
    PNP_DCB Dcb;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(DeviceObject);
    TRACE("Entered\n");

    FsRtlEnterFileSystem();
    NpAcquireExclusiveVcb();

    Irp->IoStatus.Information = 0;
    NodeType = NpDecodeFileObject(IoStack->FileObject, (PVOID*)&Dcb, (PNP_CCB*)&Ccb, NULL);
    if (NodeType != NPFS_NTC_ROOT_DCB || !Ccb)
        Status = STATUS_INVALID_PARAMETER;
    else if (IoStack->MinorFunction == IRP_MN_QUERY_DIRECTORY)
        Status = NpQueryDirectory(Dcb, Ccb, Irp);
    else if (IoStack->MinorFunction == IRP_MN_NOTIFY_CHANGE_DIRECTORY)
        Status = NpNotifyChangeDirectory(Dcb, Irp);
    else
        Status = STATUS_INVALID_DEVICE_REQUEST;

    NpReleaseVcb();
    FsRtlExitFileSystem();

    if (Status != STATUS_PENDING)
    {
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NAMED_PIPE_INCREMENT);
    }

    TRACE("Leaving, Status = %lx\n", Status);
    return Status;
}

/* EOF */
