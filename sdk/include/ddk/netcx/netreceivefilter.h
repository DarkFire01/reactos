/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Receive filtering by destination address class
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

DECLARE_HANDLE(NETRECEIVEFILTER);

typedef enum _NET_PACKET_FILTER_FLAGS
{
    NetPacketFilterFlagDirected = 0x00000001,
    NetPacketFilterFlagMulticast = 0x00000002,
    NetPacketFilterFlagAllMulticast = 0x00000004,
    NetPacketFilterFlagBroadcast = 0x00000008,
    NetPacketFilterFlagPromiscuous = 0x00000020
} NET_PACKET_FILTER_FLAGS;

DEFINE_ENUM_FLAG_OPERATORS(NET_PACKET_FILTER_FLAGS);

typedef
_Function_class_(EVT_NET_ADAPTER_SET_RECEIVE_FILTER)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_NET_ADAPTER_SET_RECEIVE_FILTER(
    _In_ NETADAPTER Adapter,
    _In_ NETRECEIVEFILTER ReceiveFilter);

typedef EVT_NET_ADAPTER_SET_RECEIVE_FILTER *PFN_NET_ADAPTER_SET_RECEIVE_FILTER;

typedef struct _NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES
{
    ULONG Size;
    NET_PACKET_FILTER_FLAGS SupportedPacketFilters;
    SIZE_T MaximumMulticastAddresses;
    PFN_NET_ADAPTER_SET_RECEIVE_FILTER EvtSetReceiveFilter;
} NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES *Capabilities,
    _In_ PFN_NET_ADAPTER_SET_RECEIVE_FILTER EvtSetReceiveFilter)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));
    Capabilities->Size = sizeof(*Capabilities);
    Capabilities->EvtSetReceiveFilter = EvtSetReceiveFilter;
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERSETRECEIVEFILTERCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES *ReceiveFilter);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterSetReceiveFilterCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES *ReceiveFilter)
{
    ((PFN_NETADAPTERSETRECEIVEFILTERCAPABILITIES)NetFunctions[NetAdapterSetReceiveFilterCapabilitiesTableIndex])(
        NetDriverGlobals, Adapter, ReceiveFilter);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
NET_PACKET_FILTER_FLAGS
(NTAPI *PFN_NETRECEIVEFILTERGETPACKETFILTER)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETRECEIVEFILTER ReceiveFilter);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NET_PACKET_FILTER_FLAGS
NTAPI
NetReceiveFilterGetPacketFilter(
    _In_ NETRECEIVEFILTER ReceiveFilter)
{
    return ((PFN_NETRECEIVEFILTERGETPACKETFILTER)NetFunctions[NetReceiveFilterGetPacketFilterTableIndex])(
        NetDriverGlobals, ReceiveFilter);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
SIZE_T
(NTAPI *PFN_NETRECEIVEFILTERGETMULTICASTADDRESSCOUNT)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETRECEIVEFILTER ReceiveFilter);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
SIZE_T
NTAPI
NetReceiveFilterGetMulticastAddressCount(
    _In_ NETRECEIVEFILTER ReceiveFilter)
{
    return ((PFN_NETRECEIVEFILTERGETMULTICASTADDRESSCOUNT)NetFunctions[NetReceiveFilterGetMulticastAddressCountTableIndex])(
        NetDriverGlobals, ReceiveFilter);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
CONST NET_ADAPTER_LINK_LAYER_ADDRESS *
(NTAPI *PFN_NETRECEIVEFILTERGETMULTICASTADDRESSLIST)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETRECEIVEFILTER ReceiveFilter);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
CONST NET_ADAPTER_LINK_LAYER_ADDRESS *
NTAPI
NetReceiveFilterGetMulticastAddressList(
    _In_ NETRECEIVEFILTER ReceiveFilter)
{
    return ((PFN_NETRECEIVEFILTERGETMULTICASTADDRESSLIST)NetFunctions[NetReceiveFilterGetMulticastAddressListTableIndex])(
        NetDriverGlobals, ReceiveFilter);
}

#ifdef __cplusplus
}
#endif
