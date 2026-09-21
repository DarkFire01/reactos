/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Adapter and device configuration store
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _NET_CONFIGURATION_QUERY_ULONG_FLAGS
{
    NET_CONFIGURATION_QUERY_ULONG_NO_FLAGS = 0x00000000,
    NET_CONFIGURATION_QUERY_ULONG_MAY_BE_STORED_AS_HEX_STRING = 0x00000001
} NET_CONFIGURATION_QUERY_ULONG_FLAGS;

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
(NTAPI *PFN_NETCONFIGURATIONCLOSE)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETCONFIGURATION Configuration);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
VOID
NTAPI
NetConfigurationClose(
    _In_ NETCONFIGURATION Configuration)
{
    ((PFN_NETCONFIGURATIONCLOSE)NetFunctions[NetConfigurationCloseTableIndex])(
        NetDriverGlobals, Configuration);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETCONFIGURATIONOPENSUBCONFIGURATION)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING SubConfigurationName,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *SubConfigurationAttributes,
    _Out_ NETCONFIGURATION *SubConfiguration);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetConfigurationOpenSubConfiguration(
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING SubConfigurationName,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *SubConfigurationAttributes,
    _Out_ NETCONFIGURATION *SubConfiguration)
{
    return ((PFN_NETCONFIGURATIONOPENSUBCONFIGURATION)NetFunctions[NetConfigurationOpenSubConfigurationTableIndex])(
        NetDriverGlobals, Configuration, SubConfigurationName, SubConfigurationAttributes, SubConfiguration);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETCONFIGURATIONQUERYULONG)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETCONFIGURATION Configuration,
    _In_ NET_CONFIGURATION_QUERY_ULONG_FLAGS Flags,
    _In_ PCUNICODE_STRING ValueName,
    _Out_ ULONG *Value);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetConfigurationQueryUlong(
    _In_ NETCONFIGURATION Configuration,
    _In_ NET_CONFIGURATION_QUERY_ULONG_FLAGS Flags,
    _In_ PCUNICODE_STRING ValueName,
    _Out_ ULONG *Value)
{
    return ((PFN_NETCONFIGURATIONQUERYULONG)NetFunctions[NetConfigurationQueryUlongTableIndex])(
        NetDriverGlobals, Configuration, Flags, ValueName, Value);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETCONFIGURATIONQUERYSTRING)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *StringAttributes,
    _Out_ WDFSTRING *WdfString);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetConfigurationQueryString(
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *StringAttributes,
    _Out_ WDFSTRING *WdfString)
{
    return ((PFN_NETCONFIGURATIONQUERYSTRING)NetFunctions[NetConfigurationQueryStringTableIndex])(
        NetDriverGlobals, Configuration, ValueName, StringAttributes, WdfString);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETCONFIGURATIONQUERYMULTISTRING)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *StringsAttributes,
    _Inout_ WDFCOLLECTION Collection);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetConfigurationQueryMultiString(
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *StringsAttributes,
    _Inout_ WDFCOLLECTION Collection)
{
    return ((PFN_NETCONFIGURATIONQUERYMULTISTRING)NetFunctions[NetConfigurationQueryMultiStringTableIndex])(
        NetDriverGlobals, Configuration, ValueName, StringsAttributes, Collection);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETCONFIGURATIONQUERYBINARY)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_ _Strict_type_match_ POOL_TYPE PoolType,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *MemoryAttributes,
    _Out_ WDFMEMORY *Memory);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetConfigurationQueryBinary(
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_ _Strict_type_match_ POOL_TYPE PoolType,
    _In_opt_ WDF_OBJECT_ATTRIBUTES *MemoryAttributes,
    _Out_ WDFMEMORY *Memory)
{
    return ((PFN_NETCONFIGURATIONQUERYBINARY)NetFunctions[NetConfigurationQueryBinaryTableIndex])(
        NetDriverGlobals, Configuration, ValueName, PoolType, MemoryAttributes, Memory);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETCONFIGURATIONQUERYLINKLAYERADDRESS)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETCONFIGURATION Configuration,
    _Out_ NET_ADAPTER_LINK_LAYER_ADDRESS *LinkLayerAddress);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetConfigurationQueryLinkLayerAddress(
    _In_ NETCONFIGURATION Configuration,
    _Out_ NET_ADAPTER_LINK_LAYER_ADDRESS *LinkLayerAddress)
{
    return ((PFN_NETCONFIGURATIONQUERYLINKLAYERADDRESS)NetFunctions[NetConfigurationQueryLinkLayerAddressTableIndex])(
        NetDriverGlobals, Configuration, LinkLayerAddress);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETCONFIGURATIONASSIGNULONG)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_ ULONG Value);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetConfigurationAssignUlong(
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_ ULONG Value)
{
    return ((PFN_NETCONFIGURATIONASSIGNULONG)NetFunctions[NetConfigurationAssignUlongTableIndex])(
        NetDriverGlobals, Configuration, ValueName, Value);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETCONFIGURATIONASSIGNUNICODESTRING)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_ PCUNICODE_STRING Value);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetConfigurationAssignUnicodeString(
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_ PCUNICODE_STRING Value)
{
    return ((PFN_NETCONFIGURATIONASSIGNUNICODESTRING)NetFunctions[NetConfigurationAssignUnicodeStringTableIndex])(
        NetDriverGlobals, Configuration, ValueName, Value);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETCONFIGURATIONASSIGNMULTISTRING)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_ WDFCOLLECTION Collection);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetConfigurationAssignMultiString(
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_ WDFCOLLECTION Collection)
{
    return ((PFN_NETCONFIGURATIONASSIGNMULTISTRING)NetFunctions[NetConfigurationAssignMultiStringTableIndex])(
        NetDriverGlobals, Configuration, ValueName, Collection);
}

typedef
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
(NTAPI *PFN_NETCONFIGURATIONASSIGNBINARY)(
    _In_ PNET_DRIVER_GLOBALS DriverGlobals,
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_reads_bytes_(BufferLength) PVOID Buffer,
    _In_ ULONG BufferLength);

_IRQL_requires_max_(PASSIVE_LEVEL)
FORCEINLINE
NTSTATUS
NTAPI
NetConfigurationAssignBinary(
    _In_ NETCONFIGURATION Configuration,
    _In_ PCUNICODE_STRING ValueName,
    _In_reads_bytes_(BufferLength) PVOID Buffer,
    _In_ ULONG BufferLength)
{
    return ((PFN_NETCONFIGURATIONASSIGNBINARY)NetFunctions[NetConfigurationAssignBinaryTableIndex])(
        NetDriverGlobals, Configuration, ValueName, Buffer, BufferLength);
}

#ifdef __cplusplus
}
#endif
