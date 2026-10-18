/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Extended processor state in thread contexts
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "k32_vista.h"

/* Laid out like the CONTEXT_EX that follows a context carrying CONTEXT_XSTATE */
typedef struct _BASEP_CONTEXT_CHUNK
{
    LONG Offset;
    ULONG Length;
} BASEP_CONTEXT_CHUNK;

typedef struct _BASEP_CONTEXT_EX
{
    BASEP_CONTEXT_CHUNK All;
    BASEP_CONTEXT_CHUNK Legacy;
    BASEP_CONTEXT_CHUNK XState;
} BASEP_CONTEXT_EX, *PBASEP_CONTEXT_EX;

#if defined(_M_AMD64)
#define BASEP_CONTEXT_ARCH          CONTEXT_AMD64
#define BASEP_CONTEXT_LEGACY_STATE  CONTEXT_FLOATING_POINT
#define BASEP_LEGACY_SSE_LENGTH     (16 * sizeof(M128A))
#define BasepLegacyArea(Context)    ((PUCHAR)&(Context)->FltSave)
#elif defined(_M_IX86)
#define BASEP_CONTEXT_ARCH          CONTEXT_i386
#define BASEP_CONTEXT_LEGACY_STATE  CONTEXT_EXTENDED_REGISTERS
#define BASEP_LEGACY_SSE_LENGTH     (8 * sizeof(M128A))
#define BasepLegacyArea(Context)    ((PUCHAR)(Context)->ExtendedRegisters)
#endif

/* The x87 part of the legacy area, ahead of the XMM registers */
#define BASEP_LEGACY_X87_LENGTH     FIELD_OFFSET(XSAVE_FORMAT, XmmRegisters)

/* Alignment of the features marked in AlignedFeatures within compacted state */
#define BASEP_XSAVE_FEATURE_ALIGN   64

static
BOOLEAN
BasepHasContextFlags(
    _In_ PCONTEXT Context,
    _In_ DWORD Flags)
{
    return ((Context->ContextFlags & Flags) == Flags);
}

/**
 * @brief
 * Finds the XSAVE header of the extended state stored after a context.
 */
static
PXSAVE_AREA_HEADER
BasepLocateXStateHeader(
    _In_ PCONTEXT Context,
    _Out_opt_ PULONG ChunkLength)
{
    PBASEP_CONTEXT_EX ContextEx;

    if (!BasepHasContextFlags(Context, CONTEXT_XSTATE))
        return NULL;

    ContextEx = (PBASEP_CONTEXT_EX)(Context + 1);
    if (ContextEx->XState.Length < sizeof(XSAVE_AREA_HEADER))
        return NULL;

    if (ChunkLength)
        *ChunkLength = ContextEx->XState.Length;

    return (PXSAVE_AREA_HEADER)((PUCHAR)ContextEx + ContextEx->XState.Offset);
}

/**
 * @brief
 * Gets the offset of an extended feature from the start of the XSAVE header.
 */
static
ULONG
BasepGetXStateFeatureOffset(
    _In_ PXSAVE_AREA_HEADER Header,
    _In_ DWORD FeatureId)
{
    PXSTATE_CONFIGURATION Configuration = &SharedUserData->XState;
    ULONG Offset = sizeof(XSAVE_AREA_HEADER);
    DWORD Index;

    if (!(Header->CompactionMask & XSTATE_COMPACTION_ENABLE_MASK))
        return Configuration->Features[FeatureId].Offset - sizeof(XSAVE_FORMAT);

    /* Compacted state packs the features present one after the other */
    for (Index = XSTATE_LEGACY_SSE + 1; Index <= FeatureId; Index++)
    {
        if (!(Header->CompactionMask & (1ULL << Index)))
            continue;

        if (Configuration->AlignedFeatures & (1ULL << Index))
            Offset = (Offset + BASEP_XSAVE_FEATURE_ALIGN - 1) & ~(BASEP_XSAVE_FEATURE_ALIGN - 1);

        if (Index == FeatureId)
            break;

        Offset += Configuration->AllFeatures[Index];
    }

    return Offset;
}

/**
 * @brief
 * Gets the processor state features the system saves for threads.
 */
DWORD64
WINAPI
GetEnabledXStateFeatures(VOID)
{
    DWORD64 Features = SharedUserData->XState.EnabledFeatures;

    if (!Features)
        Features = XSTATE_MASK_LEGACY;

    return Features;
}

/**
 * @brief
 * Gets the processor state features a context holds.
 *
 * @return
 * FALSE with ERROR_NOT_SUPPORTED for a context of another architecture.
 */
BOOL
WINAPI
GetXStateFeaturesMask(
    _In_ PCONTEXT Context,
    _Out_ PDWORD64 FeatureMask)
{
    PXSAVE_AREA_HEADER Header;
    DWORD64 Mask = 0;

    if (!BasepHasContextFlags(Context, BASEP_CONTEXT_ARCH))
    {
        BaseSetLastNTError(STATUS_NOT_SUPPORTED);
        return FALSE;
    }

    if (BasepHasContextFlags(Context, BASEP_CONTEXT_LEGACY_STATE))
        Mask = XSTATE_MASK_LEGACY;

    Header = BasepLocateXStateHeader(Context, NULL);
    if (Header)
        Mask |= Header->Mask & ~XSTATE_MASK_LEGACY;

    *FeatureMask = Mask;
    return TRUE;
}

/**
 * @brief
 * Finds where a context stores one processor state feature.
 *
 * @param[out] Length
 * Optionally receives the size of the feature.
 *
 * @return
 * The feature, or NULL when the context does not hold it.
 */
PVOID
WINAPI
LocateXStateFeature(
    _In_ PCONTEXT Context,
    _In_ DWORD FeatureId,
    _Out_opt_ PDWORD Length)
{
    PXSTATE_CONFIGURATION Configuration = &SharedUserData->XState;
    PXSAVE_AREA_HEADER Header;
    ULONG ChunkLength;
    ULONG Offset;
    ULONG Size;

    if (!BasepHasContextFlags(Context, BASEP_CONTEXT_ARCH))
        return NULL;

    if (FeatureId == XSTATE_LEGACY_FLOATING_POINT || FeatureId == XSTATE_LEGACY_SSE)
    {
        if (!BasepHasContextFlags(Context, BASEP_CONTEXT_LEGACY_STATE))
            return NULL;

        if (FeatureId == XSTATE_LEGACY_FLOATING_POINT)
        {
            if (Length)
                *Length = BASEP_LEGACY_X87_LENGTH;
            return BasepLegacyArea(Context);
        }

        if (Length)
            *Length = BASEP_LEGACY_SSE_LENGTH;
        return BasepLegacyArea(Context) + BASEP_LEGACY_X87_LENGTH;
    }

    if (FeatureId >= MAXIMUM_XSTATE_FEATURES ||
        !(Configuration->EnabledFeatures & (1ULL << FeatureId)))
    {
        return NULL;
    }

    Header = BasepLocateXStateHeader(Context, &ChunkLength);
    if (!Header || !(Header->Mask & (1ULL << FeatureId)))
        return NULL;

    Offset = BasepGetXStateFeatureOffset(Header, FeatureId);
    Size = Configuration->Features[FeatureId].Size;
    if (Offset < sizeof(XSAVE_AREA_HEADER) || Offset + Size > ChunkLength)
        return NULL;

    if (Length)
        *Length = Size;
    return (PUCHAR)Header + Offset;
}

/* EOF */
