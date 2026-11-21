/*
 * PROJECT:     ReactOS Qualcomm audio support (SDM845)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Apps SMMU state for the stream the ADSP reads host audio buffers with
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The ADSP reaches host memory through the apps SMMU as the audio stream id.
 * This only reads the SMMU so the stream entry and context bank to claim can
 * be picked against what the platform SMMU driver already uses.
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

VOID
NTAPI
QcQ6SmmuSurvey(
    _In_ ULONGLONG Base,
    _In_ USHORT StreamId)
{
    PULONG Gr0, Gr1, Cb;
    ULONG Idr0, Idr1, Smrs, Banks, Page, CbBase, i;
    ULONG Smr, S2cr, Index;
    BOOLEAN Used[Q6S_MAX_CB];
    BOOLEAN Matched = FALSE;
    BOOLEAN Hit;

    PAGED_CODE();

    Gr0 = Q6sMap(Base, PAGE_SIZE);
    if (!Gr0)
    {
        QC_LOG("smmu: cannot map 0x%I64x", Base);
        return;
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
            continue;

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

    for (i = 0; i < Banks; i++)
    {
        if (!Used[i])
            QC_LOG("smmu: context bank %lu has no stream routed to it", i);
    }

    QC_LOG("smmu: audio stream 0x%04x %s", StreamId,
           Matched ? "is matched by an entry above" : "has no stream entry, ADSP buffer reads fault");

    if (Cb)
        MmUnmapIoSpace(Cb, (SIZE_T)Banks * Page);
    if (Gr1)
        MmUnmapIoSpace(Gr1, PAGE_SIZE);
    MmUnmapIoSpace(Gr0, PAGE_SIZE);
}
