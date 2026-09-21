/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     List of armed wake sources
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_WAKE_SOURCE_LIST
{
    ULONG Size;
    PVOID Reserved[4];
} NET_WAKE_SOURCE_LIST;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_NETDEVICEGETWAKE_SOURCELIST)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _Inout_ NET_WAKE_SOURCE_LIST *List);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetDeviceGetWakeSourceList(
    _In_ WDFDEVICE Device,
    _Inout_ NET_WAKE_SOURCE_LIST *List)
{
    ((PFN_NETDEVICEGETWAKE_SOURCELIST)NetFunctions[NetDeviceGetWakeSourceListTableIndex])(
        NetDriverGlobals, Device, List);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
SIZE_T
(NTAPI *PFN_NETWAKE_SOURCELISTGETCOUNT)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NET_WAKE_SOURCE_LIST const *List);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
SIZE_T
NTAPI
NetWakeSourceListGetCount(
    _In_ NET_WAKE_SOURCE_LIST const *List)
{
    return ((PFN_NETWAKE_SOURCELISTGETCOUNT)NetFunctions[NetWakeSourceListGetCountTableIndex])(
        NetDriverGlobals, List);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NETWAKESOURCE
(NTAPI *PFN_NETWAKE_SOURCELISTGETELEMENT)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NET_WAKE_SOURCE_LIST const *List,
    _In_ SIZE_T Index);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NETWAKESOURCE
NTAPI
NetWakeSourceListGetElement(
    _In_ NET_WAKE_SOURCE_LIST const *List,
    _In_ SIZE_T Index)
{
    return ((PFN_NETWAKE_SOURCELISTGETELEMENT)NetFunctions[NetWakeSourceListGetElementTableIndex])(
        NetDriverGlobals, List, Index);
}

#ifdef __cplusplus
}
#endif
