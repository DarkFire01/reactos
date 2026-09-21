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

/* Same as above, but the extension reports whether it consumed the request. */
typedef
_Function_class_(EVT_NETEX_ADAPTER_PREPROCESS_DIRECT_OID)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
NTAPI
EVT_NETEX_ADAPTER_PREPROCESS_DIRECT_OID(
    _In_ NETADAPTER Adapter,
    _Inout_ NDIS_OID_REQUEST *Request,
    _In_ WDFCONTEXT Context);

typedef EVT_NETEX_ADAPTER_PREPROCESS_DIRECT_OID *PFN_NETEX_ADAPTER_PREPROCESS_DIRECT_OID;

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

/* Registers a driver as an extension; only some may create adapters themselves. */
typedef struct _NET_DRIVER_EXTENSION_CONFIG
{
    ULONG Size;
    BOOLEAN AllowNetAdapterCreation;
} NET_DRIVER_EXTENSION_CONFIG;

FORCEINLINE
VOID
NTAPI
NET_DRIVER_EXTENSION_CONFIG_INIT(
    _Out_ NET_DRIVER_EXTENSION_CONFIG *Config)
{
    RtlZeroMemory(Config, sizeof(*Config));
    Config->Size = sizeof(*Config);
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

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETDRIVEREXTENSIONINITIALIZE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDRIVER Driver,
    _In_ CONST NET_DRIVER_EXTENSION_CONFIG *Config);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetDriverExtensionInitialize(
    _In_ WDFDRIVER Driver,
    _In_ CONST NET_DRIVER_EXTENSION_CONFIG *Config)
{
    return ((PFN_NETDRIVEREXTENSIONINITIALIZE)NetFunctions[NetDriverExtensionInitializeTableIndex])(
        NetDriverGlobals, Driver, Config);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEREXTENSIONINITSETDIRECTOIDREQUESTPREPROCESSCALLBACK)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ PFN_NET_ADAPTER_PRE_PROCESS_DIRECT_OID_REQUEST PreprocessDirectOidRequest);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterExtensionInitSetDirectOidRequestPreprocessCallback(
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ PFN_NET_ADAPTER_PRE_PROCESS_DIRECT_OID_REQUEST PreprocessDirectOidRequest)
{
    ((PFN_NETADAPTEREXTENSIONINITSETDIRECTOIDREQUESTPREPROCESSCALLBACK)NetFunctions[NetAdapterExtensionInitSetDirectOidRequestPreprocessCallbackTableIndex])(
        NetDriverGlobals, AdapterExtensionInit, PreprocessDirectOidRequest);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETEXADAPTERINITSETDIRECTOIDPREPROCESSCALLBACK)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ PFN_NETEX_ADAPTER_PREPROCESS_DIRECT_OID PreprocessDirectOid);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetExAdapterInitSetDirectOidPreprocessCallback(
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ PFN_NETEX_ADAPTER_PREPROCESS_DIRECT_OID PreprocessDirectOid)
{
    ((PFN_NETEXADAPTERINITSETDIRECTOIDPREPROCESSCALLBACK)NetFunctions[NetExAdapterInitSetDirectOidPreprocessCallbackTableIndex])(
        NetDriverGlobals, AdapterExtensionInit, PreprocessDirectOid);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEREXTENSIONINITSETTXPEERDEMUXCALLBACK)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ PFN_NET_ADAPTER_TX_PEER_DEMUX TxPeerDemux);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterExtensionInitSetTxPeerDemuxCallback(
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ PFN_NET_ADAPTER_TX_PEER_DEMUX TxPeerDemux)
{
    ((PFN_NETADAPTEREXTENSIONINITSETTXPEERDEMUXCALLBACK)NetFunctions[NetAdapterExtensionInitSetTxPeerDemuxCallbackTableIndex])(
        NetDriverGlobals, AdapterExtensionInit, TxPeerDemux);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTERDISPATCHPREPROCESSEDDIRECTOIDREQUEST)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ PNDIS_OID_REQUEST Request,
    _In_ WDFCONTEXT Context);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterDispatchPreprocessedDirectOidRequest(
    _In_ NETADAPTER Adapter,
    _In_ PNDIS_OID_REQUEST Request,
    _In_ WDFCONTEXT Context)
{
    ((PFN_NETADAPTERDISPATCHPREPROCESSEDDIRECTOIDREQUEST)NetFunctions[NetAdapterDispatchPreprocessedDirectOidRequestTableIndex])(
        NetDriverGlobals, Adapter, Request, Context);
}

typedef
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETEXADAPTERDISPATCHPREPROCESSEDDIRECTOID)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ PNDIS_OID_REQUEST Request,
    _In_ WDFCONTEXT Context);

_IRQL_requires_max_(DISPATCH_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetExAdapterDispatchPreprocessedDirectOid(
    _In_ NETADAPTER Adapter,
    _In_ PNDIS_OID_REQUEST Request,
    _In_ WDFCONTEXT Context)
{
    return ((PFN_NETEXADAPTERDISPATCHPREPROCESSEDDIRECTOID)NetFunctions[NetExAdapterDispatchPreprocessedDirectOidTableIndex])(
        NetDriverGlobals, Adapter, Request, Context);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEREXTENSIONSETNDISPMCAPABILITIES)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_NDIS_PM_CAPABILITIES *Capabilities);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterExtensionSetNdisPmCapabilities(
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_NDIS_PM_CAPABILITIES *Capabilities)
{
    ((PFN_NETADAPTEREXTENSIONSETNDISPMCAPABILITIES)NetFunctions[NetAdapterExtensionSetNdisPmCapabilitiesTableIndex])(
        NetDriverGlobals, Adapter, Capabilities);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
(NTAPI *PFN_NETADAPTERINITALLOCATECONTEXT)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETADAPTER_INIT *AdapterInit,
    _In_ PWDF_OBJECT_ATTRIBUTES Attributes,
    _Outptr_opt_ PVOID *Context);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetAdapterInitAllocateContext(
    _Inout_ NETADAPTER_INIT *AdapterInit,
    _In_ PWDF_OBJECT_ATTRIBUTES Attributes,
    _Outptr_opt_ PVOID *Context)
{
    return ((PFN_NETADAPTERINITALLOCATECONTEXT)NetFunctions[NetAdapterInitAllocateContextTableIndex])(
        NetDriverGlobals, AdapterInit, Attributes, Context);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
PVOID
(NTAPI *PFN_NETADAPTERINITGETTYPEDCONTEXTWORKER)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER_INIT *AdapterInit,
    _In_ PCWDF_OBJECT_CONTEXT_TYPE_INFO TypeInfo);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
PVOID
NTAPI
NetAdapterInitGetTypedContextWorker(
    _In_ NETADAPTER_INIT *AdapterInit,
    _In_ PCWDF_OBJECT_CONTEXT_TYPE_INFO TypeInfo)
{
    return ((PFN_NETADAPTERINITGETTYPEDCONTEXTWORKER)NetFunctions[NetAdapterInitGetTypedContextWorkerTableIndex])(
        NetDriverGlobals, AdapterInit, TypeInfo);
}

typedef
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
(NTAPI *PFN_NETADAPTEREXTENSIONINITSETPOWERPOLICYCALLBACKS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ NET_ADAPTER_EXTENSION_POWER_POLICY_CALLBACKS *Callbacks);

_IRQL_requires_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetAdapterExtensionInitSetPowerPolicyCallbacks(
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ NET_ADAPTER_EXTENSION_POWER_POLICY_CALLBACKS *Callbacks)
{
    ((PFN_NETADAPTEREXTENSIONINITSETPOWERPOLICYCALLBACKS)NetFunctions[NetAdapterExtensionInitSetPowerPolicyCallbacksTableIndex])(
        NetDriverGlobals, AdapterExtensionInit, Callbacks);
}

#ifdef __cplusplus
}
#endif
