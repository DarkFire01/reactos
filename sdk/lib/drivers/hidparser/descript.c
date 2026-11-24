/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Report descriptor parsing and collection description
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "hidparser.h"

/* Short item prefixes handled by the parser */
#define ITEM_USAGE_PAGE_1           0x05
#define ITEM_USAGE_PAGE_2           0x06
#define ITEM_USAGE_1                0x09
#define ITEM_USAGE_2                0x0A
#define ITEM_USAGE_4                0x0B
#define ITEM_LOGICAL_MIN_1          0x15
#define ITEM_LOGICAL_MIN_2          0x16
#define ITEM_LOGICAL_MIN_4          0x17
#define ITEM_USAGE_MIN_1            0x19
#define ITEM_USAGE_MIN_2            0x1A
#define ITEM_USAGE_MIN_4            0x1B
#define ITEM_LOGICAL_MAX_1          0x25
#define ITEM_LOGICAL_MAX_2          0x26
#define ITEM_LOGICAL_MAX_4          0x27
#define ITEM_USAGE_MAX_1            0x29
#define ITEM_USAGE_MAX_2            0x2A
#define ITEM_USAGE_MAX_4            0x2B
#define ITEM_PHYSICAL_MIN_1         0x35
#define ITEM_PHYSICAL_MIN_2         0x36
#define ITEM_PHYSICAL_MIN_4         0x37
#define ITEM_DESIGNATOR_INDEX       0x39
#define ITEM_PHYSICAL_MAX_1         0x45
#define ITEM_PHYSICAL_MAX_2         0x46
#define ITEM_PHYSICAL_MAX_4         0x47
#define ITEM_DESIGNATOR_MIN         0x49
#define ITEM_UNIT_EXPONENT_1        0x55
#define ITEM_UNIT_EXPONENT_2        0x56
#define ITEM_UNIT_EXPONENT_4        0x57
#define ITEM_DESIGNATOR_MAX         0x59
#define ITEM_UNIT_1                 0x65
#define ITEM_UNIT_2                 0x66
#define ITEM_UNIT_4                 0x67
#define ITEM_REPORT_SIZE            0x75
#define ITEM_STRING_INDEX           0x79
#define ITEM_INPUT_1                0x81
#define ITEM_INPUT_2                0x82
#define ITEM_REPORT_ID              0x85
#define ITEM_STRING_MIN             0x89
#define ITEM_OUTPUT_1               0x91
#define ITEM_OUTPUT_2               0x92
#define ITEM_REPORT_COUNT_1         0x95
#define ITEM_REPORT_COUNT_2         0x96
#define ITEM_STRING_MAX             0x99
#define ITEM_COLLECTION             0xA1
#define ITEM_PUSH                   0xA4
#define ITEM_DELIMITER              0xA9
#define ITEM_FEATURE_1              0xB1
#define ITEM_FEATURE_2              0xB2
#define ITEM_POP                    0xB4
#define ITEM_END_COLLECTION         0xC0
#define ITEM_LONG                   0xFE

#define ITEM_TYPE_MASK              0x0C
#define ITEM_TYPE_GLOBAL            0x04
#define ITEM_SIZE_MASK              0x03

#define DELIMITER_OPEN              1
#define DELIMITER_CLOSE             0

/** Top level collection under construction. */
typedef struct _HIDPARSE_COLLECTION
{
    HIDP_COLLECTION_DESC Desc;
    struct _HIDPARSE_COLLECTION *Next;
} HIDPARSE_COLLECTION, *PHIDPARSE_COLLECTION;

/** Pending local usage, the newest entry is on top. */
typedef struct _HIDPARSE_USAGE
{
    BOOLEAN IsRange;
    BOOLEAN IsAlias;
    USAGE UsagePage;
    USAGE Usage;
    USAGE UsageMin;
    USAGE UsageMax;
    UCHAR Position;
    struct _HIDPARSE_USAGE *Next;
} HIDPARSE_USAGE, *PHIDPARSE_USAGE;

/** Global item state, saved and restored by Push and Pop. */
typedef struct _HIDPARSE_GLOBALS
{
    USAGE UsagePage;
    USHORT ReportSize;
    USHORT ReportCount;
    USHORT UnknownCount;
    LONG LogicalMin;
    LONG LogicalMax;
    LONG PhysicalMin;
    LONG PhysicalMax;
    LONG UnitExponent;
    ULONG Unit;
    HIDP_UNKNOWN_TOKEN Unknown[HIDPARSE_MAX_UNKNOWN_TOKENS];
    PHIDP_REPORT_IDS ReportId;
    struct _HIDPARSE_GLOBALS *Previous;
} HIDPARSE_GLOBALS, *PHIDPARSE_GLOBALS;

/** State of the sizing pass. */
typedef struct _HIDPARSE_COUNTER
{
    PHIDP_GETCOLDESC_DBG Dbg;
    LONG Depth;
    SHORT PendingUsages;
    USHORT CapCount[3];
    USHORT LinkCount;
    UCHAR CollectionCount;
    UCHAR ReportIdSlots;
    BOOLEAN AwaitingReportId;
    BOOLEAN HasUnnumberedData;
    BOOLEAN HasReportId;
} HIDPARSE_COUNTER, *PHIDPARSE_COUNTER;

/** State of the pass that fills in the preparsed data. */
typedef struct _HIDPARSE_BUILDER
{
    PUCHAR Descriptor;
    ULONG Length;
    ULONG Offset;
    POOL_TYPE PoolType;
    BOOLEAN Strict;
    PHIDP_GETCOLDESC_DBG Dbg;

    PHIDPARSE_GLOBALS Globals;
    HIDPARSE_GLOBALS BaseGlobals;
    PHIDPARSE_USAGE Usages;
    HIDPARSE_USAGE BaseUsage;

    PHIDPARSE_COLLECTION CollectionList;
    PHIDPARSE_COLLECTION Collection;
    PHIDP_PREPARSED_DATA Ppd;
    PHIDPARSE_CAP LastCap;
    BOOLEAN InCollection;
    ULONG Depth;

    PHIDP_REPORT_IDS FirstReportId;
    PHIDP_REPORT_IDS NextReportId;
    PHIDP_REPORT_IDS ReportIdEnd;
    BOOLEAN AwaitingReportId;

    PHIDPARSE_LINK_NODE LinkBase;
    PHIDPARSE_LINK_NODE LinkEnd;
    PHIDPARSE_LINK_NODE CurrentLink;
    PHIDPARSE_LINK_NODE ParentLink;
    USHORT LinkIndex;

    BOOLEAN InDelimiter;
    BOOLEAN FirstDelimiterUsage;

    BOOLEAN StringIsRange;
    BOOLEAN DesignatorIsRange;
    USHORT StringIndex;
    USHORT StringMin;
    USHORT StringMax;
    USHORT DesignatorIndex;
    USHORT DesignatorMin;
    USHORT DesignatorMax;
} HIDPARSE_BUILDER, *PHIDPARSE_BUILDER;

static
NTSTATUS
HidParseFail(
    _Inout_ PHIDP_GETCOLDESC_DBG Dbg,
    _In_ NTSTATUS Status,
    _In_ ULONG Offset,
    _In_ ULONG ErrorCode)
{
    Dbg->BreakOffset = Offset;
    Dbg->ErrorCode = ErrorCode;
    return Status;
}

static
ULONG
HidParseItemDataSize(
    _In_ UCHAR Tag)
{
    ULONG Size = Tag & ITEM_SIZE_MASK;

    return (Size == 3) ? 4 : Size;
}

static
VOID
HidParseInitReportId(
    _Out_ PHIDP_REPORT_IDS ReportId,
    _In_ UCHAR CollectionNumber)
{
    /* Lengths are kept in bits while parsing, the first byte holds the ID */
    ReportId->CollectionNumber = CollectionNumber;
    ReportId->InputLength = 8;
    ReportId->OutputLength = 8;
    ReportId->FeatureLength = 8;
}

static
NTSTATUS
HidParseCountMain(
    _Inout_ PHIDPARSE_COUNTER Counter,
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ UCHAR Data,
    _In_ ULONG Offset)
{
    USHORT NewCount;

    if (Counter->PendingUsages != 0)
    {
        if (Counter->PendingUsages < 1)
            Counter->PendingUsages = 1;

        NewCount = (USHORT)(Counter->CapCount[ReportType] + Counter->PendingUsages);
        if (NewCount < Counter->CapCount[ReportType])
        {
            return HidParseFail(Counter->Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                HIDP_GETCOLDESC_PREPARSE_RESOURCES);
        }
        Counter->CapCount[ReportType] = NewCount;

        if (Counter->AwaitingReportId)
        {
            if (Counter->HasReportId)
            {
                Counter->Dbg->Args[0] = Counter->CollectionCount;
                return HidParseFail(Counter->Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                    HIDP_GETCOLDESC_DEFAULT_ID_ERROR);
            }
            Counter->HasUnnumberedData = TRUE;
        }
        Counter->PendingUsages = 0;
    }
    else if (!(Data & HIDPARSE_MAIN_CONSTANT))
    {
        return HidParseFail(Counter->Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                            HIDP_GETCOLDESC_MAIN_ITEM_NO_USAGE);
    }

    if (Counter->Depth == 0)
    {
        return HidParseFail(Counter->Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                            HIDP_GETCOLDESC_INVALID_MAIN_ITEM);
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
HidParseAllocatePreparsed(
    _Inout_ PHIDPARSE_COUNTER Counter,
    _Inout_ PHIDPARSE_COLLECTION Collection,
    _In_ POOL_TYPE PoolType,
    _In_ ULONG Offset)
{
    PHIDP_PREPARSED_DATA Ppd;
    ULONG TotalCaps;
    ULONG Size;

    Collection->Desc.CollectionNumber = Counter->CollectionCount;

    TotalCaps = (ULONG)Counter->CapCount[HidP_Input] +
                Counter->CapCount[HidP_Output] +
                Counter->CapCount[HidP_Feature];

    /* One spare cap follows the link collection array */
    Size = (ULONG)(HIDPARSE_HEADER_SIZE + sizeof(HIDPARSE_CAP) * (TotalCaps + 1) +
                   sizeof(HIDPARSE_LINK_NODE) * Counter->LinkCount);
    Collection->Desc.PreparsedDataLength = (USHORT)Size;
    if (Size > MAXUSHORT)
    {
        return HidParseFail(Counter->Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                            HIDP_GETCOLDESC_PREPARSE_RESOURCES);
    }

    Ppd = ExAllocatePoolWithTag(PoolType, Size, HIDPARSE_POOL_TAG);
    Collection->Desc.PreparsedData = Ppd;
    if (!Ppd)
    {
        return HidParseFail(Counter->Dbg, STATUS_INSUFFICIENT_RESOURCES, Offset,
                            HIDP_GETCOLDESC_PREPARSE_RESOURCES);
    }
    RtlZeroMemory(Ppd, Size);

    Ppd->SignatureLow = HIDPARSE_SIGNATURE_LOW;
    Ppd->SignatureHigh = HIDPARSE_SIGNATURE_HIGH;

    Ppd->Report[HidP_Input].CapCount = Counter->CapCount[HidP_Input];

    Ppd->Report[HidP_Output].FirstCap = Counter->CapCount[HidP_Input];
    Ppd->Report[HidP_Output].EndCap = Counter->CapCount[HidP_Input];
    Ppd->Report[HidP_Output].CapCount = Counter->CapCount[HidP_Output];

    Ppd->Report[HidP_Feature].FirstCap = Counter->CapCount[HidP_Input] +
                                         Counter->CapCount[HidP_Output];
    Ppd->Report[HidP_Feature].EndCap = Ppd->Report[HidP_Feature].FirstCap;
    Ppd->Report[HidP_Feature].CapCount = Counter->CapCount[HidP_Feature];

    Size = (ULONG)sizeof(HIDPARSE_CAP) * (USHORT)TotalCaps;
    if (Size > MAXUSHORT)
    {
        Ppd->LinkNodeOffset = MAXUSHORT;
        return HidParseFail(Counter->Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                            HIDP_GETCOLDESC_PREPARSE_RESOURCES);
    }
    Ppd->LinkNodeOffset = (USHORT)Size;
    Ppd->LinkNodeCount = Counter->LinkCount;

    return STATUS_SUCCESS;
}

/**
 * @brief First pass: counts collections, caps, link nodes and report IDs and
 * allocates the preparsed data of every top level collection.
 */
static
NTSTATUS
HidParseCountItems(
    _In_reads_bytes_(Length) PUCHAR Descriptor,
    _In_ ULONG Length,
    _In_ POOL_TYPE PoolType,
    _Inout_ PHIDPARSE_COLLECTION *CollectionList,
    _Out_ PULONG CollectionCount,
    _Inout_ PHIDP_DEVICE_DESC DeviceDesc)
{
    HIDPARSE_COUNTER Counter;
    PHIDP_GETCOLDESC_DBG Dbg = &DeviceDesc->Dbg;
    PHIDPARSE_COLLECTION Current = NULL;
    PHIDPARSE_COLLECTION *Tail = CollectionList;
    NTSTATUS Status = STATUS_SUCCESS;
    HIDP_REPORT_TYPE ReportType;
    ULONG Offset = 0;
    ULONG DataSize;
    USHORT NewCount;
    UCHAR Tag;
    UCHAR Data;

    RtlZeroMemory(&Counter, sizeof(Counter));
    Counter.Dbg = Dbg;

    while (Offset < Length)
    {
        Tag = Descriptor[Offset++];

        switch (Tag)
        {
            case ITEM_USAGE_1:
            case ITEM_USAGE_2:
            case ITEM_USAGE_4:
            case ITEM_USAGE_MIN_1:
            case ITEM_USAGE_MIN_2:
            case ITEM_USAGE_MIN_4:
            {
                Offset += HidParseItemDataSize(Tag);
                if (Offset - 1 >= Length)
                {
                    Status = HidParseFail(Dbg, STATUS_BUFFER_TOO_SMALL, Offset,
                                          HIDP_GETCOLDESC_BUFFER);
                    goto Exit;
                }
                Counter.PendingUsages++;
                break;
            }

            case ITEM_INPUT_1:
            case ITEM_INPUT_2:
            case ITEM_OUTPUT_1:
            case ITEM_OUTPUT_2:
            case ITEM_FEATURE_1:
            case ITEM_FEATURE_2:
            {
                if (Tag == ITEM_INPUT_1 || Tag == ITEM_INPUT_2)
                    ReportType = HidP_Input;
                else if (Tag == ITEM_OUTPUT_1 || Tag == ITEM_OUTPUT_2)
                    ReportType = HidP_Output;
                else
                    ReportType = HidP_Feature;

                DataSize = HidParseItemDataSize(Tag);
                if (Offset + DataSize - 1 >= Length)
                {
                    Status = HidParseFail(Dbg, STATUS_BUFFER_TOO_SMALL, Offset,
                                          HIDP_GETCOLDESC_BUFFER);
                    goto Exit;
                }
                Data = Descriptor[Offset];
                Offset += DataSize;

                Status = HidParseCountMain(&Counter, ReportType, Data, Offset);
                if (!NT_SUCCESS(Status))
                    goto Exit;
                break;
            }

            case ITEM_END_COLLECTION:
            {
                Counter.Depth--;
                Counter.PendingUsages = 0;
                if (Counter.Depth < 0)
                {
                    Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                          HIDP_GETCOLDESC_UNEXP_END_COL);
                    goto Exit;
                }

                if (Counter.Depth == 0)
                {
                    Status = HidParseAllocatePreparsed(&Counter, Current, PoolType, Offset);
                    if (!NT_SUCCESS(Status))
                        goto Exit;
                }
                break;
            }

            case ITEM_REPORT_ID:
            {
                if (Offset >= Length)
                {
                    Status = HidParseFail(Dbg, STATUS_BUFFER_TOO_SMALL, Offset,
                                          HIDP_GETCOLDESC_BUFFER);
                    goto Exit;
                }
                Data = Descriptor[Offset++];

                if (Counter.Depth <= 0)
                {
                    Dbg->Args[0] = Data;
                    Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                          HIDP_GETCOLDESC_REPORT_ID);
                    goto Exit;
                }

                /* The first ID of a collection reuses the slot reserved when it opened */
                if (Counter.AwaitingReportId)
                    Counter.AwaitingReportId = FALSE;
                else
                    Counter.ReportIdSlots++;

                Counter.HasReportId = TRUE;
                if (Counter.HasUnnumberedData)
                {
                    Dbg->Args[0] = Counter.CollectionCount;
                    Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                          HIDP_GETCOLDESC_DEFAULT_ID_ERROR);
                    goto Exit;
                }
                break;
            }

            case ITEM_COLLECTION:
            {
                if (Offset >= Length)
                {
                    Status = HidParseFail(Dbg, STATUS_BUFFER_TOO_SMALL, Offset,
                                          HIDP_GETCOLDESC_BUFFER);
                    goto Exit;
                }
                Offset++;

                Counter.Depth++;
                if (Counter.Depth != 1)
                {
                    if (Counter.Depth > 1)
                    {
                        /* Every usage of a nested collection becomes a link node */
                        if (Counter.PendingUsages == 0)
                            Counter.PendingUsages = 1;

                        NewCount = (USHORT)(Counter.LinkCount + Counter.PendingUsages);
                        if (NewCount < Counter.LinkCount)
                        {
                            Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                                  HIDP_GETCOLDESC_PREPARSE_RESOURCES);
                            goto Exit;
                        }
                        Counter.LinkCount = NewCount;
                    }
                    Counter.PendingUsages = 0;
                    break;
                }

                Counter.CapCount[HidP_Input] = 0;
                Counter.CapCount[HidP_Output] = 0;
                Counter.CapCount[HidP_Feature] = 0;
                Counter.LinkCount = 1;

                if (Counter.PendingUsages == 0 || Counter.PendingUsages > 1)
                {
                    Dbg->Args[0] = Counter.CollectionCount;
                    Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                          HIDP_GETCOLDESC_TOP_COLLECTION_USAGE);
                    goto Exit;
                }

                if (Counter.AwaitingReportId)
                {
                    Dbg->Args[0] = Counter.CollectionCount;
                    Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                          HIDP_GETCOLDESC_NO_REPORT_ID);
                    goto Exit;
                }

                if (Counter.HasUnnumberedData)
                {
                    Dbg->Args[0] = Counter.CollectionCount;
                    Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                          HIDP_GETCOLDESC_DEFAULT_ID_ERROR);
                    goto Exit;
                }

                Counter.AwaitingReportId = TRUE;
                Counter.ReportIdSlots++;

                if (Counter.CollectionCount == MAXUCHAR)
                {
                    Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                          HIDP_GETCOLDESC_PREPARSE_RESOURCES);
                    goto Exit;
                }
                Counter.CollectionCount++;

                Current = ExAllocatePoolWithTag(PoolType, sizeof(*Current), HIDPARSE_POOL_TAG);
                if (!Current)
                {
                    Status = HidParseFail(Dbg, STATUS_INSUFFICIENT_RESOURCES, Offset,
                                          HIDP_GETCOLDESC_LINK_RESOURCES);
                    goto Exit;
                }
                RtlZeroMemory(Current, sizeof(*Current));
                *Tail = Current;
                Tail = &Current->Next;

                Counter.PendingUsages = 0;
                break;
            }

            case ITEM_DELIMITER:
            {
                if (Offset >= Length)
                {
                    Status = HidParseFail(Dbg, STATUS_BUFFER_TOO_SMALL, Offset,
                                          HIDP_GETCOLDESC_BUFFER);
                    goto Exit;
                }

                Data = Descriptor[Offset];
                if (Data != DELIMITER_OPEN)
                {
                    Dbg->Args[0] = Data;
                    Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                          HIDP_GETCOLDESC_MISMATCH_OC_DELIMITER);
                    goto Exit;
                }
                Offset++;

                /* Only plain usages may appear inside a delimiter set */
                for (;;)
                {
                    if (Offset >= Length)
                    {
                        Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                              HIDP_GETCOLDESC_NO_CLOSE_DELIMITER);
                        goto Exit;
                    }

                    Tag = Descriptor[Offset++];
                    if (Tag == ITEM_DELIMITER)
                    {
                        if (Offset >= Length)
                        {
                            Status = HidParseFail(Dbg, STATUS_BUFFER_TOO_SMALL, Offset,
                                                  HIDP_GETCOLDESC_BUFFER);
                            goto Exit;
                        }

                        Data = Descriptor[Offset];
                        if (Data != DELIMITER_CLOSE)
                        {
                            Dbg->Args[0] = Data;
                            Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                                  HIDP_GETCOLDESC_MISMATCH_OC_DELIMITER);
                            goto Exit;
                        }
                        Offset++;
                        break;
                    }

                    if (Tag != ITEM_USAGE_1 && Tag != ITEM_USAGE_2 && Tag != ITEM_USAGE_4)
                    {
                        Dbg->Args[0] = Tag;
                        Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                              HIDP_GETCOLDESC_NOT_VALID_DELIMITER);
                        goto Exit;
                    }

                    Offset += HidParseItemDataSize(Tag);
                    if (Offset - 1 >= Length)
                    {
                        Status = HidParseFail(Dbg, STATUS_BUFFER_TOO_SMALL, Offset,
                                              HIDP_GETCOLDESC_BUFFER);
                        goto Exit;
                    }
                    Counter.PendingUsages++;
                }
                break;
            }

            case ITEM_LONG:
            {
                Dbg->Args[0] = Tag;
                Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                                      HIDP_GETCOLDESC_ITEM_UNKNOWN);
                goto Exit;
            }

            default:
            {
                DataSize = HidParseItemDataSize(Tag);
                if (Offset + DataSize > Length)
                {
                    Status = HidParseFail(Dbg, STATUS_BUFFER_TOO_SMALL, Offset,
                                          HIDP_GETCOLDESC_ONE_BYTE);
                    goto Exit;
                }
                Offset += DataSize;
                break;
            }
        }
    }

    if (Counter.ReportIdSlots < Counter.CollectionCount)
    {
        Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                              HIDP_GETCOLDESC_NO_REPORT_ID);
        goto Exit;
    }

    if (Counter.Depth > 0)
    {
        Status = HidParseFail(Dbg, STATUS_COULD_NOT_INTERPRET, Offset,
                              HIDP_GETCOLDESC_UNEXP_END_COL);
        goto Exit;
    }

    if (Counter.CollectionCount == 0)
    {
        Status = HidParseFail(Dbg, STATUS_NO_DATA_DETECTED, Offset, HIDP_GETCOLDESC_NO_DATA);
        goto Exit;
    }

    DeviceDesc->ReportIDsLength = Counter.ReportIdSlots;
    DeviceDesc->ReportIDs = ExAllocatePoolWithTag(PoolType,
                                                  sizeof(HIDP_REPORT_IDS) * Counter.ReportIdSlots,
                                                  HIDPARSE_POOL_TAG);
    if (!DeviceDesc->ReportIDs)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    RtlZeroMemory(DeviceDesc->ReportIDs, sizeof(HIDP_REPORT_IDS) * Counter.ReportIdSlots);

Exit:
    *CollectionCount = Counter.CollectionCount;
    return Status;
}

static
PHIDPARSE_USAGE
HidParsePushUsage(
    _In_ PHIDPARSE_USAGE Top,
    _In_ POOL_TYPE PoolType,
    _In_ BOOLEAN KeepPosition)
{
    PHIDPARSE_USAGE Usage;

    Usage = ExAllocatePoolWithTag(PoolType, sizeof(*Usage), HIDPARSE_POOL_TAG);
    if (!Usage)
        return NULL;

    RtlZeroMemory(Usage, sizeof(*Usage));
    Usage->Next = Top;

    /* Usages inside a delimiter set describe the same field */
    if (KeepPosition)
        Usage->Position = Top->Position;
    else if (Top->IsRange)
        Usage->Position = (UCHAR)(Top->Position + Top->UsageMax - Top->UsageMin + 1);
    else
        Usage->Position = (UCHAR)(Top->Position + 1);

    return Usage;
}

static
VOID
HidParsePopUsage(
    _Inout_ PHIDPARSE_BUILDER Builder)
{
    PHIDPARSE_USAGE Top = Builder->Usages;

    if (Top->Next)
    {
        Builder->Usages = Top->Next;
        ExFreePoolWithTag(Top, HIDPARSE_POOL_TAG);
    }
}

static
VOID
HidParseClearUsages(
    _Inout_ PHIDPARSE_BUILDER Builder)
{
    while (Builder->Usages->Next)
        HidParsePopUsage(Builder);

    RtlZeroMemory(Builder->Usages, sizeof(*Builder->Usages));
}

static
VOID
HidParseClearLocals(
    _Inout_ PHIDPARSE_BUILDER Builder)
{
    Builder->StringIsRange = FALSE;
    Builder->DesignatorIsRange = FALSE;
    Builder->StringIndex = 0;
    Builder->StringMin = 0;
    Builder->StringMax = 0;
    Builder->DesignatorIndex = 0;
    Builder->DesignatorMin = 0;
    Builder->DesignatorMax = 0;
}

static
NTSTATUS
HidParseReadData(
    _Inout_ PHIDPARSE_BUILDER Builder,
    _In_ UCHAR Tag,
    _Out_ PULONG Value)
{
    PUCHAR Data = Builder->Descriptor + Builder->Offset;

    *Value = 0;

    switch (Tag & ITEM_SIZE_MASK)
    {
        case 1:
        {
            if (Builder->Offset >= Builder->Length)
            {
                return HidParseFail(Builder->Dbg, STATUS_BUFFER_TOO_SMALL, Builder->Offset,
                                    HIDP_GETCOLDESC_ONE_BYTE);
            }
            *Value = Data[0];
            Builder->Offset += 1;
            break;
        }
        case 2:
        {
            if (Builder->Offset + 1 >= Builder->Length)
            {
                return HidParseFail(Builder->Dbg, STATUS_BUFFER_TOO_SMALL, Builder->Offset,
                                    HIDP_GETCOLDESC_TWO_BYTE);
            }
            *Value = Data[0] | (Data[1] << 8);
            Builder->Offset += 2;
            break;
        }
        case 3:
        {
            if (Builder->Offset + 3 >= Builder->Length)
            {
                return HidParseFail(Builder->Dbg, STATUS_BUFFER_TOO_SMALL, Builder->Offset,
                                    HIDP_GETCOLDESC_FOUR_BYTE);
            }
            *Value = Data[0] | (Data[1] << 8) | (Data[2] << 16) | ((ULONG)Data[3] << 24);
            Builder->Offset += 4;
            break;
        }
    }

    return STATUS_SUCCESS;
}

static
LONG
HidParseSignExtend(
    _In_ UCHAR Tag,
    _In_ ULONG Value)
{
    switch (Tag & ITEM_SIZE_MASK)
    {
        case 1:
            return (LONG)(CHAR)Value;
        case 2:
            return (LONG)(SHORT)Value;
        default:
            return (LONG)Value;
    }
}

static
NTSTATUS
HidParseUsageItem(
    _Inout_ PHIDPARSE_BUILDER Builder,
    _In_ UCHAR Tag)
{
    PHIDPARSE_USAGE Top = Builder->Usages;
    PUCHAR Data = Builder->Descriptor + Builder->Offset;
    UCHAR Kind = Tag & 0xF0;
    BOOLEAN IsReusable;
    USAGE Value;

    /* An entry that holds nothing yet takes the next local usage item */
    if (Top == &Builder->BaseUsage || Top->Usage != 0)
        IsReusable = FALSE;
    else if (Kind == (ITEM_USAGE_MIN_1 & 0xF0))
        IsReusable = (Top->UsageMin == 0);
    else if (Kind == (ITEM_USAGE_MAX_1 & 0xF0))
        IsReusable = (Top->UsageMax == 0);
    else
        IsReusable = (Top->UsageMin == 0 && Top->UsageMax == 0);

    if (!IsReusable)
    {
        Top = HidParsePushUsage(Top,
                                Builder->PoolType,
                                (Kind == (ITEM_USAGE_1 & 0xF0)) && Builder->InDelimiter);
        if (!Top)
        {
            return HidParseFail(Builder->Dbg, STATUS_INSUFFICIENT_RESOURCES, Builder->Offset,
                                HIDP_GETCOLDESC_PUSH_RESOURCES);
        }
        Builder->Usages = Top;
    }

    Top->IsRange = (Kind != (ITEM_USAGE_1 & 0xF0));

    if ((Tag & ITEM_SIZE_MASK) == 1)
    {
        if (Builder->Offset >= Builder->Length)
        {
            return HidParseFail(Builder->Dbg, STATUS_BUFFER_TOO_SMALL, Builder->Offset,
                                HIDP_GETCOLDESC_ONE_BYTE);
        }
        Value = Data[0];
        Builder->Offset += 1;
    }
    else
    {
        if (Builder->Offset + 1 >= Builder->Length)
        {
            return HidParseFail(Builder->Dbg, STATUS_BUFFER_TOO_SMALL, Builder->Offset,
                                HIDP_GETCOLDESC_TWO_BYTE);
        }
        Value = Data[0] | (Data[1] << 8);

        /* The upper half of an extended usage selects the usage page */
        if ((Tag & ITEM_SIZE_MASK) == 3)
        {
            if (Builder->Offset + 3 >= Builder->Length)
            {
                return HidParseFail(Builder->Dbg, STATUS_BUFFER_TOO_SMALL, Builder->Offset + 2,
                                    HIDP_GETCOLDESC_TWO_BYTE);
            }
            Top->UsagePage = Data[2] | (Data[3] << 8);
            Builder->Offset += 2;
        }
        Builder->Offset += 2;
    }

    if (Kind == (ITEM_USAGE_MIN_1 & 0xF0))
    {
        Top->UsageMin = Value;
    }
    else if (Kind == (ITEM_USAGE_MAX_1 & 0xF0))
    {
        Top->UsageMax = Value;
    }
    else
    {
        Top->Usage = Value;
        if (Builder->InDelimiter)
        {
            Top->IsAlias = !Builder->FirstDelimiterUsage;
            Builder->FirstDelimiterUsage = FALSE;
        }
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
HidParseReportIdItem(
    _Inout_ PHIDPARSE_BUILDER Builder)
{
    PHIDP_REPORT_IDS Entry;
    PHIDP_REPORT_IDS Found = NULL;
    UCHAR ReportId;

    if (Builder->Offset >= Builder->Length)
    {
        return HidParseFail(Builder->Dbg, STATUS_BUFFER_TOO_SMALL, Builder->Offset,
                            HIDP_GETCOLDESC_ONE_BYTE);
    }
    ReportId = Builder->Descriptor[Builder->Offset++];

    for (Entry = Builder->FirstReportId; Entry != Builder->NextReportId; Entry++)
    {
        if (Entry->ReportID == ReportId)
        {
            Found = Entry;
            break;
        }
    }

    if (Found)
    {
        /* A report ID may not be shared between top level collections */
        if (Found->CollectionNumber != Builder->Collection->Desc.CollectionNumber)
        {
            Builder->Dbg->Args[0] = ITEM_REPORT_ID;
            return HidParseFail(Builder->Dbg, HIDP_STATUS_INVALID_REPORT_TYPE, Builder->Offset,
                                HIDP_GETCOLDESC_REPORT_ID);
        }
        Builder->Globals->ReportId = Found;
    }

    if (Builder->AwaitingReportId)
    {
        /* The entry reserved when the collection opened gets this ID */
        Builder->AwaitingReportId = FALSE;
    }
    else if (!Found)
    {
        if (!Builder->NextReportId || Builder->NextReportId >= Builder->ReportIdEnd)
            return STATUS_COULD_NOT_INTERPRET;

        HidParseInitReportId(Builder->NextReportId, Builder->Collection->Desc.CollectionNumber);
        Builder->Globals->ReportId = Builder->NextReportId++;
    }

    Entry = Builder->Globals->ReportId;
    if (!Entry)
        return STATUS_COULD_NOT_INTERPRET;

    Entry->ReportID = ReportId;
    if (Entry->ReportID == 0)
    {
        return HidParseFail(Builder->Dbg, HIDP_STATUS_INVALID_REPORT_TYPE, Builder->Offset,
                            HIDP_GETCOLDESC_BAD_REPORT_ID);
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
HidParseOpenCollection(
    _Inout_ PHIDPARSE_BUILDER Builder)
{
    PHIDPARSE_COLLECTION Collection;
    PHIDPARSE_USAGE Top;
    PHIDPARSE_LINK_NODE Parent;
    PHIDPARSE_LINK_NODE Node;
    PHIDP_PREPARSED_DATA Ppd;
    USAGE UsagePage;
    USAGE Usage;
    UCHAR CollectionType;

    if (Builder->Offset >= Builder->Length)
    {
        return HidParseFail(Builder->Dbg, STATUS_BUFFER_TOO_SMALL, Builder->Offset,
                            HIDP_GETCOLDESC_ONE_BYTE);
    }
    CollectionType = Builder->Descriptor[Builder->Offset++];

    Top = Builder->Usages;
    UsagePage = Top->UsagePage ? Top->UsagePage : Builder->Globals->UsagePage;
    Usage = Top->IsRange ? Top->UsageMin : Top->Usage;

    Builder->Depth++;
    if (Builder->Depth > 1)
    {
        Parent = Builder->CurrentLink;
        Builder->LinkIndex++;
        Node = &Builder->LinkBase[Builder->LinkIndex];

        /* An aliased usage would repeat until the node array overflows */
        if (Node >= Builder->LinkEnd || Top->IsAlias)
            return STATUS_COULD_NOT_INTERPRET;

        Node->LinkUsagePage = UsagePage;
        Node->LinkUsage = Usage;
        Node->NumberOfChildren = 0;
        Node->FirstChild = 0;
        Node->Parent = (USHORT)(Parent - Builder->LinkBase);
        Node->NextSibling = Parent->FirstChild;
        Node->Flags = (Node->Flags & ~HIDPARSE_LINK_TYPE_MASK) | CollectionType;

        Parent->NumberOfChildren++;
        Parent->FirstChild = Builder->LinkIndex;

        Builder->CurrentLink = Node;
        Builder->ParentLink = Parent;

        HidParseClearLocals(Builder);
        HidParseClearUsages(Builder);
        return STATUS_SUCCESS;
    }

    Collection = Builder->Collection ? Builder->Collection->Next : Builder->CollectionList;
    if (!Collection)
        return STATUS_COULD_NOT_INTERPRET;

    Builder->Collection = Collection;
    Ppd = Collection->Desc.PreparsedData;
    Builder->Ppd = Ppd;
    Builder->InCollection = TRUE;

    /* Data without a Report ID item goes to an entry with ID zero */
    if (!Builder->NextReportId || Builder->NextReportId >= Builder->ReportIdEnd)
        return STATUS_COULD_NOT_INTERPRET;

    Builder->NextReportId->ReportID = 0;
    HidParseInitReportId(Builder->NextReportId, Collection->Desc.CollectionNumber);
    Builder->Globals->ReportId = Builder->NextReportId++;
    Builder->AwaitingReportId = TRUE;

    Collection->Desc.UsagePage = UsagePage;
    Collection->Desc.Usage = Usage;
    Ppd->UsagePage = UsagePage;
    Ppd->Usage = Usage;

    HidParseClearLocals(Builder);
    HidParseClearUsages(Builder);

    Builder->LinkIndex = 0;
    Builder->LinkBase = HidParseGetLinkNodes(Ppd);
    Builder->LinkEnd = Builder->LinkBase + Ppd->LinkNodeCount;

    Node = Builder->LinkBase;
    Node->LinkUsagePage = UsagePage;
    Node->LinkUsage = Usage;
    Node->Parent = 0;
    Node->NumberOfChildren = 0;
    Node->NextSibling = 0;
    Node->FirstChild = 0;
    Node->Flags = (Node->Flags & ~HIDPARSE_LINK_TYPE_MASK) | CollectionType;

    Builder->CurrentLink = Node;
    Builder->ParentLink = Node;

    return STATUS_SUCCESS;
}

static
USHORT
HidParseFinalLength(
    _In_ UCHAR ReportId,
    _In_ USHORT BitLength)
{
    /* Numbered reports count the ID byte, unnumbered ones do not */
    if (ReportId != 0)
        return (BitLength == 8) ? 0 : (BitLength >> 3);

    return (USHORT)((BitLength >> 3) - 1);
}

static
NTSTATUS
HidParseCloseCollection(
    _Inout_ PHIDPARSE_BUILDER Builder)
{
    PHIDPARSE_COLLECTION Collection = Builder->Collection;
    PHIDP_PREPARSED_DATA Ppd = Builder->Ppd;
    PHIDP_REPORT_IDS Entry;
    USHORT Lengths[3];
    ULONG Type;

    if (Builder->Depth == 0)
        return STATUS_COULD_NOT_INTERPRET;

    Builder->Depth--;
    if (Builder->Depth != 0)
    {
        if (Builder->ParentLink >= Builder->LinkEnd ||
            Builder->ParentLink->Parent >= Ppd->LinkNodeCount)
        {
            return STATUS_COULD_NOT_INTERPRET;
        }

        Builder->CurrentLink = Builder->ParentLink;
        Builder->ParentLink = &Builder->LinkBase[Builder->ParentLink->Parent];
        return STATUS_SUCCESS;
    }

    for (Entry = Builder->NextReportId; Entry != Builder->FirstReportId; )
    {
        Entry--;
        if (Entry->CollectionNumber != Collection->Desc.CollectionNumber)
            continue;

        Lengths[HidP_Input] = Entry->InputLength;
        Lengths[HidP_Output] = Entry->OutputLength;
        Lengths[HidP_Feature] = Entry->FeatureLength;

        if ((Lengths[HidP_Input] | Lengths[HidP_Output] | Lengths[HidP_Feature]) & 7)
        {
            Builder->Dbg->Args[0] = Collection->Desc.CollectionNumber;
            Builder->Dbg->Args[1] = Entry->ReportID;
            Builder->Dbg->Args[2] = Lengths[HidP_Input];
            Builder->Dbg->Args[3] = Lengths[HidP_Output];
            Builder->Dbg->Args[4] = Lengths[HidP_Feature];
            return HidParseFail(Builder->Dbg, STATUS_COULD_NOT_INTERPRET, Builder->Offset,
                                HIDP_GETCOLDESC_BYTE_ALLIGN);
        }

        for (Type = HidP_Input; Type <= HidP_Feature; Type++)
        {
            if (Ppd->Report[Type].ReportByteLength < (Lengths[Type] >> 3))
                Ppd->Report[Type].ReportByteLength = Lengths[Type] >> 3;
        }

        Entry->InputLength = HidParseFinalLength(Entry->ReportID, Lengths[HidP_Input]);
        Entry->OutputLength = HidParseFinalLength(Entry->ReportID, Lengths[HidP_Output]);
        Entry->FeatureLength = HidParseFinalLength(Entry->ReportID, Lengths[HidP_Feature]);
    }

    /* A report that only holds the ID byte does not exist */
    for (Type = HidP_Input; Type <= HidP_Feature; Type++)
    {
        if (Ppd->Report[Type].ReportByteLength == 1)
            Ppd->Report[Type].ReportByteLength = 0;
    }

    Collection->Desc.InputLength = Ppd->Report[HidP_Input].ReportByteLength;
    Collection->Desc.OutputLength = Ppd->Report[HidP_Output].ReportByteLength;
    Collection->Desc.FeatureLength = Ppd->Report[HidP_Feature].ReportByteLength;

    return STATUS_SUCCESS;
}

/**
 * @brief Fills the fields that array and variable caps share.
 */
static
NTSTATUS
HidParseFillCap(
    _Inout_ PHIDPARSE_BUILDER Builder,
    _Out_ PHIDPARSE_CAP Cap,
    _In_ USHORT Data,
    _In_ USHORT BitOffset,
    _In_ USHORT ReportCount)
{
    PHIDPARSE_GLOBALS Globals = Builder->Globals;
    PHIDPARSE_USAGE Usage = Builder->Usages;
    ULONG BitCount;
    ULONG Flags;
    ULONG Index;

    Cap->BitField = Data;
    Cap->LinkCollection = (USHORT)(Builder->CurrentLink - Builder->LinkBase);
    Cap->LinkUsage = Builder->CurrentLink->LinkUsage;
    Cap->LinkUsagePage = Builder->CurrentLink->LinkUsagePage;
    Cap->UsagePage = Usage->UsagePage ? Usage->UsagePage : Globals->UsagePage;

    Cap->BitPosition = (UCHAR)(BitOffset & 7);
    Cap->BytePosition = BitOffset >> 3;
    Cap->ReportSize = Globals->ReportSize;
    Cap->ReportCount = ReportCount;

    BitCount = (ULONG)ReportCount * Globals->ReportSize;
    if (BitCount > MAXUSHORT && Builder->Strict)
    {
        return HidParseFail(Builder->Dbg, STATUS_COULD_NOT_INTERPRET, Builder->Offset,
                            HIDPARSE_GETCOLDESC_LENGTH_OVERFLOW);
    }
    Cap->BitCount = (USHORT)BitCount;
    Cap->NextBytePosition = Cap->BytePosition +
                            ((USHORT)(BitCount + Cap->BitPosition) >> 3) +
                            (((BitCount + Cap->BitPosition) & 7) != 0);

    if (!Globals->ReportId)
        return STATUS_COULD_NOT_INTERPRET;

    Cap->ReportID = Globals->ReportId->ReportID;
    Cap->Units = Globals->Unit;
    Cap->UnitsExp = (ULONG)Globals->UnitExponent;

    Flags = Cap->Flags & ~(HIDPARSE_CAP_ABSOLUTE | HIDPARSE_CAP_RANGE |
                           HIDPARSE_CAP_STRING_RANGE | HIDPARSE_CAP_DESIGNATOR_RANGE |
                           HIDPARSE_CAP_UNKNOWN_MASK);
    if (!(Data & HIDPARSE_MAIN_RELATIVE))
        Flags |= HIDPARSE_CAP_ABSOLUTE;
    if (Usage->IsRange)
        Flags |= HIDPARSE_CAP_RANGE;
    if (Builder->StringIsRange)
        Flags |= HIDPARSE_CAP_STRING_RANGE;
    if (Builder->DesignatorIsRange)
        Flags |= HIDPARSE_CAP_DESIGNATOR_RANGE;
    Flags |= (ULONG)Globals->UnknownCount << HIDPARSE_CAP_UNKNOWN_SHIFT;
    Cap->Flags = Flags;

    if (Usage->IsRange)
    {
        Cap->UsageMin = Usage->UsageMin;
        Cap->UsageMax = Usage->UsageMax;
    }
    else
    {
        Cap->UsageMin = Usage->Usage;
        Cap->UsageMax = Usage->Usage;
    }

    if (Builder->DesignatorIsRange)
    {
        Cap->DesignatorMin = Builder->DesignatorMin;
        Cap->DesignatorMax = Builder->DesignatorMax;
    }
    else
    {
        Cap->DesignatorMin = Builder->DesignatorIndex;
        Cap->DesignatorMax = Builder->DesignatorIndex;
    }

    if (Builder->StringIsRange)
    {
        Cap->StringMin = Builder->StringMin;
        Cap->StringMax = Builder->StringMax;
    }
    else
    {
        Cap->StringMin = Builder->StringIndex;
        Cap->StringMax = Builder->StringIndex;
    }

    for (Index = 0; Index < Globals->UnknownCount; Index++)
        Cap->UnknownTokens[Index] = Globals->Unknown[Index];

    if (Cap->UsagePage == HID_USAGE_PAGE_GENERIC)
    {
        if (Cap->UsageMin <= HIDPARSE_USAGE_SYSTEM_POWER &&
            Cap->UsageMax >= HIDPARSE_USAGE_SYSTEM_POWER)
        {
            Builder->Ppd->PowerButtonMask |= SYS_BUTTON_POWER;
        }
        if (Cap->UsageMin <= HIDPARSE_USAGE_SYSTEM_SLEEP &&
            Cap->UsageMax >= HIDPARSE_USAGE_SYSTEM_SLEEP)
        {
            Builder->Ppd->PowerButtonMask |= SYS_BUTTON_SLEEP;
        }
        if (Cap->UsageMin <= HIDPARSE_USAGE_SYSTEM_WAKE &&
            Cap->UsageMax >= HIDPARSE_USAGE_SYSTEM_WAKE)
        {
            Builder->Ppd->PowerButtonMask |= SYS_BUTTON_WAKE;
        }
    }

    Builder->LastCap = Cap;
    return STATUS_SUCCESS;
}

static
ULONG
HidParseTotalCaps(
    _In_ PHIDP_PREPARSED_DATA Ppd)
{
    return (ULONG)Ppd->Report[HidP_Input].CapCount +
           Ppd->Report[HidP_Output].CapCount +
           Ppd->Report[HidP_Feature].CapCount;
}

/**
 * @brief Array main item: every pending usage becomes one cap, newest first,
 * all sharing the same report position.
 */
static
NTSTATUS
HidParseArrayItem(
    _Inout_ PHIDPARSE_BUILDER Builder,
    _Inout_ PHIDPARSE_REPORT_INFO Info,
    _In_ UCHAR Tag,
    _In_ USHORT Data,
    _In_ USHORT BitOffset)
{
    PHIDPARSE_CAP Cap;
    NTSTATUS Status;

    /* Constant arrays describe no data and leave the local items in place */
    if (Data & HIDPARSE_MAIN_CONSTANT)
        return STATUS_SUCCESS;

    if (Builder->Usages->IsAlias)
    {
        return HidParseFail(Builder->Dbg, STATUS_COULD_NOT_INTERPRET, Builder->Offset,
                            HIDP_GETCOLDESC_UNSUPPORTED);
    }

    while (Builder->Usages != &Builder->BaseUsage)
    {
        if (Info->EndCap >= HidParseTotalCaps(Builder->Ppd))
        {
            Builder->Dbg->Args[0] = Tag;
            return HidParseFail(Builder->Dbg, STATUS_ILLEGAL_INSTRUCTION, Builder->Offset,
                                HIDP_GETCOLDESC_ITEM_UNKNOWN);
        }

        Cap = &Builder->Ppd->Caps[Info->EndCap];
        Status = HidParseFillCap(Builder, Cap, Data, BitOffset, Builder->Globals->ReportCount);
        if (!NT_SUCCESS(Status))
            return Status;

        Cap->Flags &= ~HIDPARSE_CAP_CONSTANT;
        Cap->Flags |= HIDPARSE_CAP_ARRAY_CHAIN | HIDPARSE_CAP_BUTTON;
        Cap->u.Button.LogicalMin = Builder->Globals->LogicalMin;
        Cap->u.Button.LogicalMax = Builder->Globals->LogicalMax;

        Info->EndCap++;
        HidParsePopUsage(Builder);
    }

    if (Builder->LastCap)
        Builder->LastCap->Flags &= ~HIDPARSE_CAP_ARRAY_CHAIN;

    HidParseClearLocals(Builder);
    return STATUS_SUCCESS;
}

/**
 * @brief Variable main item: the newest usage takes the trailing fields, older
 * usages take one field per usage in front of it.
 */
static
NTSTATUS
HidParseVariableItem(
    _Inout_ PHIDPARSE_BUILDER Builder,
    _Inout_ PHIDPARSE_REPORT_INFO Info,
    _In_ UCHAR Tag,
    _In_ USHORT Data,
    _In_ USHORT BitOffset)
{
    PHIDPARSE_GLOBALS Globals = Builder->Globals;
    PHIDPARSE_USAGE Usage = Builder->Usages;
    PHIDPARSE_CAP Cap;
    NTSTATUS Status;
    BOOLEAN WasAlias;
    USHORT Span = 0;
    USHORT Last;
    USHORT Fields;
    USHORT Consumed;
    USHORT Total;
    USHORT Position;
    USHORT StartPosition;

    if (Data & HIDPARSE_MAIN_CONSTANT)
    {
        /* Padding without usages produces no cap */
        if (Usage->Position == 0 ||
            (Usage->Usage == 0 && Usage->UsageMin == 0 && Usage->UsageMax == 0))
        {
            HidParseClearUsages(Builder);
            return STATUS_SUCCESS;
        }

        if (Info->EndCap < HidParseTotalCaps(Builder->Ppd))
            Builder->Ppd->Caps[Info->EndCap].Flags |= HIDPARSE_CAP_CONSTANT;
    }
    else if (Info->EndCap < HidParseTotalCaps(Builder->Ppd))
    {
        Builder->Ppd->Caps[Info->EndCap].Flags &= ~HIDPARSE_CAP_CONSTANT;
    }

    if (Usage->IsRange)
    {
        if (Usage->UsageMax < Usage->UsageMin)
        {
            Builder->Dbg->Args[0] = Tag;
            return HidParseFail(Builder->Dbg, STATUS_ILLEGAL_INSTRUCTION, Builder->Offset,
                                HIDP_GETCOLDESC_ITEM_UNKNOWN);
        }
        Span = Usage->UsageMax - Usage->UsageMin;
    }

    /* Drop usages that do not fit into the report count */
    Last = (USHORT)(Span + Usage->Position);
    while (Last > Globals->ReportCount)
    {
        if (Last > Usage->Position)
        {
            Usage->UsageMax = (USAGE)(Usage->UsageMin + Globals->ReportCount - Usage->Position);
            Last = (USHORT)(Usage->UsageMax + Usage->Position - Usage->UsageMin);
        }
        else
        {
            Last = (USHORT)(Usage->Position - 1);
            HidParsePopUsage(Builder);
            Usage = Builder->Usages;
        }
    }

    Fields = (USHORT)(Globals->ReportCount + Usage->UsageMax - Usage->UsageMin - Last + 1);
    Position = (USHORT)(BitOffset - Globals->ReportSize *
                        (USHORT)(Usage->UsageMax - Usage->UsageMin - Last + 1));
    StartPosition = Position;
    Consumed = 0;

    if (Globals->ReportCount == 0)
    {
        HidParseClearLocals(Builder);
        return STATUS_SUCCESS;
    }

    for (;;)
    {
        if (Info->EndCap >= HidParseTotalCaps(Builder->Ppd))
        {
            Builder->Dbg->Args[0] = Tag;
            return HidParseFail(Builder->Dbg, STATUS_ILLEGAL_INSTRUCTION, Builder->Offset,
                                HIDP_GETCOLDESC_ITEM_UNKNOWN);
        }

        Cap = &Builder->Ppd->Caps[Info->EndCap++];
        Status = HidParseFillCap(Builder, Cap, Data, Position, Fields);
        if (!NT_SUCCESS(Status))
            return Status;

        Cap->Flags &= ~HIDPARSE_CAP_ARRAY_CHAIN;
        Cap->Flags |= HIDPARSE_CAP_ALIAS;

        if (Globals->ReportSize == 1)
        {
            Cap->Flags |= HIDPARSE_CAP_BUTTON;
        }
        else
        {
            Cap->Flags &= ~HIDPARSE_CAP_BUTTON;
            Cap->u.Value.HasNull = (Cap->BitField & HIDPARSE_MAIN_NULL_STATE) != 0;
            Cap->u.Value.LogicalMin = Globals->LogicalMin;
            Cap->u.Value.LogicalMax = Globals->LogicalMax;
            Cap->u.Value.PhysicalMin = Globals->PhysicalMin;
            Cap->u.Value.PhysicalMax = Globals->PhysicalMax;
        }

        WasAlias = Usage->IsAlias;
        HidParsePopUsage(Builder);
        Usage = Builder->Usages;
        Position = StartPosition;

        /* Aliases share the position of the usage that ends the delimiter set */
        if (WasAlias)
            continue;

        Cap->Flags &= ~HIDPARSE_CAP_ALIAS;

        Total = (USHORT)(Fields + Consumed);
        Consumed = Total;

        Fields = (USHORT)((Usage->IsRange ? Usage->UsageMax - Usage->UsageMin : 0) + 1);
        Position = (USHORT)(StartPosition - Globals->ReportSize * Fields);
        StartPosition = Position;

        if (Total >= Globals->ReportCount)
            break;
    }

    HidParseClearLocals(Builder);
    return STATUS_SUCCESS;
}

static
NTSTATUS
HidParseMainItem(
    _Inout_ PHIDPARSE_BUILDER Builder,
    _In_ UCHAR Tag)
{
    PHIDPARSE_GLOBALS Globals = Builder->Globals;
    PHIDP_REPORT_IDS Entry = Globals->ReportId;
    PUCHAR Data = Builder->Descriptor + Builder->Offset;
    HIDP_REPORT_TYPE ReportType;
    PUSHORT ReportLength;
    USHORT BitOffset;
    USHORT Flags;
    ULONG BitCount;

    if (!Entry)
        return STATUS_COULD_NOT_INTERPRET;

    if (Tag == ITEM_INPUT_1 || Tag == ITEM_INPUT_2)
    {
        ReportType = HidP_Input;
        ReportLength = &Entry->InputLength;
    }
    else if (Tag == ITEM_OUTPUT_1 || Tag == ITEM_OUTPUT_2)
    {
        ReportType = HidP_Output;
        ReportLength = &Entry->OutputLength;
    }
    else
    {
        ReportType = HidP_Feature;
        ReportLength = &Entry->FeatureLength;
    }

    BitOffset = *ReportLength;
    BitCount = (ULONG)Globals->ReportSize * Globals->ReportCount;
    if (Builder->Strict &&
        (BitCount > MAXUSHORT || (USHORT)(BitOffset + BitCount) < BitOffset))
    {
        return HidParseFail(Builder->Dbg, STATUS_COULD_NOT_INTERPRET, Builder->Offset,
                            HIDPARSE_GETCOLDESC_LENGTH_OVERFLOW);
    }
    *ReportLength = (USHORT)(BitOffset + BitCount);

    if ((Tag & ITEM_SIZE_MASK) == 1)
    {
        if (Builder->Offset >= Builder->Length)
        {
            return HidParseFail(Builder->Dbg, STATUS_BUFFER_TOO_SMALL, Builder->Offset,
                                HIDP_GETCOLDESC_ONE_BYTE);
        }
        Flags = Data[0];
        Builder->Offset += 1;
    }
    else
    {
        if (Builder->Offset + 1 >= Builder->Length)
        {
            return HidParseFail(Builder->Dbg, STATUS_BUFFER_TOO_SMALL, Builder->Offset,
                                HIDP_GETCOLDESC_TWO_BYTE);
        }
        Flags = Data[0] | (Data[1] << 8);
        Builder->Offset += 2;
    }

    if (!Builder->InCollection)
        return STATUS_COULD_NOT_INTERPRET;

    if (Flags & HIDPARSE_MAIN_VARIABLE)
    {
        return HidParseVariableItem(Builder, &Builder->Ppd->Report[ReportType], Tag, Flags,
                                    BitOffset);
    }

    return HidParseArrayItem(Builder, &Builder->Ppd->Report[ReportType], Tag, Flags, BitOffset);
}

static
NTSTATUS
HidParseUnknownGlobal(
    _Inout_ PHIDPARSE_BUILDER Builder,
    _In_ UCHAR Tag)
{
    PHIDPARSE_GLOBALS Globals = Builder->Globals;
    NTSTATUS Status;
    ULONG Index;
    ULONG Value;

    /* Only four are kept, later ones overwrite the last slot */
    if (Globals->UnknownCount >= HIDPARSE_MAX_UNKNOWN_TOKENS)
        Globals->UnknownCount = HIDPARSE_MAX_UNKNOWN_TOKENS - 1;

    Index = Globals->UnknownCount;
    Globals->Unknown[Index].Token = Tag;

    if (Tag & ITEM_SIZE_MASK)
    {
        Status = HidParseReadData(Builder, Tag, &Value);
        if (!NT_SUCCESS(Status))
            return Status;

        Globals->Unknown[Index].BitField = Value;
    }

    Globals->UnknownCount++;
    return STATUS_SUCCESS;
}

/**
 * @brief Second pass: fills the preparsed data and the report ID table.
 */
static
NTSTATUS
HidParseBuildCollections(
    _In_reads_bytes_(Length) PUCHAR Descriptor,
    _In_ ULONG Length,
    _In_ POOL_TYPE PoolType,
    _In_ PHIDPARSE_COLLECTION CollectionList,
    _Inout_ PHIDP_DEVICE_DESC DeviceDesc,
    _In_ BOOLEAN Strict)
{
    HIDPARSE_BUILDER State;
    PHIDPARSE_BUILDER Builder = &State;
    PHIDPARSE_GLOBALS Globals;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Value;
    UCHAR Tag;

    RtlZeroMemory(Builder, sizeof(*Builder));
    Builder->Descriptor = Descriptor;
    Builder->Length = Length;
    Builder->PoolType = PoolType;
    Builder->Strict = Strict;
    Builder->Dbg = &DeviceDesc->Dbg;
    Builder->Globals = &Builder->BaseGlobals;
    Builder->Usages = &Builder->BaseUsage;
    Builder->CollectionList = CollectionList;
    Builder->FirstReportId = DeviceDesc->ReportIDs;
    Builder->NextReportId = DeviceDesc->ReportIDs;
    Builder->ReportIdEnd = DeviceDesc->ReportIDs + DeviceDesc->ReportIDsLength;
    Builder->AwaitingReportId = TRUE;
    Builder->FirstDelimiterUsage = TRUE;

    while (Builder->Offset < Length)
    {
        Tag = Descriptor[Builder->Offset++];
        Globals = Builder->Globals;

        switch (Tag)
        {
            case ITEM_USAGE_PAGE_1:
            case ITEM_USAGE_PAGE_2:
                Status = HidParseReadData(Builder, Tag, &Value);
                Globals->UsagePage = (USAGE)Value;
                break;

            case ITEM_LOGICAL_MIN_1:
            case ITEM_LOGICAL_MIN_2:
            case ITEM_LOGICAL_MIN_4:
                Status = HidParseReadData(Builder, Tag, &Value);
                Globals->LogicalMin = HidParseSignExtend(Tag, Value);
                break;

            case ITEM_LOGICAL_MAX_1:
            case ITEM_LOGICAL_MAX_2:
            case ITEM_LOGICAL_MAX_4:
                Status = HidParseReadData(Builder, Tag, &Value);
                Globals->LogicalMax = HidParseSignExtend(Tag, Value);
                break;

            case ITEM_PHYSICAL_MIN_1:
            case ITEM_PHYSICAL_MIN_2:
            case ITEM_PHYSICAL_MIN_4:
                Status = HidParseReadData(Builder, Tag, &Value);
                Globals->PhysicalMin = HidParseSignExtend(Tag, Value);
                break;

            case ITEM_PHYSICAL_MAX_1:
            case ITEM_PHYSICAL_MAX_2:
            case ITEM_PHYSICAL_MAX_4:
                Status = HidParseReadData(Builder, Tag, &Value);
                Globals->PhysicalMax = HidParseSignExtend(Tag, Value);
                break;

            case ITEM_UNIT_EXPONENT_1:
            case ITEM_UNIT_EXPONENT_2:
            case ITEM_UNIT_EXPONENT_4:
                Status = HidParseReadData(Builder, Tag, &Value);
                Globals->UnitExponent = HidParseSignExtend(Tag, Value);
                break;

            case ITEM_UNIT_1:
            case ITEM_UNIT_2:
            case ITEM_UNIT_4:
                Status = HidParseReadData(Builder, Tag, &Value);
                Globals->Unit = Value;
                break;

            case ITEM_REPORT_SIZE:
                Status = HidParseReadData(Builder, Tag, &Value);
                Globals->ReportSize = (USHORT)Value;
                break;

            case ITEM_REPORT_COUNT_1:
            case ITEM_REPORT_COUNT_2:
                Status = HidParseReadData(Builder, Tag, &Value);
                Globals->ReportCount = (USHORT)Value;
                break;

            case ITEM_REPORT_ID:
                Status = HidParseReportIdItem(Builder);
                break;

            case ITEM_PUSH:
            {
                Globals = ExAllocatePoolWithTag(PoolType, sizeof(*Globals), HIDPARSE_POOL_TAG);
                if (!Globals)
                {
                    Status = HidParseFail(Builder->Dbg, STATUS_INSUFFICIENT_RESOURCES,
                                          Builder->Offset, HIDP_GETCOLDESC_PUSH_RESOURCES);
                    break;
                }
                *Globals = *Builder->Globals;
                Globals->Previous = Builder->Globals;
                Builder->Globals = Globals;
                break;
            }

            case ITEM_POP:
            {
                if (Globals == &Builder->BaseGlobals)
                {
                    Builder->Dbg->Args[0] = Tag;
                    Status = HidParseFail(Builder->Dbg, STATUS_COULD_NOT_INTERPRET,
                                          Builder->Offset, HIDP_GETCOLDESC_ITEM_UNKNOWN);
                    break;
                }
                Builder->Globals = Globals->Previous;
                ExFreePoolWithTag(Globals, HIDPARSE_POOL_TAG);
                break;
            }

            case ITEM_USAGE_1:
            case ITEM_USAGE_2:
            case ITEM_USAGE_4:
            case ITEM_USAGE_MIN_1:
            case ITEM_USAGE_MIN_2:
            case ITEM_USAGE_MIN_4:
            case ITEM_USAGE_MAX_1:
            case ITEM_USAGE_MAX_2:
            case ITEM_USAGE_MAX_4:
                Status = HidParseUsageItem(Builder, Tag);
                break;

            case ITEM_DESIGNATOR_INDEX:
                Builder->DesignatorIsRange = FALSE;
                Status = HidParseReadData(Builder, Tag, &Value);
                Builder->DesignatorIndex = (USHORT)Value;
                break;

            case ITEM_DESIGNATOR_MIN:
                Builder->DesignatorIsRange = TRUE;
                Status = HidParseReadData(Builder, Tag, &Value);
                Builder->DesignatorMin = (USHORT)Value;
                break;

            case ITEM_DESIGNATOR_MAX:
                Builder->DesignatorIsRange = TRUE;
                Status = HidParseReadData(Builder, Tag, &Value);
                Builder->DesignatorMax = (USHORT)Value;
                break;

            case ITEM_STRING_INDEX:
                Builder->StringIsRange = FALSE;
                Status = HidParseReadData(Builder, Tag, &Value);
                Builder->StringIndex = (USHORT)Value;
                break;

            case ITEM_STRING_MIN:
                Builder->StringIsRange = TRUE;
                Status = HidParseReadData(Builder, Tag, &Value);
                Builder->StringMin = (USHORT)Value;
                break;

            case ITEM_STRING_MAX:
                Builder->StringIsRange = TRUE;
                Status = HidParseReadData(Builder, Tag, &Value);
                Builder->StringMax = (USHORT)Value;
                break;

            case ITEM_DELIMITER:
            {
                Status = HidParseReadData(Builder, Tag, &Value);
                if (!NT_SUCCESS(Status))
                    break;

                if (Value == DELIMITER_OPEN)
                {
                    Builder->InDelimiter = TRUE;
                    Builder->FirstDelimiterUsage = TRUE;
                }
                else if (Value == DELIMITER_CLOSE)
                {
                    Builder->InDelimiter = FALSE;
                }
                break;
            }

            case ITEM_COLLECTION:
                Status = HidParseOpenCollection(Builder);
                break;

            case ITEM_END_COLLECTION:
                Status = HidParseCloseCollection(Builder);
                break;

            case ITEM_INPUT_1:
            case ITEM_INPUT_2:
            case ITEM_OUTPUT_1:
            case ITEM_OUTPUT_2:
            case ITEM_FEATURE_1:
            case ITEM_FEATURE_2:
                Status = HidParseMainItem(Builder, Tag);
                break;

            default:
            {
                /* Unknown global items are kept as extended attributes */
                if ((Tag & ITEM_TYPE_MASK) == ITEM_TYPE_GLOBAL)
                {
                    Status = HidParseUnknownGlobal(Builder, Tag);
                    break;
                }

                Builder->Dbg->Args[0] = Tag;
                Status = HidParseFail(Builder->Dbg, STATUS_ILLEGAL_INSTRUCTION,
                                      Builder->Offset, HIDP_GETCOLDESC_ITEM_UNKNOWN);
                break;
            }
        }

        if (!NT_SUCCESS(Status))
            break;
    }

    if (NT_SUCCESS(Status))
        DeviceDesc->ReportIDsLength = (ULONG)(Builder->NextReportId - Builder->FirstReportId);

    while (Builder->Globals != &Builder->BaseGlobals)
    {
        Globals = Builder->Globals;
        Builder->Globals = Globals->Previous;
        ExFreePoolWithTag(Globals, HIDPARSE_POOL_TAG);
    }
    HidParseClearUsages(Builder);

    return Status;
}

/**
 * @brief Numbers the data indices of every report type, array chains are
 * numbered from their last cap backwards.
 */
static
VOID
HidParseAssignDataIndices(
    _Inout_ PHIDP_PREPARSED_DATA Ppd)
{
    PHIDPARSE_REPORT_INFO Info;
    PHIDPARSE_CAP Cap;
    PHIDPARSE_CAP Last;
    USHORT DataIndex;
    ULONG Type;
    ULONG Index;

    for (Type = HidP_Input; Type <= HidP_Feature; Type++)
    {
        Info = &Ppd->Report[Type];
        DataIndex = 0;

        for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
        {
            Cap = &Ppd->Caps[Index];
            Last = HidParseChainEnd(Cap);
            Index += (ULONG)(Last - Cap);

            for (;;)
            {
                Last->DataIndexMin = DataIndex;
                Last->DataIndexMax = DataIndex + (Last->UsageMax - Last->UsageMin);
                DataIndex = Last->DataIndexMax + 1;

                if (Last == Cap)
                    break;
                Last--;
            }
        }
    }
}

static
VOID
HidParseFreeCollections(
    _In_opt_ PHIDPARSE_COLLECTION Collection)
{
    PHIDPARSE_COLLECTION Next;

    while (Collection)
    {
        Next = Collection->Next;
        if (Collection->Desc.PreparsedData)
            ExFreePoolWithTag(Collection->Desc.PreparsedData, HIDPARSE_POOL_TAG);
        ExFreePoolWithTag(Collection, HIDPARSE_POOL_TAG);
        Collection = Next;
    }
}

static
NTSTATUS
HidParseGetDescription(
    _In_reads_bytes_(DescLength) PHIDP_REPORT_DESCRIPTOR ReportDesc,
    _In_ ULONG DescLength,
    _In_ POOL_TYPE PoolType,
    _Out_ PHIDP_DEVICE_DESC DeviceDescription,
    _In_ BOOLEAN Strict)
{
    PHIDPARSE_COLLECTION CollectionList = NULL;
    PHIDPARSE_COLLECTION Collection;
    PHIDP_COLLECTION_DESC CollectionDesc;
    ULONG CollectionCount;
    ULONG Index;
    NTSTATUS Status;

    RtlZeroMemory(DeviceDescription, sizeof(*DeviceDescription));

    Status = HidParseCountItems(ReportDesc,
                                DescLength,
                                PoolType,
                                &CollectionList,
                                &CollectionCount,
                                DeviceDescription);
    if (CollectionCount == 0)
    {
        Status = STATUS_NO_DATA_DETECTED;
        goto Cleanup;
    }
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    Status = HidParseBuildCollections(ReportDesc,
                                      DescLength,
                                      PoolType,
                                      CollectionList,
                                      DeviceDescription,
                                      Strict);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    CollectionDesc = ExAllocatePoolWithTag(PoolType,
                                           sizeof(*CollectionDesc) * CollectionCount,
                                           HIDPARSE_POOL_TAG);
    if (!CollectionDesc)
    {
        DeviceDescription->Dbg.BreakOffset = DescLength;
        DeviceDescription->Dbg.ErrorCode = HIDP_GETCOLDESC_RESOURCES;
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    RtlZeroMemory(CollectionDesc, sizeof(*CollectionDesc) * CollectionCount);

    DeviceDescription->CollectionDesc = CollectionDesc;
    DeviceDescription->CollectionDescLength = CollectionCount;

    for (Index = 0; CollectionList; Index++)
    {
        Collection = CollectionList;
        CollectionList = Collection->Next;

        CollectionDesc[Index] = Collection->Desc;
        HidParseAssignDataIndices(Collection->Desc.PreparsedData);
        ExFreePoolWithTag(Collection, HIDPARSE_POOL_TAG);
    }

    return STATUS_SUCCESS;

Cleanup:
    HidParseFreeCollections(CollectionList);

    if (DeviceDescription->ReportIDs)
    {
        ExFreePoolWithTag(DeviceDescription->ReportIDs, HIDPARSE_POOL_TAG);
        DeviceDescription->ReportIDs = NULL;
        DeviceDescription->ReportIDsLength = 0;
    }

    return Status;
}

NTSTATUS
NTAPI
HidP_GetCollectionDescription(
    _In_reads_bytes_(DescLength) PHIDP_REPORT_DESCRIPTOR ReportDesc,
    _In_ ULONG DescLength,
    _In_ POOL_TYPE PoolType,
    _Out_ PHIDP_DEVICE_DESC DeviceDescription)
{
    return HidParseGetDescription(ReportDesc, DescLength, PoolType, DeviceDescription, FALSE);
}

NTSTATUS
NTAPI
HidP_GetCollectionDescription2(
    _In_reads_bytes_(DescLength) PHIDP_REPORT_DESCRIPTOR ReportDesc,
    _In_ ULONG DescLength,
    _In_ POOL_TYPE PoolType,
    _Out_ PHIDP_DEVICE_DESC DeviceDescription)
{
    return HidParseGetDescription(ReportDesc, DescLength, PoolType, DeviceDescription, TRUE);
}

VOID
NTAPI
HidP_FreeCollectionDescription(
    _In_ PHIDP_DEVICE_DESC DeviceDescription)
{
    ULONG Index;

    if (DeviceDescription->CollectionDesc)
    {
        for (Index = 0; Index < DeviceDescription->CollectionDescLength; Index++)
        {
            if (DeviceDescription->CollectionDesc[Index].PreparsedData)
            {
                ExFreePoolWithTag(DeviceDescription->CollectionDesc[Index].PreparsedData,
                                  HIDPARSE_POOL_TAG);
            }
        }
        ExFreePoolWithTag(DeviceDescription->CollectionDesc, HIDPARSE_POOL_TAG);
    }

    if (DeviceDescription->ReportIDs)
        ExFreePoolWithTag(DeviceDescription->ReportIDs, HIDPARSE_POOL_TAG);
}
