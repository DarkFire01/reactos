/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Report field access, data lists and report initialization
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "hidparser.h"

/**
 * @brief Reads a little endian bit field, keeping the low 32 bits of wider fields.
 */
ULONG
NTAPI
HidParseReadBits(
    _In_ const UCHAR *Buffer,
    _In_ ULONG BitOffset,
    _In_ ULONG BitLength)
{
    ULONG Value = 0;
    ULONG Shift = 0;
    ULONG Chunk;

    Buffer += BitOffset >> 3;
    BitOffset &= 7;

    while (BitLength != 0 && Shift < 32)
    {
        Chunk = min(8 - BitOffset, BitLength);
        Value |= ((ULONG)(*Buffer >> BitOffset) & ((1UL << Chunk) - 1)) << Shift;

        Shift += Chunk;
        BitLength -= Chunk;
        BitOffset = 0;
        Buffer++;
    }

    return Value;
}

/**
 * @brief Writes a little endian bit field, bytes past BufferLength are skipped.
 */
VOID
NTAPI
HidParseWriteBits(
    _Inout_ PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _In_ ULONG BitOffset,
    _In_ ULONG BitLength,
    _In_ ULONG Value)
{
    ULONG Byte = BitOffset >> 3;
    ULONG Shift = BitOffset & 7;
    ULONG Chunk;
    ULONG Mask;

    while (BitLength != 0 && Byte < BufferLength)
    {
        Chunk = min(8 - Shift, BitLength);
        Mask = ((1UL << Chunk) - 1) << Shift;
        Buffer[Byte] = (UCHAR)((Buffer[Byte] & ~Mask) | ((Value << Shift) & Mask));

        Value >>= Chunk;
        BitLength -= Chunk;
        Shift = 0;
        Byte++;
    }
}

/**
 * @brief Removes Value from an array field and closes the gap behind it.
 */
NTSTATUS
NTAPI
HidParseDeleteArrayEntry(
    _In_ ULONG BitOffset,
    _In_ USHORT ReportSize,
    _In_ USHORT ReportCount,
    _In_ ULONG Value,
    _Inout_updates_bytes_(ReportLength) PUCHAR Report,
    _In_ ULONG ReportLength)
{
    ULONG Remaining = ReportCount;
    ULONG Next;

    if (Remaining != 0)
    {
        while (HidParseReadBits(Report, BitOffset, ReportSize) != Value)
        {
            BitOffset += ReportSize;
            if (--Remaining == 0)
                return HIDP_STATUS_BUTTON_NOT_PRESSED;
        }
    }
    else if (Value != 0)
    {
        return HIDP_STATUS_BUTTON_NOT_PRESSED;
    }

    for (; Remaining > 1; Remaining--)
    {
        Next = HidParseReadBits(Report, BitOffset + ReportSize, ReportSize);
        HidParseWriteBits(Report, ReportLength, BitOffset, ReportSize, Next);
        BitOffset += ReportSize;
    }
    HidParseWriteBits(Report, ReportLength, BitOffset, ReportSize, 0);

    return HIDP_STATUS_SUCCESS;
}

static
BOOLEAN
HidParseTestBit(
    _In_ const UCHAR *Report,
    _In_ USHORT Bit)
{
    return (Report[Bit >> 3] & (1 << (Bit & 7))) != 0;
}

NTSTATUS
NTAPI
HidP_GetData(
    _In_ HIDP_REPORT_TYPE ReportType,
    _Out_writes_to_(*DataLength, *DataLength) PHIDP_DATA DataList,
    _Inout_ PULONG DataLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _Out_writes_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    PUCHAR Buffer = (PUCHAR)Report;
    PHIDPARSE_REPORT_INFO Info;
    PHIDPARSE_CAP Cap;
    PHIDPARSE_CAP Last;
    USHORT Count = 0;
    USHORT Field;
    USHORT Bit;
    USHORT Value;
    USHORT DataIndex;
    ULONG Index;

    if (!HidParseIsValid(PreparsedData))
        return HIDP_STATUS_INVALID_PREPARSED_DATA;

    Info = HidParseGetReportInfo(PreparsedData, ReportType);
    if (!Info)
        return HIDP_STATUS_INVALID_REPORT_TYPE;

    if ((USHORT)ReportLength != Info->ReportByteLength)
        return HIDP_STATUS_INVALID_REPORT_LENGTH;
    if (Info->ReportByteLength == 0)
        return HIDP_STATUS_REPORT_DOES_NOT_EXIST;

    for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
    {
        Cap = &PreparsedData->Caps[Index];

        /* Value arrays have no data index per field */
        if (!(Cap->Flags & HIDPARSE_CAP_RANGE) && Cap->ReportCount != 1)
            continue;
        if (Buffer[0] && Cap->ReportID != Buffer[0])
            continue;

        if (!(Cap->Flags & HIDPARSE_CAP_BUTTON))
        {
            Field = 0;
            for (Bit = Cap->BitPosition;
                 Bit < (ULONG)Cap->BitPosition + Cap->BitCount;
                 Bit += Cap->ReportSize)
            {
                if (Count < *DataLength)
                {
                    DataList[Count].RawValue = HidParseReadBits(Buffer,
                                                                8 * Cap->BytePosition + Bit,
                                                                Cap->ReportSize);
                    DataList[Count].DataIndex = Cap->DataIndexMin + Field;
                }
                Count++;
                Field++;
            }
        }
        else if (Cap->ReportSize == 1)
        {
            for (Bit = Cap->BitPosition; Bit < (ULONG)Cap->BitPosition + Cap->BitCount; Bit++)
            {
                if (!HidParseTestBit(Buffer, (USHORT)(8 * Cap->BytePosition + Bit)))
                    continue;

                DataIndex = Cap->DataIndexMin;
                if (Cap->Flags & HIDPARSE_CAP_RANGE)
                    DataIndex += Bit - Cap->BitPosition;

                if (Count < *DataLength)
                {
                    DataList[Count].On = TRUE;
                    DataList[Count].DataIndex = DataIndex;
                }
                Count++;
            }
        }
        else
        {
            for (Bit = Cap->BitPosition;
                 Bit < (ULONG)Cap->BitPosition + Cap->BitCount;
                 Bit += Cap->ReportSize)
            {
                Value = (USHORT)HidParseReadBits(Buffer,
                                                 8 * Cap->BytePosition + Bit,
                                                 Cap->ReportSize);
                if (Value == 0)
                    continue;

                /* Array values index the whole chain starting at its last cap */
                Last = HidParseChainEnd(Cap);
                Index += (ULONG)(Last - Cap);
                Cap = Last;

                DataIndex = Cap->DataIndexMin + Value;
                if (Count < *DataLength)
                {
                    if (Cap->UsageMin != 0)
                        DataIndex--;

                    DataList[Count].On = TRUE;
                    DataList[Count].DataIndex = DataIndex;
                }
                Count++;
            }
        }
    }

    Index = *DataLength;
    *DataLength = Count;
    return (Index < Count) ? HIDP_STATUS_BUFFER_TOO_SMALL : HIDP_STATUS_SUCCESS;
}

static
NTSTATUS
HidParseSetOneData(
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_ PHIDPARSE_REPORT_INFO Info,
    _In_ PHIDP_DATA Data,
    _Inout_updates_bytes_(ReportLength) PUCHAR Report,
    _In_ ULONG ReportLength)
{
    PHIDPARSE_CAP Cap;
    PHIDPARSE_CAP Last;
    NTSTATUS Status;
    BOOLEAN IsWrongReport = FALSE;
    BOOLEAN IsNotPressed = FALSE;
    BOOLEAN IsFull = FALSE;
    USHORT Bit;
    USHORT Mask;
    ULONG Value;
    ULONG Index;

    if (ReportLength == 0)
        return HIDP_STATUS_INVALID_REPORT_LENGTH;

    for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
    {
        Cap = &PreparsedData->Caps[Index];

        if (Data->DataIndex < Cap->DataIndexMin || Data->DataIndex > Cap->DataIndexMax)
            continue;

        if (!(Cap->Flags & HIDPARSE_CAP_RANGE) && Cap->ReportCount != 1)
            return HIDP_STATUS_IS_VALUE_ARRAY;

        if (Report[0])
        {
            if (Cap->ReportID != Report[0])
            {
                IsWrongReport = TRUE;
                continue;
            }
        }
        else
        {
            Report[0] = Cap->ReportID;
        }

        if (!(Cap->Flags & HIDPARSE_CAP_BUTTON))
        {
            Bit = (USHORT)(Cap->BitPosition + 8 * Cap->BytePosition +
                           Cap->ReportSize * (Data->DataIndex - Cap->DataIndexMin));
            HidParseWriteBits(Report, ReportLength, Bit, Cap->ReportSize, Data->RawValue);
            return HIDP_STATUS_SUCCESS;
        }

        if (Cap->ReportSize == 1)
        {
            Bit = (USHORT)(Data->DataIndex + Cap->BitPosition + 8 * Cap->BytePosition -
                           Cap->DataIndexMin);
            if ((ULONG)(Bit >> 3) >= ReportLength)
                return HIDP_STATUS_INVALID_REPORT_LENGTH;

            Mask = 1 << (Bit & 7);
            if (Data->On)
            {
                Report[Bit >> 3] |= Mask;
            }
            else
            {
                if (!(Report[Bit >> 3] & Mask))
                    return HIDP_STATUS_BUTTON_NOT_PRESSED;
                Report[Bit >> 3] &= ~Mask;
            }
            return HIDP_STATUS_SUCCESS;
        }

        if (!Data->On)
        {
            Last = HidParseChainEnd(Cap);
            Index += (ULONG)(Last - Cap);

            Value = Data->DataIndex - Last->DataIndexMin;
            if (Last->UsageMin != 0)
                Value++;

            Status = HidParseDeleteArrayEntry((USHORT)(Last->BitPosition + 8 * Last->BytePosition),
                                              Last->ReportSize,
                                              Last->ReportCount,
                                              Value,
                                              Report,
                                              ReportLength);
            if (Status == HIDP_STATUS_BUTTON_NOT_PRESSED)
            {
                IsNotPressed = TRUE;
                continue;
            }
            return Status;
        }

        /* Store the value in the first free array field */
        for (Bit = Cap->BitPosition;
             Bit < (ULONG)Cap->BitPosition + Cap->BitCount;
             Bit += Cap->ReportSize)
        {
            if (HidParseReadBits(Report, 8 * Cap->BytePosition + Bit, Cap->ReportSize) & 0xFFFF)
            {
                IsFull = TRUE;
                continue;
            }

            Last = HidParseChainEnd(Cap);
            Value = Data->DataIndex - Last->DataIndexMin;
            if (Last->UsageMin != 0)
                Value++;

            HidParseWriteBits(Report,
                              ReportLength,
                              8 * Last->BytePosition + Bit,
                              Last->ReportSize,
                              Value);
            return HIDP_STATUS_SUCCESS;
        }
    }

    if (IsWrongReport)
        return HIDP_STATUS_INCOMPATIBLE_REPORT_ID;
    if (IsNotPressed)
        return HIDP_STATUS_BUTTON_NOT_PRESSED;
    if (IsFull)
        return HIDP_STATUS_BUFFER_TOO_SMALL;

    return HIDP_STATUS_DATA_INDEX_NOT_FOUND;
}

NTSTATUS
NTAPI
HidP_SetData(
    _In_ HIDP_REPORT_TYPE ReportType,
    _Inout_updates_to_(*DataLength, *DataLength) PHIDP_DATA DataList,
    _Inout_ PULONG DataLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_reads_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    PHIDPARSE_REPORT_INFO Info;
    NTSTATUS Status = HIDP_STATUS_REPORT_DOES_NOT_EXIST;
    ULONG Index;

    if (!HidParseIsValid(PreparsedData))
        return HIDP_STATUS_INVALID_PREPARSED_DATA;

    Info = HidParseGetReportInfo(PreparsedData, ReportType);
    if (!Info)
        return HIDP_STATUS_INVALID_REPORT_TYPE;

    if ((USHORT)ReportLength != Info->ReportByteLength)
        return HIDP_STATUS_INVALID_REPORT_LENGTH;
    if (Info->ReportByteLength == 0)
        return HIDP_STATUS_REPORT_DOES_NOT_EXIST;

    /* An empty list keeps the initial status */
    for (Index = 0; Index < *DataLength; Index++)
    {
        Status = HidParseSetOneData(PreparsedData,
                                    Info,
                                    &DataList[Index],
                                    (PUCHAR)Report,
                                    ReportLength);
        if (!NT_SUCCESS(Status))
            break;
    }

    *DataLength = Index;
    return Status;
}

NTSTATUS
NTAPI
HidP_InitializeReportForID(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ UCHAR ReportID,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _Out_writes_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    PUCHAR Buffer = (PUCHAR)Report;
    PHIDPARSE_REPORT_INFO Info;
    PHIDPARSE_CAP Cap;
    NTSTATUS Status = HIDP_STATUS_REPORT_DOES_NOT_EXIST;
    ULONG Mask;
    LONG NullValue;
    ULONG Bit;
    ULONG Field;
    ULONG Index;

    if (!HidParseIsValid(PreparsedData))
        return HIDP_STATUS_INVALID_PREPARSED_DATA;

    Info = HidParseGetReportInfo(PreparsedData, ReportType);
    if (!Info)
        return HIDP_STATUS_INVALID_REPORT_TYPE;

    if ((USHORT)ReportLength != Info->ReportByteLength)
        return HIDP_STATUS_INVALID_REPORT_LENGTH;
    if (Info->ReportByteLength == 0)
        return HIDP_STATUS_REPORT_DOES_NOT_EXIST;

    RtlZeroMemory(Buffer, ReportLength);
    Buffer[0] = ReportID;

    /* Windows walks as many caps as the report has, but always from the first cap */
    for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
    {
        Cap = &PreparsedData->Caps[Index - Info->FirstCap];
        if (Cap->ReportID != ReportID)
            continue;

        Status = HIDP_STATUS_SUCCESS;

        if (Cap->Flags & (HIDPARSE_CAP_CONSTANT | HIDPARSE_CAP_BUTTON | HIDPARSE_CAP_ALIAS))
            continue;
        if (!Cap->u.Value.HasNull)
            continue;

        /* The null value is the one just below the logical minimum */
        Mask = (Cap->ReportSize == 32) ? MAXULONG : (1UL << (Cap->ReportSize & 31)) - 1;
        if (Cap->u.Value.LogicalMax >= Cap->u.Value.LogicalMin)
            NullValue = (LONG)(Mask & (ULONG)(Cap->u.Value.LogicalMin - 1));
        else
            NullValue = 0;

        if (NullValue >= Cap->u.Value.LogicalMin && NullValue <= Cap->u.Value.LogicalMax)
            continue;
        if (NullValue == 0)
            continue;

        Bit = Cap->BitPosition + 8 * Cap->BytePosition;
        if (!(Cap->Flags & HIDPARSE_CAP_RANGE))
        {
            HidParseWriteBits(Buffer, ReportLength, Bit, Cap->ReportSize, (ULONG)NullValue);
            continue;
        }

        for (Field = 0; Field < Cap->ReportCount; Field++)
        {
            HidParseWriteBits(Buffer, ReportLength, Bit, Cap->ReportSize, (ULONG)NullValue);
            Bit += Cap->ReportSize;
        }
    }

    return Status;
}
