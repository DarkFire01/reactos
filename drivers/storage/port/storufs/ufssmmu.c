/*
 * PROJECT:     ReactOS UFS Host Controller Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hand the controller's SMMU stream over from firmware
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "storufs.h"

#define NDEBUG
#include <debug.h>

/* ARM SMMUv2 register layout */
#define SMMU_IDR0               0x20
#define SMMU_IDR1               0x24
#define SMMU_SMR_BASE           0x800
#define SMMU_S2CR_BASE          0xC00
#define SMMU_CTX_SCTLR          0x00
#define SMMU_CTX_TTBR0_LO       0x20
#define SMMU_CTX_TTBR0_HI       0x24
#define SMMU_CTX_FSR            0x58

#define SMMU_SMR_GROUPS(Idr0)   ((Idr0) & 0xFF)
#define SMMU_CTX_COUNT(Idr1)    ((Idr1) & 0xFF)
#define SMMU_LARGE_PAGES        0x80000000
#define SMMU_PAGE_SHIFT(Idr1)   ((((Idr1) >> 28) & 7) + 1)
#define SMMU_SMR_ENABLED        0x80000000
#define SMMU_STREAM_BITS        0x7FFF
#define SMMU_S2CR_KIND(S2cr)    (((S2cr) >> 16) & 3)
#define SMMU_S2CR_CONTEXT(S2cr) ((S2cr) & 0xFF)
#define SMMU_S2CR_TRANSLATE     0
#define SMMU_SCTLR_ENABLE       0x1

static ULONG UfsSmmuPhysical;
static ULONG UfsSmmuStream;
static ULONG UfsSmmuStreamMask;
static ULONG UfsSmmuMatches;
static ULONG UfsSmmuDisabled;
static ULONG UfsSmmuLastSmr;
static ULONG UfsSmmuLastS2cr;
static ULONG UfsSmmuLastSctlr;

VOID
UfsSmmuReport(VOID)
{
    DPRINT1("UFS: SMMU base %lx, %lu SMR match(es), %lu bank(s) turned off, SMR 0x%lx S2CR 0x%lx SCTLR 0x%lx\n",
            UfsSmmuPhysical, UfsSmmuMatches, UfsSmmuDisabled,
            UfsSmmuLastSmr, UfsSmmuLastS2cr, UfsSmmuLastSctlr);
}

VOID
UfsSmmuReadParameters(
    _In_ PUNICODE_STRING RegistryPath)
{
    RTL_QUERY_REGISTRY_TABLE Table[4];
    UNICODE_STRING KeyPath;
    NTSTATUS Status;

    KeyPath.Length = 0;
    KeyPath.MaximumLength = RegistryPath->Length + sizeof(L"\\Parameters");
    KeyPath.Buffer = ExAllocatePoolWithTag(PagedPool, KeyPath.MaximumLength, 'sfuS');
    if (KeyPath.Buffer == NULL)
        return;

    RtlCopyUnicodeString(&KeyPath, RegistryPath);
    RtlAppendUnicodeToString(&KeyPath, L"\\Parameters");

    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    Table[0].Name = L"SmmuBase";
    Table[0].EntryContext = &UfsSmmuPhysical;
    Table[1].Flags = RTL_QUERY_REGISTRY_DIRECT;
    Table[1].Name = L"SmmuStreamId";
    Table[1].EntryContext = &UfsSmmuStream;
    Table[2].Flags = RTL_QUERY_REGISTRY_DIRECT;
    Table[2].Name = L"SmmuStreamMask";
    Table[2].EntryContext = &UfsSmmuStreamMask;

    Status = RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, KeyPath.Buffer, Table, NULL, NULL);
    if (!NT_SUCCESS(Status))
        UfsSmmuPhysical = 0;

    ExFreePoolWithTag(KeyPath.Buffer, 'sfuS');
}

static
ULONG
UfsSmmuRead(
    _In_ PUCHAR Base,
    _In_ ULONG_PTR Offset)
{
    return READ_REGISTER_ULONG((PULONG)(Base + Offset));
}

static
VOID
UfsSmmuWrite(
    _In_ PUCHAR Base,
    _In_ ULONG_PTR Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(Base + Offset), Value);
}

/**
 * @brief Turns off translation on every context bank firmware left behind
 * for the controller's stream, so its DMA reaches physical memory directly.
 */
VOID
UfsSmmuHandoff(VOID)
{
    PHYSICAL_ADDRESS Physical;
    PUCHAR Smmu;
    PUCHAR Context;
    SIZE_T Span;
    ULONG Idr0;
    ULONG Idr1;
    ULONG PageSize;
    ULONG HalfPages;
    ULONG Group;
    ULONG Smr;
    ULONG S2cr;
    ULONG Bank;
    ULONG Sctlr;
    ULONG Fsr;
    ULONG Ignore;
    BOOLEAN Matched = FALSE;

    if (UfsSmmuPhysical == 0)
        return;

    Physical.QuadPart = UfsSmmuPhysical;
    Smmu = MmMapIoSpace(Physical, PAGE_SIZE, MmNonCached);
    if (Smmu == NULL)
        return;

    Idr0 = UfsSmmuRead(Smmu, SMMU_IDR0);
    Idr1 = UfsSmmuRead(Smmu, SMMU_IDR1);
    MmUnmapIoSpace(Smmu, PAGE_SIZE);

    PageSize = (Idr1 & SMMU_LARGE_PAGES) ? 0x10000 : 0x1000;
    HalfPages = 1 << SMMU_PAGE_SHIFT(Idr1);
    Span = (SIZE_T)HalfPages * PageSize * 2;

    Smmu = MmMapIoSpace(Physical, Span, MmNonCached);
    if (Smmu == NULL)
        return;

    DPRINT1("UFS: SMMU %lx IDR0 0x%lx IDR1 0x%lx, stream 0x%lx/0x%lx\n",
            UfsSmmuPhysical, Idr0, Idr1, UfsSmmuStream, UfsSmmuStreamMask);

    Ignore = (UfsSmmuStreamMask & SMMU_STREAM_BITS);
    for (Group = 0; Group < SMMU_SMR_GROUPS(Idr0); Group++)
    {
        Smr = UfsSmmuRead(Smmu, SMMU_SMR_BASE + Group * 4);
        if (!(Smr & SMMU_SMR_ENABLED))
            continue;

        if (((Smr ^ UfsSmmuStream) & ~(Ignore | (Smr >> 16)) & SMMU_STREAM_BITS) != 0)
            continue;

        Matched = TRUE;
        UfsSmmuMatches++;
        S2cr = UfsSmmuRead(Smmu, SMMU_S2CR_BASE + Group * 4);
        UfsSmmuLastSmr = Smr;
        UfsSmmuLastS2cr = S2cr;
        DPRINT1("UFS: SMR%lu 0x%lx S2CR 0x%lx\n", Group, Smr, S2cr);

        if (SMMU_S2CR_KIND(S2cr) != SMMU_S2CR_TRANSLATE)
            continue;

        Bank = SMMU_S2CR_CONTEXT(S2cr);
        if (Bank >= SMMU_CTX_COUNT(Idr1))
            continue;

        Context = Smmu + (SIZE_T)(HalfPages + Bank) * PageSize;
        Sctlr = UfsSmmuRead(Context, SMMU_CTX_SCTLR);
        Fsr = UfsSmmuRead(Context, SMMU_CTX_FSR);
        UfsSmmuLastSctlr = Sctlr;
        DPRINT1("UFS: CB%lu CBAR 0x%lx SCTLR 0x%lx TTBR0 0x%lx%08lx FSR 0x%lx\n",
                Bank,
                UfsSmmuRead(Smmu, PageSize + Bank * 4),
                Sctlr,
                UfsSmmuRead(Context, SMMU_CTX_TTBR0_HI),
                UfsSmmuRead(Context, SMMU_CTX_TTBR0_LO),
                Fsr);

        if (Sctlr & SMMU_SCTLR_ENABLE)
        {
            UfsSmmuWrite(Context, SMMU_CTX_SCTLR, Sctlr & ~SMMU_SCTLR_ENABLE);
            UfsSmmuWrite(Context, SMMU_CTX_FSR, Fsr);
            UfsSmmuDisabled++;
            DPRINT1("UFS: CB%lu translation off, SCTLR now 0x%lx\n",
                    Bank, UfsSmmuRead(Context, SMMU_CTX_SCTLR));
        }
    }

    if (!Matched)
        DPRINT1("UFS: no SMR covers stream 0x%lx\n", UfsSmmuStream);

    MmUnmapIoSpace(Smmu, Span);
}
