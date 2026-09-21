/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Protocol offloads armed while the device sleeps
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_POWER_OFFLOAD_ARP_PARAMETERS
{
    ULONG Size;
    ULONG Id;
    NET_IPV4_ADDRESS RemoteIPv4Address;
    NET_IPV4_ADDRESS HostIPv4Address;
    NET_ADAPTER_LINK_LAYER_ADDRESS LinkLayerAddress;
} NET_POWER_OFFLOAD_ARP_PARAMETERS;

typedef struct _NET_POWER_OFFLOAD_NS_PARAMETERS
{
    ULONG Size;
    ULONG Id;
    NET_IPV6_ADDRESS RemoteIPv6Address;
    NET_IPV6_ADDRESS SolicitedNodeIPv6Address;
    NET_IPV6_ADDRESS TargetIPv6Addresses[2];
    NET_ADAPTER_LINK_LAYER_ADDRESS LinkLayerAddress;
} NET_POWER_OFFLOAD_NS_PARAMETERS;

typedef enum _NET_POWER_OFFLOAD_TYPE
{
    NetPowerOffloadTypeArp = 1,
    NetPowerOffloadTypeNS
} NET_POWER_OFFLOAD_TYPE;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NET_POWER_OFFLOAD_TYPE
(NTAPI *PFN_NETPOWEROFFLOADGETTYPE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPOWEROFFLOAD PowerOffload);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NET_POWER_OFFLOAD_TYPE
NTAPI
NetPowerOffloadGetType(
    _In_ NETPOWEROFFLOAD PowerOffload)
{
    return ((PFN_NETPOWEROFFLOADGETTYPE)NetFunctions[NetPowerOffloadGetTypeTableIndex])(
        NetDriverGlobals, PowerOffload);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_NETPOWEROFFLOADGETARPPARAMETERS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPOWEROFFLOAD PowerOffload,
    _Inout_ NET_POWER_OFFLOAD_ARP_PARAMETERS *Parameters);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetPowerOffloadGetArpParameters(
    _In_ NETPOWEROFFLOAD PowerOffload,
    _Inout_ NET_POWER_OFFLOAD_ARP_PARAMETERS *Parameters)
{
    ((PFN_NETPOWEROFFLOADGETARPPARAMETERS)
        NetFunctions[NetPowerOffloadGetArpParametersTableIndex])(
            NetDriverGlobals, PowerOffload, Parameters);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_NETPOWEROFFLOADGETNSPARAMETERS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPOWEROFFLOAD PowerOffload,
    _Inout_ NET_POWER_OFFLOAD_NS_PARAMETERS *Parameters);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetPowerOffloadGetNSParameters(
    _In_ NETPOWEROFFLOAD PowerOffload,
    _Inout_ NET_POWER_OFFLOAD_NS_PARAMETERS *Parameters)
{
    ((PFN_NETPOWEROFFLOADGETNSPARAMETERS)
        NetFunctions[NetPowerOffloadGetNSParametersTableIndex])(
            NetDriverGlobals, PowerOffload, Parameters);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NETADAPTER
(NTAPI *PFN_NETPOWEROFFLOADGETADAPTER)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPOWEROFFLOAD PowerOffload);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
NETADAPTER
NTAPI
NetPowerOffloadGetAdapter(
    _In_ NETPOWEROFFLOAD PowerOffload)
{
    return ((PFN_NETPOWEROFFLOADGETADAPTER)NetFunctions[NetPowerOffloadGetAdapterTableIndex])(
        NetDriverGlobals, PowerOffload);
}

#ifdef __cplusplus
}
#endif
