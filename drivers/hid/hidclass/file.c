/*
 * PROJECT:     ReactOS HID Class Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Handles on collections, output reports through writes
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

static IO_COMPLETION_ROUTINE HidClassWriteCompletion;

static
NTSTATUS
NTAPI
HidClassCompleteIrp(
    _In_ PIRP Irp,
    _In_ NTSTATUS Status)
{
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/**
 * @brief
 * Finds the session of the opener. A request from session zero may still
 * act for another session through its token.
 */
static
NTSTATUS
NTAPI
HidClassGetRequestorSession(
    _In_ PIRP Irp,
    _In_ PIO_STACK_LOCATION IoStack,
    _Out_ PULONG SessionId)
{
    PSECURITY_SUBJECT_CONTEXT Subject;
    PACCESS_TOKEN Token;
    NTSTATUS Status;

    Status = IoGetRequestorSessionId(Irp, SessionId);
    if (!NT_SUCCESS(Status) || *SessionId != 0)
        return Status;

    if (!IoStack->Parameters.Create.SecurityContext || !IoStack->Parameters.Create.SecurityContext->AccessState)
        return STATUS_SUCCESS;

    Subject = &IoStack->Parameters.Create.SecurityContext->AccessState->SubjectSecurityContext;
    Token = Subject->ClientToken ? Subject->ClientToken : Subject->PrimaryToken;
    if (Token)
        SeQuerySessionIdToken(Token, SessionId);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Checks a new open against the handles already open on the collection.
 */
static
BOOLEAN
NTAPI
HidClassShareAccessAllowed(
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ USHORT ShareAccess)
{
    if (Pdo->ExclusiveCount)
        return FALSE;

    if (Pdo->OpenCount && ShareAccess == 0)
        return FALSE;

    if (Pdo->DenyReadCount && (DesiredAccess & FILE_READ_DATA))
        return FALSE;

    if (Pdo->DenyWriteCount && (DesiredAccess & FILE_WRITE_DATA))
        return FALSE;

    if (Pdo->ReaderCount && !(ShareAccess & FILE_SHARE_READ))
        return FALSE;

    if (Pdo->WriterCount && !(ShareAccess & FILE_SHARE_WRITE))
        return FALSE;

    return TRUE;
}

NTSTATUS
NTAPI
HidClassCreate(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PHIDCLASS_FILE_EXTENSION FileExtension;
    PHIDCLASS_COLLECTION Collection;
    PHIDCLASS_PDO_EXTENSION Pdo;
    PHIDCLASS_FDO_EXTENSION Fdo;
    KPROCESSOR_MODE CheckMode;
    ACCESS_MASK DesiredAccess;
    USHORT ShareAccess;
    BOOLEAN HasTcb;
    ULONG SessionId = 0;
    NTSTATUS Status;
    KIRQL OldIrql;

    if (!DeviceExtension->IsClientPdo)
        return HidClassCompleteIrp(Irp, STATUS_UNSUCCESSFUL);

    Pdo = &DeviceExtension->Pdo;
    Fdo = Pdo->Fdo;

    Status = HidClassGetRequestorSession(Irp, IoStack, &SessionId);
    if (!NT_SUCCESS(Status))
        return HidClassCompleteIrp(Irp, Status);

    Irp->IoStatus.Information = 0;

    Collection = HidClassFindCollection(Fdo, Pdo->CollectionNumber);
    if (!Collection)
        return HidClassCompleteIrp(Irp, STATUS_DEVICE_NOT_CONNECTED);

    /* A forced access check judges a kernel caller as if it came from user mode */
    CheckMode = (IoStack->Flags & SL_FORCE_ACCESS_CHECK) ? UserMode : Irp->RequestorMode;
    HasTcb = SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_TCB_PRIVILEGE), CheckMode);
    if (!HasTcb && (IoStack->Flags & SL_FORCE_ACCESS_CHECK))
        DPRINT("HIDCLASS: forced access check open without TCB privilege\n");

    FileExtension = ExAllocatePoolZero(NonPagedPool, sizeof(*FileExtension), HIDCLASS_TAG);
    if (!FileExtension)
        return HidClassCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES);

    DesiredAccess = IoStack->Parameters.Create.SecurityContext->DesiredAccess;
    ShareAccess = IoStack->Parameters.Create.ShareAccess;

    if (!HidClassShareAccessAllowed(Pdo, DesiredAccess, ShareAccess))
    {
        DPRINT("HIDCLASS: open refused, access %lx share %x\n", DesiredAccess, ShareAccess);
        ExFreePoolWithTag(FileExtension, HIDCLASS_TAG);
        return HidClassCompleteIrp(Irp, STATUS_SHARING_VIOLATION);
    }

    if (IoStack->Parameters.Create.Options & FILE_DIRECTORY_FILE)
    {
        ExFreePoolWithTag(FileExtension, HIDCLASS_TAG);
        return HidClassCompleteIrp(Irp, STATUS_NOT_A_DIRECTORY);
    }

    KeAcquireSpinLock(&Collection->FileLock, &OldIrql);

    if (Fdo->IoState == HidClassIoFail || Pdo->State < HidClassPdoStarted || Pdo->State > HidClassPdoStopped)
    {
        KeReleaseSpinLock(&Collection->FileLock, OldIrql);
        ExFreePoolWithTag(FileExtension, HIDCLASS_TAG);
        return HidClassCompleteIrp(Irp, STATUS_DEVICE_NOT_CONNECTED);
    }

    FileExtension->CollectionNumber = Pdo->CollectionNumber;
    FileExtension->Fdo = Fdo;
    FileExtension->Pdo = Pdo;
    FileExtension->FileObject = IoStack->FileObject;
    FileExtension->DesiredAccess = DesiredAccess;
    FileExtension->ShareAccess = ShareAccess;
    FileExtension->RequestorMode = Irp->RequestorMode;
    FileExtension->SessionId = SessionId;
    FileExtension->HasTcbPrivilege = HasTcb;
    FileExtension->MaxReportCount = HIDCLASS_DEFAULT_INPUT_BUFFERS;
    InitializeListHead(&FileExtension->PendingReads);
    InitializeListHead(&FileExtension->Reports);
    KeInitializeSpinLock(&FileExtension->Lock);

    /* The first reader of a keyboard gets what was typed before it opened */
    if ((DesiredAccess & FILE_READ_DATA) && Collection->Kind == HidClassCollectionKeyboard)
    {
        while (!IsListEmpty(&Collection->UnreadReports))
        {
            InsertTailList(&FileExtension->Reports, RemoveHeadList(&Collection->UnreadReports));
            FileExtension->ReportCount++;
        }
        Collection->UnreadReportCount = 0;
    }

    InsertHeadList(&Collection->FileList, &FileExtension->CollectionLink);
    IoStack->FileObject->FsContext = FileExtension;

    if (DesiredAccess & FILE_READ_DATA)
    {
        InterlockedIncrement(&Pdo->ReaderCount);
        InterlockedIncrement(&Collection->ReaderCount);
    }

    if (DesiredAccess & FILE_WRITE_DATA)
        InterlockedIncrement(&Pdo->WriterCount);

    if (!(ShareAccess & FILE_SHARE_READ))
        InterlockedIncrement(&Pdo->DenyReadCount);

    if (!(ShareAccess & FILE_SHARE_WRITE))
        InterlockedIncrement(&Pdo->DenyWriteCount);

    if (ShareAccess == 0)
        InterlockedIncrement(&Pdo->ExclusiveCount);

    KeReleaseSpinLock(&Collection->FileLock, OldIrql);

    HidClassUpdateOpenCount(Fdo, Collection, Pdo, TRUE);

    return HidClassCompleteIrp(Irp, STATUS_SUCCESS);
}

/**
 * @brief
 * Frees the queued reports of a closing handle and fails its waiting reads.
 */
static
VOID
NTAPI
HidClassDestroyFileExtension(
    _In_ PHIDCLASS_COLLECTION Collection,
    _In_ PHIDCLASS_FILE_EXTENSION FileExtension)
{
    LIST_ENTRY Completed;
    KIRQL OldIrql;
    PIRP Irp;

    HidClassFlushReports(FileExtension);

    InitializeListHead(&Completed);
    KeAcquireSpinLock(&FileExtension->Lock, &OldIrql);
    while ((Irp = HidClassDequeuePendingRead(Collection, FileExtension)) != NULL)
        InsertTailList(&Completed, &Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&FileExtension->Lock, OldIrql);

    while (!IsListEmpty(&Completed))
    {
        Irp = CONTAINING_RECORD(RemoveHeadList(&Completed), IRP, Tail.Overlay.ListEntry);
        Irp->IoStatus.Status = STATUS_DEVICE_NOT_CONNECTED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    ExFreePoolWithTag(FileExtension, HIDCLASS_TAG);
}

NTSTATUS
NTAPI
HidClassClose(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PHIDCLASS_FILE_EXTENSION FileExtension;
    PHIDCLASS_COLLECTION Collection;
    PHIDCLASS_PDO_EXTENSION Pdo;
    PHIDCLASS_FDO_EXTENSION Fdo;
    LONG Remaining;
    KIRQL OldIrql;

    if (!DeviceExtension->IsClientPdo)
        return HidClassCompleteIrp(Irp, STATUS_UNSUCCESSFUL);

    Pdo = &DeviceExtension->Pdo;
    Fdo = Pdo->Fdo;
    Irp->IoStatus.Information = 0;

    FileExtension = IoStack->FileObject->FsContext;
    if (!FileExtension)
        return HidClassCompleteIrp(Irp, STATUS_DEVICE_NOT_CONNECTED);

    Collection = HidClassFindCollection(Fdo, Pdo->CollectionNumber);
    Remaining = HidClassUpdateOpenCount(Fdo, Collection, Pdo, FALSE);
    if (!Collection)
        return HidClassCompleteIrp(Irp, STATUS_DEVICE_NOT_CONNECTED);

    KeAcquireSpinLock(&Collection->FileLock, &OldIrql);

    if (FileExtension->DesiredAccess & FILE_READ_DATA)
    {
        InterlockedDecrement(&Pdo->ReaderCount);
        InterlockedDecrement(&Collection->ReaderCount);
    }

    if (FileExtension->DesiredAccess & FILE_WRITE_DATA)
        InterlockedDecrement(&Pdo->WriterCount);

    if (!(FileExtension->ShareAccess & FILE_SHARE_READ))
        InterlockedDecrement(&Pdo->DenyReadCount);

    if (!(FileExtension->ShareAccess & FILE_SHARE_WRITE))
        InterlockedDecrement(&Pdo->DenyWriteCount);

    if (FileExtension->ShareAccess == 0)
        InterlockedDecrement(&Pdo->ExclusiveCount);

    RemoveEntryList(&FileExtension->CollectionLink);
    KeReleaseSpinLock(&Collection->FileLock, OldIrql);

    if (FileExtension->HasTcbPrivilege)
    {
        KeAcquireSpinLock(&Collection->SecureReadLock, &OldIrql);
        Collection->SecureReadCount -= FileExtension->SecureReadCount;
        FileExtension->SecureReadCount = 0;
        KeReleaseSpinLock(&Collection->SecureReadLock, OldIrql);
    }

    HidClassDestroyFileExtension(Collection, FileExtension);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    /* The device was removed while this was the last open handle */
    if (Remaining == 0)
        HidClassCleanUpFdo(Fdo);

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HidClassWriteCompletion(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PHIDCLASS_PDO_EXTENSION Pdo = Context;
    PHIDP_COLLECTION_DESC CollectionDesc;

    UNREFERENCED_PARAMETER(DeviceObject);

    ExFreePoolWithTag(Irp->UserBuffer, HIDCLASS_TAG);
    Irp->UserBuffer = NULL;

    HidClassEndIo(Pdo->Fdo);

    if (NT_SUCCESS(Irp->IoStatus.Status))
    {
        CollectionDesc = HidClassFindCollectionDesc(Pdo->Fdo, Pdo->CollectionNumber);
        Irp->IoStatus.Information = CollectionDesc ? CollectionDesc->OutputLength : 0;
    }

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    return Irp->IoStatus.Status;
}

/**
 * @brief
 * Sends an output report from a caller buffer to the minidriver.
 *
 * @param[in] Buffer
 * The report, its first byte is the report ID or zero on devices without IDs.
 *
 * @param[out] Sent
 * TRUE when the IRP went to the minidriver and completes from there.
 */
NTSTATUS
NTAPI
HidClassSendWriteReport(
    _In_ PHIDCLASS_FDO_EXTENSION Fdo,
    _In_ PHIDCLASS_PDO_EXTENSION Pdo,
    _In_ PIRP Irp,
    _In_reads_bytes_opt_(Length) PUCHAR Buffer,
    _In_ ULONG Length,
    _Out_ PBOOLEAN Sent)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PHIDCLASS_FILE_EXTENSION FileExtension = NULL;
    PHIDP_COLLECTION_DESC CollectionDesc;
    PHIDCLASS_XFER_PACKET_EX PacketEx;
    PHIDP_REPORT_IDS ReportId;
    PUCHAR Data = Buffer;
    ULONG DataLength = Length;
    ULONG PacketSize;
    BOOLEAN UseEx;
    UCHAR Id = 0;

    *Sent = FALSE;

    if (!Buffer || !Length)
        return STATUS_INVALID_USER_BUFFER;

    if (IoStack->FileObject)
    {
        FileExtension = IoStack->FileObject->FsContext;
        if (!FileExtension)
            return STATUS_PRIVILEGE_NOT_HELD;
    }

    CollectionDesc = HidClassFindCollectionDesc(Fdo, Pdo->CollectionNumber);
    if (!CollectionDesc)
        return STATUS_DEVICE_NOT_CONNECTED;

    if (!CollectionDesc->OutputLength)
        return STATUS_INVALID_DEVICE_REQUEST;

    if (HidClassDeviceUsesReportIds(Fdo))
    {
        Id = Buffer[0];
    }
    else
    {
        Data = Buffer + 1;
        DataLength = Length - 1;
    }

    ReportId = HidClassFindReportId(Fdo, Id);
    if (!ReportId)
        return STATUS_INVALID_PARAMETER;

    if (FileExtension && ReportId->CollectionNumber != FileExtension->CollectionNumber)
        return STATUS_INVALID_DEVICE_REQUEST;

    if (!ReportId->OutputLength || DataLength < ReportId->OutputLength)
        return STATUS_INVALID_PARAMETER;

    /* The timed form only goes to a minidriver that announced it */
    UseEx = (IoStack->MajorFunction == IRP_MJ_DEVICE_CONTROL) && Fdo->WriteReportExSupported;
    PacketSize = UseEx ? sizeof(HIDCLASS_XFER_PACKET_EX) : sizeof(HID_XFER_PACKET);

    PacketEx = ExAllocatePoolZero(NonPagedPool, PacketSize, HIDCLASS_TAG);
    if (!PacketEx)
        return STATUS_INSUFFICIENT_RESOURCES;

    PacketEx->Packet.reportBuffer = Data;
    PacketEx->Packet.reportBufferLen = ReportId->OutputLength;
    PacketEx->Packet.reportId = Id;
    if (UseEx)
    {
        PacketEx->Flags |= HIDCLASS_XFER_FLAG_TIMEOUT;
        PacketEx->Timeout = 0;
    }

    Irp->UserBuffer = PacketEx;
    IoStack = IoGetNextIrpStackLocation(Irp);
    RtlZeroMemory(IoStack, sizeof(*IoStack));
    IoStack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    IoStack->Parameters.DeviceIoControl.IoControlCode = UseEx ? IOCTL_HIDCLASS_WRITE_REPORT_EX : IOCTL_HID_WRITE_REPORT;
    IoStack->Parameters.DeviceIoControl.InputBufferLength = PacketSize;
    IoSetCompletionRoutine(Irp, HidClassWriteCompletion, Pdo, TRUE, TRUE, TRUE);

    *Sent = TRUE;
    return HidClassCallMinidriver(Fdo, Irp);
}

NTSTATUS
NTAPI
HidClassWrite(
    _In_ PHIDCLASS_DEVICE_EXTENSION DeviceExtension,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PHIDCLASS_PDO_EXTENSION Pdo;
    PHIDCLASS_FDO_EXTENSION Fdo;
    BOOLEAN Counted;
    BOOLEAN Sent;
    NTSTATUS Status;

    if (!DeviceExtension->IsClientPdo)
        return HidClassCompleteIrp(Irp, STATUS_INVALID_PARAMETER_1);

    Pdo = &DeviceExtension->Pdo;
    Fdo = Pdo->Fdo;

    if (Pdo->State != HidClassPdoStarted || Fdo->IoState == HidClassIoFail)
        return HidClassCompleteIrp(Irp, STATUS_DEVICE_NOT_CONNECTED);

    Status = HidClassBeginIo(DeviceExtension, Irp, &Counted);
    if (Status == STATUS_PENDING)
        return Status;

    if (!NT_SUCCESS(Status))
        return HidClassCompleteIrp(Irp, Status);

    Status = HidClassSendWriteReport(Fdo,
                                     Pdo,
                                     Irp,
                                     HidClassMapIrpBuffer(Irp),
                                     IoStack->Parameters.Write.Length,
                                     &Sent);
    if (!Sent)
    {
        HidClassCompleteIrp(Irp, Status);
        if (Counted)
            HidClassEndIo(Fdo);
    }

    return Status;
}
