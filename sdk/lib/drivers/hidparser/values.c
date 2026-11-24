/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Value usage access
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "hidparser.h"

/* Signed difference with the wrap around of 32 bit arithmetic */
#define HIDPARSE_DIFF(a, b)     ((LONG)((ULONG)(a) - (ULONG)(b)))

static
NTSTATUS
HidParseCheckReport(
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ ULONG ReportLength,
    _Out_ PHIDPARSE_REPORT_INFO *Info)
{
    if (!HidParseIsValid(PreparsedData))
        return HIDP_STATUS_INVALID_PREPARSED_DATA;

    *Info = HidParseGetReportInfo(PreparsedData, ReportType);
    if (!*Info)
        return HIDP_STATUS_INVALID_REPORT_TYPE;

    if ((USHORT)ReportLength != (*Info)->ReportByteLength)
        return HIDP_STATUS_INVALID_REPORT_LENGTH;
    if ((*Info)->ReportByteLength == 0)
        return HIDP_STATUS_REPORT_DOES_NOT_EXIST;

    return HIDP_STATUS_SUCCESS;
}

/**
 * @brief Finds the value cap that holds Usage in this report.
 * @param BitOffset Receives the bit offset of the field of Usage.
 */
static
NTSTATUS
HidParseFindValue(
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _In_ USAGE Usage,
    _In_ const UCHAR *Report,
    _In_ ULONG ReportLength,
    _Out_ PHIDPARSE_CAP *FoundCap,
    _Out_ PULONG BitOffset)
{
    PHIDPARSE_REPORT_INFO Info;
    PHIDPARSE_CAP Cap;
    BOOLEAN IsWrongReport = FALSE;
    NTSTATUS Status;
    ULONG Index;

    Status = HidParseCheckReport(PreparsedData, ReportType, ReportLength, &Info);
    if (!NT_SUCCESS(Status))
        return Status;

    for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
    {
        Cap = &PreparsedData->Caps[Index];

        if ((Cap->Flags & HIDPARSE_CAP_BUTTON) || Cap->UsagePage != UsagePage)
            continue;
        if (!HidParseMatchLink(LinkCollection, Cap))
            continue;

        if (Cap->Flags & HIDPARSE_CAP_RANGE)
        {
            if (Usage < Cap->UsageMin || Usage > Cap->UsageMax)
                continue;

            *BitOffset = Cap->BitPosition + 8 * Cap->BytePosition +
                         Cap->ReportSize * (Usage - Cap->UsageMin);
        }
        else
        {
            if (Usage != Cap->UsageMin)
                continue;

            *BitOffset = Cap->BitPosition + 8 * Cap->BytePosition;
        }

        if (Report[0] && Cap->ReportID != Report[0])
        {
            IsWrongReport = TRUE;
            continue;
        }

        *FoundCap = Cap;
        return HIDP_STATUS_SUCCESS;
    }

    return IsWrongReport ? HIDP_STATUS_INCOMPATIBLE_REPORT_ID : HIDP_STATUS_USAGE_NOT_FOUND;
}

/**
 * @brief Finds the value array cap of Usage, ranges and single fields are rejected.
 */
static
NTSTATUS
HidParseFindValueArray(
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _In_ USAGE Usage,
    _In_ const UCHAR *Report,
    _In_ ULONG ReportLength,
    _Out_ PHIDPARSE_CAP *FoundCap)
{
    PHIDPARSE_REPORT_INFO Info;
    PHIDPARSE_CAP Cap;
    BOOLEAN IsWrongReport = FALSE;
    NTSTATUS Status;
    ULONG Index;

    Status = HidParseCheckReport(PreparsedData, ReportType, ReportLength, &Info);
    if (!NT_SUCCESS(Status))
        return Status;

    for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
    {
        Cap = &PreparsedData->Caps[Index];

        if ((Cap->Flags & HIDPARSE_CAP_BUTTON) || Cap->UsagePage != UsagePage)
            continue;
        if (!HidParseMatchLink(LinkCollection, Cap))
            continue;

        if (Cap->Flags & HIDPARSE_CAP_RANGE)
        {
            if (Cap->UsageMin <= Usage && Usage <= Cap->UsageMax)
                return HIDP_STATUS_NOT_VALUE_ARRAY;
            continue;
        }

        if (Cap->UsageMin != Usage)
            continue;
        if (Cap->ReportCount == 1)
            return HIDP_STATUS_NOT_VALUE_ARRAY;

        if (Report[0] && Cap->ReportID != Report[0])
        {
            IsWrongReport = TRUE;
            continue;
        }

        *FoundCap = Cap;
        return HIDP_STATUS_SUCCESS;
    }

    return IsWrongReport ? HIDP_STATUS_INCOMPATIBLE_REPORT_ID : HIDP_STATUS_USAGE_NOT_FOUND;
}

static
ULONG
HidParseSignBit(
    _In_ USHORT ReportSize)
{
    /* Shift counts wrap like the processor does, a zero size gives bit 31 */
    return 1UL << ((ReportSize - 1) & 31);
}

NTSTATUS
NTAPI
HidP_GetUsageValue(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _In_ USAGE Usage,
    _Out_ PULONG UsageValue,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_reads_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    PHIDPARSE_CAP Cap;
    NTSTATUS Status;
    ULONG BitOffset;

    Status = HidParseFindValue(PreparsedData,
                               ReportType,
                               UsagePage,
                               LinkCollection,
                               Usage,
                               (PUCHAR)Report,
                               ReportLength,
                               &Cap,
                               &BitOffset);
    if (!NT_SUCCESS(Status))
        return Status;

    *UsageValue = HidParseReadBits((PUCHAR)Report, (USHORT)BitOffset, Cap->ReportSize);
    return HIDP_STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HidP_GetScaledUsageValue(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _In_ USAGE Usage,
    _Out_ PLONG UsageValue,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_reads_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    PHIDPARSE_CAP Cap;
    NTSTATUS Status;
    ULONG BitOffset;
    ULONG SignBit;
    LONG LogicalMin;
    LONG LogicalMax;
    LONG PhysicalMin;
    LONG PhysicalMax;
    LONG Value;
    LONG Scaled;

    Status = HidParseFindValue(PreparsedData,
                               ReportType,
                               UsagePage,
                               LinkCollection,
                               Usage,
                               (PUCHAR)Report,
                               ReportLength,
                               &Cap,
                               &BitOffset);
    if (!NT_SUCCESS(Status))
        return Status;

    Value = (LONG)HidParseReadBits((PUCHAR)Report, (USHORT)BitOffset, Cap->ReportSize);
    SignBit = HidParseSignBit(Cap->ReportSize);
    if ((ULONG)Value & SignBit)
        Value = (LONG)((ULONG)Value | (0 - SignBit));

    LogicalMin = Cap->u.Value.LogicalMin;
    LogicalMax = Cap->u.Value.LogicalMax;
    PhysicalMin = Cap->u.Value.PhysicalMin;
    PhysicalMax = Cap->u.Value.PhysicalMax;

    if (PhysicalMin == 0 && PhysicalMax == 0)
    {
        /* Without a physical range the logical value is reported as is */
        if (LogicalMax == LogicalMin)
        {
            *UsageValue = 0;
            return HIDP_STATUS_BAD_LOG_PHY_VALUES;
        }
        Scaled = Value;
    }
    else
    {
        if (LogicalMax <= LogicalMin || PhysicalMax <= PhysicalMin)
        {
            *UsageValue = 0;
            return HIDP_STATUS_BAD_LOG_PHY_VALUES;
        }

        Scaled = (LONG)(PhysicalMin +
                        (LONGLONG)HIDPARSE_DIFF(Value, LogicalMin) *
                        HIDPARSE_DIFF(PhysicalMax, PhysicalMin) /
                        HIDPARSE_DIFF(LogicalMax, LogicalMin));
    }

    *UsageValue = Scaled;
    if (LogicalMin <= Value && Value <= LogicalMax)
        return HIDP_STATUS_SUCCESS;

    *UsageValue = 0;
    return Cap->u.Value.HasNull ? HIDP_STATUS_NULL : HIDP_STATUS_VALUE_OUT_OF_RANGE;
}

NTSTATUS
NTAPI
HidP_GetUsageValueArray(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _In_ USAGE Usage,
    _Inout_updates_bytes_(UsageValueByteLength) PCHAR UsageValue,
    _In_ USHORT UsageValueByteLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_reads_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    PHIDPARSE_CAP Cap;
    NTSTATUS Status;
    ULONG BitOffset;
    ULONG FieldBytes;
    ULONG Field;
    ULONG Byte;

    Status = HidParseFindValueArray(PreparsedData,
                                    ReportType,
                                    UsagePage,
                                    LinkCollection,
                                    Usage,
                                    (PUCHAR)Report,
                                    ReportLength,
                                    &Cap);
    if (!NT_SUCCESS(Status))
        return Status;

    if (8 * (LONG)UsageValueByteLength < (LONG)((ULONG)Cap->ReportCount * Cap->ReportSize))
        return HIDP_STATUS_BUFFER_TOO_SMALL;

    /* Only whole byte fields are supported */
    if (Cap->ReportSize & 7)
        return HIDP_STATUS_NOT_IMPLEMENTED;

    BitOffset = Cap->BitPosition + 8 * Cap->BytePosition;
    FieldBytes = Cap->ReportSize >> 3;

    if (BitOffset & 7)
    {
        for (Field = 0; Field < Cap->ReportCount; Field++)
        {
            for (Byte = 0; Byte < FieldBytes; Byte++)
            {
                *UsageValue++ = (CHAR)HidParseReadBits((PUCHAR)Report, BitOffset, 8);
                BitOffset += 8;
            }
        }
    }
    else
    {
        RtlMoveMemory(UsageValue, Report + (BitOffset >> 3), FieldBytes * Cap->ReportCount);
    }

    return HIDP_STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HidP_SetUsageValue(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _In_ USAGE Usage,
    _In_ ULONG UsageValue,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _Inout_updates_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    PHIDPARSE_CAP Cap;
    NTSTATUS Status;
    ULONG BitOffset;

    Status = HidParseFindValue(PreparsedData,
                               ReportType,
                               UsagePage,
                               LinkCollection,
                               Usage,
                               (PUCHAR)Report,
                               ReportLength,
                               &Cap,
                               &BitOffset);
    if (!NT_SUCCESS(Status))
        return Status;

    Report[0] = Cap->ReportID;
    HidParseWriteBits((PUCHAR)Report,
                      ReportLength,
                      8 * (USHORT)(BitOffset >> 3) + (BitOffset & 7),
                      Cap->ReportSize,
                      UsageValue);

    return HIDP_STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HidP_SetScaledUsageValue(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _In_ USAGE Usage,
    _In_ LONG UsageValue,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _Inout_updates_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    PHIDPARSE_CAP Cap;
    NTSTATUS Status;
    ULONG BitOffset;
    ULONG SignBit;
    LONG LogicalMin;
    LONG LogicalMax;
    LONG PhysicalMin;
    LONG PhysicalMax;
    LONG Numerator;
    LONG Denominator;
    ULONG Raw;

    Status = HidParseFindValue(PreparsedData,
                               ReportType,
                               UsagePage,
                               LinkCollection,
                               Usage,
                               (PUCHAR)Report,
                               ReportLength,
                               &Cap,
                               &BitOffset);
    if (!NT_SUCCESS(Status))
        return Status;

    Report[0] = Cap->ReportID;

    LogicalMin = Cap->u.Value.LogicalMin;
    LogicalMax = Cap->u.Value.LogicalMax;
    PhysicalMin = Cap->u.Value.PhysicalMin;
    PhysicalMax = Cap->u.Value.PhysicalMax;
    SignBit = HidParseSignBit(Cap->ReportSize);

    if (PhysicalMin != 0 || PhysicalMax != 0)
    {
        if (LogicalMax <= LogicalMin || PhysicalMax <= PhysicalMin)
            return HIDP_STATUS_BAD_LOG_PHY_VALUES;

        if (UsageValue < PhysicalMin || UsageValue > PhysicalMax)
        {
            /* Out of range values become the null value when the field has one */
            if (!Cap->u.Value.HasNull)
                return HIDP_STATUS_VALUE_OUT_OF_RANGE;
            Raw = SignBit;
        }
        else
        {
            Numerator = (LONG)((ULONG)HIDPARSE_DIFF(UsageValue, PhysicalMin) *
                               ((ULONG)HIDPARSE_DIFF(LogicalMax, LogicalMin) + 1));
            Denominator = (LONG)((ULONG)HIDPARSE_DIFF(PhysicalMax, PhysicalMin) + 1);
            if (Denominator == 0)
                return HIDP_STATUS_BAD_LOG_PHY_VALUES;

            Raw = (ULONG)LogicalMin + (ULONG)(LONG)((LONGLONG)Numerator / Denominator);
        }
    }
    else
    {
        if (LogicalMax == LogicalMin)
            return HIDP_STATUS_BAD_LOG_PHY_VALUES;

        if (UsageValue >= LogicalMin && UsageValue <= LogicalMax)
        {
            if (UsageValue < 0)
                Raw = (ULONG)UsageValue | SignBit;
            else
                Raw = (ULONG)UsageValue & (SignBit - 1);
        }
        else
        {
            if (!Cap->u.Value.HasNull)
                return HIDP_STATUS_VALUE_OUT_OF_RANGE;
            Raw = SignBit;
        }
    }

    HidParseWriteBits((PUCHAR)Report,
                      ReportLength,
                      8 * (USHORT)(BitOffset >> 3) + (BitOffset & 7),
                      Cap->ReportSize,
                      Raw);

    return HIDP_STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HidP_SetUsageValueArray(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _In_ USAGE Usage,
    _In_reads_bytes_(UsageValueByteLength) PCHAR UsageValue,
    _In_ USHORT UsageValueByteLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _Inout_updates_bytes_(ReportLength) PCHAR Report,
    _In_ ULONG ReportLength)
{
    PHIDPARSE_CAP Cap;
    NTSTATUS Status;
    ULONG BitOffset;
    ULONG FieldBytes;
    ULONG Field;
    ULONG Byte;

    Status = HidParseFindValueArray(PreparsedData,
                                    ReportType,
                                    UsagePage,
                                    LinkCollection,
                                    Usage,
                                    (PUCHAR)Report,
                                    ReportLength,
                                    &Cap);
    if (!NT_SUCCESS(Status))
        return Status;

    Report[0] = Cap->ReportID;

    if (8 * (LONG)UsageValueByteLength < (LONG)((ULONG)Cap->ReportCount * Cap->ReportSize))
        return HIDP_STATUS_BUFFER_TOO_SMALL;

    if (Cap->ReportSize & 7)
        return HIDP_STATUS_NOT_IMPLEMENTED;

    BitOffset = Cap->BitPosition + 8 * Cap->BytePosition;
    FieldBytes = (UCHAR)(Cap->ReportSize >> 3);

    for (Field = 0; Field < Cap->ReportCount; Field++)
    {
        for (Byte = 0; Byte < FieldBytes; Byte++)
        {
            HidParseWriteBits((PUCHAR)Report,
                              ReportLength,
                              8 * (USHORT)(BitOffset >> 3) + (BitOffset & 7),
                              8,
                              (UCHAR)*UsageValue++);
            BitOffset += 8;
        }
    }

    return HIDP_STATUS_SUCCESS;
}
