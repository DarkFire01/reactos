/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Device side of a NetAdapterCx driver
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The preview callbacks let a driver reject a wake source or power offload
 * the framework is about to arm.
 */
typedef
_Function_class_(EVT_NET_DEVICE_PREVIEW_WAKE_SOURCE)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EVT_NET_DEVICE_PREVIEW_WAKE_SOURCE(
    _In_ WDFDEVICE Device,
    _In_ NETWAKESOURCE WakeSource);

typedef EVT_NET_DEVICE_PREVIEW_WAKE_SOURCE *PFN_NET_DEVICE_PREVIEW_WAKE_SOURCE;

typedef
_Function_class_(EVT_NET_DEVICE_PREVIEW_POWER_OFFLOAD)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
EVT_NET_DEVICE_PREVIEW_POWER_OFFLOAD(
    _In_ WDFDEVICE Device,
    _In_ NETPOWEROFFLOAD PowerOffload);

typedef EVT_NET_DEVICE_PREVIEW_POWER_OFFLOAD *PFN_NET_DEVICE_PREVIEW_POWER_OFFLOAD;

typedef struct _NET_DEVICE_POWER_POLICY_EVENT_CALLBACKS
{
    ULONG Size;
    PFN_NET_DEVICE_PREVIEW_WAKE_SOURCE EvtDevicePreviewBitmapPattern;
    PFN_NET_DEVICE_PREVIEW_POWER_OFFLOAD EvtDevicePreviewArpOffload;
    PFN_NET_DEVICE_PREVIEW_POWER_OFFLOAD EvtDevicePreviewNSOffload;
} NET_DEVICE_POWER_POLICY_EVENT_CALLBACKS;

FORCEINLINE
VOID
NTAPI
NET_DEVICE_POWER_POLICY_EVENT_CALLBACKS_INIT(
    _Out_ NET_DEVICE_POWER_POLICY_EVENT_CALLBACKS *Callbacks)
{
    RtlZeroMemory(Callbacks, sizeof(*Callbacks));

    Callbacks->Size = sizeof(*Callbacks);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETDEVICEINITCONFIG)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ PWDFDEVICE_INIT DeviceInit);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetDeviceInitConfig(
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    return ((PFN_NETDEVICEINITCONFIG)NetFunctions[NetDeviceInitConfigTableIndex])(
        NetDriverGlobals, DeviceInit);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETDEVICEOPENCONFIGURATION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *ConfigurationAttributes,
    _Out_ NETCONFIGURATION *Configuration);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetDeviceOpenConfiguration(
    _In_ WDFDEVICE Device,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *ConfigurationAttributes,
    _Out_ NETCONFIGURATION *Configuration)
{
    return ((PFN_NETDEVICEOPENCONFIGURATION)
        NetFunctions[NetDeviceOpenConfigurationTableIndex])(
            NetDriverGlobals, Device, ConfigurationAttributes, Configuration);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_NETDEVICEINITSETPOWERPOLICYEVENTCALLBACKS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ const NET_DEVICE_POWER_POLICY_EVENT_CALLBACKS *Callbacks);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetDeviceInitSetPowerPolicyEventCallbacks(
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ const NET_DEVICE_POWER_POLICY_EVENT_CALLBACKS *Callbacks)
{
    ((PFN_NETDEVICEINITSETPOWERPOLICYEVENTCALLBACKS)
        NetFunctions[NetDeviceInitSetPowerPolicyEventCallbacksTableIndex])(
            NetDriverGlobals, DeviceInit, Callbacks);
}

#ifdef __cplusplus
}
#endif
