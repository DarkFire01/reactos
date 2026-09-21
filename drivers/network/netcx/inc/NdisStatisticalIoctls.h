/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Output of the histogram statistics control code
 *
 * Only debugging tools read this, and no reference has the shipping layout,
 * so the field order here is local. Every histogram in one reply has the same
 * bucket count, which is what makes the fixed entry stride work.
 */

#pragma once

#include <ntddk.h>

#define NDIS_HISTOGRAM_NAME_LENGTH  128

enum class NDIS_HISTOGRAM_AUXILIARY_DATATYPE : ULONG
{
    None = 0,
};

typedef struct _NDIS_SINGLE_HISTOGRAM_ENTRY
{
    WCHAR Name[NDIS_HISTOGRAM_NAME_LENGTH];
    ULONG64 TimestampUs;
    NDIS_HISTOGRAM_AUXILIARY_DATATYPE AuxiliaryDataType;
    USHORT Buckets[ANYSIZE_ARRAY];
} NDIS_SINGLE_HISTOGRAM_ENTRY;

typedef struct _NDIS_COLLECT_HISTOGRAM_OUT
{
    WCHAR Name[NDIS_HISTOGRAM_NAME_LENGTH];
    ULONG NumHistograms;
    ULONG HistogramEntryStride;
    ULONG NumBucketsPerHistogram;
    ULONG64 BucketWidth;
    ULONG64 SmallestBucketValue;

    /* NumHistograms entries, HistogramEntryStride bytes apart. */
    UCHAR Histograms[ANYSIZE_ARRAY];
} NDIS_COLLECT_HISTOGRAM_OUT;
