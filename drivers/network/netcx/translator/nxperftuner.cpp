/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Ring and buffer pool sizing from system memory and link speed
 */

#include <ndis.h>
#include <ndk/exfuncs.h>

#include "NxPerfTuner.hpp"

enum class NxPerfClass
{
    Small,
    Medium,
    Large,
};

static UINT64 g_TotalPhysicalBytes;

static const UINT64 OneGigabyte = 1ull << 30;

/* With 2 GB to 64 GB of memory, only a link at least this fast gets the large class. */
static const UINT64 FastLinkSpeed = 0x240000000ull;

/* Wi-Fi adapters above this speed get deeper Tx rings on a medium system. */
static const UINT64 FastWirelessLinkSpeed = 0x80000000ull;

/* Largest packet that still counts as a standard Ethernet frame when GSO is off. */
static const UINT64 StandardFrameLimit = 1550;

static const UINT64 SmallBounceBufferSize = 2048;
static const ULONG LargeBounceBufferCount = 256;

static
bool
IsPowerOfTwo(
    _In_ ULONG Value)
{
    return Value != 0 && (Value & (Value - 1)) == 0;
}

static
NxPerfClass
NxPerfClassify(
    _In_ UINT64 NominalLinkSpeed)
{
    if (g_TotalPhysicalBytes <= OneGigabyte)
        return NxPerfClass::Small;

    if (g_TotalPhysicalBytes > 64 * OneGigabyte)
        return NxPerfClass::Large;

    /* An unknown link speed is reported as all ones and never counts as fast. */
    bool const fastLink = NominalLinkSpeed >= FastLinkSpeed &&
                          NominalLinkSpeed != NDIS_LINK_SPEED_UNKNOWN;

    if (fastLink && g_TotalPhysicalBytes >= 2 * OneGigabyte)
        return NxPerfClass::Large;

    return NxPerfClass::Medium;
}

/* The driver's preferred ring size may raise the computed one by at most four times. */
static
ULONG
NxPerfApplyRingHint(
    _In_ NX_PERF_NIC_GENERAL_CHARACTERISTICS const &Nic,
    _In_ ULONG Hint,
    _In_ ULONG Computed)
{
    if (Nic.IsDriverVerifierEnabled)
        return Computed * 2;

    if (Hint < Computed)
        return Computed;

    return min(Hint, Computed * 4);
}

_Use_decl_annotations_
NTSTATUS
NxPerfTunerInitialize(
    void)
{
    SYSTEM_PHYSICAL_MEMORY_INFORMATION memoryInfo;

    NTSTATUS const status = ZwQuerySystemInformation(SystemPhysicalMemoryInformation,
                                                     &memoryInfo,
                                                     sizeof(memoryInfo),
                                                     nullptr);
    if (!NT_SUCCESS(status))
        return status;

    g_TotalPhysicalBytes = memoryInfo.TotalPhysicalBytes;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
void
NxPerfTunerCleanup(
    void)
{
    g_TotalPhysicalBytes = 0;
}

_Use_decl_annotations_
void
NxPerfTunerCalculateTxParameters(
    NX_PERF_TX_NIC_CHARACTERISTICS const *Characteristics,
    NX_PERF_TX_TUNING_PARAMETERS *Parameters)
{
    ULONG packetCount;
    ULONG fragmentsPerPacket;

    switch (NxPerfClassify(Characteristics->NominalLinkSpeed))
    {
        case NxPerfClass::Small:
            packetCount = 32;
            fragmentsPerPacket = 4;
            break;

        case NxPerfClass::Medium:
            if (g_TotalPhysicalBytes >= 2 * OneGigabyte &&
                Characteristics->Nic.MediaType == NdisMediumNative802_11 &&
                Characteristics->NominalLinkSpeed >= FastWirelessLinkSpeed)
            {
                packetCount = 1024;
                fragmentsPerPacket = 2;
            }
            else
            {
                packetCount = 256;
                fragmentsPerPacket = 4;
            }
            break;

        default:
            packetCount = 4096;
            fragmentsPerPacket = 16;
            break;
    }

    /* Never plan for more fragments than the hardware can chain, rounded up to a power of two. */
    ULONG const hardwareLimit = Characteristics->MaxFragmentsPerPacket;
    if (hardwareLimit != 0 && hardwareLimit < fragmentsPerPacket)
    {
        fragmentsPerPacket = 1;
        while (fragmentsPerPacket < hardwareLimit)
            fragmentsPerPacket <<= 1;
    }

    if (Characteristics->MaxPacketSizeWithGso <= StandardFrameLimit)
        fragmentsPerPacket = min(fragmentsPerPacket, 4ul);

    ULONG const fragmentCount = NxPerfApplyRingHint(Characteristics->Nic,
                                                    Characteristics->FragmentRingNumberOfElementsHint,
                                                    packetCount * fragmentsPerPacket);

    Parameters->FragmentRingElementCount = fragmentCount;
    Parameters->PacketRingElementCount = fragmentCount / fragmentsPerPacket;

    /* Small buffers are cheap enough to give every packet slot one. */
    if (Characteristics->MaximumFragmentBufferSize <= SmallBounceBufferSize)
        Parameters->NumberOfBounceBuffers = Parameters->PacketRingElementCount;
    else
        Parameters->NumberOfBounceBuffers = LargeBounceBufferCount;

    NT_ASSERT(IsPowerOfTwo(Parameters->PacketRingElementCount));
    NT_ASSERT(IsPowerOfTwo(Parameters->FragmentRingElementCount));
}

_Use_decl_annotations_
void
NxPerfTunerCalculateRxParameters(
    NX_PERF_RX_NIC_CHARACTERISTICS const *Characteristics,
    NX_PERF_RX_TUNING_PARAMETERS *Parameters)
{
    ULONG ringSize;
    ULONG buffersPerSlot;

    switch (NxPerfClassify(Characteristics->NominalLinkSpeed))
    {
        case NxPerfClass::Small:
            ringSize = 32;
            buffersPerSlot = 1;
            break;

        case NxPerfClass::Medium:
            ringSize = 256;
            buffersPerSlot = 2;
            break;

        default:
            ringSize = 4096;
            buffersPerSlot = 4;
            break;
    }

    ringSize = NxPerfApplyRingHint(Characteristics->Nic,
                                   Characteristics->FragmentRingNumberOfElementsHint,
                                   ringSize);

    Parameters->PacketRingElementCount = ringSize;
    Parameters->FragmentRingElementCount = ringSize;
    Parameters->NumberOfBuffers = ringSize * buffersPerSlot;
    Parameters->NumberOfNbls = ringSize * buffersPerSlot;

    NT_ASSERT(IsPowerOfTwo(Parameters->PacketRingElementCount));
}
