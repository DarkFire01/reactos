/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Ring and buffer pool sizing from system memory and link speed
 */

#pragma once

struct NX_PERF_NIC_GENERAL_CHARACTERISTICS
{
    ULONG MediaType;
    bool IsDriverVerifierEnabled;
};

struct NX_PERF_TX_NIC_CHARACTERISTICS
{
    NX_PERF_NIC_GENERAL_CHARACTERISTICS Nic;
    ULONG FragmentRingNumberOfElementsHint;
    UINT64 MaximumFragmentBufferSize;
    ULONG MaxFragmentsPerPacket;
    UINT64 MaxPacketSizeWithGso;
    UINT64 NominalLinkSpeed;
};

struct NX_PERF_RX_NIC_CHARACTERISTICS
{
    NX_PERF_NIC_GENERAL_CHARACTERISTICS Nic;
    ULONG FragmentRingNumberOfElementsHint;
    UINT64 MaximumFragmentBufferSize;
    UINT64 MaxPacketSizeWithRsc;
    UINT64 NominalLinkSpeed;
};

struct NX_PERF_TX_TUNING_PARAMETERS
{
    ULONG PacketRingElementCount;
    ULONG FragmentRingElementCount;
    ULONG NumberOfBounceBuffers;
};

struct NX_PERF_RX_TUNING_PARAMETERS
{
    ULONG PacketRingElementCount;
    ULONG FragmentRingElementCount;
    ULONG NumberOfBuffers;
    ULONG NumberOfNbls;
};

/* Samples the amount of physical memory the sizing decisions are based on. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NxPerfTunerInitialize(
    void);

_IRQL_requires_(PASSIVE_LEVEL)
void
NxPerfTunerCleanup(
    void);

_IRQL_requires_max_(DISPATCH_LEVEL)
void
NxPerfTunerCalculateTxParameters(
    _In_ NX_PERF_TX_NIC_CHARACTERISTICS const *Characteristics,
    _Out_ NX_PERF_TX_TUNING_PARAMETERS *Parameters);

_IRQL_requires_max_(DISPATCH_LEVEL)
void
NxPerfTunerCalculateRxParameters(
    _In_ NX_PERF_RX_NIC_CHARACTERISTICS const *Characteristics,
    _Out_ NX_PERF_RX_TUNING_PARAMETERS *Parameters);
