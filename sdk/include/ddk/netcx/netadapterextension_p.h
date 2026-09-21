/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Adapter extension interface
 *
 * An extension driver, such as a media specific class extension, sits between
 * NetAdapterCx and the client driver. It is handed a NETADAPTEREXT_INIT while
 * the adapter is being set up and can intercept OID requests from there.
 */

#pragma once

#include <netcx/netadapter.h>

#ifdef __cplusplus
extern "C" {
#endif

struct _NETADAPTEREXT_INIT;
typedef struct _NETADAPTEREXT_INIT NETADAPTEREXT_INIT;

typedef
_Function_class_(EVT_NET_ADAPTER_PRE_PROCESS_OID_REQUEST)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_NET_ADAPTER_PRE_PROCESS_OID_REQUEST(
    _In_ NETADAPTER Adapter,
    _Inout_ NDIS_OID_REQUEST *Request,
    _In_ WDFCONTEXT Context);

typedef EVT_NET_ADAPTER_PRE_PROCESS_OID_REQUEST *PFN_NET_ADAPTER_PRE_PROCESS_OID_REQUEST;

typedef
_Function_class_(EVT_NET_ADAPTER_PRE_PROCESS_DIRECT_OID_REQUEST)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_NET_ADAPTER_PRE_PROCESS_DIRECT_OID_REQUEST(
    _In_ NETADAPTER Adapter,
    _Inout_ NDIS_OID_REQUEST *Request,
    _In_ WDFCONTEXT Context);

typedef EVT_NET_ADAPTER_PRE_PROCESS_DIRECT_OID_REQUEST
    *PFN_NET_ADAPTER_PRE_PROCESS_DIRECT_OID_REQUEST;

/* Picks the transmit queue a frame for a given Wi-Fi peer goes to. */
typedef
_Function_class_(EVT_NET_ADAPTER_TX_PEER_DEMUX)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
SIZE_T
NTAPI
EVT_NET_ADAPTER_TX_PEER_DEMUX(
    _In_ NETADAPTER Adapter,
    _In_ NET_EUI48_ADDRESS const *Address);

typedef EVT_NET_ADAPTER_TX_PEER_DEMUX *PFN_NET_ADAPTER_TX_PEER_DEMUX;

typedef
_Function_class_(EVT_NET_ADAPTER_UPDATE_NDIS_PM_PARAMETERS)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_NET_ADAPTER_UPDATE_NDIS_PM_PARAMETERS(
    _In_ NETADAPTER Adapter,
    _Inout_ NDIS_PM_PARAMETERS *PmParameters);

typedef EVT_NET_ADAPTER_UPDATE_NDIS_PM_PARAMETERS *PFN_NET_ADAPTER_UPDATE_NDIS_PM_PARAMETERS;

typedef struct _NET_ADAPTER_EXTENSION_POWER_POLICY_CALLBACKS
{
    ULONG Size;
    PFN_NET_ADAPTER_UPDATE_NDIS_PM_PARAMETERS EvtUpdateNdisPmParameters;
} NET_ADAPTER_EXTENSION_POWER_POLICY_CALLBACKS;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_EXTENSION_POWER_POLICY_CALLBACKS_INIT(
    _Out_ NET_ADAPTER_EXTENSION_POWER_POLICY_CALLBACKS *Callbacks)
{
    RtlZeroMemory(Callbacks, sizeof(*Callbacks));

    Callbacks->Size = sizeof(*Callbacks);
}

/* Wake capabilities the extension reports on top of the client driver's. */
typedef struct _NET_ADAPTER_NDIS_PM_CAPABILITIES
{
    ULONG Size;
    ULONG MediaSpecificWakeUpEvents;
    ULONG SupportedProtocolOffloads;
} NET_ADAPTER_NDIS_PM_CAPABILITIES;

FORCEINLINE
VOID
NTAPI
NET_ADAPTER_NDIS_PM_CAPABILITIES_INIT(
    _Out_ NET_ADAPTER_NDIS_PM_CAPABILITIES *Capabilities)
{
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));

    Capabilities->Size = sizeof(*Capabilities);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NETADAPTER
(NTAPI *PFN_NETADAPTERINITGETCREATEDADAPTER)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER_INIT *AdapterInit);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NETADAPTER
NTAPI
NetAdapterInitGetCreatedAdapter(
    _In_ NETADAPTER_INIT *AdapterInit)
{
    return ((PFN_NETADAPTERINITGETCREATEDADAPTER)NetFunctions[NetAdapterInitGetCreatedAdapterTableIndex])(
        NetDriverGlobals, AdapterInit);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NETADAPTEREXT_INIT *
(NTAPI *PFN_NETADAPTEREXTENSIONINITALLOCATE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER_INIT *AdapterInit);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NETADAPTEREXT_INIT *
NTAPI
NetAdapterExtensionInitAllocate(
    _In_ NETADAPTER_INIT *AdapterInit)
{
    return ((PFN_NETADAPTEREXTENSIONINITALLOCATE)NetFunctions[NetAdapterExtensionInitAllocateTableIndex])(
        NetDriverGlobals, AdapterInit);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_NETADAPTEREXTENSIONINITSETOIDREQUESTPREPROCESSCALLBACK)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ PFN_NET_ADAPTER_PRE_PROCESS_OID_REQUEST PreprocessOidRequest);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterExtensionInitSetOidRequestPreprocessCallback(
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ PFN_NET_ADAPTER_PRE_PROCESS_OID_REQUEST PreprocessOidRequest)
{
    ((PFN_NETADAPTEREXTENSIONINITSETOIDREQUESTPREPROCESSCALLBACK)NetFunctions[NetAdapterExtensionInitSetOidRequestPreprocessCallbackTableIndex])(
        NetDriverGlobals, AdapterExtensionInit, PreprocessOidRequest);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_NETADAPTEREXTENSIONINITSETNDISPMCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ NET_ADAPTER_NDIS_PM_CAPABILITIES const *Capabilities);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterExtensionInitSetNdisPmCapabilities(
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ NET_ADAPTER_NDIS_PM_CAPABILITIES const *Capabilities)
{
    ((PFN_NETADAPTEREXTENSIONINITSETNDISPMCAPABILITIES)NetFunctions[NetAdapterExtensionInitSetNdisPmCapabilitiesTableIndex])(
        NetDriverGlobals, AdapterExtensionInit, Capabilities);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_NETADAPTERDISPATCHPREPROCESSEDOIDREQUEST)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NDIS_OID_REQUEST *Request,
    _In_ WDFCONTEXT Context);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterDispatchPreprocessedOidRequest(
    _In_ NETADAPTER Adapter,
    _In_ NDIS_OID_REQUEST *Request,
    _In_ WDFCONTEXT Context)
{
    ((PFN_NETADAPTERDISPATCHPREPROCESSEDOIDREQUEST)NetFunctions[NetAdapterDispatchPreprocessedOidRequestTableIndex])(
        NetDriverGlobals, Adapter, Request, Context);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFOBJECT
(NTAPI *PFN_NETADAPTERGETPARENT)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
WDFOBJECT
NTAPI
NetAdapterGetParent(
    _In_ NETADAPTER Adapter)
{
    return ((PFN_NETADAPTERGETPARENT)NetFunctions[NetAdapterGetParentTableIndex])(
        NetDriverGlobals, Adapter);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
ULONG
(NTAPI *PFN_NETADAPTERGETLINKLAYERMTUSIZE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
ULONG
NTAPI
NetAdapterGetLinkLayerMtuSize(
    _In_ NETADAPTER Adapter)
{
    return ((PFN_NETADAPTERGETLINKLAYERMTUSIZE)NetFunctions[NetAdapterGetLinkLayerMtuSizeTableIndex])(
        NetDriverGlobals, Adapter);
}

#ifdef __cplusplus
}
#endif
