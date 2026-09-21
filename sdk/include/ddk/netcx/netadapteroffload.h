/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Checksum, segmentation, coalescing and tagging offloads
 *
 * These replace the single NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES of
 * adapter 2.0, which stays in netadapter.h for older clients.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _NET_ADAPTER_OFFLOAD_LAYER3_FLAGS
{
    NetAdapterOffloadLayer3FlagIPv4NoOptions = 0x00000001,
    NetAdapterOffloadLayer3FlagIPv4WithOptions = 0x00000002,
    NetAdapterOffloadLayer3FlagIPv6NoExtensions = 0x00000004,
    NetAdapterOffloadLayer3FlagIPv6WithExtensions = 0x00000008
} NET_ADAPTER_OFFLOAD_LAYER3_FLAGS;

DEFINE_ENUM_FLAG_OPERATORS(NET_ADAPTER_OFFLOAD_LAYER3_FLAGS);

typedef enum _NET_ADAPTER_OFFLOAD_LAYER4_FLAGS
{
    NetAdapterOffloadLayer4FlagTcpNoOptions = 0x00000001,
    NetAdapterOffloadLayer4FlagTcpWithOptions = 0x00000002,
    NetAdapterOffloadLayer4FlagUdp = 0x00000004
} NET_ADAPTER_OFFLOAD_LAYER4_FLAGS;

DEFINE_ENUM_FLAG_OPERATORS(NET_ADAPTER_OFFLOAD_LAYER4_FLAGS);

typedef enum _NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_FLAGS
{
    NetAdapterOffloadIeee8021PriorityTaggingFlag = 0x00000001,
    NetAdapterOffloadIeee8021VlanTaggingFlag = 0x00000002
} NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_FLAGS;

DEFINE_ENUM_FLAG_OPERATORS(NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_FLAGS);

/* Every offload reports a change of its active settings through one of these. */
typedef
_Function_class_(EVT_NET_ADAPTER_OFFLOAD_SET_TX_CHECKSUM)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_NET_ADAPTER_OFFLOAD_SET_TX_CHECKSUM(
    _In_ NETADAPTER Adapter,
    _In_ NETOFFLOAD Offload);

typedef EVT_NET_ADAPTER_OFFLOAD_SET_TX_CHECKSUM *PFN_NET_ADAPTER_OFFLOAD_SET_TX_CHECKSUM;

typedef
_Function_class_(EVT_NET_ADAPTER_OFFLOAD_SET_RX_CHECKSUM)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_NET_ADAPTER_OFFLOAD_SET_RX_CHECKSUM(
    _In_ NETADAPTER Adapter,
    _In_ NETOFFLOAD Offload);

typedef EVT_NET_ADAPTER_OFFLOAD_SET_RX_CHECKSUM *PFN_NET_ADAPTER_OFFLOAD_SET_RX_CHECKSUM;

typedef
_Function_class_(EVT_NET_ADAPTER_OFFLOAD_SET_GSO)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_NET_ADAPTER_OFFLOAD_SET_GSO(
    _In_ NETADAPTER Adapter,
    _In_ NETOFFLOAD Offload);

typedef EVT_NET_ADAPTER_OFFLOAD_SET_GSO *PFN_NET_ADAPTER_OFFLOAD_SET_GSO;

typedef
_Function_class_(EVT_NET_ADAPTER_OFFLOAD_SET_RSC)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_NET_ADAPTER_OFFLOAD_SET_RSC(
    _In_ NETADAPTER Adapter,
    _In_ NETOFFLOAD Offload);

typedef EVT_NET_ADAPTER_OFFLOAD_SET_RSC *PFN_NET_ADAPTER_OFFLOAD_SET_RSC;

typedef struct _NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES
{
    ULONG Size;
    NET_ADAPTER_OFFLOAD_LAYER3_FLAGS Layer3Flags;
    NET_ADAPTER_OFFLOAD_LAYER4_FLAGS Layer4Flags;
    USHORT Layer3HeaderOffsetLimit;
    USHORT Layer4HeaderOffsetLimit;
    PFN_NET_ADAPTER_OFFLOAD_SET_TX_CHECKSUM EvtAdapterOffloadSetTxChecksum;
} NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES *Capabilities,
    _In_ NET_ADAPTER_OFFLOAD_LAYER3_FLAGS Layer3Flags,
    _In_ PFN_NET_ADAPTER_OFFLOAD_SET_TX_CHECKSUM EvtAdapterOffloadSetTxChecksum)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->Layer3Flags = Layer3Flags;
    Capabilities->EvtAdapterOffloadSetTxChecksum = EvtAdapterOffloadSetTxChecksum;
}

typedef struct _NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES
{
    ULONG Size;
    PFN_NET_ADAPTER_OFFLOAD_SET_RX_CHECKSUM EvtAdapterOffloadSetRxChecksum;
} NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES *Capabilities,
    _In_ PFN_NET_ADAPTER_OFFLOAD_SET_RX_CHECKSUM EvtAdapterOffloadSetRxChecksum)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->EvtAdapterOffloadSetRxChecksum = EvtAdapterOffloadSetRxChecksum;
}

/* Covers both LSO and USO; Layer4Flags selects which. */
typedef struct _NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES
{
    ULONG Size;
    NET_ADAPTER_OFFLOAD_LAYER3_FLAGS Layer3Flags;
    NET_ADAPTER_OFFLOAD_LAYER4_FLAGS Layer4Flags;
    USHORT Layer4HeaderOffsetLimit;
    SIZE_T MaximumOffloadSize;
    SIZE_T MinimumSegmentCount;
    PFN_NET_ADAPTER_OFFLOAD_SET_GSO EvtAdapterOffloadSetGso;
} NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES *Capabilities,
    _In_ NET_ADAPTER_OFFLOAD_LAYER3_FLAGS Layer3Flags,
    _In_ NET_ADAPTER_OFFLOAD_LAYER4_FLAGS Layer4Flags,
    _In_ SIZE_T MaximumOffloadSize,
    _In_ SIZE_T MinimumSegmentCount,
    _In_ PFN_NET_ADAPTER_OFFLOAD_SET_GSO EvtAdapterOffloadSetGso)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->Layer3Flags = Layer3Flags;
    Capabilities->Layer4Flags = Layer4Flags;
    Capabilities->MaximumOffloadSize = MaximumOffloadSize;
    Capabilities->MinimumSegmentCount = MinimumSegmentCount;
    Capabilities->EvtAdapterOffloadSetGso = EvtAdapterOffloadSetGso;
}

typedef struct _NET_ADAPTER_OFFLOAD_RSC_CAPABILITIES
{
    ULONG Size;
    NET_ADAPTER_OFFLOAD_LAYER3_FLAGS Layer3Flags;
    NET_ADAPTER_OFFLOAD_LAYER4_FLAGS Layer4Flags;
    BOOLEAN TcpTimestampOption;
    PFN_NET_ADAPTER_OFFLOAD_SET_RSC EvtAdapterOffloadSetRsc;
} NET_ADAPTER_OFFLOAD_RSC_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_OFFLOAD_RSC_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_OFFLOAD_RSC_CAPABILITIES *Capabilities,
    _In_ NET_ADAPTER_OFFLOAD_LAYER3_FLAGS Layer3Flags,
    _In_ NET_ADAPTER_OFFLOAD_LAYER4_FLAGS Layer4Flags,
    _In_ PFN_NET_ADAPTER_OFFLOAD_SET_RSC EvtAdapterOffloadSetRsc)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->Layer3Flags = Layer3Flags;
    Capabilities->Layer4Flags = Layer4Flags;
    Capabilities->EvtAdapterOffloadSetRsc = EvtAdapterOffloadSetRsc;
}

typedef struct _NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES
{
    ULONG Size;
    NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_FLAGS Flags;
} NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES *Capabilities,
    _In_ NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_FLAGS Flags)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->Flags = Flags;
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEROFFLOADSETTXCHECKSUMCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES *HardwareCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterOffloadSetTxChecksumCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES *HardwareCapabilities)
{
    ((PFN_NETADAPTEROFFLOADSETTXCHECKSUMCAPABILITIES)NetFunctions[NetAdapterOffloadSetTxChecksumCapabilitiesTableIndex])(
        NetDriverGlobals, Adapter, HardwareCapabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISTXCHECKSUMIPV4ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsTxChecksumIPv4Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISTXCHECKSUMIPV4ENABLED)NetFunctions[NetOffloadIsTxChecksumIPv4EnabledTableIndex])(
        NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISTXCHECKSUMTCPENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsTxChecksumTcpEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISTXCHECKSUMTCPENABLED)NetFunctions[NetOffloadIsTxChecksumTcpEnabledTableIndex])(
        NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISTXCHECKSUMUDPENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsTxChecksumUdpEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISTXCHECKSUMUDPENABLED)NetFunctions[NetOffloadIsTxChecksumUdpEnabledTableIndex])(
        NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEROFFLOADSETRXCHECKSUMCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterOffloadSetRxChecksumCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES *Capabilities)
{
    ((PFN_NETADAPTEROFFLOADSETRXCHECKSUMCAPABILITIES)NetFunctions[NetAdapterOffloadSetRxChecksumCapabilitiesTableIndex])(
        NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISRXCHECKSUMIPV4ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsRxChecksumIPv4Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISRXCHECKSUMIPV4ENABLED)NetFunctions[NetOffloadIsRxChecksumIPv4EnabledTableIndex])(
        NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISRXCHECKSUMTCPENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsRxChecksumTcpEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISRXCHECKSUMTCPENABLED)NetFunctions[NetOffloadIsRxChecksumTcpEnabledTableIndex])(
        NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISRXCHECKSUMUDPENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsRxChecksumUdpEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISRXCHECKSUMUDPENABLED)NetFunctions[NetOffloadIsRxChecksumUdpEnabledTableIndex])(
        NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEROFFLOADSETGSOCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES *HardwareCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterOffloadSetGsoCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES *HardwareCapabilities)
{
    ((PFN_NETADAPTEROFFLOADSETGSOCAPABILITIES)NetFunctions[NetAdapterOffloadSetGsoCapabilitiesTableIndex])(
        NetDriverGlobals, Adapter, HardwareCapabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISLSOIPV4ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsLsoIPv4Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISLSOIPV4ENABLED)NetFunctions[NetOffloadIsLsoIPv4EnabledTableIndex])(
        NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISLSOIPV6ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsLsoIPv6Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISLSOIPV6ENABLED)NetFunctions[NetOffloadIsLsoIPv6EnabledTableIndex])(
        NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISUSOIPV4ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsUsoIPv4Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISUSOIPV4ENABLED)NetFunctions[NetOffloadIsUsoIPv4EnabledTableIndex])(
        NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISUSOIPV6ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsUsoIPv6Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISUSOIPV6ENABLED)NetFunctions[NetOffloadIsUsoIPv6EnabledTableIndex])(
        NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEROFFLOADSETRSCCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_RSC_CAPABILITIES *HardwareCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterOffloadSetRscCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_RSC_CAPABILITIES *HardwareCapabilities)
{
    ((PFN_NETADAPTEROFFLOADSETRSCCAPABILITIES)NetFunctions[NetAdapterOffloadSetRscCapabilitiesTableIndex])(
        NetDriverGlobals, Adapter, HardwareCapabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISTCPRSCIPV4ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsTcpRscIPv4Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISTCPRSCIPV4ENABLED)NetFunctions[NetOffloadIsTcpRscIPv4EnabledTableIndex])(
        NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISTCPRSCIPV6ENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsTcpRscIPv6Enabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISTCPRSCIPV6ENABLED)NetFunctions[NetOffloadIsTcpRscIPv6EnabledTableIndex])(
        NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISRSCTCPTIMESTAMPOPTIONENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsRscTcpTimestampOptionEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISRSCTCPTIMESTAMPOPTIONENABLED)NetFunctions[NetOffloadIsRscTcpTimestampOptionEnabledTableIndex])(
        NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
(NTAPI *PFN_NETOFFLOADISUDPRSCENABLED)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
BOOLEAN
NTAPI
NetOffloadIsUdpRscEnabled(
    _In_ NETOFFLOAD Offload)
{
    return ((PFN_NETOFFLOADISUDPRSCENABLED)NetFunctions[NetOffloadIsUdpRscEnabledTableIndex])(
        NetDriverGlobals, Offload);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEROFFLOADSETIEEE8021QTAGCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES *HardwareCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterOffloadSetIeee8021qTagCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES *HardwareCapabilities)
{
    ((PFN_NETADAPTEROFFLOADSETIEEE8021QTAGCAPABILITIES)NetFunctions[NetAdapterOffloadSetIeee8021qTagCapabilitiesTableIndex])(
        NetDriverGlobals, Adapter, HardwareCapabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERPAUSEOFFLOADCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterPauseOffloadCapabilities(
    _In_ NETADAPTER Adapter)
{
    ((PFN_NETADAPTERPAUSEOFFLOADCAPABILITIES)NetFunctions[NetAdapterPauseOffloadCapabilitiesTableIndex])(
        NetDriverGlobals, Adapter);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERRESUMEOFFLOADCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterResumeOffloadCapabilities(
    _In_ NETADAPTER Adapter)
{
    ((PFN_NETADAPTERRESUMEOFFLOADCAPABILITIES)NetFunctions[NetAdapterResumeOffloadCapabilitiesTableIndex])(
        NetDriverGlobals, Adapter);
}

#ifdef __cplusplus
}
#endif
