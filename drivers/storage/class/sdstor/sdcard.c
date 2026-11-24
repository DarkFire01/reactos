/*
 * PROJECT:     ReactOS Storage Stack / SD storage class driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Card register decoding, capacity and identity
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

/* Extended CSD offset of the little endian 32 bit sector count. */
#define MMC_EXT_CSD_SEC_COUNT 212

typedef struct _SDSTOR_MMC_VENDOR
{
    UCHAR Id;
    PCSTR Name;
} SDSTOR_MMC_VENDOR;

/* JEDEC manufacturer IDs seen on eMMC parts. */
static const SDSTOR_MMC_VENDOR SdStorMmcVendors[] =
{
    { 0x11, "Toshiba" },
    { 0x13, "Micron" },
    { 0x15, "Samsung" },
    { 0x45, "SanDisk" },
    { 0x70, "Kingston" },
    { 0x90, "Hynix" },
    { 0xFE, "Micron" }
};

/* Extract the inclusive bit range [HighBit:LowBit] from a 16-byte register that
   is stored most significant byte first, with bit 127 in Register[0] bit 7. */
static
ULONG
SdStorExtractBits(
    _In_reads_bytes_(16) const UCHAR *Register,
    _In_ ULONG HighBit,
    _In_ ULONG LowBit)
{
    ULONG Value = 0;
    ULONG Bit;

    for (Bit = HighBit; ; Bit--)
    {
        UCHAR Byte = Register[15 - (Bit >> 3)];
        Value = (Value << 1) | ((Byte >> (Bit & 7)) & 1);

        if (Bit == LowBit)
            break;
    }

    return Value;
}

static
VOID
SdStorFillString(
    _Out_writes_(Size) PUCHAR Destination,
    _In_ ULONG Size,
    _In_reads_(Count) const UCHAR *Source,
    _In_ ULONG Count)
{
    ULONG Index;

    for (Index = 0; Index < Size; Index++)
    {
        if (Index < Count && Source[Index] >= 0x20 && Source[Index] < 0x7F)
            Destination[Index] = Source[Index];
        else
            Destination[Index] = ' ';
    }
}

/* PRV is two BCD style nibbles, shown as "n.m". */
static
VOID
SdStorFillRevision(
    _Out_writes_(4) PUCHAR Revision,
    _In_ UCHAR Value)
{
    static const CHAR Digits[] = "0123456789ABCDEF";

    Revision[0] = Digits[(Value >> 4) & 0xF];
    Revision[1] = '.';
    Revision[2] = Digits[Value & 0xF];
    Revision[3] = ' ';
}

static
VOID
SdStorDecodeSdCid(
    _Inout_ PSDSTOR_CARD_DATA Card)
{
    /* SD CID: MID[0], OID[1..2], PNM[3..7] (5 chars), PRV[8], PSN[9..12]. */
    SdStorFillString(Card->VendorId, sizeof(Card->VendorId), &Card->Cid[1], 2);
    SdStorFillString(Card->ProductId, sizeof(Card->ProductId), &Card->Cid[3], 5);
    SdStorFillRevision(Card->ProductRevision, Card->Cid[8]);
    RtlCopyMemory(Card->SerialNumber, &Card->Cid[9], sizeof(Card->SerialNumber));
}

static
VOID
SdStorDecodeMmcCid(
    _Inout_ PSDSTOR_CARD_DATA Card)
{
    PCSTR Vendor = "";
    ULONG Index;

    /* MMC CID: MID[0], CBX[1], OID[2], PNM[3..8] (6 chars), PRV[9], PSN[10..13]. */
    for (Index = 0; Index < RTL_NUMBER_OF(SdStorMmcVendors); Index++)
    {
        if (SdStorMmcVendors[Index].Id == Card->Cid[0])
        {
            Vendor = SdStorMmcVendors[Index].Name;
            break;
        }
    }

    SdStorFillString(Card->VendorId,
                     sizeof(Card->VendorId),
                     (const UCHAR *)Vendor,
                     (ULONG)strlen(Vendor));
    SdStorFillString(Card->ProductId, sizeof(Card->ProductId), &Card->Cid[3], 6);
    SdStorFillRevision(Card->ProductRevision, Card->Cid[9]);
    RtlCopyMemory(Card->SerialNumber, &Card->Cid[10], sizeof(Card->SerialNumber));
}

/* C_SIZE, C_SIZE_MULT and READ_BL_LEN, shared by SD 1.x and every MMC CSD layout. */
static
ULONGLONG
SdStorLegacyCsdCapacity(
    _In_ PSDSTOR_CARD_DATA Card)
{
    ULONG ReadBlockLen = SdStorExtractBits(Card->Csd, 83, 80);
    ULONG CSize = SdStorExtractBits(Card->Csd, 73, 62);
    ULONG CSizeMult = SdStorExtractBits(Card->Csd, 49, 47);
    ULONGLONG BlockNr = (ULONGLONG)(CSize + 1) << (CSizeMult + 2);

    return BlockNr * ((ULONGLONG)1 << ReadBlockLen);
}

static
VOID
SdStorDecodeCsdFlags(
    _Inout_ PSDSTOR_CARD_DATA Card)
{
    if (SdStorExtractBits(Card->Csd, 13, 13) != 0 ||
        SdStorExtractBits(Card->Csd, 12, 12) != 0)
    {
        Card->Flags |= SDSTOR_CARD_WRITE_PROTECT;
    }
}

static
VOID
SdStorDecodeSdCsd(
    _Inout_ PSDSTOR_CARD_DATA Card)
{
    ULONG CsdStructure;
    ULONGLONG Capacity;

    CsdStructure = (Card->Csd[0] >> 6) & 0x3;

    if (CsdStructure == 0)
    {
        Capacity = SdStorLegacyCsdCapacity(Card);
    }
    else
    {
        ULONG CSize = SdStorExtractBits(Card->Csd, 69, 48);

        Capacity = (ULONGLONG)(CSize + 1) * 512 * 1024;
    }

    SdStorDecodeCsdFlags(Card);
    Card->CapacityBytes = Capacity;
}

/* Sector addressed devices report their size only in EXT_CSD SEC_COUNT. */
static
NTSTATUS
SdStorDecodeMmcCapacity(
    _In_ PSDSTOR_FDO_EXTENSION FdoExtension)
{
    PSDSTOR_CARD_DATA Card = &FdoExtension->Card;
    PUCHAR ExtCsd;
    ULONG SectorCount;
    NTSTATUS Status;

    SdStorDecodeCsdFlags(Card);

    if (!(Card->Flags & SDSTOR_CARD_HIGH_CAPACITY))
    {
        Card->CapacityBytes = SdStorLegacyCsdCapacity(Card);
        return STATUS_SUCCESS;
    }

    if (Card->SpecVersion < 4)
        return STATUS_UNRECOGNIZED_MEDIA;

    ExtCsd = ExAllocatePoolWithTag(NonPagedPool, SDPORT_EXT_CSD_LENGTH, SDSTOR_POOL_TAG);
    if (ExtCsd == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = SdStorGetProperty(FdoExtension->LowerDevice,
                               SdPropertyExtCsd,
                               ExtCsd,
                               SDPORT_EXT_CSD_LENGTH);
    if (NT_SUCCESS(Status))
    {
        RtlCopyMemory(&SectorCount, &ExtCsd[MMC_EXT_CSD_SEC_COUNT], sizeof(SectorCount));
        Card->CapacityBytes = (ULONGLONG)SectorCount * SDSTOR_BLOCK_LENGTH;
    }

    ExFreePoolWithTag(ExtCsd, SDSTOR_POOL_TAG);
    return Status;
}

NTSTATUS
NTAPI
SdStorQueryCardData(
    _In_ PSDSTOR_FDO_EXTENSION FdoExtension)
{
    PSDSTOR_CARD_DATA Card = &FdoExtension->Card;
    ULONG CardType;
    ULONG Value;
    NTSTATUS Status;

    RtlZeroMemory(Card, sizeof(*Card));

    Status = SdStorGetProperty(FdoExtension->LowerDevice, SdPropertyCardDetect, &Value, sizeof(Value));
    if (NT_SUCCESS(Status) && Value == 0)
        return STATUS_NO_MEDIA_IN_DEVICE;

    /* The port driver refuses card properties when identification failed */
    Status = SdStorGetProperty(FdoExtension->LowerDevice, SdPropertyCardType, &CardType, sizeof(CardType));
    if (!NT_SUCCESS(Status))
        return Status;

    if (CardType != SdCardTypeSdMemory && CardType != SdCardTypeMmc)
        return STATUS_UNRECOGNIZED_MEDIA;

    Status = SdStorGetProperty(FdoExtension->LowerDevice, SdPropertyCid, Card->Cid, sizeof(Card->Cid));
    if (!NT_SUCCESS(Status))
        return Status;

    Status = SdStorGetProperty(FdoExtension->LowerDevice, SdPropertyCsd, Card->Csd, sizeof(Card->Csd));
    if (!NT_SUCCESS(Status))
        return Status;

    Status = SdStorGetProperty(FdoExtension->LowerDevice, SdPropertyHighCapacity, &Value, sizeof(Value));
    if (NT_SUCCESS(Status) && Value != 0)
        Card->Flags |= SDSTOR_CARD_HIGH_CAPACITY;

    Status = SdStorGetProperty(FdoExtension->LowerDevice, SdPropertyRca, &Value, sizeof(Value));
    if (NT_SUCCESS(Status))
        Card->Rca = (USHORT)Value;

    if (CardType == SdCardTypeMmc)
    {
        Card->Flags |= SDSTOR_CARD_MMC;
        Card->SpecVersion = (Card->Csd[0] >> 2) & 0xF;
        SdStorDecodeMmcCid(Card);

        Status = SdStorDecodeMmcCapacity(FdoExtension);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("sdstor: MMC capacity unavailable 0x%08lx\n", Status);
            return Status;
        }
    }
    else
    {
        Card->Flags |= SDSTOR_CARD_SD;
        SdStorDecodeSdCid(Card);
        SdStorDecodeSdCsd(Card);

        /* Only SD slots carry a write protect switch */
        Status = SdStorGetProperty(FdoExtension->LowerDevice, SdPropertyWriteProtect, &Value, sizeof(Value));
        if (NT_SUCCESS(Status) && Value != 0)
            Card->Flags |= SDSTOR_CARD_WRITE_PROTECT;
    }

    Card->BlockLength = SDSTOR_BLOCK_LENGTH;
    Card->BlockCount = Card->CapacityBytes / SDSTOR_BLOCK_LENGTH;

    DPRINT1("sdstor: %s card '%.8s' '%.16s', %I64u bytes, %I64u blocks of %lu, flags 0x%lx\n",
            (Card->Flags & SDSTOR_CARD_MMC) ? "MMC" : "SD",
            Card->VendorId,
            Card->ProductId,
            Card->CapacityBytes,
            Card->BlockCount,
            Card->BlockLength,
            Card->Flags);

    if (Card->BlockCount == 0)
        return STATUS_UNRECOGNIZED_MEDIA;

    Card->Flags |= SDSTOR_CARD_PRESENT;
    return STATUS_SUCCESS;
}
