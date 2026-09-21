/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Entry points a client driver reaches through the function table
 *
 * Declared in table order, which is the order of NETFUNCENUM. The table
 * itself is emitted once, into the file that defines
 * NX_DYNAMICS_GENERATE_TABLE, and every slot is typed after the routine it
 * holds so the whole table is a constant initializer.
 */

#pragma once

#define NETEXPORT(Name) imp_##Name

struct _NDIS_MINIPORT_ADAPTER_NATIVE_802_11_ATTRIBUTES;

extern "C" {

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NETADAPTER_INIT *
NTAPI
NETEXPORT(NetAdapterInitAllocate)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ WDFDEVICE Device);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterInitFree)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER_INIT *AdapterInit);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterInitSetDatapathCallbacks)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _Inout_ NETADAPTER_INIT *AdapterInit,
    _In_ NET_ADAPTER_DATAPATH_CALLBACKS *DatapathCallbacks);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetAdapterCreate)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER_INIT *AdapterInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *AdapterAttributes,
    _Out_ NETADAPTER *Adapter);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetAdapterStart)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterStop)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterSetLinkLayerCapabilities)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_CAPABILITIES *LinkLayerCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterSetLinkLayerMtuSize)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ ULONG MtuSize);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterPowerOffloadSetArpCapabilities)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_POWER_OFFLOAD_ARP_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterPowerOffloadSetNSCapabilities)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_POWER_OFFLOAD_NS_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterWakeSetBitmapCapabilities)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_WAKE_BITMAP_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterWakeSetMediaChangeCapabilities)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_WAKE_MEDIA_CHANGE_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterWakeSetMagicPacketCapabilities)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_WAKE_MAGIC_PACKET_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterWakeSetPacketFilterCapabilities)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_WAKE_PACKET_FILTER_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterSetDataPathCapabilities)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_TX_CAPABILITIES *TxCapabilities,
    _In_ NET_ADAPTER_RX_CAPABILITIES *RxCapabilities);

_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterSetLinkState)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_STATE *State);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NET_LUID
NTAPI
NETEXPORT(NetAdapterGetNetLuid)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ NETADAPTER Adapter);

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetAdapterOpenConfiguration)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ NETADAPTER Adapter,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *ConfigurationAttributes,
    _Out_ NETCONFIGURATION *Configuration);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterSetPermanentLinkLayerAddress)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS *LinkLayerAddress);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterSetCurrentLinkLayerAddress)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_LINK_LAYER_ADDRESS *LinkLayerAddress);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterOffloadSetChecksumCapabilities)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_CHECKSUM_CAPABILITIES *HardwareCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsChecksumIPv4Enabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsChecksumTcpEnabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsChecksumUdpEnabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterReportWakeReasonPacket)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_WAKE_REASON_PACKET *Reason);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterReportWakeReasonMediaChange)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_IF_MEDIA_CONNECT_STATE Reason);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NETADAPTER
NTAPI
NETEXPORT(NetAdapterInitGetCreatedAdapter)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER_INIT *AdapterInit);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NETADAPTEREXT_INIT *
NTAPI
NETEXPORT(NetAdapterExtensionInitAllocate)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER_INIT *AdapterInit);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterExtensionInitSetOidRequestPreprocessCallback)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ PFN_NET_ADAPTER_PRE_PROCESS_OID_REQUEST PreprocessOidRequest);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterDispatchPreprocessedOidRequest)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NDIS_OID_REQUEST *Request,
    _In_ WDFCONTEXT Context);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
WDFOBJECT
NTAPI
NETEXPORT(NetAdapterGetParent)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
ULONG
NTAPI
NETEXPORT(NetAdapterGetLinkLayerMtuSize)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterExtensionInitSetNdisPmCapabilities)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ CONST NET_ADAPTER_NDIS_PM_CAPABILITIES *Capabilities);

WDFAPI
NDIS_HANDLE
NTAPI
NETEXPORT(NetAdapterWdmGetNdisHandle)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
NDIS_HANDLE
NTAPI
NETEXPORT(NetAdapterDriverWdmGetHandle)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ WDFDRIVER Driver);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetConfigurationClose)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ NETCONFIGURATION Configuration);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetConfigurationOpenSubConfiguration)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING SubConfigurationName,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *SubConfigurationAttributes,
    _Out_ NETCONFIGURATION *SubConfiguration);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetConfigurationQueryUlong)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ NETCONFIGURATION Configuration,
    _In_ NET_CONFIGURATION_QUERY_ULONG_FLAGS Flags,
    _In_ PCUNICODE_STRING ValueName,
    _Out_ PULONG Value);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetConfigurationQueryString)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *StringAttributes,
    _Out_ WDFSTRING *WdfString);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetConfigurationQueryMultiString)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *StringsAttributes,
    _Inout_ WDFCOLLECTION Collection);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetConfigurationQueryBinary)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _Strict_type_match_ _In_ POOL_TYPE PoolType,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *MemoryAttributes,
    _Out_ WDFMEMORY *WdfMemory);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetConfigurationQueryLinkLayerAddress)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETCONFIGURATION Configuration,
    _Out_ NET_ADAPTER_LINK_LAYER_ADDRESS *LinkLayerAddress);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetConfigurationAssignUlong)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_ ULONG Value);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetConfigurationAssignUnicodeString)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_ PCUNICODE_STRING Value);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetConfigurationAssignMultiString)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_ WDFCOLLECTION StringsCollection);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetConfigurationAssignBinary)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_reads_bytes_(BufferLength) void *Buffer,
    _In_ ULONG BufferLength);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetDeviceInitConfig)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _Inout_ WDFDEVICE_INIT *DeviceInit);

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetDeviceOpenConfiguration)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ WDFDEVICE Device,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *ConfigurationAttributes,
    _Out_ NETCONFIGURATION *Configuration);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetDeviceInitSetPowerPolicyEventCallbacks)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _Inout_ PWDFDEVICE_INIT DeviceInit,
    _In_ CONST NET_DEVICE_POWER_POLICY_EVENT_CALLBACKS *Callbacks);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetDeviceInitSetResetConfig)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ NET_DEVICE_RESET_CONFIG *ResetConfig);

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetDeviceAssignSupportedOidList)(
    _In_ NET_DRIVER_GLOBALS *Globals,
    _In_ WDFDEVICE Device,
    _In_ NDIS_OID const *SupportedOids,
    _In_ SIZE_T SupportedOidsCount);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NET_POWER_OFFLOAD_TYPE
NTAPI
NETEXPORT(NetPowerOffloadGetType)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPOWEROFFLOAD PowerOffload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetPowerOffloadGetArpParameters)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPOWEROFFLOAD PowerOffload,
    _Inout_ NET_POWER_OFFLOAD_ARP_PARAMETERS *Parameters);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetPowerOffloadGetNSParameters)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPOWEROFFLOAD PowerOffload,
    _Inout_ NET_POWER_OFFLOAD_NS_PARAMETERS *Parameters);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetDeviceGetPowerOffloadList)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _Inout_ NET_POWER_OFFLOAD_LIST *List);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
SIZE_T
NTAPI
NETEXPORT(NetPowerOffloadListGetCount)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ const NET_POWER_OFFLOAD_LIST *List);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NETPOWEROFFLOAD
NTAPI
NETEXPORT(NetPowerOffloadListGetElement)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ const NET_POWER_OFFLOAD_LIST *List,
    _In_ SIZE_T Index);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterSetReceiveScalingCapabilities)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_RECEIVE_SCALING_CAPABILITIES const *Capabilities);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetRxQueueCreate)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _Inout_ NETRXQUEUE_INIT *NetRxQueueInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *RxQueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG *Configuration,
    _Out_ NETPACKETQUEUE *RxQueue);

_IRQL_requires_max_(HIGH_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetRxQueueNotifyMoreReceivedPacketsAvailable)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETPACKETQUEUE RxQueue);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
ULONG
NTAPI
NETEXPORT(NetRxQueueInitGetQueueId)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETRXQUEUE_INIT *NetRxQueueInit);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NET_RING_COLLECTION const *
NTAPI
NETEXPORT(NetRxQueueGetRingCollection)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETPACKETQUEUE NetRxQueue);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetRxQueueGetExtension)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETPACKETQUEUE NetRxQueue,
    _In_ NET_EXTENSION_QUERY const *Query,
    _Out_ NET_EXTENSION *Extension);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetTxQueueCreate)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _Inout_ NETTXQUEUE_INIT *NetTxQueueInit,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *TxQueueAttributes,
    _In_ NET_PACKET_QUEUE_CONFIG *Configuration,
    _Out_ NETPACKETQUEUE *TxQueue);

_IRQL_requires_max_(HIGH_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetTxQueueNotifyMoreCompletedPacketsAvailable)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETPACKETQUEUE TxQueue);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
ULONG
NTAPI
NETEXPORT(NetTxQueueInitGetQueueId)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETTXQUEUE_INIT *NetTxQueueInit);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NET_RING_COLLECTION const *
NTAPI
NETEXPORT(NetTxQueueGetRingCollection)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETPACKETQUEUE NetTxQueue);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetTxQueueGetExtension)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETPACKETQUEUE NetTxQueue,
    _In_ NET_EXTENSION_QUERY const *Query,
    _Out_ NET_EXTENSION *Extension);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NET_WAKE_SOURCE_TYPE
NTAPI
NETEXPORT(NetWakeSourceGetType)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETWAKESOURCE WakeSource);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NETADAPTER
NTAPI
NETEXPORT(NetWakeSourceGetAdapter)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETWAKESOURCE WakeSource);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetWakeSourceGetBitmapParameters)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETWAKESOURCE WakeSource,
    _Inout_ NET_WAKE_SOURCE_BITMAP_PARAMETERS *Parameters);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetWakeSourceGetMediaChangeParameters)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETWAKESOURCE WakeSource,
    _Inout_ NET_WAKE_SOURCE_MEDIA_CHANGE_PARAMETERS *Parameters);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetDeviceGetWakeSourceList)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDEVICE Device,
    _Inout_ NET_WAKE_SOURCE_LIST *List);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
SIZE_T
NTAPI
NETEXPORT(NetWakeSourceListGetCount)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ const NET_WAKE_SOURCE_LIST *List);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NETWAKESOURCE
NTAPI
NETEXPORT(NetWakeSourceListGetElement)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ const NET_WAKE_SOURCE_LIST *List,
    _In_ SIZE_T Index);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterWakeSetEapolPacketCapabilities)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_WAKE_EAPOL_PACKET_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
VOID
NTAPI
NETEXPORT(NetAdapterSetReceiveFilterCapabilities)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES *ReceiveFilter);

_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
NET_PACKET_FILTER_FLAGS
NTAPI
NETEXPORT(NetReceiveFilterGetPacketFilter)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETRECEIVEFILTER ReceiveFilter);

_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
SIZE_T
NTAPI
NETEXPORT(NetReceiveFilterGetMulticastAddressCount)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETRECEIVEFILTER ReceiveFilter);

_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
NET_ADAPTER_LINK_LAYER_ADDRESS const *
NTAPI
NETEXPORT(NetReceiveFilterGetMulticastAddressList)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETRECEIVEFILTER ReceiveFilter);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetDriverExtensionInitialize)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ WDFDRIVER Driver,
    _In_ const NET_DRIVER_EXTENSION_CONFIG *Config);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterExtensionInitSetDirectOidRequestPreprocessCallback)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ PFN_NET_ADAPTER_PRE_PROCESS_DIRECT_OID_REQUEST PreprocessDirectOidRequest);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetExAdapterInitSetDirectOidPreprocessCallback)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ PFN_NETEX_ADAPTER_PREPROCESS_DIRECT_OID PreprocessDirectOid);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterExtensionInitSetTxPeerDemuxCallback)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ PFN_NET_ADAPTER_TX_PEER_DEMUX TxPeerDemux);

_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterDispatchPreprocessedDirectOidRequest)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NDIS_OID_REQUEST *Request,
    _In_ WDFCONTEXT Context);

_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetExAdapterDispatchPreprocessedDirectOid)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NDIS_OID_REQUEST *Request,
    _In_ WDFCONTEXT Context);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterExtensionSetNdisPmCapabilities)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ CONST NET_ADAPTER_NDIS_PM_CAPABILITIES *Capabilities);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetAdapterInitAllocateContext)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _Inout_ NETADAPTER_INIT *AdapterInit,
    _In_ WDF_OBJECT_ATTRIBUTES *Attributes,
    _Outptr_opt_ void ** Context);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void *
NTAPI
NETEXPORT(NetAdapterInitGetTypedContextWorker)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER_INIT *AdapterInit,
    _In_ CONST WDF_OBJECT_CONTEXT_TYPE_INFO *TypeInfo);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterOffloadSetTxChecksumCapabilities)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_OFFLOAD_TX_CHECKSUM_CAPABILITIES *HardwareCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsTxChecksumIPv4Enabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsTxChecksumTcpEnabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsTxChecksumUdpEnabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterOffloadSetRxChecksumCapabilities)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ const NET_ADAPTER_OFFLOAD_RX_CHECKSUM_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsRxChecksumIPv4Enabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsRxChecksumTcpEnabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsRxChecksumUdpEnabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterOffloadSetGsoCapabilities)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_GSO_CAPABILITIES *HardwareCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsLsoIPv4Enabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsLsoIPv6Enabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsUsoIPv4Enabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsUsoIPv6Enabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterOffloadSetRscCapabilities)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_RSC_CAPABILITIES *HardwareCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsTcpRscIPv4Enabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsTcpRscIPv6Enabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsRscTcpTimestampOptionEnabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterOffloadSetIeee8021qTagCapabilities)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_ADAPTER_OFFLOAD_IEEE8021Q_TAG_CAPABILITIES *HardwareCapabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterInitAddTxDemux)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER_INIT *AdapterInit,
    _In_ NET_ADAPTER_TX_DEMUX const *Demux);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NET_ADAPTER_TX_DEMUX const *
NTAPI
NETEXPORT(NetAdapterGetTxPeerAddressDemux)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetBufferQueueCreate)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETEXECUTIONCONTEXT ExecutionContext,
    _In_ NET_BUFFER_QUEUE_CONFIG const *Config,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *Attributes,
    _Outptr_ NETBUFFERQUEUE *BufferQueue);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetBufferQueueGetExtension)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETBUFFERQUEUE BufferQueue,
    _In_ NET_EXTENSION_QUERY const *Query,
    _Out_ NET_EXTENSION *Extension);

_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
NET_RING *
NTAPI
NETEXPORT(NetBufferQueueGetRing)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETBUFFERQUEUE BufferQueue);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetDeviceInitSetResetCapabilities)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _Inout_ PWDFDEVICE_INIT DeviceInit,
    _In_ const NET_DEVICE_RESET_CAPABILITIES *Capabilities);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetDeviceStoreResetDiagnostics)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ SIZE_T ResetDiagnosticsSize,
    _In_reads_bytes_(ResetDiagnosticsSize) const UINT8 *ResetDiagnosticsBuffer);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetDeviceRequestReset)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ WDFDEVICE Device);

_Must_inspect_result_
_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetExecutionContextCreate)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_ NET_EXECUTION_CONTEXT_CONFIG const *Config,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *ClientAttributes,
    _Out_ NETEXECUTIONCONTEXT *ExecutionContext);

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetExecutionContextTaskCreate)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETEXECUTIONCONTEXT NetExecutionContextHandle,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *ClientAttributes,
    _In_ NET_EXECUTION_CONTEXT_TASK_CONFIG *NetExecutionContextTaskConfig,
    _Out_ NETEXECUTIONCONTEXTTASK *NetExecutionContextTaskHandle);

_IRQL_requires_max_(HIGH_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetExecutionContextNotify)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETEXECUTIONCONTEXT ExecutionContext);

_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetExecutionContextTaskEnqueue)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETEXECUTIONCONTEXTTASK NetExecutionContextTaskHandle);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetExecutionContextTaskWaitCompletion)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _Inout_ NETEXECUTIONCONTEXTTASK NetExecutionContextTaskHandle);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NETADAPTER
NTAPI
NETEXPORT(NetPowerOffloadGetAdapter)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETPOWEROFFLOAD PowerOffload);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
UINT8
NTAPI
NETEXPORT(NetTxQueueGetDemux8021p)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETPACKETQUEUE Queue);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterExtensionInitSetPowerPolicyCallbacks)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _Inout_ NETADAPTEREXT_INIT *AdapterExtensionInit,
    _In_ NET_ADAPTER_EXTENSION_POWER_POLICY_CALLBACKS *Callbacks);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterInitSetSelfManagedPowerReferences)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER_INIT *AdapterInit,
    BOOLEAN SelfManagedPowerReference);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NETADAPTER_INIT *
NTAPI
NETEXPORT(NetAdapterLightweightInitAllocate)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ GUID const *NetLuidGuid);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterWifiDestroyPeerAddressDatapath)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ SIZE_T Demux);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterPauseOffloadCapabilities)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterResumeOffloadCapabilities)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NET_WAKE_REASON_TYPE
NTAPI
NETEXPORT(NetAdapterQueryWakeReason)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
UINT8
NTAPI
NETEXPORT(NetTxQueueGetDemuxWmmInfo)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETPACKETQUEUE Queue);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
NET_EUI48_ADDRESS
NTAPI
NETEXPORT(NetTxQueueGetDemuxPeerAddress)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETPACKETQUEUE Queue);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
BOOLEAN
NTAPI
NETEXPORT(NetOffloadIsUdpRscEnabled)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETOFFLOAD Offload);

_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterCompleteOidRequest)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NDIS_OID_REQUEST *Request,
    _In_ NDIS_STATUS Status);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetAdapterSetNative80211Attributes)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ struct _NDIS_MINIPORT_ADAPTER_NATIVE_802_11_ATTRIBUTES *Attributes);

_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterIndicateMiniportStatus)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NDIS_STATUS_INDICATION *StatusIndication);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterWifiAddPeer)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_EUI48_ADDRESS const *Address);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetAdapterWifiRemovePeer)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETADAPTER Adapter,
    _In_ NET_EUI48_ADDRESS const *Address);

_IRQL_requires_max_(PASSIVE_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetTxQueueGetDemuxPeerAddressV2)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETPACKETQUEUE Queue,
    _Out_ NET_EUI48_ADDRESS *Address);

_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
ULONG
NTAPI
NETEXPORT(NetDeviceGetSupportedDeviceResetTypes)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ WDFDEVICE Device);

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetMemoryCollectionCreate)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ WDFDEVICE Device,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *Attributes,
    _In_ NET_MEMORY_COLLECTION_CONFIG const *Config,
    _Out_ NETMEMORYCOLLECTION *MemoryCollection);

_Must_inspect_result_
_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
NTSTATUS
NTAPI
NETEXPORT(NetMemoryCreate)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETMEMORYCOLLECTION MemoryCollection,
    _In_ NET_MEMORY_CONFIG const *Config,
    _Out_ void ** Memory);

_IRQL_requires_max_(DISPATCH_LEVEL)
WDFAPI
void
NTAPI
NETEXPORT(NetMemoryDestroy)(
    _In_ NET_DRIVER_GLOBALS *DriverGlobals,
    _In_ NETMEMORYCOLLECTION MemoryCollection,
    _In_ void *Memory);

}

#ifdef NX_DYNAMICS_GENERATE_TABLE

typedef struct _NETFUNCTIONS
{
    decltype(&NETEXPORT(NetAdapterInitAllocate)) pfnNetAdapterInitAllocate;
    decltype(&NETEXPORT(NetAdapterInitFree)) pfnNetAdapterInitFree;
    decltype(&NETEXPORT(NetAdapterInitSetDatapathCallbacks)) pfnNetAdapterInitSetDatapathCallbacks;
    decltype(&NETEXPORT(NetAdapterCreate)) pfnNetAdapterCreate;
    decltype(&NETEXPORT(NetAdapterStart)) pfnNetAdapterStart;
    decltype(&NETEXPORT(NetAdapterStop)) pfnNetAdapterStop;
    decltype(&NETEXPORT(NetAdapterSetLinkLayerCapabilities)) pfnNetAdapterSetLinkLayerCapabilities;
    decltype(&NETEXPORT(NetAdapterSetLinkLayerMtuSize)) pfnNetAdapterSetLinkLayerMtuSize;
    decltype(&NETEXPORT(NetAdapterPowerOffloadSetArpCapabilities)) pfnNetAdapterPowerOffloadSetArpCapabilities;
    decltype(&NETEXPORT(NetAdapterPowerOffloadSetNSCapabilities)) pfnNetAdapterPowerOffloadSetNSCapabilities;
    decltype(&NETEXPORT(NetAdapterWakeSetBitmapCapabilities)) pfnNetAdapterWakeSetBitmapCapabilities;
    decltype(&NETEXPORT(NetAdapterWakeSetMediaChangeCapabilities)) pfnNetAdapterWakeSetMediaChangeCapabilities;
    decltype(&NETEXPORT(NetAdapterWakeSetMagicPacketCapabilities)) pfnNetAdapterWakeSetMagicPacketCapabilities;
    decltype(&NETEXPORT(NetAdapterWakeSetPacketFilterCapabilities)) pfnNetAdapterWakeSetPacketFilterCapabilities;
    decltype(&NETEXPORT(NetAdapterSetDataPathCapabilities)) pfnNetAdapterSetDataPathCapabilities;
    decltype(&NETEXPORT(NetAdapterSetLinkState)) pfnNetAdapterSetLinkState;
    decltype(&NETEXPORT(NetAdapterGetNetLuid)) pfnNetAdapterGetNetLuid;
    decltype(&NETEXPORT(NetAdapterOpenConfiguration)) pfnNetAdapterOpenConfiguration;
    decltype(&NETEXPORT(NetAdapterSetPermanentLinkLayerAddress)) pfnNetAdapterSetPermanentLinkLayerAddress;
    decltype(&NETEXPORT(NetAdapterSetCurrentLinkLayerAddress)) pfnNetAdapterSetCurrentLinkLayerAddress;
    decltype(&NETEXPORT(NetAdapterOffloadSetChecksumCapabilities)) pfnNetAdapterOffloadSetChecksumCapabilities;
    decltype(&NETEXPORT(NetOffloadIsChecksumIPv4Enabled)) pfnNetOffloadIsChecksumIPv4Enabled;
    decltype(&NETEXPORT(NetOffloadIsChecksumTcpEnabled)) pfnNetOffloadIsChecksumTcpEnabled;
    decltype(&NETEXPORT(NetOffloadIsChecksumUdpEnabled)) pfnNetOffloadIsChecksumUdpEnabled;
    decltype(&NETEXPORT(NetAdapterReportWakeReasonPacket)) pfnNetAdapterReportWakeReasonPacket;
    decltype(&NETEXPORT(NetAdapterReportWakeReasonMediaChange)) pfnNetAdapterReportWakeReasonMediaChange;
    decltype(&NETEXPORT(NetAdapterInitGetCreatedAdapter)) pfnNetAdapterInitGetCreatedAdapter;
    decltype(&NETEXPORT(NetAdapterExtensionInitAllocate)) pfnNetAdapterExtensionInitAllocate;
    decltype(&NETEXPORT(NetAdapterExtensionInitSetOidRequestPreprocessCallback)) pfnNetAdapterExtensionInitSetOidRequestPreprocessCallback;
    decltype(&NETEXPORT(NetAdapterDispatchPreprocessedOidRequest)) pfnNetAdapterDispatchPreprocessedOidRequest;
    decltype(&NETEXPORT(NetAdapterGetParent)) pfnNetAdapterGetParent;
    decltype(&NETEXPORT(NetAdapterGetLinkLayerMtuSize)) pfnNetAdapterGetLinkLayerMtuSize;
    decltype(&NETEXPORT(NetAdapterExtensionInitSetNdisPmCapabilities)) pfnNetAdapterExtensionInitSetNdisPmCapabilities;
    decltype(&NETEXPORT(NetAdapterWdmGetNdisHandle)) pfnNetAdapterWdmGetNdisHandle;
    decltype(&NETEXPORT(NetAdapterDriverWdmGetHandle)) pfnNetAdapterDriverWdmGetHandle;
    decltype(&NETEXPORT(NetConfigurationClose)) pfnNetConfigurationClose;
    decltype(&NETEXPORT(NetConfigurationOpenSubConfiguration)) pfnNetConfigurationOpenSubConfiguration;
    decltype(&NETEXPORT(NetConfigurationQueryUlong)) pfnNetConfigurationQueryUlong;
    decltype(&NETEXPORT(NetConfigurationQueryString)) pfnNetConfigurationQueryString;
    decltype(&NETEXPORT(NetConfigurationQueryMultiString)) pfnNetConfigurationQueryMultiString;
    decltype(&NETEXPORT(NetConfigurationQueryBinary)) pfnNetConfigurationQueryBinary;
    decltype(&NETEXPORT(NetConfigurationQueryLinkLayerAddress)) pfnNetConfigurationQueryLinkLayerAddress;
    decltype(&NETEXPORT(NetConfigurationAssignUlong)) pfnNetConfigurationAssignUlong;
    decltype(&NETEXPORT(NetConfigurationAssignUnicodeString)) pfnNetConfigurationAssignUnicodeString;
    decltype(&NETEXPORT(NetConfigurationAssignMultiString)) pfnNetConfigurationAssignMultiString;
    decltype(&NETEXPORT(NetConfigurationAssignBinary)) pfnNetConfigurationAssignBinary;
    decltype(&NETEXPORT(NetDeviceInitConfig)) pfnNetDeviceInitConfig;
    decltype(&NETEXPORT(NetDeviceOpenConfiguration)) pfnNetDeviceOpenConfiguration;
    decltype(&NETEXPORT(NetDeviceInitSetPowerPolicyEventCallbacks)) pfnNetDeviceInitSetPowerPolicyEventCallbacks;
    decltype(&NETEXPORT(NetDeviceInitSetResetConfig)) pfnNetDeviceInitSetResetConfig;
    decltype(&NETEXPORT(NetDeviceAssignSupportedOidList)) pfnNetDeviceAssignSupportedOidList;
    decltype(&NETEXPORT(NetPowerOffloadGetType)) pfnNetPowerOffloadGetType;
    decltype(&NETEXPORT(NetPowerOffloadGetArpParameters)) pfnNetPowerOffloadGetArpParameters;
    decltype(&NETEXPORT(NetPowerOffloadGetNSParameters)) pfnNetPowerOffloadGetNSParameters;
    decltype(&NETEXPORT(NetDeviceGetPowerOffloadList)) pfnNetDeviceGetPowerOffloadList;
    decltype(&NETEXPORT(NetPowerOffloadListGetCount)) pfnNetPowerOffloadListGetCount;
    decltype(&NETEXPORT(NetPowerOffloadListGetElement)) pfnNetPowerOffloadListGetElement;
    decltype(&NETEXPORT(NetAdapterSetReceiveScalingCapabilities)) pfnNetAdapterSetReceiveScalingCapabilities;
    decltype(&NETEXPORT(NetRxQueueCreate)) pfnNetRxQueueCreate;
    decltype(&NETEXPORT(NetRxQueueNotifyMoreReceivedPacketsAvailable)) pfnNetRxQueueNotifyMoreReceivedPacketsAvailable;
    decltype(&NETEXPORT(NetRxQueueInitGetQueueId)) pfnNetRxQueueInitGetQueueId;
    decltype(&NETEXPORT(NetRxQueueGetRingCollection)) pfnNetRxQueueGetRingCollection;
    decltype(&NETEXPORT(NetRxQueueGetExtension)) pfnNetRxQueueGetExtension;
    decltype(&NETEXPORT(NetTxQueueCreate)) pfnNetTxQueueCreate;
    decltype(&NETEXPORT(NetTxQueueNotifyMoreCompletedPacketsAvailable)) pfnNetTxQueueNotifyMoreCompletedPacketsAvailable;
    decltype(&NETEXPORT(NetTxQueueInitGetQueueId)) pfnNetTxQueueInitGetQueueId;
    decltype(&NETEXPORT(NetTxQueueGetRingCollection)) pfnNetTxQueueGetRingCollection;
    decltype(&NETEXPORT(NetTxQueueGetExtension)) pfnNetTxQueueGetExtension;
    decltype(&NETEXPORT(NetWakeSourceGetType)) pfnNetWakeSourceGetType;
    decltype(&NETEXPORT(NetWakeSourceGetAdapter)) pfnNetWakeSourceGetAdapter;
    decltype(&NETEXPORT(NetWakeSourceGetBitmapParameters)) pfnNetWakeSourceGetBitmapParameters;
    decltype(&NETEXPORT(NetWakeSourceGetMediaChangeParameters)) pfnNetWakeSourceGetMediaChangeParameters;
    decltype(&NETEXPORT(NetDeviceGetWakeSourceList)) pfnNetDeviceGetWakeSourceList;
    decltype(&NETEXPORT(NetWakeSourceListGetCount)) pfnNetWakeSourceListGetCount;
    decltype(&NETEXPORT(NetWakeSourceListGetElement)) pfnNetWakeSourceListGetElement;
    decltype(&NETEXPORT(NetAdapterWakeSetEapolPacketCapabilities)) pfnNetAdapterWakeSetEapolPacketCapabilities;
    decltype(&NETEXPORT(NetAdapterSetReceiveFilterCapabilities)) pfnNetAdapterSetReceiveFilterCapabilities;
    decltype(&NETEXPORT(NetReceiveFilterGetPacketFilter)) pfnNetReceiveFilterGetPacketFilter;
    decltype(&NETEXPORT(NetReceiveFilterGetMulticastAddressCount)) pfnNetReceiveFilterGetMulticastAddressCount;
    decltype(&NETEXPORT(NetReceiveFilterGetMulticastAddressList)) pfnNetReceiveFilterGetMulticastAddressList;
    decltype(&NETEXPORT(NetDriverExtensionInitialize)) pfnNetDriverExtensionInitialize;
    decltype(&NETEXPORT(NetAdapterExtensionInitSetDirectOidRequestPreprocessCallback)) pfnNetAdapterExtensionInitSetDirectOidRequestPreprocessCallback;
    decltype(&NETEXPORT(NetExAdapterInitSetDirectOidPreprocessCallback)) pfnNetExAdapterInitSetDirectOidPreprocessCallback;
    decltype(&NETEXPORT(NetAdapterExtensionInitSetTxPeerDemuxCallback)) pfnNetAdapterExtensionInitSetTxPeerDemuxCallback;
    decltype(&NETEXPORT(NetAdapterDispatchPreprocessedDirectOidRequest)) pfnNetAdapterDispatchPreprocessedDirectOidRequest;
    decltype(&NETEXPORT(NetExAdapterDispatchPreprocessedDirectOid)) pfnNetExAdapterDispatchPreprocessedDirectOid;
    decltype(&NETEXPORT(NetAdapterExtensionSetNdisPmCapabilities)) pfnNetAdapterExtensionSetNdisPmCapabilities;
    decltype(&NETEXPORT(NetAdapterInitAllocateContext)) pfnNetAdapterInitAllocateContext;
    decltype(&NETEXPORT(NetAdapterInitGetTypedContextWorker)) pfnNetAdapterInitGetTypedContextWorker;
    decltype(&NETEXPORT(NetAdapterOffloadSetTxChecksumCapabilities)) pfnNetAdapterOffloadSetTxChecksumCapabilities;
    decltype(&NETEXPORT(NetOffloadIsTxChecksumIPv4Enabled)) pfnNetOffloadIsTxChecksumIPv4Enabled;
    decltype(&NETEXPORT(NetOffloadIsTxChecksumTcpEnabled)) pfnNetOffloadIsTxChecksumTcpEnabled;
    decltype(&NETEXPORT(NetOffloadIsTxChecksumUdpEnabled)) pfnNetOffloadIsTxChecksumUdpEnabled;
    decltype(&NETEXPORT(NetAdapterOffloadSetRxChecksumCapabilities)) pfnNetAdapterOffloadSetRxChecksumCapabilities;
    decltype(&NETEXPORT(NetOffloadIsRxChecksumIPv4Enabled)) pfnNetOffloadIsRxChecksumIPv4Enabled;
    decltype(&NETEXPORT(NetOffloadIsRxChecksumTcpEnabled)) pfnNetOffloadIsRxChecksumTcpEnabled;
    decltype(&NETEXPORT(NetOffloadIsRxChecksumUdpEnabled)) pfnNetOffloadIsRxChecksumUdpEnabled;
    decltype(&NETEXPORT(NetAdapterOffloadSetGsoCapabilities)) pfnNetAdapterOffloadSetGsoCapabilities;
    decltype(&NETEXPORT(NetOffloadIsLsoIPv4Enabled)) pfnNetOffloadIsLsoIPv4Enabled;
    decltype(&NETEXPORT(NetOffloadIsLsoIPv6Enabled)) pfnNetOffloadIsLsoIPv6Enabled;
    decltype(&NETEXPORT(NetOffloadIsUsoIPv4Enabled)) pfnNetOffloadIsUsoIPv4Enabled;
    decltype(&NETEXPORT(NetOffloadIsUsoIPv6Enabled)) pfnNetOffloadIsUsoIPv6Enabled;
    decltype(&NETEXPORT(NetAdapterOffloadSetRscCapabilities)) pfnNetAdapterOffloadSetRscCapabilities;
    decltype(&NETEXPORT(NetOffloadIsTcpRscIPv4Enabled)) pfnNetOffloadIsTcpRscIPv4Enabled;
    decltype(&NETEXPORT(NetOffloadIsTcpRscIPv6Enabled)) pfnNetOffloadIsTcpRscIPv6Enabled;
    decltype(&NETEXPORT(NetOffloadIsRscTcpTimestampOptionEnabled)) pfnNetOffloadIsRscTcpTimestampOptionEnabled;
    decltype(&NETEXPORT(NetAdapterOffloadSetIeee8021qTagCapabilities)) pfnNetAdapterOffloadSetIeee8021qTagCapabilities;
    decltype(&NETEXPORT(NetAdapterInitAddTxDemux)) pfnNetAdapterInitAddTxDemux;
    decltype(&NETEXPORT(NetAdapterGetTxPeerAddressDemux)) pfnNetAdapterGetTxPeerAddressDemux;
    decltype(&NETEXPORT(NetBufferQueueCreate)) pfnNetBufferQueueCreate;
    decltype(&NETEXPORT(NetBufferQueueGetExtension)) pfnNetBufferQueueGetExtension;
    decltype(&NETEXPORT(NetBufferQueueGetRing)) pfnNetBufferQueueGetRing;
    decltype(&NETEXPORT(NetDeviceInitSetResetCapabilities)) pfnNetDeviceInitSetResetCapabilities;
    decltype(&NETEXPORT(NetDeviceStoreResetDiagnostics)) pfnNetDeviceStoreResetDiagnostics;
    decltype(&NETEXPORT(NetDeviceRequestReset)) pfnNetDeviceRequestReset;
    decltype(&NETEXPORT(NetExecutionContextCreate)) pfnNetExecutionContextCreate;
    decltype(&NETEXPORT(NetExecutionContextTaskCreate)) pfnNetExecutionContextTaskCreate;
    decltype(&NETEXPORT(NetExecutionContextNotify)) pfnNetExecutionContextNotify;
    decltype(&NETEXPORT(NetExecutionContextTaskEnqueue)) pfnNetExecutionContextTaskEnqueue;
    decltype(&NETEXPORT(NetExecutionContextTaskWaitCompletion)) pfnNetExecutionContextTaskWaitCompletion;
    decltype(&NETEXPORT(NetPowerOffloadGetAdapter)) pfnNetPowerOffloadGetAdapter;
    decltype(&NETEXPORT(NetTxQueueGetDemux8021p)) pfnNetTxQueueGetDemux8021p;
    decltype(&NETEXPORT(NetAdapterExtensionInitSetPowerPolicyCallbacks)) pfnNetAdapterExtensionInitSetPowerPolicyCallbacks;
    decltype(&NETEXPORT(NetAdapterInitSetSelfManagedPowerReferences)) pfnNetAdapterInitSetSelfManagedPowerReferences;
    decltype(&NETEXPORT(NetAdapterLightweightInitAllocate)) pfnNetAdapterLightweightInitAllocate;
    decltype(&NETEXPORT(NetAdapterWifiDestroyPeerAddressDatapath)) pfnNetAdapterWifiDestroyPeerAddressDatapath;
    decltype(&NETEXPORT(NetAdapterPauseOffloadCapabilities)) pfnNetAdapterPauseOffloadCapabilities;
    decltype(&NETEXPORT(NetAdapterResumeOffloadCapabilities)) pfnNetAdapterResumeOffloadCapabilities;
    decltype(&NETEXPORT(NetAdapterQueryWakeReason)) pfnNetAdapterQueryWakeReason;
    decltype(&NETEXPORT(NetTxQueueGetDemuxWmmInfo)) pfnNetTxQueueGetDemuxWmmInfo;
    decltype(&NETEXPORT(NetTxQueueGetDemuxPeerAddress)) pfnNetTxQueueGetDemuxPeerAddress;
    decltype(&NETEXPORT(NetOffloadIsUdpRscEnabled)) pfnNetOffloadIsUdpRscEnabled;
    decltype(&NETEXPORT(NetAdapterCompleteOidRequest)) pfnNetAdapterCompleteOidRequest;
    decltype(&NETEXPORT(NetAdapterSetNative80211Attributes)) pfnNetAdapterSetNative80211Attributes;
    decltype(&NETEXPORT(NetAdapterIndicateMiniportStatus)) pfnNetAdapterIndicateMiniportStatus;
    decltype(&NETEXPORT(NetAdapterWifiAddPeer)) pfnNetAdapterWifiAddPeer;
    decltype(&NETEXPORT(NetAdapterWifiRemovePeer)) pfnNetAdapterWifiRemovePeer;
    decltype(&NETEXPORT(NetTxQueueGetDemuxPeerAddressV2)) pfnNetTxQueueGetDemuxPeerAddressV2;
    decltype(&NETEXPORT(NetDeviceGetSupportedDeviceResetTypes)) pfnNetDeviceGetSupportedDeviceResetTypes;
    decltype(&NETEXPORT(NetMemoryCollectionCreate)) pfnNetMemoryCollectionCreate;
    decltype(&NETEXPORT(NetMemoryCreate)) pfnNetMemoryCreate;
    decltype(&NETEXPORT(NetMemoryDestroy)) pfnNetMemoryDestroy;
} NETFUNCTIONS;

C_ASSERT(sizeof(NETFUNCTIONS) == NetFunctionTableNumEntries * sizeof(PVOID));

typedef struct _NETVERSION
{
    ULONG Size;
    ULONG FuncCount;
    NETFUNCTIONS Functions;
} NETVERSION;

NETVERSION NetVersion =
{
    sizeof(NETVERSION),
    NetFunctionTableNumEntries,
    {
        NETEXPORT(NetAdapterInitAllocate),
        NETEXPORT(NetAdapterInitFree),
        NETEXPORT(NetAdapterInitSetDatapathCallbacks),
        NETEXPORT(NetAdapterCreate),
        NETEXPORT(NetAdapterStart),
        NETEXPORT(NetAdapterStop),
        NETEXPORT(NetAdapterSetLinkLayerCapabilities),
        NETEXPORT(NetAdapterSetLinkLayerMtuSize),
        NETEXPORT(NetAdapterPowerOffloadSetArpCapabilities),
        NETEXPORT(NetAdapterPowerOffloadSetNSCapabilities),
        NETEXPORT(NetAdapterWakeSetBitmapCapabilities),
        NETEXPORT(NetAdapterWakeSetMediaChangeCapabilities),
        NETEXPORT(NetAdapterWakeSetMagicPacketCapabilities),
        NETEXPORT(NetAdapterWakeSetPacketFilterCapabilities),
        NETEXPORT(NetAdapterSetDataPathCapabilities),
        NETEXPORT(NetAdapterSetLinkState),
        NETEXPORT(NetAdapterGetNetLuid),
        NETEXPORT(NetAdapterOpenConfiguration),
        NETEXPORT(NetAdapterSetPermanentLinkLayerAddress),
        NETEXPORT(NetAdapterSetCurrentLinkLayerAddress),
        NETEXPORT(NetAdapterOffloadSetChecksumCapabilities),
        NETEXPORT(NetOffloadIsChecksumIPv4Enabled),
        NETEXPORT(NetOffloadIsChecksumTcpEnabled),
        NETEXPORT(NetOffloadIsChecksumUdpEnabled),
        NETEXPORT(NetAdapterReportWakeReasonPacket),
        NETEXPORT(NetAdapterReportWakeReasonMediaChange),
        NETEXPORT(NetAdapterInitGetCreatedAdapter),
        NETEXPORT(NetAdapterExtensionInitAllocate),
        NETEXPORT(NetAdapterExtensionInitSetOidRequestPreprocessCallback),
        NETEXPORT(NetAdapterDispatchPreprocessedOidRequest),
        NETEXPORT(NetAdapterGetParent),
        NETEXPORT(NetAdapterGetLinkLayerMtuSize),
        NETEXPORT(NetAdapterExtensionInitSetNdisPmCapabilities),
        NETEXPORT(NetAdapterWdmGetNdisHandle),
        NETEXPORT(NetAdapterDriverWdmGetHandle),
        NETEXPORT(NetConfigurationClose),
        NETEXPORT(NetConfigurationOpenSubConfiguration),
        NETEXPORT(NetConfigurationQueryUlong),
        NETEXPORT(NetConfigurationQueryString),
        NETEXPORT(NetConfigurationQueryMultiString),
        NETEXPORT(NetConfigurationQueryBinary),
        NETEXPORT(NetConfigurationQueryLinkLayerAddress),
        NETEXPORT(NetConfigurationAssignUlong),
        NETEXPORT(NetConfigurationAssignUnicodeString),
        NETEXPORT(NetConfigurationAssignMultiString),
        NETEXPORT(NetConfigurationAssignBinary),
        NETEXPORT(NetDeviceInitConfig),
        NETEXPORT(NetDeviceOpenConfiguration),
        NETEXPORT(NetDeviceInitSetPowerPolicyEventCallbacks),
        NETEXPORT(NetDeviceInitSetResetConfig),
        NETEXPORT(NetDeviceAssignSupportedOidList),
        NETEXPORT(NetPowerOffloadGetType),
        NETEXPORT(NetPowerOffloadGetArpParameters),
        NETEXPORT(NetPowerOffloadGetNSParameters),
        NETEXPORT(NetDeviceGetPowerOffloadList),
        NETEXPORT(NetPowerOffloadListGetCount),
        NETEXPORT(NetPowerOffloadListGetElement),
        NETEXPORT(NetAdapterSetReceiveScalingCapabilities),
        NETEXPORT(NetRxQueueCreate),
        NETEXPORT(NetRxQueueNotifyMoreReceivedPacketsAvailable),
        NETEXPORT(NetRxQueueInitGetQueueId),
        NETEXPORT(NetRxQueueGetRingCollection),
        NETEXPORT(NetRxQueueGetExtension),
        NETEXPORT(NetTxQueueCreate),
        NETEXPORT(NetTxQueueNotifyMoreCompletedPacketsAvailable),
        NETEXPORT(NetTxQueueInitGetQueueId),
        NETEXPORT(NetTxQueueGetRingCollection),
        NETEXPORT(NetTxQueueGetExtension),
        NETEXPORT(NetWakeSourceGetType),
        NETEXPORT(NetWakeSourceGetAdapter),
        NETEXPORT(NetWakeSourceGetBitmapParameters),
        NETEXPORT(NetWakeSourceGetMediaChangeParameters),
        NETEXPORT(NetDeviceGetWakeSourceList),
        NETEXPORT(NetWakeSourceListGetCount),
        NETEXPORT(NetWakeSourceListGetElement),
        NETEXPORT(NetAdapterWakeSetEapolPacketCapabilities),
        NETEXPORT(NetAdapterSetReceiveFilterCapabilities),
        NETEXPORT(NetReceiveFilterGetPacketFilter),
        NETEXPORT(NetReceiveFilterGetMulticastAddressCount),
        NETEXPORT(NetReceiveFilterGetMulticastAddressList),
        NETEXPORT(NetDriverExtensionInitialize),
        NETEXPORT(NetAdapterExtensionInitSetDirectOidRequestPreprocessCallback),
        NETEXPORT(NetExAdapterInitSetDirectOidPreprocessCallback),
        NETEXPORT(NetAdapterExtensionInitSetTxPeerDemuxCallback),
        NETEXPORT(NetAdapterDispatchPreprocessedDirectOidRequest),
        NETEXPORT(NetExAdapterDispatchPreprocessedDirectOid),
        NETEXPORT(NetAdapterExtensionSetNdisPmCapabilities),
        NETEXPORT(NetAdapterInitAllocateContext),
        NETEXPORT(NetAdapterInitGetTypedContextWorker),
        NETEXPORT(NetAdapterOffloadSetTxChecksumCapabilities),
        NETEXPORT(NetOffloadIsTxChecksumIPv4Enabled),
        NETEXPORT(NetOffloadIsTxChecksumTcpEnabled),
        NETEXPORT(NetOffloadIsTxChecksumUdpEnabled),
        NETEXPORT(NetAdapterOffloadSetRxChecksumCapabilities),
        NETEXPORT(NetOffloadIsRxChecksumIPv4Enabled),
        NETEXPORT(NetOffloadIsRxChecksumTcpEnabled),
        NETEXPORT(NetOffloadIsRxChecksumUdpEnabled),
        NETEXPORT(NetAdapterOffloadSetGsoCapabilities),
        NETEXPORT(NetOffloadIsLsoIPv4Enabled),
        NETEXPORT(NetOffloadIsLsoIPv6Enabled),
        NETEXPORT(NetOffloadIsUsoIPv4Enabled),
        NETEXPORT(NetOffloadIsUsoIPv6Enabled),
        NETEXPORT(NetAdapterOffloadSetRscCapabilities),
        NETEXPORT(NetOffloadIsTcpRscIPv4Enabled),
        NETEXPORT(NetOffloadIsTcpRscIPv6Enabled),
        NETEXPORT(NetOffloadIsRscTcpTimestampOptionEnabled),
        NETEXPORT(NetAdapterOffloadSetIeee8021qTagCapabilities),
        NETEXPORT(NetAdapterInitAddTxDemux),
        NETEXPORT(NetAdapterGetTxPeerAddressDemux),
        NETEXPORT(NetBufferQueueCreate),
        NETEXPORT(NetBufferQueueGetExtension),
        NETEXPORT(NetBufferQueueGetRing),
        NETEXPORT(NetDeviceInitSetResetCapabilities),
        NETEXPORT(NetDeviceStoreResetDiagnostics),
        NETEXPORT(NetDeviceRequestReset),
        NETEXPORT(NetExecutionContextCreate),
        NETEXPORT(NetExecutionContextTaskCreate),
        NETEXPORT(NetExecutionContextNotify),
        NETEXPORT(NetExecutionContextTaskEnqueue),
        NETEXPORT(NetExecutionContextTaskWaitCompletion),
        NETEXPORT(NetPowerOffloadGetAdapter),
        NETEXPORT(NetTxQueueGetDemux8021p),
        NETEXPORT(NetAdapterExtensionInitSetPowerPolicyCallbacks),
        NETEXPORT(NetAdapterInitSetSelfManagedPowerReferences),
        NETEXPORT(NetAdapterLightweightInitAllocate),
        NETEXPORT(NetAdapterWifiDestroyPeerAddressDatapath),
        NETEXPORT(NetAdapterPauseOffloadCapabilities),
        NETEXPORT(NetAdapterResumeOffloadCapabilities),
        NETEXPORT(NetAdapterQueryWakeReason),
        NETEXPORT(NetTxQueueGetDemuxWmmInfo),
        NETEXPORT(NetTxQueueGetDemuxPeerAddress),
        NETEXPORT(NetOffloadIsUdpRscEnabled),
        NETEXPORT(NetAdapterCompleteOidRequest),
        NETEXPORT(NetAdapterSetNative80211Attributes),
        NETEXPORT(NetAdapterIndicateMiniportStatus),
        NETEXPORT(NetAdapterWifiAddPeer),
        NETEXPORT(NetAdapterWifiRemovePeer),
        NETEXPORT(NetTxQueueGetDemuxPeerAddressV2),
        NETEXPORT(NetDeviceGetSupportedDeviceResetTypes),
        NETEXPORT(NetMemoryCollectionCreate),
        NETEXPORT(NetMemoryCreate),
        NETEXPORT(NetMemoryDestroy),
    }
};

#endif
