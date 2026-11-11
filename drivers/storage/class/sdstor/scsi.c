/*
 * PROJECT:     ReactOS Storage Stack / SD storage class driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     SCSI request block handling and SD command mapping
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

#define SD_CMD_SET_BLOCK_COUNT    23
#define SD_CMD_READ_SINGLE_BLOCK  17
#define SD_CMD_READ_MULTI_BLOCK   18
#define SD_CMD_WRITE_SINGLE_BLOCK 24
#define SD_CMD_WRITE_MULTI_BLOCK  25

#define SDSTOR_MAX_TRANSFER_LENGTH (64 * 1024)
#define SDSTOR_MAX_PHYSICAL_PAGES  (SDSTOR_MAX_TRANSFER_LENGTH / PAGE_SIZE + 1)

static
PVOID
SdStorGetDataBuffer(
    _In_ PIRP Irp,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    if (Irp->MdlAddress != NULL)
        return MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);

    return Srb->DataBuffer;
}

static
NTSTATUS
SdStorIssueCommand(
    _In_ PSDSTOR_FDO_EXTENSION FdoExtension,
    _In_ UCHAR Index,
    _In_ ULONG Argument,
    _In_ SDPORT_RESPONSE_TYPE ResponseType)
{
    SDPORT_REQUEST Request;

    RtlZeroMemory(&Request, sizeof(Request));
    Request.Type = SdRequestCommand;
    Request.Data.Command.Index = Index;
    Request.Data.Command.Argument = Argument;
    Request.Data.Command.ResponseType = ResponseType;
    Request.Data.Command.TransferType = SdTransferTypeCommandOnly;

    return SdStorSubmitRequest(FdoExtension->LowerDevice, &Request);
}

static
NTSTATUS
SdStorReadWrite(
    _In_ PSDSTOR_FDO_EXTENSION FdoExtension,
    _In_ PIRP Irp,
    _In_ PSCSI_REQUEST_BLOCK Srb,
    _In_ ULONGLONG Lba,
    _In_ ULONG Blocks,
    _In_ BOOLEAN Write)
{
    PSDSTOR_CARD_DATA Card = &FdoExtension->Card;
    SDPORT_REQUEST Request;
    PMDL Mdl;
    ULONG Argument;
    NTSTATUS Status;

    if (Blocks == 0)
    {
        Srb->DataTransferLength = 0;
        return STATUS_SUCCESS;
    }

    if (Lba + Blocks > Card->BlockCount)
        return STATUS_INVALID_PARAMETER;

    Mdl = Irp->MdlAddress;
    if (Mdl == NULL)
        return STATUS_INVALID_PARAMETER;

    Argument = (Card->Flags & SDSTOR_CARD_HIGH_CAPACITY) ?
               (ULONG)Lba : (ULONG)(Lba * SDSTOR_BLOCK_LENGTH);

    if (Blocks > 1)
    {
        Status = SdStorIssueCommand(FdoExtension, SD_CMD_SET_BLOCK_COUNT, Blocks, SdResponseTypeR1);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    RtlZeroMemory(&Request, sizeof(Request));
    Request.Type = SdRequestCommandWithTransfer;
    Request.Data.Command.Index = Write ?
        (Blocks > 1 ? SD_CMD_WRITE_MULTI_BLOCK : SD_CMD_WRITE_SINGLE_BLOCK) :
        (Blocks > 1 ? SD_CMD_READ_MULTI_BLOCK : SD_CMD_READ_SINGLE_BLOCK);
    Request.Data.Command.Argument = Argument;
    Request.Data.Command.ResponseType = SdResponseTypeR1;
    Request.Data.Command.TransferType =
        (Blocks > 1) ? SdTransferTypeMultiBlock : SdTransferTypeSingleBlock;
    Request.Data.Command.TransferDirection =
        Write ? SdTransferDirectionWrite : SdTransferDirectionRead;
    Request.Data.Command.BlockSize = SDSTOR_BLOCK_LENGTH;
    Request.Data.Command.BlockCount = Blocks;
    Request.Data.Command.Length = Blocks * SDSTOR_BLOCK_LENGTH;
    Request.Mdl = Mdl;
    Request.DataBuffer = SdStorGetDataBuffer(Irp, Srb);
    Request.DataLength = Blocks * SDSTOR_BLOCK_LENGTH;

    Status = SdStorSubmitRequest(FdoExtension->LowerDevice, &Request);
    if (NT_SUCCESS(Status))
        Srb->DataTransferLength = Blocks * SDSTOR_BLOCK_LENGTH;

    return Status;
}

static
NTSTATUS
SdStorInquiry(
    _In_ PSDSTOR_FDO_EXTENSION FdoExtension,
    _In_ PIRP Irp,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PSDSTOR_CARD_DATA Card = &FdoExtension->Card;
    PINQUIRYDATA Inquiry;

    if (Srb->DataTransferLength < sizeof(INQUIRYDATA))
        return STATUS_BUFFER_TOO_SMALL;

    Inquiry = SdStorGetDataBuffer(Irp, Srb);
    if (Inquiry == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Inquiry, sizeof(INQUIRYDATA));
    Inquiry->DeviceType = DIRECT_ACCESS_DEVICE;
    Inquiry->RemovableMedia = 1;
    Inquiry->Versions = 2;
    Inquiry->ResponseDataFormat = 2;
    Inquiry->AdditionalLength = sizeof(INQUIRYDATA) - RTL_SIZEOF_THROUGH_FIELD(INQUIRYDATA, AdditionalLength);
    RtlCopyMemory(Inquiry->VendorId, Card->VendorId, sizeof(Card->VendorId));
    RtlCopyMemory(Inquiry->ProductId, Card->ProductId, sizeof(Card->ProductId));
    RtlCopyMemory(Inquiry->ProductRevisionLevel, Card->ProductRevision, sizeof(Card->ProductRevision));

    Srb->DataTransferLength = sizeof(INQUIRYDATA);
    return STATUS_SUCCESS;
}

static
NTSTATUS
SdStorReadCapacity(
    _In_ PSDSTOR_FDO_EXTENSION FdoExtension,
    _In_ PIRP Irp,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PSDSTOR_CARD_DATA Card = &FdoExtension->Card;
    PREAD_CAPACITY_DATA Capacity;
    ULONG LastLba;

    if (Srb->DataTransferLength < sizeof(READ_CAPACITY_DATA))
        return STATUS_BUFFER_TOO_SMALL;

    Capacity = SdStorGetDataBuffer(Irp, Srb);
    if (Capacity == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (Card->BlockCount > 0xFFFFFFFF)
        LastLba = 0xFFFFFFFF;
    else
        LastLba = (ULONG)(Card->BlockCount - 1);

    Capacity->LogicalBlockAddress = RtlUlongByteSwap(LastLba);
    Capacity->BytesPerBlock = RtlUlongByteSwap(SDSTOR_BLOCK_LENGTH);

    Srb->DataTransferLength = sizeof(READ_CAPACITY_DATA);
    return STATUS_SUCCESS;
}

static
NTSTATUS
SdStorReadCapacity16(
    _In_ PSDSTOR_FDO_EXTENSION FdoExtension,
    _In_ PIRP Irp,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PSDSTOR_CARD_DATA Card = &FdoExtension->Card;
    PREAD_CAPACITY_DATA_EX Capacity;
    ULONGLONG LastLba;

    if (Srb->DataTransferLength < sizeof(READ_CAPACITY_DATA_EX))
        return STATUS_BUFFER_TOO_SMALL;

    Capacity = SdStorGetDataBuffer(Irp, Srb);
    if (Capacity == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Capacity, sizeof(READ_CAPACITY_DATA_EX));
    LastLba = (Card->BlockCount != 0) ? Card->BlockCount - 1 : 0;

    Capacity->LogicalBlockAddress.QuadPart = (LONGLONG)RtlUlonglongByteSwap(LastLba);
    Capacity->BytesPerBlock = RtlUlongByteSwap(SDSTOR_BLOCK_LENGTH);

    Srb->DataTransferLength = sizeof(READ_CAPACITY_DATA_EX);
    return STATUS_SUCCESS;
}

static
NTSTATUS
SdStorModeSense(
    _In_ PSDSTOR_FDO_EXTENSION FdoExtension,
    _In_ PIRP Irp,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PUCHAR Buffer;

    if (Srb->DataTransferLength < 4)
        return STATUS_BUFFER_TOO_SMALL;

    Buffer = SdStorGetDataBuffer(Irp, Srb);
    if (Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Buffer, 4);
    Buffer[0] = 3;
    if (FdoExtension->Card.Flags & SDSTOR_CARD_WRITE_PROTECT)
        Buffer[2] = 0x80;

    Srb->DataTransferLength = 4;
    return STATUS_SUCCESS;
}

static
NTSTATUS
SdStorRequestSense(
    _In_ PIRP Irp,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PSENSE_DATA Sense;

    if (Srb->DataTransferLength < sizeof(SENSE_DATA))
        return STATUS_BUFFER_TOO_SMALL;

    Sense = SdStorGetDataBuffer(Irp, Srb);
    if (Sense == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Sense, sizeof(SENSE_DATA));
    Sense->ErrorCode = 0x70;
    Sense->Valid = 1;
    Sense->AdditionalSenseLength = sizeof(SENSE_DATA) - RTL_SIZEOF_THROUGH_FIELD(SENSE_DATA, AdditionalSenseLength);

    Srb->DataTransferLength = sizeof(SENSE_DATA);
    return STATUS_SUCCESS;
}

static
UCHAR
SdStorSrbStatusFromNtStatus(
    _In_ NTSTATUS Status)
{
    if (NT_SUCCESS(Status))
        return SRB_STATUS_SUCCESS;

    if (Status == STATUS_MEDIA_WRITE_PROTECTED)
        return SRB_STATUS_ERROR;

    if (Status == STATUS_INVALID_DEVICE_REQUEST)
        return SRB_STATUS_INVALID_REQUEST;

    return SRB_STATUS_ERROR;
}

static
NTSTATUS
SdStorExecuteScsi(
    _In_ PSDSTOR_FDO_EXTENSION FdoExtension,
    _In_ PIRP Irp,
    _In_ PSCSI_REQUEST_BLOCK Srb)
{
    PUCHAR Cdb = Srb->Cdb;
    PSDSTOR_CARD_DATA Card = &FdoExtension->Card;
    ULONGLONG Lba;
    ULONG Blocks;
    NTSTATUS Status;

    if (!(Card->Flags & SDSTOR_CARD_PRESENT))
        return STATUS_NO_MEDIA_IN_DEVICE;

    switch (Cdb[0])
    {
        case SCSIOP_TEST_UNIT_READY:
        case SCSIOP_VERIFY:
        case SCSIOP_START_STOP_UNIT:
        case SCSIOP_SYNCHRONIZE_CACHE:
        case SCSIOP_MEDIUM_REMOVAL:
            return STATUS_SUCCESS;

        case SCSIOP_INQUIRY:
            return SdStorInquiry(FdoExtension, Irp, Srb);

        case SCSIOP_READ_CAPACITY:
            return SdStorReadCapacity(FdoExtension, Irp, Srb);

        case SCSIOP_READ_CAPACITY16:
            return SdStorReadCapacity16(FdoExtension, Irp, Srb);

        case SCSIOP_MODE_SENSE:
            return SdStorModeSense(FdoExtension, Irp, Srb);

        case SCSIOP_REQUEST_SENSE:
            return SdStorRequestSense(Irp, Srb);

        case SCSIOP_READ:
            Lba = ((ULONG)Cdb[2] << 24) | ((ULONG)Cdb[3] << 16) | ((ULONG)Cdb[4] << 8) | Cdb[5];
            Blocks = ((ULONG)Cdb[7] << 8) | Cdb[8];
            return SdStorReadWrite(FdoExtension, Irp, Srb, Lba, Blocks, FALSE);

        case SCSIOP_WRITE:
            if (Card->Flags & SDSTOR_CARD_WRITE_PROTECT)
                return STATUS_MEDIA_WRITE_PROTECTED;
            Lba = ((ULONG)Cdb[2] << 24) | ((ULONG)Cdb[3] << 16) | ((ULONG)Cdb[4] << 8) | Cdb[5];
            Blocks = ((ULONG)Cdb[7] << 8) | Cdb[8];
            return SdStorReadWrite(FdoExtension, Irp, Srb, Lba, Blocks, TRUE);

        case SCSIOP_READ16:
            Lba = ((ULONGLONG)Cdb[2] << 56) | ((ULONGLONG)Cdb[3] << 48) |
                  ((ULONGLONG)Cdb[4] << 40) | ((ULONGLONG)Cdb[5] << 32) |
                  ((ULONGLONG)Cdb[6] << 24) | ((ULONGLONG)Cdb[7] << 16) |
                  ((ULONGLONG)Cdb[8] << 8) | Cdb[9];
            Blocks = ((ULONG)Cdb[10] << 24) | ((ULONG)Cdb[11] << 16) |
                     ((ULONG)Cdb[12] << 8) | Cdb[13];
            return SdStorReadWrite(FdoExtension, Irp, Srb, Lba, Blocks, FALSE);

        case SCSIOP_WRITE16:
            if (Card->Flags & SDSTOR_CARD_WRITE_PROTECT)
                return STATUS_MEDIA_WRITE_PROTECTED;
            Lba = ((ULONGLONG)Cdb[2] << 56) | ((ULONGLONG)Cdb[3] << 48) |
                  ((ULONGLONG)Cdb[4] << 40) | ((ULONGLONG)Cdb[5] << 32) |
                  ((ULONGLONG)Cdb[6] << 24) | ((ULONGLONG)Cdb[7] << 16) |
                  ((ULONGLONG)Cdb[8] << 8) | Cdb[9];
            Blocks = ((ULONG)Cdb[10] << 24) | ((ULONG)Cdb[11] << 16) |
                     ((ULONG)Cdb[12] << 8) | Cdb[13];
            return SdStorReadWrite(FdoExtension, Irp, Srb, Lba, Blocks, TRUE);

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    return Status;
}

/* EXECUTE_SCSI drops down to sdport through a synchronous request, which must
   run at PASSIVE_LEVEL.  The class driver can dispatch at DISPATCH_LEVEL, so the
   transfer is handed to a work item. */
typedef struct _SDSTOR_SCSI_WORK
{
    PIO_WORKITEM WorkItem;
    PIRP Irp;
} SDSTOR_SCSI_WORK, *PSDSTOR_SCSI_WORK;

static
VOID
NTAPI
SdStorScsiWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PSDSTOR_SCSI_WORK Work = Context;
    PSDSTOR_PDO_EXTENSION PdoExtension = DeviceObject->DeviceExtension;
    PIRP Irp = Work->Irp;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PSCSI_REQUEST_BLOCK Srb = IoStack->Parameters.Scsi.Srb;
    NTSTATUS Status;

    Status = SdStorExecuteScsi(PdoExtension->FdoExtension, Irp, Srb);

    Srb->SrbStatus = SdStorSrbStatusFromNtStatus(Status);
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Srb->DataTransferLength;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    IoFreeWorkItem(Work->WorkItem);
    ExFreePoolWithTag(Work, SDSTOR_POOL_TAG);
}

NTSTATUS
NTAPI
SdStorDispatchScsi(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PSDSTOR_PDO_EXTENSION PdoExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PSCSI_REQUEST_BLOCK Srb;
    NTSTATUS Status;

    if (PdoExtension->ExtensionType != SdstorExtensionPdo)
    {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    Srb = IoStack->Parameters.Scsi.Srb;
    if (Srb == NULL)
    {
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    if (Srb->Function == SRB_FUNCTION_EXECUTE_SCSI)
    {
        PSDSTOR_SCSI_WORK Work;

        Work = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Work), SDSTOR_POOL_TAG);
        if (Work != NULL)
            Work->WorkItem = IoAllocateWorkItem(DeviceObject);

        if (Work == NULL || Work->WorkItem == NULL)
        {
            if (Work != NULL)
                ExFreePoolWithTag(Work, SDSTOR_POOL_TAG);
            Srb->SrbStatus = SRB_STATUS_ERROR;
            Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Work->Irp = Irp;
        IoMarkIrpPending(Irp);
        IoQueueWorkItem(Work->WorkItem, SdStorScsiWorker, DelayedWorkQueue, Work);
        return STATUS_PENDING;
    }

    switch (Srb->Function)
    {
        case SRB_FUNCTION_CLAIM_DEVICE:
            if (PdoExtension->Claimed)
            {
                Srb->SrbStatus = SRB_STATUS_BUSY;
                Irp->IoStatus.Status = STATUS_DEVICE_BUSY;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_DEVICE_BUSY;
            }
            PdoExtension->Claimed = TRUE;
            Srb->DataBuffer = DeviceObject;
            Status = STATUS_SUCCESS;
            break;

        case SRB_FUNCTION_RELEASE_DEVICE:
            PdoExtension->Claimed = FALSE;
            Status = STATUS_SUCCESS;
            break;

        case SRB_FUNCTION_RELEASE_QUEUE:
        case SRB_FUNCTION_FLUSH:
        case SRB_FUNCTION_SHUTDOWN:
        case SRB_FUNCTION_FLUSH_QUEUE:
            Status = STATUS_SUCCESS;
            break;

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    Srb->SrbStatus = SdStorSrbStatusFromNtStatus(Status);
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Srb->DataTransferLength;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/* Trailing spaces and nulls are not part of a descriptor string. */
static
ULONG
SdStorFieldLength(
    _In_reads_(MaxLength) const UCHAR *Field,
    _In_ ULONG MaxLength)
{
    ULONG Length = MaxLength;

    while (Length > 0 && (Field[Length - 1] == ' ' || Field[Length - 1] == '\0'))
        Length--;

    return Length;
}

static
NTSTATUS
SdStorQueryProperty(
    _In_ PSDSTOR_PDO_EXTENSION PdoExtension,
    _In_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PSDSTOR_CARD_DATA Card = &PdoExtension->FdoExtension->Card;
    ULONG OutputLength = IoStack->Parameters.DeviceIoControl.OutputBufferLength;
    PSTORAGE_PROPERTY_QUERY Query;
    PSTORAGE_DESCRIPTOR_HEADER Header;

    if (IoStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(STORAGE_PROPERTY_QUERY))
        return STATUS_INVALID_PARAMETER;

    Query = Irp->AssociatedIrp.SystemBuffer;

    if (Query->PropertyId != StorageDeviceProperty &&
        Query->PropertyId != StorageAdapterProperty)
        return STATUS_NOT_SUPPORTED;

    if (Query->QueryType == PropertyExistsQuery)
        return STATUS_SUCCESS;

    if (Query->QueryType != PropertyStandardQuery)
        return STATUS_INVALID_PARAMETER;

    if (OutputLength < sizeof(STORAGE_DESCRIPTOR_HEADER))
        return STATUS_BUFFER_TOO_SMALL;

    if (Query->PropertyId == StorageAdapterProperty)
    {
        PSTORAGE_ADAPTER_DESCRIPTOR Adapter;

        if (OutputLength < sizeof(STORAGE_ADAPTER_DESCRIPTOR))
        {
            Header = Irp->AssociatedIrp.SystemBuffer;
            Header->Version = sizeof(STORAGE_ADAPTER_DESCRIPTOR);
            Header->Size = sizeof(STORAGE_ADAPTER_DESCRIPTOR);
            Irp->IoStatus.Information = sizeof(STORAGE_DESCRIPTOR_HEADER);
            return STATUS_SUCCESS;
        }

        Adapter = Irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(Adapter, sizeof(*Adapter));
        Adapter->Version = sizeof(STORAGE_ADAPTER_DESCRIPTOR);
        Adapter->Size = sizeof(STORAGE_ADAPTER_DESCRIPTOR);
        Adapter->MaximumTransferLength = SDSTOR_MAX_TRANSFER_LENGTH;
        Adapter->MaximumPhysicalPages = SDSTOR_MAX_PHYSICAL_PAGES;
        Adapter->AlignmentMask = 0;
        Adapter->BusType = BusTypeSd;
        Irp->IoStatus.Information = sizeof(STORAGE_ADAPTER_DESCRIPTOR);
        return STATUS_SUCCESS;
    }
    else
    {
        PSTORAGE_DEVICE_DESCRIPTOR Device;
        ULONG VendorLength = SdStorFieldLength(Card->VendorId, sizeof(Card->VendorId));
        ULONG ProductLength = SdStorFieldLength(Card->ProductId, sizeof(Card->ProductId));
        ULONG RevisionLength = SdStorFieldLength(Card->ProductRevision, sizeof(Card->ProductRevision));
        ULONG TotalLength = sizeof(STORAGE_DEVICE_DESCRIPTOR) + VendorLength + ProductLength + RevisionLength + 3;
        PUCHAR Buffer;

        if (OutputLength < TotalLength)
        {
            Header = Irp->AssociatedIrp.SystemBuffer;
            Header->Version = TotalLength;
            Header->Size = TotalLength;
            Irp->IoStatus.Information = sizeof(STORAGE_DESCRIPTOR_HEADER);
            return STATUS_SUCCESS;
        }

        Device = Irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(Device, sizeof(STORAGE_DEVICE_DESCRIPTOR));
        Device->Version = sizeof(STORAGE_DEVICE_DESCRIPTOR);
        Device->Size = TotalLength;
        Device->DeviceType = DIRECT_ACCESS_DEVICE;
        Device->RemovableMedia = TRUE;
        Device->CommandQueueing = FALSE;
        Device->BusType = BusTypeSd;
        Device->VendorIdOffset = sizeof(STORAGE_DEVICE_DESCRIPTOR) - sizeof(UCHAR);
        Device->ProductIdOffset = Device->VendorIdOffset + VendorLength + 1;
        Device->ProductRevisionOffset = Device->ProductIdOffset + ProductLength + 1;
        Device->SerialNumberOffset = 0;
        Device->RawPropertiesLength = VendorLength + ProductLength + RevisionLength + 3;

        Buffer = (PUCHAR)Device + sizeof(STORAGE_DEVICE_DESCRIPTOR) - sizeof(UCHAR);
        RtlCopyMemory(Buffer, Card->VendorId, VendorLength);
        Buffer[VendorLength] = '\0';
        Buffer += VendorLength + 1;
        RtlCopyMemory(Buffer, Card->ProductId, ProductLength);
        Buffer[ProductLength] = '\0';
        Buffer += ProductLength + 1;
        RtlCopyMemory(Buffer, Card->ProductRevision, RevisionLength);
        Buffer[RevisionLength] = '\0';

        Irp->IoStatus.Information = TotalLength;
        return STATUS_SUCCESS;
    }
}

NTSTATUS
NTAPI
SdStorDispatchDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PSDSTOR_FDO_EXTENSION FdoExtension = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    if (FdoExtension->ExtensionType == SdstorExtensionFdo)
    {
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(FdoExtension->LowerDevice, Irp);
    }

    switch (IoStack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_STORAGE_QUERY_PROPERTY:
            Status = SdStorQueryProperty((PSDSTOR_PDO_EXTENSION)FdoExtension, Irp);
            break;

        case IOCTL_SCSI_GET_ADDRESS:
        {
            PSCSI_ADDRESS Address = Irp->AssociatedIrp.SystemBuffer;

            if (IoStack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(SCSI_ADDRESS))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            RtlZeroMemory(Address, sizeof(*Address));
            Address->Length = sizeof(SCSI_ADDRESS);
            Irp->IoStatus.Information = sizeof(SCSI_ADDRESS);
            Status = STATUS_SUCCESS;
            break;
        }

        default:
            Status = STATUS_NOT_SUPPORTED;
            break;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}
