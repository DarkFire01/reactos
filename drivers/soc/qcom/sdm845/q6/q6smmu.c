/*
 * PROJECT:     ReactOS Qualcomm audio support (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Apps SMMU state for the stream the ADSP reads host audio buffers with
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The ADSP reaches host memory through the apps SMMU as the audio stream id.
 * With no stream entry its reads fault, so the stream gets a free entry routed
 * to a free context bank with translation off, which passes physical addresses.
 */

#include <ntddk.h>

#define QCDBG_COMPONENT "qcomq6"
#include <qcom/qcomdbg.h>

#include "q6smmu.h"

/* Global register space 0, stream matching and stream to context routing */
#define Q6S_IDR0                    0x020
#define Q6S_IDR1                    0x024
#define Q6S_SMR(n)                  (0x800 + (n) * 4)
#define Q6S_S2CR(n)                 (0xC00 + (n) * 4)

/* Global register space 1, one attribute word per context bank */
#define Q6S_CBAR(n)                 ((n) * 4)

/* Context bank space */
#define Q6S_CB_SCTLR                0x000

#define Q6S_IDR0_NUMSMRG(v)         ((v) & 0xFF)
#define Q6S_IDR0_SMS                (1UL << 27)
#define Q6S_IDR1_PAGESIZE_64K       (1UL << 31)
#define Q6S_IDR1_NUMPAGENDXB(v)     (((v) >> 28) & 0x7)
#define Q6S_IDR1_NUMCB(v)           ((v) & 0xFF)

#define Q6S_SMR_VALID               (1UL << 31)
#define Q6S_SMR_MASK(v)             (((v) >> 16) & 0x7FFF)
#define Q6S_SMR_ID(v)               ((v) & 0x7FFF)
#define Q6S_S2CR_TYPE(v)            (((v) >> 16) & 0x3)
#define Q6S_S2CR_CBNDX(v)           ((v) & 0xFF)
#define Q6S_CBAR_TYPE(v)            (((v) >> 16) & 0x3)
#define Q6S_CBAR_S1_ONLY            (1UL << 16)
#define Q6S_SCTLR_M                 (1UL << 0)

#define Q6S_MAX_CB                  128

static
PULONG
Q6sMap(
    _In_ ULONGLONG Address,
    _In_ SIZE_T Size)
{
    PHYSICAL_ADDRESS Physical;

    Physical.QuadPart = (LONGLONG)Address;
    return MmMapIoSpace(Physical, Size, MmNonCached);
}

/* Claims a free stream entry and context bank for StreamId, reading each register back */
static
BOOLEAN
Q6sRoute(
    _In_ PULONG Gr0,
    _In_ PULONG Gr1,
    _In_ PULONG Cb,
    _In_ ULONG Page,
    _In_ ULONG Entry,
    _In_ ULONG Bank,
    _In_ USHORT StreamId,
    _In_ USHORT Mask)
{
    ULONG Cbar, Sctlr, S2cr, Smr;
    ULONG Want = Q6S_SMR_VALID | ((ULONG)Mask << 16) | StreamId;

    WRITE_REGISTER_ULONG(&Gr1[Q6S_CBAR(Bank) / 4], Q6S_CBAR_S1_ONLY);
    WRITE_REGISTER_ULONG(&Cb[(Bank * Page + Q6S_CB_SCTLR) / 4], 0);
    WRITE_REGISTER_ULONG(&Gr0[Q6S_S2CR(Entry) / 4], Bank);
    WRITE_REGISTER_ULONG(&Gr0[Q6S_SMR(Entry) / 4], Want);

    Cbar = READ_REGISTER_ULONG(&Gr1[Q6S_CBAR(Bank) / 4]);
    Sctlr = READ_REGISTER_ULONG(&Cb[(Bank * Page + Q6S_CB_SCTLR) / 4]);
    S2cr = READ_REGISTER_ULONG(&Gr0[Q6S_S2CR(Entry) / 4]);
    Smr = READ_REGISTER_ULONG(&Gr0[Q6S_SMR(Entry) / 4]);

    QC_LOG("smmu: routed stream 0x%04x mask 0x%04x through entry %lu to bank %lu: cbar 0x%08lx sctlr 0x%08lx s2cr 0x%08lx smr 0x%08lx",
           StreamId, Mask, Entry, Bank, Cbar, Sctlr, S2cr, Smr);

    return Q6S_CBAR_TYPE(Cbar) == 1 && !(Sctlr & Q6S_SCTLR_M) &&
           Q6S_S2CR_TYPE(S2cr) == 0 && Q6S_S2CR_CBNDX(S2cr) == Bank &&
           Smr == Want;
}

BOOLEAN
NTAPI
QcQ6SmmuRouteStream(
    _In_ ULONGLONG Base,
    _In_ USHORT StreamId,
    _In_ USHORT Mask,
    _In_ BOOLEAN Route)
{
    PULONG Gr0, Gr1, Cb;
    ULONG Idr0, Idr1, Smrs, Banks, Page, CbBase, i;
    ULONG Smr, S2cr, Index;
    ULONG FreeEntry = MAXULONG, FreeBank = MAXULONG;
    BOOLEAN Used[Q6S_MAX_CB];
    BOOLEAN Matched = FALSE;
    BOOLEAN Routed = FALSE;
    BOOLEAN Hit;

    PAGED_CODE();

    Gr0 = Q6sMap(Base, PAGE_SIZE);
    if (!Gr0)
    {
        QC_LOG("smmu: cannot map 0x%I64x", Base);
        return FALSE;
    }

    Idr0 = READ_REGISTER_ULONG(&Gr0[Q6S_IDR0 / 4]);
    Idr1 = READ_REGISTER_ULONG(&Gr0[Q6S_IDR1 / 4]);
    Smrs = Q6S_IDR0_NUMSMRG(Idr0);
    Banks = min(Q6S_IDR1_NUMCB(Idr1), Q6S_MAX_CB);
    Page = (Idr1 & Q6S_IDR1_PAGESIZE_64K) ? 0x10000 : 0x1000;
    CbBase = (1UL << (Q6S_IDR1_NUMPAGENDXB(Idr1) + 1)) * Page;

    QC_LOG("smmu 0x%I64x: idr0 0x%08lx idr1 0x%08lx, %lu stream entries%s, %lu context banks, %lu byte pages, banks at +0x%lx",
           Base, Idr0, Idr1, Smrs, (Idr0 & Q6S_IDR0_SMS) ? "" : " (no matching)", Banks, Page, CbBase);

    /* Stream entries sit below the 4K of the first page whatever the page size */
    if (Smrs > 128)
        Smrs = 128;

    Gr1 = Q6sMap(Base + Page, PAGE_SIZE);
    Cb = Q6sMap(Base + CbBase, (SIZE_T)Banks * Page);
    RtlZeroMemory(Used, sizeof(Used));

    for (i = 0; i < Smrs; i++)
    {
        Smr = READ_REGISTER_ULONG(&Gr0[Q6S_SMR(i) / 4]);
        if (!(Smr & Q6S_SMR_VALID))
        {
            FreeEntry = i;
            continue;
        }

        S2cr = READ_REGISTER_ULONG(&Gr0[Q6S_S2CR(i) / 4]);
        Index = Q6S_S2CR_CBNDX(S2cr);
        if (Q6S_S2CR_TYPE(S2cr) == 0 && Index < Banks)
            Used[Index] = TRUE;

        Hit = ((Q6S_SMR_ID(Smr) ^ StreamId) & ~Q6S_SMR_MASK(Smr) & 0x7FFF) == 0;
        if (Hit)
            Matched = TRUE;

        QC_LOG("smmu: entry %lu id 0x%04lx mask 0x%04lx, s2cr type %lu bank %lu, cbar type %lu, sctlr 0x%08lx%s",
               i, Q6S_SMR_ID(Smr), Q6S_SMR_MASK(Smr), Q6S_S2CR_TYPE(S2cr), Index,
               (Gr1 && Index < Banks) ? Q6S_CBAR_TYPE(READ_REGISTER_ULONG(&Gr1[Q6S_CBAR(Index) / 4])) : 0,
               (Cb && Index < Banks) ? READ_REGISTER_ULONG(&Cb[(Index * Page + Q6S_CB_SCTLR) / 4]) : 0,
               Hit ? " <- audio stream" : "");
    }

    /* The highest bank nothing routes to and that is not translating */
    for (i = Banks; Cb && i-- > 0; )
    {
        if (!Used[i] && !(READ_REGISTER_ULONG(&Cb[(i * Page + Q6S_CB_SCTLR) / 4]) & Q6S_SCTLR_M))
        {
            FreeBank = i;
            break;
        }
    }

    QC_LOG("smmu: stream 0x%04x %s, free entry %ld, free bank %ld", StreamId,
           Matched ? "is matched by an entry above" : "has no stream entry",
           (LONG)FreeEntry, (LONG)FreeBank);

    if (Matched)
    {
        /* Someone else routes it already; trust that route rather than add a second one */
        Routed = TRUE;
    }
    else if (!Route)
    {
        QC_LOG("smmu: routing stream 0x%04x is turned off", StreamId);
    }
    else if (FreeEntry == MAXULONG || FreeBank == MAXULONG || !Gr1 || !Cb)
    {
        QC_LOG("smmu: no free stream entry or context bank for stream 0x%04x", StreamId);
    }
    else
    {
        Routed = Q6sRoute(Gr0, Gr1, Cb, Page, FreeEntry, FreeBank, StreamId, Mask);
        QC_LOG("smmu: stream 0x%04x route %s", StreamId, Routed ? "in place" : "did not take, the registers read back different");
    }

    if (Cb)
        MmUnmapIoSpace(Cb, (SIZE_T)Banks * Page);
    if (Gr1)
        MmUnmapIoSpace(Gr1, PAGE_SIZE);
    MmUnmapIoSpace(Gr0, PAGE_SIZE);
    return Routed;
}
