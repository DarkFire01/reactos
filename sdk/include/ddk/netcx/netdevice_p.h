/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private view of the device side of a NetAdapterCx driver
 */

#pragma once

#include <netcx/netdevice.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef
_Function_class_(EVT_NET_DEVICE_RESET)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_NET_DEVICE_RESET(
    _In_ WDFDEVICE Device);

typedef EVT_NET_DEVICE_RESET *PFN_NET_DEVICE_RESET;

typedef struct _NET_DEVICE_RESET_CONFIG
{
    ULONG Size;
    PFN_NET_DEVICE_RESET EvtDeviceReset;
} NET_DEVICE_RESET_CONFIG;

FORCEINLINE
VOID
NTAPI
NET_DEVICE_RESET_CONFIG_INIT(
    _Out_ NET_DEVICE_RESET_CONFIG *ResetConfig,
    _In_ PFN_NET_DEVICE_RESET EvtDeviceReset)
{
    RtlZeroMemory(ResetConfig, sizeof(*ResetConfig));

    ResetConfig->Size = sizeof(*ResetConfig);
    ResetConfig->EvtDeviceReset = EvtDeviceReset;
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_NETDEVICEINITSETRESETCONFIG)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ NET_DEVICE_RESET_CONFIG *ResetConfig);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetDeviceInitSetResetConfig(
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ NET_DEVICE_RESET_CONFIG *ResetConfig)
{
    ((PFN_NETDEVICEINITSETRESETCONFIG)NetFunctions[NetDeviceInitSetResetConfigTableIndex])(
        NetDriverGlobals, DeviceInit, ResetConfig);
}

typedef
_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETDEVICEASSIGNSUPPORTEDOIDLIST)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_reads_(SupportedOidsCount) NDIS_OID const *SupportedOids,
    _In_ SIZE_T SupportedOidsCount);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetDeviceAssignSupportedOidList(
    _In_ WDFDEVICE Device,
    _In_reads_(SupportedOidsCount) NDIS_OID const *SupportedOids,
    _In_ SIZE_T SupportedOidsCount)
{
    return ((PFN_NETDEVICEASSIGNSUPPORTEDOIDLIST)NetFunctions[NetDeviceAssignSupportedOidListTableIndex])(
        NetDriverGlobals, Device, SupportedOids, SupportedOidsCount);
}

#ifdef __cplusplus
}
#endif
