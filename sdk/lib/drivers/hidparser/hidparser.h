/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private definitions and preparsed data layout
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define _HIDPI_
#define _HIDPI_NO_FUNCTION_MACROS_
#include <ntddk.h>
#include <hidpddi.h>
#include <poclass.h>

#define HIDPARSE_POOL_TAG               'PdiH'

/* "HidP KDR" stored as two little endian ULONGs */
#define HIDPARSE_SIGNATURE_LOW          0x50646948
#define HIDPARSE_SIGNATURE_HIGH         0x52444B20

/* Flags of HIDPARSE_CAP */
#define HIDPARSE_CAP_ARRAY_CHAIN        0x00000001
#define HIDPARSE_CAP_CONSTANT           0x00000002
#define HIDPARSE_CAP_BUTTON             0x00000004
#define HIDPARSE_CAP_ABSOLUTE           0x00000008
#define HIDPARSE_CAP_RANGE              0x00000010
#define HIDPARSE_CAP_ALIAS              0x00000020
#define HIDPARSE_CAP_STRING_RANGE       0x00000040
#define HIDPARSE_CAP_DESIGNATOR_RANGE   0x00000080
#define HIDPARSE_CAP_UNKNOWN_SHIFT      28
#define HIDPARSE_CAP_UNKNOWN_MASK       0xF0000000

/* Flags of HIDPARSE_LINK_NODE */
#define HIDPARSE_LINK_TYPE_MASK         0x000000FF
#define HIDPARSE_LINK_ALIAS             0x00000100

#define HIDPARSE_MAX_UNKNOWN_TOKENS     4

/* Main item data bits used by the parser */
#define HIDPARSE_MAIN_CONSTANT          0x01
#define HIDPARSE_MAIN_VARIABLE          0x02
#define HIDPARSE_MAIN_RELATIVE          0x04
#define HIDPARSE_MAIN_NULL_STATE        0x40

/* Generic desktop system control usages reported through HidP_SysPowerCaps */
#define HIDPARSE_USAGE_SYSTEM_POWER     0x81
#define HIDPARSE_USAGE_SYSTEM_SLEEP     0x82
#define HIDPARSE_USAGE_SYSTEM_WAKE      0x83

#ifndef HIDP_STATUS_NOT_BUTTON_ARRAY
#define HIDP_STATUS_NOT_BUTTON_ARRAY    (HIDP_ERROR_CODES(0xC, 0x21))

typedef struct _HIDP_BUTTON_ARRAY_DATA
{
    USHORT ArrayIndex;
    BOOLEAN On;
} HIDP_BUTTON_ARRAY_DATA, *PHIDP_BUTTON_ARRAY_DATA;
#endif

/* Report length overflow, only reported by HidP_GetCollectionDescription2 */
#define HIDPARSE_GETCOLDESC_LENGTH_OVERFLOW 0x24

/** One report field as stored in the preparsed data. */
typedef struct _HIDPARSE_CAP
{
    USAGE UsagePage;
    UCHAR ReportID;
    UCHAR BitPosition;
    USHORT ReportSize;
    USHORT ReportCount;
    USHORT BytePosition;
    USHORT BitCount;
    ULONG BitField;
    USHORT NextBytePosition;
    USHORT LinkCollection;
    USAGE LinkUsagePage;
    USAGE LinkUsage;
    ULONG Flags;
    HIDP_UNKNOWN_TOKEN UnknownTokens[HIDPARSE_MAX_UNKNOWN_TOKENS];
    USAGE UsageMin;
    USAGE UsageMax;
    USHORT StringMin;
    USHORT StringMax;
    USHORT DesignatorMin;
    USHORT DesignatorMax;
    USHORT DataIndexMin;
    USHORT DataIndexMax;
    union
    {
        struct
        {
            LONG LogicalMin;
            LONG LogicalMax;
        } Button;
        struct
        {
            BOOLEAN HasNull;
            UCHAR Reserved[3];
            LONG LogicalMin;
            LONG LogicalMax;
            LONG PhysicalMin;
            LONG PhysicalMax;
        } Value;
    } u;
    ULONG Units;
    ULONG UnitsExp;
} HIDPARSE_CAP, *PHIDPARSE_CAP;

/** Link collection node as stored in the preparsed data. */
typedef struct _HIDPARSE_LINK_NODE
{
    USAGE LinkUsage;
    USAGE LinkUsagePage;
    USHORT Parent;
    USHORT NumberOfChildren;
    USHORT NextSibling;
    USHORT FirstChild;
    ULONG Flags;
} HIDPARSE_LINK_NODE, *PHIDPARSE_LINK_NODE;

/** Cap range of one report type. */
typedef struct _HIDPARSE_REPORT_INFO
{
    USHORT FirstCap;
    USHORT CapCount;
    USHORT EndCap;
    USHORT ReportByteLength;
} HIDPARSE_REPORT_INFO, *PHIDPARSE_REPORT_INFO;

/** Preparsed data blob, binary compatible with the Windows layout. */
struct _HIDP_PREPARSED_DATA
{
    ULONG SignatureLow;
    ULONG SignatureHigh;
    USAGE Usage;
    USAGE UsagePage;
    ULONG PowerButtonMask;
    HIDPARSE_REPORT_INFO Report[3];
    USHORT LinkNodeOffset;
    USHORT LinkNodeCount;
    HIDPARSE_CAP Caps[ANYSIZE_ARRAY];
};

C_ASSERT(sizeof(HIDPARSE_CAP) == 104);
C_ASSERT(sizeof(HIDPARSE_LINK_NODE) == 16);
C_ASSERT(FIELD_OFFSET(HIDPARSE_CAP, UnknownTokens) == 28);
C_ASSERT(FIELD_OFFSET(HIDPARSE_CAP, UsageMin) == 60);
C_ASSERT(FIELD_OFFSET(HIDPARSE_CAP, u) == 76);
C_ASSERT(FIELD_OFFSET(HIDPARSE_CAP, Units) == 96);
C_ASSERT(FIELD_OFFSET(struct _HIDP_PREPARSED_DATA, PowerButtonMask) == 12);
C_ASSERT(FIELD_OFFSET(struct _HIDP_PREPARSED_DATA, Report) == 16);
C_ASSERT(FIELD_OFFSET(struct _HIDP_PREPARSED_DATA, LinkNodeOffset) == 40);
C_ASSERT(FIELD_OFFSET(struct _HIDP_PREPARSED_DATA, Caps) == 44);

#define HIDPARSE_HEADER_SIZE            FIELD_OFFSET(struct _HIDP_PREPARSED_DATA, Caps)

FORCEINLINE
BOOLEAN
HidParseIsValid(
    _In_opt_ PHIDP_PREPARSED_DATA PreparsedData)
{
    return (PreparsedData &&
            PreparsedData->SignatureLow == HIDPARSE_SIGNATURE_LOW &&
            PreparsedData->SignatureHigh == HIDPARSE_SIGNATURE_HIGH);
}

FORCEINLINE
PHIDPARSE_REPORT_INFO
HidParseGetReportInfo(
    _In_ PHIDP_PREPARSED_DATA PreparsedData,
    _In_ HIDP_REPORT_TYPE ReportType)
{
    if ((ULONG)ReportType > HidP_Feature)
        return NULL;

    return &PreparsedData->Report[ReportType];
}

FORCEINLINE
PHIDPARSE_LINK_NODE
HidParseGetLinkNodes(
    _In_ PHIDP_PREPARSED_DATA PreparsedData)
{
    return (PHIDPARSE_LINK_NODE)((PUCHAR)PreparsedData->Caps + PreparsedData->LinkNodeOffset);
}

/** Link collection filter shared by all usage lookups. */
FORCEINLINE
BOOLEAN
HidParseMatchLink(
    _In_ USHORT LinkCollection,
    _In_ PHIDPARSE_CAP Cap)
{
    if (LinkCollection == HIDP_LINK_COLLECTION_UNSPECIFIED)
        return TRUE;

    if (LinkCollection == Cap->LinkCollection)
        return TRUE;

    return (LinkCollection == HIDP_LINK_COLLECTION_ROOT && Cap->LinkCollection == 0);
}

/** Last cap of an array chain that starts at Cap. */
FORCEINLINE
PHIDPARSE_CAP
HidParseChainEnd(
    _In_ PHIDPARSE_CAP Cap)
{
    while (Cap->Flags & HIDPARSE_CAP_ARRAY_CHAIN)
        Cap++;

    return Cap;
}

ULONG
NTAPI
HidParseReadBits(
    _In_ const UCHAR *Buffer,
    _In_ ULONG BitOffset,
    _In_ ULONG BitLength);

VOID
NTAPI
HidParseWriteBits(
    _Inout_ PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _In_ ULONG BitOffset,
    _In_ ULONG BitLength,
    _In_ ULONG Value);

NTSTATUS
NTAPI
HidParseDeleteArrayEntry(
    _In_ ULONG BitOffset,
    _In_ USHORT ReportSize,
    _In_ USHORT ReportCount,
    _In_ ULONG Value,
    _Inout_updates_bytes_(ReportLength) PUCHAR Report,
    _In_ ULONG ReportLength);

/* Exports that hidpi.h only provides as macros */
NTSTATUS
NTAPI
HidP_GetButtonCaps(
    _In_ HIDP_REPORT_TYPE ReportType,
    _Out_ PHIDP_BUTTON_CAPS ButtonCaps,
    _Inout_ PUSHORT ButtonCapsLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData);

NTSTATUS
NTAPI
HidP_GetValueCaps(
    _In_ HIDP_REPORT_TYPE ReportType,
    _Out_ PHIDP_VALUE_CAPS ValueCaps,
    _Inout_ PUSHORT ValueCapsLength,
    _In_ PHIDP_PREPARSED_DATA PreparsedData);

/* Exports not declared by the ReactOS headers */
NTSTATUS
NTAPI
HidP_GetCollectionDescription2(
    _In_reads_bytes_(DescLength) PHIDP_REPORT_DESCRIPTOR ReportDesc,
    _In_ ULONG DescLength,
    _In_ POOL_TYPE PoolType,
    _Out_ PHIDP_DEVICE_DESC DeviceDescription);

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
    _In_ ULONG ReportLength);

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
    _In_ ULONG ReportLength);
