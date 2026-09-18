/*
 * PROJECT:     ReactOS NVM Express Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SCSI command translation
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "stornvme.h"

#define NDEBUG
#include <debug.h>


/* FUNCTIONS ******************************************************************/

/**
 * @brief Finds the namespace a request is addressed to.
 *
 * Logical unit numbers run from zero while namespace identifiers run from
 * one, so the two are one apart.
 */
PNVME_NAMESPACE
NvmpNamespaceFromLun(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ ULONG Lun)
{
    PNVME_NAMESPACE Namespace;

    if (Lun >= Adapter->NamespaceCount)
        return NULL;

    Namespace = &Adapter->Namespaces[Lun];
    if (!Namespace->Present)
        return NULL;

    return Namespace;
}


/**
 * @brief Describes a failure the way the class layer expects to read it.
 *
 * @return TRUE when the request carried somewhere to put the sense data, which
 *         is what decides whether the caller may mark it valid.
 */
BOOLEAN
NvmpSetSenseData(
    _In_ PVOID Srb,
    _In_ UCHAR SenseKey,
    _In_ UCHAR AdditionalSenseCode,
    _In_ UCHAR AdditionalSenseCodeQualifier)
{
    PSENSE_DATA Sense;
    UCHAR Length;

    Sense = SrbGetSenseInfoBuffer(Srb);
    Length = SrbGetSenseInfoBufferLength(Srb);

    if (Sense == NULL || Length < sizeof(SENSE_DATA))
        return FALSE;

    RtlZeroMemory(Sense, sizeof(SENSE_DATA));

    Sense->ErrorCode = SCSI_SENSE_ERRORCODE_FIXED_CURRENT;
    Sense->Valid = 1;
    Sense->SenseKey = SenseKey;
    Sense->AdditionalSenseLength = sizeof(SENSE_DATA) -
                                   FIELD_OFFSET(SENSE_DATA, AdditionalSenseLength) - 1;
    Sense->AdditionalSenseCode = AdditionalSenseCode;
    Sense->AdditionalSenseCodeQualifier = AdditionalSenseCodeQualifier;

    SrbSetScsiStatus(Srb, SCSISTAT_CHECK_CONDITION);

    return TRUE;
}


/**
 * @brief Finishes a request that failed, with the reason attached.
 *
 * The status only claims the sense data is valid when there was a buffer to
 * put it in, because that is the flag the class layer reads it on.
 */
VOID
NvmpCompleteWithSense(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb,
    _In_ UCHAR SenseKey,
    _In_ UCHAR AdditionalSenseCode,
    _In_ UCHAR AdditionalSenseCodeQualifier)
{
    UCHAR SrbStatus = SRB_STATUS_ERROR;

    if (NvmpSetSenseData(Srb, SenseKey, AdditionalSenseCode,
                         AdditionalSenseCodeQualifier))
    {
        SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
    }

    NvmpCompleteRequest(Adapter, Srb, SrbStatus);
}


/**
 * @brief Copies a field out of the identify data into a fixed width SCSI one.
 *
 * SCSI text fields are blank filled rather than terminated, which is how the
 * identify data carries them too, so what is short gets padded.
 */
static
VOID
NvmpCopyTextField(
    _Out_writes_bytes_(TargetLength) PUCHAR Target,
    _In_ ULONG TargetLength,
    _In_reads_bytes_(SourceLength) const UCHAR *Source,
    _In_ ULONG SourceLength)
{
    ULONG Length = min(TargetLength, SourceLength);

    RtlCopyMemory(Target, Source, Length);
    if (Length < TargetLength)
        RtlFillMemory(Target + Length, TargetLength - Length, ' ');
}


/**
 * @brief Answers the standard inquiry.
 */
static
VOID
NvmpInquiryStandard(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb,
    _In_ PVOID Buffer,
    _In_ ULONG Length)
{
    INQUIRYDATA Inquiry;

    RtlZeroMemory(&Inquiry, sizeof(Inquiry));

    Inquiry.DeviceType = DIRECT_ACCESS_DEVICE;
    Inquiry.DeviceTypeQualifier = DEVICE_CONNECTED;

    /* Claim SPC-3, which is what the command set below amounts to */
    Inquiry.Versions = 5;
    Inquiry.ResponseDataFormat = 2;
    Inquiry.HiSupport = 1;
    Inquiry.CommandQueue = 1;
    Inquiry.AdditionalLength = sizeof(INQUIRYDATA) -
                               FIELD_OFFSET(INQUIRYDATA, AdditionalLength) - 1;

    NvmpCopyTextField(Inquiry.VendorId, sizeof(Inquiry.VendorId),
                      (const UCHAR *)"NVMe", 4);
    NvmpCopyTextField(Inquiry.ProductId, sizeof(Inquiry.ProductId),
                      Adapter->ModelNumber, sizeof(Adapter->ModelNumber));
    NvmpCopyTextField(Inquiry.ProductRevisionLevel, sizeof(Inquiry.ProductRevisionLevel),
                      Adapter->FirmwareRevision, sizeof(Adapter->FirmwareRevision));

    if (Length > sizeof(Inquiry))
        Length = sizeof(Inquiry);

    RtlCopyMemory(Buffer, &Inquiry, Length);
    SrbSetDataTransferLength(Srb, Length);
}


/**
 * @brief Answers one of the vital product data pages.
 */
static
BOOLEAN
NvmpInquiryVpd(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb,
    _In_ PNVME_NAMESPACE Namespace,
    _In_ UCHAR Page,
    _In_ PVOID Buffer,
    _In_ ULONG Length)
{
    UCHAR Data[128];
    ULONG Used;

    RtlZeroMemory(Data, sizeof(Data));

    switch (Page)
    {
        case VPD_SUPPORTED_PAGES:
        {
            PVPD_SUPPORTED_PAGES_PAGE Pages = (PVPD_SUPPORTED_PAGES_PAGE)Data;

            Pages->DeviceType = DIRECT_ACCESS_DEVICE;
            Pages->DeviceTypeQualifier = DEVICE_CONNECTED;
            Pages->PageCode = VPD_SUPPORTED_PAGES;
            Pages->PageLength = 5;
            Pages->SupportedPageList[0] = VPD_SUPPORTED_PAGES;
            Pages->SupportedPageList[1] = VPD_SERIAL_NUMBER;
            Pages->SupportedPageList[2] = VPD_DEVICE_IDENTIFIERS;
            Pages->SupportedPageList[3] = VPD_BLOCK_LIMITS;
            Pages->SupportedPageList[4] = VPD_BLOCK_DEVICE_CHARACTERISTICS;

            Used = FIELD_OFFSET(VPD_SUPPORTED_PAGES_PAGE, SupportedPageList) + 5;
            break;
        }

        case VPD_BLOCK_LIMITS:
        {
            PVPD_BLOCK_LIMITS_PAGE Limits = (PVPD_BLOCK_LIMITS_PAGE)Data;
            ULONG Blocks;
            USHORT Granularity = 1;

            Limits->DeviceType = DIRECT_ACCESS_DEVICE;
            Limits->DeviceTypeQualifier = DEVICE_CONNECTED;
            Limits->PageCode = VPD_BLOCK_LIMITS;
            Limits->PageLength[1] = VPD_BLOCK_LIMITS_LENGTH;

            /* The class layer counts in blocks, the controller in bytes */
            Blocks = Adapter->MaximumTransferLength / Namespace->BlockSize;
            REVERSE_BYTES(Limits->MaximumTransferLength, &Blocks);

            REVERSE_BYTES_SHORT(Limits->OptimalTransferLengthGranularity, &Granularity);

            Used = FIELD_OFFSET(VPD_BLOCK_LIMITS_PAGE, PageLength) +
                   sizeof(Limits->PageLength) + VPD_BLOCK_LIMITS_LENGTH;
            break;
        }

        case VPD_BLOCK_DEVICE_CHARACTERISTICS:
        {
            PVPD_BLOCK_DEVICE_CHARACTERISTICS_PAGE Characteristics =
                (PVPD_BLOCK_DEVICE_CHARACTERISTICS_PAGE)Data;

            Characteristics->DeviceType = DIRECT_ACCESS_DEVICE;
            Characteristics->DeviceTypeQualifier = DEVICE_CONNECTED;
            Characteristics->PageCode = VPD_BLOCK_DEVICE_CHARACTERISTICS;
            Characteristics->PageLength = VPD_BLOCK_LIMITS_LENGTH;

            /* Nothing in here spins, which is what the one means */
            Characteristics->MediumRotationRateLsb = 1;

            Used = FIELD_OFFSET(VPD_BLOCK_DEVICE_CHARACTERISTICS_PAGE, PageLength) +
                   sizeof(Characteristics->PageLength) + VPD_BLOCK_LIMITS_LENGTH;
            break;
        }

        case VPD_SERIAL_NUMBER:
        {
            PVPD_SERIAL_NUMBER_PAGE Serial = (PVPD_SERIAL_NUMBER_PAGE)Data;

            Serial->DeviceType = DIRECT_ACCESS_DEVICE;
            Serial->DeviceTypeQualifier = DEVICE_CONNECTED;
            Serial->PageCode = VPD_SERIAL_NUMBER;
            Serial->PageLength = sizeof(Adapter->SerialNumber);

            NvmpCopyTextField(Serial->SerialNumber,
                              sizeof(Adapter->SerialNumber),
                              Adapter->SerialNumber,
                              sizeof(Adapter->SerialNumber));

            Used = FIELD_OFFSET(VPD_SERIAL_NUMBER_PAGE, SerialNumber) +
                   sizeof(Adapter->SerialNumber);
            break;
        }

        case VPD_DEVICE_IDENTIFIERS:
        {
            PVPD_IDENTIFICATION_PAGE Page83 = (PVPD_IDENTIFICATION_PAGE)Data;
            PVPD_IDENTIFICATION_DESCRIPTOR Descriptor;
            PUCHAR Identifier;
            ULONG IdentifierLength;

            Page83->DeviceType = DIRECT_ACCESS_DEVICE;
            Page83->DeviceTypeQualifier = DEVICE_CONNECTED;
            Page83->PageCode = VPD_DEVICE_IDENTIFIERS;

            /*
             * Built the way the specification suggests for a device with no
             * identifier of its own: the vendor, the model, the serial and
             * the namespace, which together are unique.
             */
            Descriptor = (PVPD_IDENTIFICATION_DESCRIPTOR)Page83->Descriptors;
            Descriptor->CodeSet = VpdCodeSetAscii;
            Descriptor->IdentifierType = VpdIdentifierTypeVendorId;
            Descriptor->Association = VpdAssocDevice;

            Identifier = Descriptor->Identifier;
            IdentifierLength = 0;

            NvmpCopyTextField(Identifier + IdentifierLength, 8,
                              (const UCHAR *)"NVMe", 4);
            IdentifierLength += 8;

            NvmpCopyTextField(Identifier + IdentifierLength,
                              sizeof(Adapter->ModelNumber),
                              Adapter->ModelNumber, sizeof(Adapter->ModelNumber));
            IdentifierLength += sizeof(Adapter->ModelNumber);

            NvmpCopyTextField(Identifier + IdentifierLength,
                              sizeof(Adapter->SerialNumber),
                              Adapter->SerialNumber, sizeof(Adapter->SerialNumber));
            IdentifierLength += sizeof(Adapter->SerialNumber);

            Identifier[IdentifierLength++] = (UCHAR)('0' + (Namespace->NamespaceId % 10));

            Descriptor->IdentifierLength = (UCHAR)IdentifierLength;
            Page83->PageLength = (UCHAR)(FIELD_OFFSET(VPD_IDENTIFICATION_DESCRIPTOR, Identifier) +
                                         IdentifierLength);

            Used = FIELD_OFFSET(VPD_IDENTIFICATION_PAGE, Descriptors) + Page83->PageLength;
            break;
        }

        default:
            return FALSE;
    }

    if (Length > Used)
        Length = Used;

    RtlCopyMemory(Buffer, Data, Length);
    SrbSetDataTransferLength(Srb, Length);

    return TRUE;
}


/**
 * @brief Reports the size of a namespace in the ten byte form.
 *
 * The ten byte form cannot express a namespace past four thousand million
 * blocks, and says so by reporting the largest value it can.
 */
static
VOID
NvmpReadCapacity(
    _In_ PVOID Srb,
    _In_ PNVME_NAMESPACE Namespace,
    _In_ PVOID Buffer,
    _In_ ULONG Length)
{
    READ_CAPACITY_DATA Capacity;
    ULONGLONG LastBlock;

    RtlZeroMemory(&Capacity, sizeof(Capacity));

    LastBlock = Namespace->BlockCount - 1;
    if (LastBlock > MAXULONG)
        LastBlock = MAXULONG;

    REVERSE_BYTES(&Capacity.LogicalBlockAddress, &LastBlock);
    REVERSE_BYTES(&Capacity.BytesPerBlock, &Namespace->BlockSize);

    if (Length > sizeof(Capacity))
        Length = sizeof(Capacity);

    RtlCopyMemory(Buffer, &Capacity, Length);
    SrbSetDataTransferLength(Srb, Length);
}


/**
 * @brief Reports the size of a namespace in the sixteen byte form.
 */
static
VOID
NvmpReadCapacity16(
    _In_ PVOID Srb,
    _In_ PNVME_NAMESPACE Namespace,
    _In_ PVOID Buffer,
    _In_ ULONG Length)
{
    READ_CAPACITY_DATA_EX Capacity;
    ULONGLONG LastBlock;

    RtlZeroMemory(&Capacity, sizeof(Capacity));

    LastBlock = Namespace->BlockCount - 1;

    REVERSE_BYTES_QUAD(&Capacity.LogicalBlockAddress, &LastBlock);
    REVERSE_BYTES(&Capacity.BytesPerBlock, &Namespace->BlockSize);

    if (Length > sizeof(Capacity))
        Length = sizeof(Capacity);

    RtlCopyMemory(Buffer, &Capacity, Length);
    SrbSetDataTransferLength(Srb, Length);
}


/**
 * @brief Lists the logical units behind this target.
 */
static
VOID
NvmpReportLuns(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb,
    _In_ PVOID Buffer,
    _In_ ULONG Length)
{
    PLUN_LIST List = Buffer;
    ULONG Count = 0;
    ULONG Capacity;
    ULONG Lun;
    ULONG Bytes;

    RtlZeroMemory(Buffer, Length);

    if (Length < sizeof(LUN_LIST))
    {
        SrbSetDataTransferLength(Srb, 0);
        return;
    }

    /* Whatever will not fit is still counted, so the caller can ask again */
    Capacity = (Length - sizeof(LUN_LIST)) / 8;

    for (Lun = 0; Lun < Adapter->NamespaceCount; Lun++)
    {
        if (!Adapter->Namespaces[Lun].Present)
            continue;

        if (Count < Capacity)
        {
            /* A logical unit below 256 is named by the second byte alone */
            List->Lun[Count][1] = (UCHAR)Lun;
        }

        Count++;
    }

    Bytes = Count * 8;
    REVERSE_BYTES(&List->LunListLength, &Bytes);

    Bytes = sizeof(LUN_LIST) + min(Count, Capacity) * 8;
    SrbSetDataTransferLength(Srb, min(Bytes, Length));
}


/**
 * @brief Answers a mode sense with the pages this driver has to say something
 *        about.
 *
 * Only the caching page carries anything of interest, and only because the
 * class layer needs to know whether a flush is worth sending.
 */
static
VOID
NvmpModeSense(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb,
    _In_ UCHAR PageCode,
    _In_ PVOID Buffer,
    _In_ ULONG Length)
{
    UCHAR Data[sizeof(MODE_PARAMETER_HEADER) + sizeof(MODE_CACHING_PAGE)];
    PMODE_PARAMETER_HEADER Header = (PMODE_PARAMETER_HEADER)Data;
    PMODE_CACHING_PAGE Caching;
    ULONG Used = sizeof(MODE_PARAMETER_HEADER);

    RtlZeroMemory(Data, sizeof(Data));

    if (PageCode == MODE_PAGE_CACHING || PageCode == MODE_SENSE_RETURN_ALL)
    {
        Caching = (PMODE_CACHING_PAGE)(Data + sizeof(MODE_PARAMETER_HEADER));

        Caching->PageCode = MODE_PAGE_CACHING;
        Caching->PageLength = sizeof(MODE_CACHING_PAGE) -
                              FIELD_OFFSET(MODE_CACHING_PAGE, PageLength) - 1;
        Caching->WriteCacheEnable = Adapter->VolatileWriteCache ? 1 : 0;

        Used += sizeof(MODE_CACHING_PAGE);
    }

    Header->ModeDataLength = (UCHAR)(Used - 1);

    if (Length > Used)
        Length = Used;

    RtlCopyMemory(Buffer, Data, Length);
    SrbSetDataTransferLength(Srb, Length);
}


/**
 * @brief Handles the commands this driver can answer without the controller.
 *
 * @return TRUE when the request has been dealt with and carries its result.
 */
BOOLEAN
NvmpTranslateScsi(
    _In_ PNVME_ADAPTER_EXTENSION Adapter,
    _In_ PVOID Srb)
{
    PNVME_NAMESPACE Namespace;
    PCDB Cdb;
    PVOID Buffer;
    ULONG Length;
    UCHAR Lun;

    Cdb = SrbGetCdb(Srb);
    if (Cdb == NULL)
    {
        SrbSetSrbStatus(Srb, SRB_STATUS_INVALID_REQUEST);
        return TRUE;
    }

    SrbGetPathTargetLun(Srb, NULL, NULL, &Lun);

    Namespace = NvmpNamespaceFromLun(Adapter, Lun);
    if (Namespace == NULL)
    {
        SrbSetSrbStatus(Srb, SRB_STATUS_NO_DEVICE);
        return TRUE;
    }

    Buffer = SrbGetDataBuffer(Srb);
    Length = SrbGetDataTransferLength(Srb);

    switch (Cdb->CDB6GENERIC.OperationCode)
    {
        case SCSIOP_TEST_UNIT_READY:
            SrbSetDataTransferLength(Srb, 0);
            SrbSetSrbStatus(Srb, SRB_STATUS_SUCCESS);
            return TRUE;

        case SCSIOP_INQUIRY:
        {
            if (Buffer == NULL)
            {
                SrbSetSrbStatus(Srb, SRB_STATUS_INVALID_REQUEST);
                return TRUE;
            }

            if (Cdb->CDB6INQUIRY3.EnableVitalProductData)
            {
                if (!NvmpInquiryVpd(Adapter, Srb, Namespace,
                                    Cdb->CDB6INQUIRY3.PageCode, Buffer, Length))
                {
                    UCHAR SrbStatus = SRB_STATUS_ERROR;

                    if (NvmpSetSenseData(Srb, SCSI_SENSE_ILLEGAL_REQUEST,
                                         SCSI_ADSENSE_INVALID_CDB, 0))
                    {
                        SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
                    }

                    SrbSetSrbStatus(Srb, SrbStatus);
                    return TRUE;
                }
            }
            else
            {
                NvmpInquiryStandard(Adapter, Srb, Buffer, Length);
            }

            SrbSetSrbStatus(Srb, SRB_STATUS_SUCCESS);
            return TRUE;
        }

        case SCSIOP_READ_CAPACITY:
            if (Buffer == NULL)
            {
                SrbSetSrbStatus(Srb, SRB_STATUS_INVALID_REQUEST);
                return TRUE;
            }

            NvmpReadCapacity(Srb, Namespace, Buffer, Length);
            SrbSetSrbStatus(Srb, SRB_STATUS_SUCCESS);
            return TRUE;

        case SCSIOP_READ_CAPACITY16:
            if (Cdb->READ_CAPACITY16.ServiceAction != SERVICE_ACTION_READ_CAPACITY16)
                break;

            if (Buffer == NULL)
            {
                SrbSetSrbStatus(Srb, SRB_STATUS_INVALID_REQUEST);
                return TRUE;
            }

            NvmpReadCapacity16(Srb, Namespace, Buffer, Length);
            SrbSetSrbStatus(Srb, SRB_STATUS_SUCCESS);
            return TRUE;

        case SCSIOP_REPORT_LUNS:
            if (Buffer == NULL)
            {
                SrbSetSrbStatus(Srb, SRB_STATUS_INVALID_REQUEST);
                return TRUE;
            }

            NvmpReportLuns(Adapter, Srb, Buffer, Length);
            SrbSetSrbStatus(Srb, SRB_STATUS_SUCCESS);
            return TRUE;

        case SCSIOP_MODE_SENSE:
            if (Buffer == NULL)
            {
                SrbSetSrbStatus(Srb, SRB_STATUS_INVALID_REQUEST);
                return TRUE;
            }

            NvmpModeSense(Adapter, Srb, Cdb->MODE_SENSE.PageCode, Buffer, Length);
            SrbSetSrbStatus(Srb, SRB_STATUS_SUCCESS);
            return TRUE;

        case SCSIOP_START_STOP_UNIT:
        case SCSIOP_MEDIUM_REMOVAL:
        case SCSIOP_VERIFY:
        case SCSIOP_VERIFY16:
            /* Nothing of these applies to a namespace that is always there */
            SrbSetDataTransferLength(Srb, 0);
            SrbSetSrbStatus(Srb, SRB_STATUS_SUCCESS);
            return TRUE;

        default:
            break;
    }

    return FALSE;
}
