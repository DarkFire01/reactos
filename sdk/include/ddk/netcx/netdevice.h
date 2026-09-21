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

typedef
_Function_class_(EVT_NET_DEVICE_COLLECT_RESET_DIAGNOSTICS)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_NET_DEVICE_COLLECT_RESET_DIAGNOSTICS(
    _In_ WDFDEVICE Device);

typedef EVT_NET_DEVICE_COLLECT_RESET_DIAGNOSTICS *PFN_NET_DEVICE_COLLECT_RESET_DIAGNOSTICS;

/* The largest blob NetDeviceStoreResetDiagnostics keeps. */
#define MAX_RESET_DIAGNOSTICS_SIZE  0x100000

/* The diagnostics GUID tags the blob the driver hands to NetDeviceStoreResetDiagnostics. */
typedef struct _NET_DEVICE_RESET_CAPABILITIES
{
    ULONG Size;
    GUID ResetDiagnosticsGuid;
    PFN_NET_DEVICE_COLLECT_RESET_DIAGNOSTICS EvtNetDeviceCollectResetDiagnostics;
} NET_DEVICE_RESET_CAPABILITIES;

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETDEVICEINITSETRESETCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ PWDFDEVICE_INIT DeviceInit,
    _In_ CONST NET_DEVICE_RESET_CAPABILITIES *ResetCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetDeviceInitSetResetCapabilities(
    _Inout_ PWDFDEVICE_INIT DeviceInit,
    _In_ CONST NET_DEVICE_RESET_CAPABILITIES *ResetCapabilities)
{
    ((PFN_NETDEVICEINITSETRESETCAPABILITIES)NetFunctions[NetDeviceInitSetResetCapabilitiesTableIndex])(
        NetDriverGlobals, DeviceInit, ResetCapabilities);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETDEVICESTORERESETDIAGNOSTICS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ SIZE_T ResetDiagnosticsSize,
    _In_reads_bytes_(ResetDiagnosticsSize) CONST UINT8 *ResetDiagnosticsBuffer);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetDeviceStoreResetDiagnostics(
    _In_ WDFDEVICE Device,
    _In_ SIZE_T ResetDiagnosticsSize,
    _In_reads_bytes_(ResetDiagnosticsSize) CONST UINT8 *ResetDiagnosticsBuffer)
{
    ((PFN_NETDEVICESTORERESETDIAGNOSTICS)NetFunctions[NetDeviceStoreResetDiagnosticsTableIndex])(
        NetDriverGlobals, Device, ResetDiagnosticsSize, ResetDiagnosticsBuffer);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETDEVICEREQUESTRESET)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetDeviceRequestReset(
    _In_ WDFDEVICE Device)
{
    ((PFN_NETDEVICEREQUESTRESET)NetFunctions[NetDeviceRequestResetTableIndex])(
        NetDriverGlobals, Device);
}

#ifdef __cplusplus
}
#endif
