/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     List of armed power offloads
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_POWER_OFFLOAD_LIST
{
    ULONG Size;
    PVOID Reserved[4];
} NET_POWER_OFFLOAD_LIST;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_NETDEVICEGETPOWER_OFFLOADLIST)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _Inout_ NET_POWER_OFFLOAD_LIST *List);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetDeviceGetPowerOffloadList(
    _In_ WDFDEVICE Device,
    _Inout_ NET_POWER_OFFLOAD_LIST *List)
{
    ((PFN_NETDEVICEGETPOWER_OFFLOADLIST)NetFunctions[NetDeviceGetPowerOffloadListTableIndex])(
        NetDriverGlobals, Device, List);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
SIZE_T
(NTAPI *PFN_NETPOWER_OFFLOADLISTGETCOUNT)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NET_POWER_OFFLOAD_LIST const *List);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
SIZE_T
NTAPI
NetPowerOffloadListGetCount(
    _In_ NET_POWER_OFFLOAD_LIST const *List)
{
    return ((PFN_NETPOWER_OFFLOADLISTGETCOUNT)NetFunctions[NetPowerOffloadListGetCountTableIndex])(
        NetDriverGlobals, List);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NETPOWEROFFLOAD
(NTAPI *PFN_NETPOWER_OFFLOADLISTGETELEMENT)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NET_POWER_OFFLOAD_LIST const *List,
    _In_ SIZE_T Index);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NETPOWEROFFLOAD
NTAPI
NetPowerOffloadListGetElement(
    _In_ NET_POWER_OFFLOAD_LIST const *List,
    _In_ SIZE_T Index)
{
    return ((PFN_NETPOWER_OFFLOADLISTGETELEMENT)NetFunctions[NetPowerOffloadListGetElementTableIndex])(
        NetDriverGlobals, List, Index);
}

#ifdef __cplusplus
}
#endif
