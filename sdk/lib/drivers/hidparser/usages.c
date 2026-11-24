/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Button usage access
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "hidparser.h"

#define HIDPARSE_POWER_USAGE_LIST       32

static
BOOLEAN
HidParseTestBit(
    _In_ const UCHAR *Report,
    _In_ USHORT Bit)
{
    return (Report[Bit >> 3] & (1 << (Bit & 7))) != 0;
}

/**
 * @brief Maps an array field value to a usage, walking the chain from its last cap.
 * @return Usage in the low word and its usage page in the high word, zero if none.
 */
static
ULONG
HidParseArrayValueToUsage(
    _In_ PHIDPARSE_CAP First,
    _In_ ULONG Value)
{
    PHIDPARSE_CAP Cap = HidParseChainEnd(First);
    USAGE Minimum;
    USHORT Span;

    for (;;)
    {
        if (Cap->Flags & HIDPARSE_CAP_RANGE)
        {
            Minimum = Cap->UsageMin ? Cap->UsageMin : 1;
            Span = (USHORT)(Cap->UsageMax - Minimum + 1);
            if (Value <= Span)
                return (USAGE)(Minimum - 1 + Value) | ((ULONG)Cap->UsagePage << 16);

            Value -= Span;
        }
        else
        {
            if (Value == 1)
                return Cap->UsageMin | ((ULONG)Cap->UsagePage << 16);

            Value--;
        }

        if (Cap == First)
            return 0;
        Cap--;
    }
}

/**
 * @brief Maps a usage to the array field value of the chain that starts at First.
 * @return One based value, zero if the chain does not contain the usage.
 */
static
ULONG
HidParseUsageToArrayValue(
    _In_ PHIDPARSE_CAP First,
    _In_ USAGE Usage)
{
    PHIDPARSE_CAP Cap = HidParseChainEnd(First);
    ULONG Value = 0;
    USAGE Minimum;

    for (;;)
    {
        if (Cap->Flags & HIDPARSE_CAP_RANGE)
        {
            Minimum = Cap->UsageMin ? Cap->UsageMin : 1;
            if (Minimum <= Usage && Usage <= Cap->UsageMax)
                return Value + Usage - Minimum + 1;

            Value += Cap->UsageMax - Minimum + 1;
        }
        else
        {
            Value++;
            if (Usage == Cap->UsageMin)
                return Value;
        }

        if (Cap == First)
            return 0;
        Cap--;
    }
}

static
VOID
HidParseStoreUsage(
    _Out_ PUSAGE UsageList,
    _In_ USAGE UsagePage,
    _In_ ULONG Index,
    _In_ USAGE Usage,
    _In_ USAGE CapUsagePage)
{
    PUSAGE_AND_PAGE PairList = (PUSAGE_AND_PAGE)UsageList;

    /* Without a usage page filter the caller gets usage and page pairs */
    if (UsagePage)
    {
        UsageList[Index] = Usage;
    }
    else
    {
        PairList[Index].Usage = Usage;
        PairList[Index].UsagePage = CapUsagePage;
    }
}

NTSTATUS
NTAPI
HidP_GetUsages(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _Out_writes_to_(*UsageLength, *UsageLength) USAGE *UsageList,
    _Inout_ ULONG *UsageLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _Out_writes_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    PUCHAR Buffer = (PUCHAR)Report;
    PHIDPARSE_REPORT_INFO Info;
    PHIDPARSE_CAP Cap;
    BOOLEAN IsFound = FALSE;
    BOOLEAN IsWrongReport = FALSE;
    NTSTATUS Status;
    USHORT Count = 0;
    USHORT Bit;
    USAGE Usage;
    ULONG Value;
    ULONG Index;

    if (!HidParseIsValid(PreparsedData))
        return HIDP_STATUS_INVALID_PREPARSED_DATA;

    /* Only the USAGE sized part is cleared, even for usage and page pairs */
    RtlZeroMemory(UsageList, sizeof(USAGE) * *UsageLength);

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

        if (!(Cap->Flags & HIDPARSE_CAP_BUTTON))
            continue;
        if (UsagePage && Cap->UsagePage != UsagePage)
            continue;
        if (!HidParseMatchLink(LinkCollection, Cap))
            continue;

        if (Buffer[0] && Cap->ReportID != Buffer[0])
        {
            IsWrongReport = TRUE;
            continue;
        }
        IsFound = TRUE;

        if (Cap->ReportSize == 1)
        {
            for (Bit = Cap->BitPosition; Bit < (ULONG)Cap->BitPosition + Cap->BitCount; Bit++)
            {
                if (!HidParseTestBit(Buffer, (USHORT)(Bit + 8 * Cap->BytePosition)))
                    continue;

                Usage = Cap->UsageMin;
                if (Cap->Flags & HIDPARSE_CAP_RANGE)
                    Usage = (USAGE)(Usage + Bit - Cap->BitPosition);

                if (Count < *UsageLength)
                    HidParseStoreUsage(UsageList, UsagePage, Count, Usage, Cap->UsagePage);
                Count++;
            }
            continue;
        }

        for (Bit = Cap->BitPosition;
             Bit < (ULONG)Cap->BitPosition + Cap->BitCount;
             Bit += Cap->ReportSize)
        {
            Value = HidParseReadBits(Buffer, 8 * Cap->BytePosition + Bit, Cap->ReportSize);
            if (Value == 0)
                continue;

            Value = HidParseArrayValueToUsage(Cap, Value);
            if ((USAGE)Value == 0)
                continue;

            if (Count < *UsageLength)
                HidParseStoreUsage(UsageList, UsagePage, Count, (USAGE)Value, (USAGE)(Value >> 16));
            Count++;
        }

        Index += (ULONG)(HidParseChainEnd(Cap) - Cap);
    }

    Status = (*UsageLength >= Count) ? HIDP_STATUS_SUCCESS : HIDP_STATUS_BUFFER_TOO_SMALL;
    *UsageLength = Count;

    if (!IsFound)
        Status = IsWrongReport ? HIDP_STATUS_INCOMPATIBLE_REPORT_ID : HIDP_STATUS_USAGE_NOT_FOUND;

    return Status;
}

NTSTATUS
NTAPI
HidP_GetUsagesEx(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USHORT LinkCollection,
    _Inout_updates_to_(*UsageLength, *UsageLength) PUSAGE_AND_PAGE ButtonList,
    _Inout_ ULONG *UsageLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_reads_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    return HidP_GetUsages(ReportType,
                          0,
                          LinkCollection,
                          &ButtonList->Usage,
                          UsageLength,
                          PreparsedData,
                          Report,
                          ReportLength);
}

static
NTSTATUS
HidParseSetUsage(
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_ PHIDPARSE_REPORT_INFO Info,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _In_ USAGE Usage,
    _Inout_updates_bytes_(ReportLength) PUCHAR Report,
    _In_ ULONG ReportLength,
    _In_ BOOLEAN IsSet)
{
    PHIDPARSE_CAP Cap;
    PHIDPARSE_CAP Previous = NULL;
    PHIDPARSE_CAP ChainStart = NULL;
    PHIDPARSE_CAP Last;
    BOOLEAN IsWrongReport = FALSE;
    BOOLEAN IsNotPressed = FALSE;
    BOOLEAN IsFull = FALSE;
    NTSTATUS Status;
    USHORT DeleteOffset = 0;
    USHORT Bit;
    UCHAR Mask;
    ULONG Value;
    ULONG Index;

    if (ReportLength == 0)
        return HIDP_STATUS_INVALID_REPORT_LENGTH;

    for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
    {
        Cap = &PreparsedData->Caps[Index];

        if (!Previous || !(Previous->Flags & HIDPARSE_CAP_ARRAY_CHAIN))
            ChainStart = Cap;
        Previous = Cap;

        if (!(Cap->Flags & HIDPARSE_CAP_BUTTON) || Cap->UsagePage != UsagePage)
            continue;
        if (!HidParseMatchLink(LinkCollection, Cap))
            continue;
        if (Cap->Flags & HIDPARSE_CAP_RANGE)
        {
            if (Usage < Cap->UsageMin || Usage > Cap->UsageMax)
                continue;
        }
        else if (Usage != Cap->UsageMin)
        {
            continue;
        }

        if (Report[0] && Cap->ReportID != Report[0])
        {
            IsWrongReport = TRUE;
            continue;
        }
        Report[0] = Cap->ReportID;

        if (Cap->ReportSize == 1)
        {
            Bit = (USHORT)(Usage + 8 * Cap->BytePosition + Cap->BitPosition - Cap->UsageMin);
            if ((ULONG)(Bit >> 3) >= ReportLength)
                return HIDP_STATUS_INVALID_REPORT_LENGTH;

            Mask = (UCHAR)(1 << (Bit & 7));
            if (IsSet)
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

        if (IsSet)
        {
            for (Bit = Cap->BitPosition;
                 Bit < (ULONG)Cap->BitPosition + Cap->BitCount;
                 Bit += Cap->ReportSize)
            {
                if (HidParseReadBits(Report, 8 * Cap->BytePosition + Bit, Cap->ReportSize))
                {
                    IsFull = TRUE;
                    continue;
                }

                Value = HidParseUsageToArrayValue(ChainStart, Usage);
                if (Value == 0)
                    return HIDP_STATUS_INTERNAL_ERROR;

                HidParseWriteBits(Report,
                                  ReportLength,
                                  8 * Cap->BytePosition + Bit,
                                  Cap->ReportSize,
                                  Value);
                return HIDP_STATUS_SUCCESS;
            }

            Last = HidParseChainEnd(Cap);
            Index += (ULONG)(Last - Cap);
            Previous = Last;
            continue;
        }

        /* Like Windows, the array start ignores the bit position and adds up over matches */
        Value = HidParseUsageToArrayValue(ChainStart, Usage);
        DeleteOffset = (USHORT)(DeleteOffset + 8 * Cap->BytePosition);
        Status = HidParseDeleteArrayEntry(DeleteOffset,
                                          Cap->ReportSize,
                                          Cap->ReportCount,
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

    if (IsWrongReport)
        return HIDP_STATUS_INCOMPATIBLE_REPORT_ID;
    if (IsNotPressed)
        return HIDP_STATUS_BUTTON_NOT_PRESSED;
    if (IsFull)
        return HIDP_STATUS_BUFFER_TOO_SMALL;

    return HIDP_STATUS_USAGE_NOT_FOUND;
}

static
NTSTATUS
HidParseSetUsageList(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _In_ PUSAGE UsageList,
    _Inout_ PULONG UsageLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _Inout_ PCHAR Report,
    _In_ ULONG ReportLength,
    _In_ BOOLEAN IsSet)
{
    PHIDPARSE_REPORT_INFO Info;
    NTSTATUS Status = HIDP_STATUS_SUCCESS;
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

    for (Index = 0; Index < *UsageLength; Index++)
    {
        if (UsageList[Index] == 0)
            continue;

        Status = HidParseSetUsage(PreparsedData,
                                  Info,
                                  UsagePage,
                                  LinkCollection,
                                  UsageList[Index],
                                  (PUCHAR)Report,
                                  ReportLength,
                                  IsSet);
        if (!NT_SUCCESS(Status))
            break;
    }

    *UsageLength = Index;
    return Status;
}

NTSTATUS
NTAPI
HidP_SetUsages(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _Inout_updates_to_(*UsageLength, *UsageLength) PUSAGE UsageList,
    _Inout_ PULONG UsageLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_reads_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    return HidParseSetUsageList(ReportType,
                                UsagePage,
                                LinkCollection,
                                UsageList,
                                UsageLength,
                                PreparsedData,
                                Report,
                                ReportLength,
                                TRUE);
}

NTSTATUS
NTAPI
HidP_UnsetUsages(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_opt_ USHORT LinkCollection,
    _Inout_updates_to_(*UsageLength, *UsageLength) PUSAGE UsageList,
    _Inout_ PULONG UsageLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_reads_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    return HidParseSetUsageList(ReportType,
                                UsagePage,
                                LinkCollection,
                                UsageList,
                                UsageLength,
                                PreparsedData,
                                Report,
                                ReportLength,
                                FALSE);
}

NTSTATUS
NTAPI
HidP_GetButtonArray(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _In_ USAGE Usage,
    _Out_writes_to_(*ButtonDataLength, *ButtonDataLength) PHIDP_BUTTON_ARRAY_DATA ButtonData,
    _Inout_ PUSHORT ButtonDataLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_reads_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    PUCHAR Buffer = (PUCHAR)Report;
    PHIDPARSE_REPORT_INFO Info;
    PHIDPARSE_CAP Cap;
    BOOLEAN IsWrongReport = FALSE;
    NTSTATUS Status = HIDP_STATUS_SUCCESS;
    USHORT Count = 0;
    USHORT Field;
    ULONG Index;

    if (!HidParseIsValid(PreparsedData))
        return HIDP_STATUS_INVALID_PREPARSED_DATA;

    RtlZeroMemory(ButtonData, sizeof(*ButtonData) * *ButtonDataLength);

    Info = HidParseGetReportInfo(PreparsedData, ReportType);
    if (!Info)
        return HIDP_STATUS_INVALID_REPORT_TYPE;

    if (Info->ReportByteLength == 0)
        return HIDP_STATUS_REPORT_DOES_NOT_EXIST;
    if ((USHORT)ReportLength != Info->ReportByteLength)
        return HIDP_STATUS_INVALID_REPORT_LENGTH;

    for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
    {
        Cap = &PreparsedData->Caps[Index];

        if (!(Cap->Flags & HIDPARSE_CAP_BUTTON))
            continue;
        if (UsagePage && Cap->UsagePage != UsagePage)
            continue;
        if (!HidParseMatchLink(LinkCollection, Cap))
            continue;

        if (Cap->Flags & HIDPARSE_CAP_RANGE)
        {
            if (Cap->UsageMin <= Usage && Usage <= Cap->UsageMax)
                return HIDP_STATUS_NOT_BUTTON_ARRAY;
            continue;
        }

        if (Cap->UsageMin != Usage)
            continue;
        if (Cap->ReportCount == 1)
            return HIDP_STATUS_NOT_BUTTON_ARRAY;

        if (Buffer[0] && Cap->ReportID != Buffer[0])
        {
            IsWrongReport = TRUE;
            continue;
        }

        for (Field = 0; Field < Cap->ReportCount; Field++)
        {
            if (!HidParseTestBit(Buffer,
                                 (USHORT)(Field + 8 * Cap->BytePosition + Cap->BitPosition)))
            {
                continue;
            }

            if (Count >= *ButtonDataLength)
            {
                Status = HIDP_STATUS_BUFFER_TOO_SMALL;
            }
            else
            {
                ButtonData[Count].ArrayIndex = Field;
                ButtonData[Count].On = TRUE;
            }
            Count++;
        }

        *ButtonDataLength = Count;
        return Status;
    }

    return IsWrongReport ? HIDP_STATUS_INCOMPATIBLE_REPORT_ID : HIDP_STATUS_USAGE_NOT_FOUND;
}

NTSTATUS
NTAPI
HidP_SetButtonArray(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _In_ USAGE Usage,
    _In_reads_(ButtonDataLength) PHIDP_BUTTON_ARRAY_DATA ButtonData,
    _In_ USHORT ButtonDataLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _Inout_updates_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    PUCHAR Buffer = (PUCHAR)Report;
    PHIDPARSE_REPORT_INFO Info;
    PHIDPARSE_CAP Cap;
    BOOLEAN IsWrongReport = FALSE;
    USHORT Entry;
    USHORT Start;
    UCHAR Mask;
    ULONG Bit;
    ULONG Index;

    if (!HidParseIsValid(PreparsedData))
        return HIDP_STATUS_INVALID_PREPARSED_DATA;

    Info = HidParseGetReportInfo(PreparsedData, ReportType);
    if (!Info)
        return HIDP_STATUS_INVALID_REPORT_TYPE;

    if (Info->ReportByteLength == 0)
        return HIDP_STATUS_REPORT_DOES_NOT_EXIST;
    if ((USHORT)ReportLength != Info->ReportByteLength)
        return HIDP_STATUS_INVALID_REPORT_LENGTH;

    for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
    {
        Cap = &PreparsedData->Caps[Index];

        if (!(Cap->Flags & HIDPARSE_CAP_BUTTON))
            continue;
        if (UsagePage && Cap->UsagePage != UsagePage)
            continue;
        if (!HidParseMatchLink(LinkCollection, Cap))
            continue;

        if (Cap->Flags & HIDPARSE_CAP_RANGE)
        {
            if (Cap->UsageMin <= Usage && Usage <= Cap->UsageMax)
                return HIDP_STATUS_NOT_BUTTON_ARRAY;
            continue;
        }

        if (Cap->UsageMin != Usage)
            continue;
        if (Cap->ReportCount == 1)
            return HIDP_STATUS_NOT_BUTTON_ARRAY;

        if (Buffer[0] && Cap->ReportID != Buffer[0])
        {
            IsWrongReport = TRUE;
            continue;
        }

        Start = (USHORT)(Cap->BitPosition + 8 * Cap->BytePosition);
        for (Entry = 0; Entry < ButtonDataLength; Entry++)
        {
            if (ButtonData[Entry].ArrayIndex > Cap->ReportCount)
                return HIDP_STATUS_DATA_INDEX_OUT_OF_RANGE;

            Bit = (ULONG)Start + ButtonData[Entry].ArrayIndex;
            if ((Bit >> 3) >= ReportLength)
                return HIDP_STATUS_INVALID_REPORT_LENGTH;

            Mask = (UCHAR)(1 << (Bit & 7));
            if (ButtonData[Entry].On)
                Buffer[Bit >> 3] |= Mask;
            else
                Buffer[Bit >> 3] &= ~Mask;
        }

        return HIDP_STATUS_SUCCESS;
    }

    return IsWrongReport ? HIDP_STATUS_INCOMPATIBLE_REPORT_ID : HIDP_STATUS_USAGE_NOT_FOUND;
}

NTSTATUS
NTAPI
HidP_SysPowerEvent(
    _In_reads_bytes_(HidPacketLength) PCHAR HidPacket,
    _In_ USHORT HidPacketLength,
    _In_ PHIDP_PREPARSED_DATA Ppd,
    _Out_ PULONG OutputBuffer)
{
    USAGE UsageList[HIDPARSE_POWER_USAGE_LIST];
    ULONG UsageLength = (ULONG)RTL_NUMBER_OF(UsageList);
    NTSTATUS Status;
    ULONG Index;

    *OutputBuffer = 0;

    if (!Ppd->PowerButtonMask)
        return STATUS_NOT_SUPPORTED;

    Status = HidP_GetUsages(HidP_Input,
                            HID_USAGE_PAGE_GENERIC,
                            HIDP_LINK_COLLECTION_UNSPECIFIED,
                            UsageList,
                            &UsageLength,
                            Ppd,
                            HidPacket,
                            HidPacketLength);
    if (!NT_SUCCESS(Status))
        return Status;

    for (Index = 0; Index < UsageLength; Index++)
    {
        switch (UsageList[Index])
        {
            case HIDPARSE_USAGE_SYSTEM_POWER:
                *OutputBuffer |= SYS_BUTTON_POWER;
                break;
            case HIDPARSE_USAGE_SYSTEM_SLEEP:
                *OutputBuffer |= SYS_BUTTON_SLEEP;
                break;
            case HIDPARSE_USAGE_SYSTEM_WAKE:
                *OutputBuffer |= SYS_BUTTON_WAKE;
                break;
        }
    }

    return Status;
}
