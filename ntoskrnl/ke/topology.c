/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Processor core and package topology
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* CPUID leaves and fields the topology comes from */
#define KI_CPUID_EXTENDED_TOPOLOGY      0x0000000B
#define KI_CPUID_CACHE_PARAMETERS       0x00000004
#define KI_CPUID_EXTENDED_FEATURES      0x80000001
#define KI_CPUID_ADDRESS_SIZES          0x80000008
#define KI_CPUID_AMD_TOPOLOGY           0x8000001E

#define KI_TOPOLOGY_LEVEL_SMT           1
#define KI_TOPOLOGY_LEVEL_CORE          2

#define KI_CPUID1_EDX_HTT               (1UL << 28)
#define KI_CPUID81_ECX_TOPOLOGY_EXT     (1UL << 22)

/* Full APIC IDs, the KPRCB only keeps the low byte */
static ULONG KiProcessorApicId[MAXIMUM_PROCESSORS];

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief Gets the number of bits needed to number Count items.
 */
static
ULONG
KiTopologyFieldWidth(
    _In_ ULONG Count)
{
    ULONG Width = 0;

    while (Width < 31 && (1UL << Width) < Count)
        Width++;

    return Width;
}

/**
 * @brief Counts the processors in an affinity set.
 */
static
ULONG
KiCountAffinity(
    _In_ KAFFINITY Set)
{
    ULONG Count = 0;

    while (Set)
    {
        Set &= Set - 1;
        Count++;
    }

    return Count;
}

/**
 * @brief Reads the threads per core, cores per package and APIC ID of the current processor.
 */
static
VOID
KiDetectProcessorTopology(
    _Inout_ PKPRCB Prcb)
{
    INT Regs[4];
    ULONG MaxLeaf, MaxExtendedLeaf, Version, Count, PackageThreads, SubLeaf, LevelType;
    ULONG Cores = 1, Threads = 1, ApicId, GroupSize;

    __cpuid(Regs, 0);
    MaxLeaf = (ULONG)Regs[0];
    __cpuid(Regs, 0x80000000);
    MaxExtendedLeaf = (ULONG)Regs[0];

    __cpuid(Regs, 1);
    Version = (ULONG)Regs[3];
    Count = ((ULONG)Regs[1] >> 16) & 0xFF;
    ApicId = (ULONG)Regs[1] >> 24;

    if (Prcb->CpuVendor == CPU_INTEL || Prcb->CpuVendor == CPU_VIA)
    {
        if (MaxLeaf >= KI_CPUID_EXTENDED_TOPOLOGY)
            __cpuidex(Regs, KI_CPUID_EXTENDED_TOPOLOGY, 0);

        if (MaxLeaf >= KI_CPUID_EXTENDED_TOPOLOGY && Regs[1] != 0)
        {
            /* Each level gives the APIC ID bits below the next one */
            ApicId = (ULONG)Regs[3];
            PackageThreads = 1;
            SubLeaf = 0;
            do
            {
                __cpuidex(Regs, KI_CPUID_EXTENDED_TOPOLOGY, SubLeaf++);
                LevelType = ((ULONG)Regs[2] >> 8) & 0xFF;
                if (LevelType == KI_TOPOLOGY_LEVEL_SMT)
                    Threads = 1UL << (Regs[0] & 0x1F);
                else if (LevelType == KI_TOPOLOGY_LEVEL_CORE)
                    PackageThreads = 1UL << (Regs[0] & 0x1F);
            } while (Regs[1] & 0xFFFF);

            Cores = max(1, PackageThreads / Threads);
        }
        else
        {
            if (MaxLeaf >= KI_CPUID_CACHE_PARAMETERS)
            {
                __cpuidex(Regs, KI_CPUID_CACHE_PARAMETERS, 0);
                Cores = 1UL << KiTopologyFieldWidth(((ULONG)Regs[0] >> 26) + 1);
            }

            if ((Version & KI_CPUID1_EDX_HTT) && Count != 0)
                Threads = max(1, (1UL << KiTopologyFieldWidth(Count)) / Cores);
        }
    }
    else if (Prcb->CpuVendor == CPU_AMD && MaxExtendedLeaf >= KI_CPUID_ADDRESS_SIZES)
    {
        /* This first counts every thread of the package */
        __cpuid(Regs, KI_CPUID_ADDRESS_SIZES);
        if ((Regs[2] >> 12) & 0xF)
            Cores = 1UL << ((Regs[2] >> 12) & 0xF);
        else
            Cores = 1UL << KiTopologyFieldWidth((Regs[2] & 0xFF) + 1);

        if (MaxExtendedLeaf >= KI_CPUID_AMD_TOPOLOGY)
        {
            __cpuid(Regs, KI_CPUID_EXTENDED_FEATURES);
            if ((ULONG)Regs[2] & KI_CPUID81_ECX_TOPOLOGY_EXT)
            {
                __cpuid(Regs, KI_CPUID_AMD_TOPOLOGY);
                Threads = (((ULONG)Regs[1] >> 8) & 0xFF) + 1;
                Cores = max(1, Cores / Threads);
            }
        }
    }

    /* A package wider than an affinity mask is cut down to one */
    GroupSize = sizeof(KAFFINITY) * 8;
    if (Threads * Cores > GroupSize)
    {
        Threads = min(Threads, GroupSize);
        Cores = GroupSize / Threads;
    }

    Prcb->LogicalProcessorsPerCore = (UCHAR)Threads;
    Prcb->CoresPerPhysicalProcessor = (UCHAR)Cores;
    Prcb->InitialApicId = (UCHAR)ApicId;
    KiProcessorApicId[Prcb->Number] = ApicId;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief Finds the core and package siblings of a starting processor.
 *
 * @remarks Processors start one at a time, so every earlier processor is already
 * described and gets this one added to its own sets.
 */
VOID
NTAPI
KiSetProcessorTopology(
    _Inout_ PKPRCB Prcb)
{
    ULONG PackageMask, CoreMask, ApicId, PeerApicId, PackageSize, Population;
    KAFFINITY CoreSet;
    PKPRCB Peer;
    ULONG Index;

    KiDetectProcessorTopology(Prcb);

    Prcb->PackageProcessorSet = Prcb->SetMember;
    Prcb->MultiThreadProcessorSet = Prcb->SetMember;
    Prcb->MultiThreadSetMaster = Prcb;

    if (Prcb->Number == 0 ||
        (Prcb->CoresPerPhysicalProcessor == 1 && Prcb->LogicalProcessorsPerCore == 1))
    {
        return;
    }

    PackageSize = (ULONG)Prcb->CoresPerPhysicalProcessor * Prcb->LogicalProcessorsPerCore;
    PackageMask = ~(PackageSize - 1);
    CoreMask = ~((ULONG)Prcb->LogicalProcessorsPerCore - 1);
    ApicId = KiProcessorApicId[Prcb->Number];
    CoreSet = Prcb->SetMember;

    for (Index = 0; Index < Prcb->Number; Index++)
    {
        Peer = KiProcessorBlock[Index];
        if (Peer == NULL)
            continue;

        PeerApicId = KiProcessorApicId[Index];
        if ((PeerApicId & PackageMask) == (ApicId & PackageMask))
        {
            Prcb->PackageProcessorSet |= Peer->SetMember;
            Peer->PackageProcessorSet |= Prcb->SetMember;

            /* More processors in a package than CPUID allows means the IDs cannot be trusted */
            Population = KiCountAffinity(Peer->PackageProcessorSet);
            if (Population > PackageSize)
            {
                KeBugCheckEx(MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED,
                             (ULONG_PTR)Peer,
                             Population,
                             Peer->CoresPerPhysicalProcessor,
                             Peer->LogicalProcessorsPerCore);
            }
        }

        if ((PeerApicId & CoreMask) == (ApicId & CoreMask) && Peer->ParentNode == Prcb->ParentNode)
        {
            CoreSet |= Peer->SetMember;
            Peer->MultiThreadProcessorSet |= CoreSet;
            if (Prcb->MultiThreadSetMaster == Prcb)
                Prcb->MultiThreadSetMaster = Peer->MultiThreadSetMaster;
        }
    }

    Population = KiCountAffinity(Prcb->PackageProcessorSet);
    if (Population > PackageSize)
    {
        KeBugCheckEx(MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED,
                     (ULONG_PTR)Prcb,
                     Population,
                     Prcb->CoresPerPhysicalProcessor,
                     Prcb->LogicalProcessorsPerCore);
    }

    Prcb->MultiThreadProcessorSet = CoreSet;
}

/* EOF */
