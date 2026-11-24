/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Capability and collection queries
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "hidparser.h"

/* Size Windows reports for an extended attributes block without tokens */
#define HIDPARSE_EXT_ATTR_BASE_SIZE     12

NTSTATUS
NTAPI
HidP_GetCaps(
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _Out_ PHIDP_CAPS Capabilities)
{
    PHIDPARSE_REPORT_INFO Info;
    PHIDPARSE_CAP Cap;
    PUSHORT ButtonCaps;
    PUSHORT ValueCaps;
    PUSHORT DataIndices;
    ULONG Type;
    ULONG Index;

    if (!HidParseIsValid(PreparsedData))
        return HIDP_STATUS_INVALID_PREPARSED_DATA;

    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->UsagePage = PreparsedData->UsagePage;
    Capabilities->Usage = PreparsedData->Usage;
    Capabilities->InputReportByteLength = PreparsedData->Report[HidP_Input].ReportByteLength;
    Capabilities->OutputReportByteLength = PreparsedData->Report[HidP_Output].ReportByteLength;
    Capabilities->FeatureReportByteLength = PreparsedData->Report[HidP_Feature].ReportByteLength;
    Capabilities->NumberLinkCollectionNodes = PreparsedData->LinkNodeCount;

    for (Type = HidP_Input; Type <= HidP_Feature; Type++)
    {
        if (Type == HidP_Input)
        {
            ButtonCaps = &Capabilities->NumberInputButtonCaps;
            ValueCaps = &Capabilities->NumberInputValueCaps;
            DataIndices = &Capabilities->NumberInputDataIndices;
        }
        else if (Type == HidP_Output)
        {
            ButtonCaps = &Capabilities->NumberOutputButtonCaps;
            ValueCaps = &Capabilities->NumberOutputValueCaps;
            DataIndices = &Capabilities->NumberOutputDataIndices;
        }
        else
        {
            ButtonCaps = &Capabilities->NumberFeatureButtonCaps;
            ValueCaps = &Capabilities->NumberFeatureValueCaps;
            DataIndices = &Capabilities->NumberFeatureDataIndices;
        }

        Info = &PreparsedData->Report[Type];
        for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
        {
            Cap = &PreparsedData->Caps[Index];

            if (Cap->Flags & HIDPARSE_CAP_BUTTON)
                (*ButtonCaps)++;
            else
                (*ValueCaps)++;

            *DataIndices += Cap->DataIndexMax - Cap->DataIndexMin + 1;
        }
    }

    return HIDP_STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HidP_GetSpecificButtonCaps(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _In_ USAGE Usage,
    _Out_ PHIDP_BUTTON_CAPS ButtonCaps,
    _Inout_ PUSHORT ButtonCapsLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData)
{
    PHIDPARSE_REPORT_INFO Info;
    PHIDP_BUTTON_CAPS Out;
    PHIDPARSE_CAP Cap;
    NTSTATUS Status = HIDP_STATUS_USAGE_NOT_FOUND;
    USHORT Count = 0;
    ULONG Index;

    if (!HidParseIsValid(PreparsedData))
        return HIDP_STATUS_INVALID_PREPARSED_DATA;

    Info = HidParseGetReportInfo(PreparsedData, ReportType);
    if (!Info)
        return HIDP_STATUS_INVALID_REPORT_TYPE;

    for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
    {
        Cap = &PreparsedData->Caps[Index];

        if (!(Cap->Flags & HIDPARSE_CAP_BUTTON))
            continue;
        if (UsagePage && UsagePage != Cap->UsagePage)
            continue;
        if (!HidParseMatchLink(LinkCollection, Cap))
            continue;
        if (Usage && (Cap->UsageMin > Usage || Usage > Cap->UsageMax))
            continue;

        if (Count >= *ButtonCapsLength)
        {
            Status = HIDP_STATUS_BUFFER_TOO_SMALL;
            Count++;
            continue;
        }

        Status = HIDP_STATUS_SUCCESS;
        Out = &ButtonCaps[Count++];

        Out->UsagePage = Cap->UsagePage;
        Out->ReportID = Cap->ReportID;
        Out->IsAlias = (Cap->Flags & HIDPARSE_CAP_ALIAS) != 0;
        Out->BitField = (USHORT)Cap->BitField;
        Out->LinkCollection = Cap->LinkCollection;
        Out->LinkUsage = Cap->LinkUsage;
        Out->LinkUsagePage = Cap->LinkUsagePage;
        Out->IsRange = (Cap->Flags & HIDPARSE_CAP_RANGE) != 0;
        Out->IsStringRange = (Cap->Flags & HIDPARSE_CAP_STRING_RANGE) != 0;
        Out->IsDesignatorRange = (Cap->Flags & HIDPARSE_CAP_DESIGNATOR_RANGE) != 0;
        Out->IsAbsolute = (Cap->Flags & HIDPARSE_CAP_ABSOLUTE) != 0;
        Out->Range.UsageMin = Cap->UsageMin;
        Out->Range.UsageMax = Cap->UsageMax;
        Out->Range.StringMin = Cap->StringMin;
        Out->Range.StringMax = Cap->StringMax;
        Out->Range.DesignatorMin = Cap->DesignatorMin;
        Out->Range.DesignatorMax = Cap->DesignatorMax;
        Out->Range.DataIndexMin = Cap->DataIndexMin;
        Out->Range.DataIndexMax = Cap->DataIndexMax;

        /* The first reserved field carries the report count, ranges report one */
        Out->Reserved[0] = Out->IsRange ? 1 : Cap->ReportCount;
    }

    *ButtonCapsLength = Count;
    return Status;
}

NTSTATUS
NTAPI
HidP_GetButtonCaps(
    _In_ HIDP_REPORT_TYPE ReportType,
    _Out_ PHIDP_BUTTON_CAPS ButtonCaps,
    _Inout_ PUSHORT ButtonCapsLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData)
{
    return HidP_GetSpecificButtonCaps(ReportType,
                                      0,
                                      0,
                                      0,
                                      ButtonCaps,
                                      ButtonCapsLength,
                                      PreparsedData);
}

NTSTATUS
NTAPI
HidP_GetSpecificValueCaps(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ USHORT LinkCollection,
    _In_ USAGE Usage,
    _Out_ PHIDP_VALUE_CAPS ValueCaps,
    _Inout_ PUSHORT ValueCapsLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData)
{
    PHIDPARSE_REPORT_INFO Info;
    PHIDP_VALUE_CAPS Out;
    PHIDPARSE_CAP Cap;
    NTSTATUS Status = HIDP_STATUS_USAGE_NOT_FOUND;
    USHORT Count = 0;
    ULONG Index;

    if (!HidParseIsValid(PreparsedData))
        return HIDP_STATUS_INVALID_PREPARSED_DATA;

    Info = HidParseGetReportInfo(PreparsedData, ReportType);
    if (!Info)
        return HIDP_STATUS_INVALID_REPORT_TYPE;

    for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
    {
        Cap = &PreparsedData->Caps[Index];

        if (Cap->Flags & HIDPARSE_CAP_BUTTON)
            continue;
        if (UsagePage && UsagePage != Cap->UsagePage)
            continue;
        if (!HidParseMatchLink(LinkCollection, Cap))
            continue;
        if (Usage && (Cap->UsageMin > Usage || Usage > Cap->UsageMax))
            continue;

        if (Count >= *ValueCapsLength)
        {
            Status = HIDP_STATUS_BUFFER_TOO_SMALL;
            Count++;
            continue;
        }

        Status = HIDP_STATUS_SUCCESS;
        Out = &ValueCaps[Count++];

        Out->UsagePage = Cap->UsagePage;
        Out->ReportID = Cap->ReportID;
        Out->IsAlias = (Cap->Flags & HIDPARSE_CAP_ALIAS) != 0;
        Out->BitField = (USHORT)Cap->BitField;
        Out->LinkCollection = Cap->LinkCollection;
        Out->LinkUsage = Cap->LinkUsage;
        Out->LinkUsagePage = Cap->LinkUsagePage;
        Out->IsRange = (Cap->Flags & HIDPARSE_CAP_RANGE) != 0;
        Out->IsStringRange = (Cap->Flags & HIDPARSE_CAP_STRING_RANGE) != 0;
        Out->IsDesignatorRange = (Cap->Flags & HIDPARSE_CAP_DESIGNATOR_RANGE) != 0;
        Out->IsAbsolute = (Cap->Flags & HIDPARSE_CAP_ABSOLUTE) != 0;
        Out->HasNull = Cap->u.Value.HasNull;
        Out->BitSize = Cap->ReportSize;
        Out->ReportCount = Out->IsRange ? 1 : Cap->ReportCount;
        Out->UnitsExp = Cap->UnitsExp;
        Out->Units = Cap->Units;
        Out->LogicalMin = Cap->u.Value.LogicalMin;
        Out->LogicalMax = Cap->u.Value.LogicalMax;
        Out->PhysicalMin = Cap->u.Value.PhysicalMin;
        Out->PhysicalMax = Cap->u.Value.PhysicalMax;
        Out->Range.UsageMin = Cap->UsageMin;
        Out->Range.UsageMax = Cap->UsageMax;
        Out->Range.StringMin = Cap->StringMin;
        Out->Range.StringMax = Cap->StringMax;
        Out->Range.DesignatorMin = Cap->DesignatorMin;
        Out->Range.DesignatorMax = Cap->DesignatorMax;
        Out->Range.DataIndexMin = Cap->DataIndexMin;
        Out->Range.DataIndexMax = Cap->DataIndexMax;
    }

    *ValueCapsLength = Count;
    return Status;
}

NTSTATUS
NTAPI
HidP_GetValueCaps(
    _In_ HIDP_REPORT_TYPE ReportType,
    _Out_ PHIDP_VALUE_CAPS ValueCaps,
    _Inout_ PUSHORT ValueCapsLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData)
{
    return HidP_GetSpecificValueCaps(ReportType,
                                     0,
                                     0,
                                     0,
                                     ValueCaps,
                                     ValueCapsLength,
                                     PreparsedData);
}

NTSTATUS
NTAPI
HidP_GetLinkCollectionNodes(
    _Out_ PHIDP_LINK_COLLECTION_NODE LinkCollectionNodes,
    _Inout_ PULONG LinkCollectionNodesLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData)
{
    PHIDPARSE_LINK_NODE Node;
    PHIDP_LINK_COLLECTION_NODE Out;
    NTSTATUS Status;
    ULONG Available;
    ULONG Index;

    if (!HidParseIsValid(PreparsedData))
        return HIDP_STATUS_INVALID_PREPARSED_DATA;

    Available = *LinkCollectionNodesLength;
    if (Available >= PreparsedData->LinkNodeCount)
    {
        Status = HIDP_STATUS_SUCCESS;
        Available = PreparsedData->LinkNodeCount;
    }
    else
    {
        Status = HIDP_STATUS_BUFFER_TOO_SMALL;
    }
    *LinkCollectionNodesLength = PreparsedData->LinkNodeCount;

    /* Reserved bits and UserContext belong to the caller */
    Node = HidParseGetLinkNodes(PreparsedData);
    for (Index = 0; Index < Available; Index++)
    {
        Out = &LinkCollectionNodes[Index];
        Out->LinkUsage = Node[Index].LinkUsage;
        Out->LinkUsagePage = Node[Index].LinkUsagePage;
        Out->Parent = Node[Index].Parent;
        Out->NumberOfChildren = Node[Index].NumberOfChildren;
        Out->NextSibling = Node[Index].NextSibling;
        Out->FirstChild = Node[Index].FirstChild;
        Out->CollectionType = Node[Index].Flags & HIDPARSE_LINK_TYPE_MASK;
        Out->IsAlias = (Node[Index].Flags & HIDPARSE_LINK_ALIAS) != 0;
    }

    return Status;
}

NTSTATUS
NTAPI
HidP_GetExtendedAttributes(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USHORT DataIndex,
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _Out_writes_to_(*LengthAttributes, *LengthAttributes) PHIDP_EXTENDED_ATTRIBUTES Attributes,
    _Inout_ PULONG LengthAttributes)
{
    PHIDPARSE_REPORT_INFO Info;
    PHIDPARSE_CAP Cap;
    ULONG TokenCount;
    ULONG Required;
    ULONG CopyLength;
    ULONG Room;
    ULONG Index;

    if (!HidParseIsValid(PreparsedData))
        return HIDP_STATUS_INVALID_PREPARSED_DATA;

    Info = HidParseGetReportInfo(PreparsedData, ReportType);
    if (!Info)
        return HIDP_STATUS_INVALID_REPORT_TYPE;

    /* Windows walks as many caps as the report has, but always from the first cap */
    for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
    {
        Cap = &PreparsedData->Caps[Index - Info->FirstCap];
        if (DataIndex < Cap->DataIndexMin || DataIndex > Cap->DataIndexMax)
            continue;

        TokenCount = (Cap->Flags & HIDPARSE_CAP_UNKNOWN_MASK) >> HIDPARSE_CAP_UNKNOWN_SHIFT;
        Required = HIDPARSE_EXT_ATTR_BASE_SIZE + (ULONG)sizeof(HIDP_UNKNOWN_TOKEN) * TokenCount;

        /* GlobalUnknowns stays NULL, the tokens follow the header in Data */
        RtlZeroMemory(Attributes, *LengthAttributes);
        if (*LengthAttributes > 0)
            Attributes->NumGlobalUnknowns = (UCHAR)TokenCount;

        CopyLength = min(*LengthAttributes, Required);
        if (CopyLength > HIDPARSE_EXT_ATTR_BASE_SIZE &&
            *LengthAttributes > FIELD_OFFSET(HIDP_EXTENDED_ATTRIBUTES, Data))
        {
            CopyLength -= HIDPARSE_EXT_ATTR_BASE_SIZE;
            Room = *LengthAttributes - FIELD_OFFSET(HIDP_EXTENDED_ATTRIBUTES, Data);
            RtlCopyMemory(Attributes->Data, Cap->UnknownTokens, min(CopyLength, Room));
        }

        if (*LengthAttributes < Required)
            return HIDP_STATUS_BUFFER_TOO_SMALL;

        return HIDP_STATUS_SUCCESS;
    }

    return HIDP_STATUS_DATA_INDEX_NOT_FOUND;
}

ULONG
NTAPI
HidP_MaxDataListLength(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ PHIDP_PREPARSED_DATA PreparsedData)
{
    PHIDPARSE_REPORT_INFO Info;
    PHIDPARSE_CAP Cap;
    ULONG Length = 0;
    ULONG Index;

    /* Either half of the signature is enough here */
    if (!PreparsedData ||
        (PreparsedData->SignatureLow != HIDPARSE_SIGNATURE_LOW &&
         PreparsedData->SignatureHigh != HIDPARSE_SIGNATURE_HIGH))
    {
        return 0;
    }

    Info = HidParseGetReportInfo(PreparsedData, ReportType);
    if (!Info)
        return 0;

    for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
    {
        Cap = &PreparsedData->Caps[Index];

        if (Cap->Flags & (HIDPARSE_CAP_BUTTON | HIDPARSE_CAP_RANGE))
            Length += Cap->ReportCount;
        else
            Length++;
    }

    return Length;
}

ULONG
NTAPI
HidP_MaxUsageListLength(
    _In_ HIDP_REPORT_TYPE ReportType,
    _In_ USAGE UsagePage,
    _In_ PHIDP_PREPARSED_DATA PreparsedData)
{
    PHIDPARSE_REPORT_INFO Info;
    PHIDPARSE_CAP Cap;
    ULONG Length = 0;
    ULONG Index;

    if (!PreparsedData ||
        (PreparsedData->SignatureLow != HIDPARSE_SIGNATURE_LOW &&
         PreparsedData->SignatureHigh != HIDPARSE_SIGNATURE_HIGH))
    {
        return 0;
    }

    Info = HidParseGetReportInfo(PreparsedData, ReportType);
    if (!Info)
        return 0;

    for (Index = Info->FirstCap; Index < Info->EndCap; Index++)
    {
        Cap = &PreparsedData->Caps[Index];

        if ((Cap->Flags & HIDPARSE_CAP_BUTTON) && (!UsagePage || Cap->UsagePage == UsagePage))
            Length += Cap->ReportCount;
    }

    return Length;
}

NTSTATUS
NTAPI
HidP_SysPowerCaps(
    _In_ PHIDP_PREPARSED_DATA Ppd,
    _Out_ PULONG OutputBuffer)
{
    *OutputBuffer = Ppd->PowerButtonMask;
    return STATUS_SUCCESS;
}
