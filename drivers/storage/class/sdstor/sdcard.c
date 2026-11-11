/*
 * PROJECT:     ReactOS Storage Stack / SD storage class driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Card register decoding, capacity and identity
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

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

static
VOID
SdStorDecodeCid(
    _Inout_ PSDSTOR_CARD_DATA Card)
{
    /* SD CID: MID[0], OID[1..2], PNM[3..7] (5 chars), PRV[8], PSN[9..12]. */
    SdStorFillString(Card->VendorId, sizeof(Card->VendorId), &Card->Cid[1], 2);
    SdStorFillString(Card->ProductId, sizeof(Card->ProductId), &Card->Cid[3], 5);

    Card->ProductRevision[0] = '0' + ((Card->Cid[8] >> 4) & 0xF);
    Card->ProductRevision[1] = '.';
    Card->ProductRevision[2] = '0' + (Card->Cid[8] & 0xF);
    Card->ProductRevision[3] = ' ';

    RtlCopyMemory(Card->SerialNumber, &Card->Cid[9], sizeof(Card->SerialNumber));
}

static
VOID
SdStorDecodeCsd(
    _Inout_ PSDSTOR_CARD_DATA Card)
{
    ULONG CsdStructure;
    ULONGLONG Capacity;

    CsdStructure = (Card->Csd[0] >> 6) & 0x3;

    if (CsdStructure == 0)
    {
        ULONG ReadBlockLen = SdStorExtractBits(Card->Csd, 83, 80);
        ULONG CSize = SdStorExtractBits(Card->Csd, 73, 62);
        ULONG CSizeMult = SdStorExtractBits(Card->Csd, 49, 47);
        ULONGLONG BlockNr = (ULONGLONG)(CSize + 1) << (CSizeMult + 2);

        Capacity = BlockNr * ((ULONGLONG)1 << ReadBlockLen);
    }
    else
    {
        ULONG CSize = SdStorExtractBits(Card->Csd, 69, 48);

        Capacity = (ULONGLONG)(CSize + 1) * 512 * 1024;
    }

    if (SdStorExtractBits(Card->Csd, 13, 13) != 0 ||
        SdStorExtractBits(Card->Csd, 12, 12) != 0)
    {
        Card->Flags |= SDSTOR_CARD_WRITE_PROTECT;
    }

    Card->BlockLength = SDSTOR_BLOCK_LENGTH;
    Card->CapacityBytes = Capacity;
    Card->BlockCount = Capacity / SDSTOR_BLOCK_LENGTH;
}

NTSTATUS
NTAPI
SdStorQueryCardData(
    _In_ PSDSTOR_FDO_EXTENSION FdoExtension)
{
    PSDSTOR_CARD_DATA Card = &FdoExtension->Card;
    ULONG Value;
    NTSTATUS Status;

    RtlZeroMemory(Card, sizeof(*Card));

    Status = SdStorGetProperty(FdoExtension->LowerDevice, SdPropertyCardDetect, &Value, sizeof(Value));
    if (NT_SUCCESS(Status) && Value == 0)
        return STATUS_NO_MEDIA_IN_DEVICE;

    Status = SdStorGetProperty(FdoExtension->LowerDevice, SdPropertyCid, Card->Cid, sizeof(Card->Cid));
    if (!NT_SUCCESS(Status))
        return Status;

    Status = SdStorGetProperty(FdoExtension->LowerDevice, SdPropertyCsd, Card->Csd, sizeof(Card->Csd));
    if (!NT_SUCCESS(Status))
        return Status;

    Card->Flags |= SDSTOR_CARD_PRESENT | SDSTOR_CARD_SD;

    Status = SdStorGetProperty(FdoExtension->LowerDevice, SdPropertyHighCapacity, &Value, sizeof(Value));
    if (NT_SUCCESS(Status) && Value != 0)
        Card->Flags |= SDSTOR_CARD_HIGH_CAPACITY;

    Status = SdStorGetProperty(FdoExtension->LowerDevice, SdPropertyRca, &Value, sizeof(Value));
    if (NT_SUCCESS(Status))
        Card->Rca = (USHORT)Value;

    SdStorDecodeCid(Card);
    SdStorDecodeCsd(Card);

    DPRINT1("sdstor: card %I64u bytes, %I64u blocks of %lu, flags 0x%lx\n",
            Card->CapacityBytes, Card->BlockCount, Card->BlockLength, Card->Flags);

    Status = SdStorGetProperty(FdoExtension->LowerDevice, SdPropertyWriteProtect, &Value, sizeof(Value));
    if (NT_SUCCESS(Status) && Value != 0)
        Card->Flags |= SDSTOR_CARD_WRITE_PROTECT;

    return STATUS_SUCCESS;
}
