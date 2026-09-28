/*
 * PROJECT:     ReactOS Runtime Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Bitmaps indexed in 64 bits
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <rtl.h>

#define NDEBUG
#include <debug.h>

/*
 * RTL_BITMAP_EX has the layout of the 64 bit engine in bitmap64.c, so the
 * searching and range routines are shared with it. The whole map routines and
 * the interlocked runs are not: they work in 32 bit units, which is how much
 * of the buffer a caller sized for this interface is known to own.
 */
C_ASSERT(sizeof(RTL_BITMAP_EX) == sizeof(RTL_BITMAP64));
C_ASSERT(FIELD_OFFSET(RTL_BITMAP_EX, Buffer) == FIELD_OFFSET(RTL_BITMAP64, Buffer));

#define AS_BITMAP64(BitMapHeader) ((PRTL_BITMAP64)(BitMapHeader))

static
SIZE_T
RtlpBitMapExBytesInUse(
    _In_ PRTL_BITMAP_EX BitMapHeader)
{
    return (SIZE_T)((BitMapHeader->SizeOfBitMap + 31) / 32) * sizeof(ULONG);
}

/* The low Count bits set, for a run that ends inside one 32 bit word. */
static
ULONG
RtlpBitRunMask(
    _In_ ULONG64 Count)
{
    if (Count >= 32)
        return MAXULONG;

    return (1UL << Count) - 1;
}

VOID
NTAPI
RtlInitializeBitMapEx(
    _Out_ PRTL_BITMAP_EX BitMapHeader,
    _In_opt_ PULONG64 BitMapBuffer,
    _In_opt_ ULONG64 SizeOfBitMap)
{
    BitMapHeader->SizeOfBitMap = SizeOfBitMap;
    BitMapHeader->Buffer = BitMapBuffer;
}

VOID
NTAPI
RtlClearAllBitsEx(
    _In_ PRTL_BITMAP_EX BitMapHeader)
{
    RtlZeroMemory(BitMapHeader->Buffer, RtlpBitMapExBytesInUse(BitMapHeader));
}

VOID
NTAPI
RtlSetAllBitsEx(
    _In_ PRTL_BITMAP_EX BitMapHeader)
{
    RtlFillMemory(BitMapHeader->Buffer, RtlpBitMapExBytesInUse(BitMapHeader), 0xFF);
}

VOID
NTAPI
RtlClearBitEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 BitNumber)
{
    ASSERT(BitNumber < BitMapHeader->SizeOfBitMap);

    _bittestandreset64((LONG64 *)BitMapHeader->Buffer, (LONG64)BitNumber);
}

VOID
NTAPI
RtlSetBitEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 BitNumber)
{
    ASSERT(BitNumber < BitMapHeader->SizeOfBitMap);

    _bittestandset64((LONG64 *)BitMapHeader->Buffer, (LONG64)BitNumber);
}

VOID
NTAPI
RtlClearBitsEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 StartingIndex,
    _In_ ULONG64 NumberToClear)
{
    RtlClearBits64(AS_BITMAP64(BitMapHeader), StartingIndex, NumberToClear);
}

VOID
NTAPI
RtlSetBitsEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 StartingIndex,
    _In_ ULONG64 NumberToSet)
{
    RtlSetBits64(AS_BITMAP64(BitMapHeader), StartingIndex, NumberToSet);
}

ULONG64
NTAPI
RtlFindSetBitsEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 NumberToFind,
    _In_ ULONG64 HintIndex)
{
    return RtlFindSetBits64(AS_BITMAP64(BitMapHeader), NumberToFind, HintIndex);
}

ULONG64
NTAPI
RtlFindSetBitsAndClearEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 NumberToFind,
    _In_ ULONG64 HintIndex)
{
    return RtlFindSetBitsAndClear64(AS_BITMAP64(BitMapHeader), NumberToFind, HintIndex);
}

ULONG64
NTAPI
RtlNumberOfClearBitsEx(
    _In_ PRTL_BITMAP_EX BitMapHeader)
{
    return RtlNumberOfClearBits64(AS_BITMAP64(BitMapHeader));
}

ULONG64
NTAPI
RtlFindClearBitsAndSetEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 NumberToFind,
    _In_ ULONG64 HintIndex)
{
    return RtlFindClearBitsAndSet64(AS_BITMAP64(BitMapHeader), NumberToFind, HintIndex);
}

ULONG64
NTAPI
RtlNumberOfSetBitsEx(
    _In_ PRTL_BITMAP_EX BitMapHeader)
{
    return RtlNumberOfSetBits64(AS_BITMAP64(BitMapHeader));
}

/*
 * Only the words at either end of the run can be shared with bits outside
 * it, so only those need an interlocked update. The words in between belong
 * to the run alone.
 */
VOID
NTAPI
RtlInterlockedSetBitRunEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 StartingIndex,
    _In_ ULONG64 NumberToSet)
{
    volatile LONG *Word = (volatile LONG *)BitMapHeader->Buffer + (StartingIndex / 32);
    ULONG Shift = (ULONG)(StartingIndex % 32);

    if (Shift + NumberToSet <= 32)
    {
        InterlockedOr(Word, (LONG)(RtlpBitRunMask(NumberToSet) << Shift));
        return;
    }

    if (Shift != 0)
    {
        InterlockedOr(Word, (LONG)(MAXULONG << Shift));
        NumberToSet -= 32 - Shift;
        Word++;
    }

    for (; NumberToSet >= 32; NumberToSet -= 32)
        *Word++ = (LONG)MAXULONG;

    if (NumberToSet != 0)
        InterlockedOr(Word, (LONG)RtlpBitRunMask(NumberToSet));
}

VOID
NTAPI
RtlInterlockedClearBitRunEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 StartingIndex,
    _In_ ULONG64 NumberToClear)
{
    volatile LONG *Word = (volatile LONG *)BitMapHeader->Buffer + (StartingIndex / 32);
    ULONG Shift = (ULONG)(StartingIndex % 32);

    if (Shift + NumberToClear <= 32)
    {
        InterlockedAnd(Word, (LONG)~(RtlpBitRunMask(NumberToClear) << Shift));
        return;
    }

    if (Shift != 0)
    {
        InterlockedAnd(Word, (LONG)~(MAXULONG << Shift));
        NumberToClear -= 32 - Shift;
        Word++;
    }

    for (; NumberToClear >= 32; NumberToClear -= 32)
        *Word++ = 0;

    if (NumberToClear != 0)
        InterlockedAnd(Word, (LONG)~RtlpBitRunMask(NumberToClear));
}

BOOLEAN
NTAPI
RtlTestBitEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 BitNumber)
{
    return RtlTestBit64(AS_BITMAP64(BitMapHeader), BitNumber);
}

BOOLEAN
NTAPI
RtlAreBitsClearEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 StartingIndex,
    _In_ ULONG64 Length)
{
    return RtlAreBitsClear64(AS_BITMAP64(BitMapHeader), StartingIndex, Length);
}

BOOLEAN
NTAPI
RtlAreBitsSetEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 StartingIndex,
    _In_ ULONG64 Length)
{
    return RtlAreBitsSet64(AS_BITMAP64(BitMapHeader), StartingIndex, Length);
}

ULONG64
NTAPI
RtlFindClearBitsEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 NumberToFind,
    _In_ ULONG64 HintIndex)
{
    return RtlFindClearBits64(AS_BITMAP64(BitMapHeader), NumberToFind, HintIndex);
}

ULONG64
NTAPI
RtlFindNextForwardRunClearEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 FromIndex,
    _Out_ PULONG64 StartingRunIndex)
{
    return RtlFindNextForwardRunClear64(AS_BITMAP64(BitMapHeader),
                                        FromIndex,
                                        StartingRunIndex);
}

ULONG64
NTAPI
RtlFindNextForwardRunSetEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 FromIndex,
    _Out_ PULONG64 StartingRunIndex)
{
    return RtlFindNextForwardRunSet64(AS_BITMAP64(BitMapHeader),
                                      FromIndex,
                                      StartingRunIndex);
}

/**
 * @brief
 * Counts the bits that are set in part of a bitmap.
 *
 * @param[in] StartingIndex
 * First bit of the range.
 *
 * @param[in] Length
 * How many bits the range covers. A range running past the end of the map is
 * cut short at the end.
 *
 * @return
 * How many of those bits are set.
 */
ULONG64
NTAPI
RtlNumberOfSetBitsInRangeEx(
    _In_ PRTL_BITMAP_EX BitMapHeader,
    _In_ ULONG64 StartingIndex,
    _In_ ULONG64 Length)
{
    ULONG64 Count = 0;
    ULONG64 Index;

    if (StartingIndex >= BitMapHeader->SizeOfBitMap)
        return 0;

    if (Length > (BitMapHeader->SizeOfBitMap - StartingIndex))
        Length = BitMapHeader->SizeOfBitMap - StartingIndex;

    for (Index = 0; Index < Length; Index++)
    {
        if (RtlTestBit64(AS_BITMAP64(BitMapHeader), StartingIndex + Index))
            Count++;
    }

    return Count;
}

/**
 * @brief
 * Copies a whole bitmap into another one, starting at a given bit.
 *
 * @remarks
 * A bit at a time, which is as fast as this needs to be and leaves no doubt
 * about what happens at either end of the range.
 */
VOID
NTAPI
RtlCopyBitMapEx(
    _In_ PRTL_BITMAP_EX Source,
    _In_ PRTL_BITMAP_EX Destination,
    _In_ ULONG64 TargetBit)
{
    ULONG64 Length = Source->SizeOfBitMap;
    ULONG64 Index;

    if (TargetBit >= Destination->SizeOfBitMap)
        return;

    if (Length > (Destination->SizeOfBitMap - TargetBit))
        Length = Destination->SizeOfBitMap - TargetBit;

    for (Index = 0; Index < Length; Index++)
    {
        if (RtlTestBit64(AS_BITMAP64(Source), Index))
            RtlSetBitEx(Destination, TargetBit + Index);
        else
            RtlClearBitEx(Destination, TargetBit + Index);
    }
}

/**
 * @brief
 * Copies part of a bitmap into the beginning of another one.
 */
VOID
NTAPI
RtlExtractBitMapEx(
    _In_ PRTL_BITMAP_EX Source,
    _In_ PRTL_BITMAP_EX Destination,
    _In_ ULONG64 StartingIndex,
    _In_ ULONG64 NumberOfBits)
{
    ULONG64 Index;

    if (StartingIndex >= Source->SizeOfBitMap)
        return;

    if (NumberOfBits > (Source->SizeOfBitMap - StartingIndex))
        NumberOfBits = Source->SizeOfBitMap - StartingIndex;

    if (NumberOfBits > Destination->SizeOfBitMap)
        NumberOfBits = Destination->SizeOfBitMap;

    for (Index = 0; Index < NumberOfBits; Index++)
    {
        if (RtlTestBit64(AS_BITMAP64(Source), StartingIndex + Index))
            RtlSetBitEx(Destination, Index);
        else
            RtlClearBitEx(Destination, Index);
    }
}

/* EOF */
