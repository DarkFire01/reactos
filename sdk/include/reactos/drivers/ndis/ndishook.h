/*
 * PROJECT:     ReactOS NDIS library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The miniport hook NPI a WDI upper edge provides to NDIS
 *
 * NDIS is the client and the upper edge the provider. A WLAN miniport's
 * registration is handed to the provider, which registers the real NDIS
 * miniport with handlers of its own. From then on NDIS routes the miniport's
 * OID completions and status indications to the provider, which reaches the
 * miniport's own handlers and NDIS's plain paths through the client dispatch.
 */

#pragma once

#include <netioddk.h>
#include <dot11wdi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* {2227E813-8D8B-11D4-ABAD-009027719E09} */
DEFINE_GUID(NDIS_HOOK_NPI_ID, 0x2227E813, 0x8D8B, 0x11D4, 0xAB, 0xAD, 0x00, 0x90, 0x27, 0x71, 0x9E, 0x09);

typedef enum _NDIS_HOOK_TYPE
{
    NdisHookTypeNone = 0,
    NdisHookTypeWdi = 1,
    NdisHookTypeCount
} NDIS_HOOK_TYPE;

/* NpiSpecificCharacteristics of the provider's registration instance */
typedef struct _NDIS_HOOK_PROVIDER_CHARACTERISTICS
{
    NDIS_HOOK_TYPE Type;
} NDIS_HOOK_PROVIDER_CHARACTERISTICS, *PNDIS_HOOK_PROVIDER_CHARACTERISTICS;

/* The provider's dispatch */

typedef
NDIS_STATUS
(NTAPI NDIS_HOOK_REGISTER_WDI_DRIVER)(
    _In_ PVOID ProviderBindingContext,
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _In_opt_ NDIS_HANDLE MiniportDriverContext,
    _In_ PNDIS_MINIPORT_DRIVER_CHARACTERISTICS MiniportDriverCharacteristics,
    _In_ PNDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS MiniportWdiCharacteristics,
    _Out_ PNDIS_HANDLE NdisMiniportDriverHandle);

typedef
VOID
(NTAPI NDIS_HOOK_DEREGISTER_WDI_DRIVER)(
    _In_ PVOID ProviderBindingContext,
    _In_ NDIS_HANDLE NdisMiniportDriverHandle,
    _In_ NDIS_HANDLE HookDriverHandle);

typedef
VOID
(NTAPI NDIS_HOOK_OID_REQUEST_COMPLETE)(
    _In_ PVOID ProviderBindingContext,
    _In_ NDIS_HANDLE HookAdapterHandle,
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_OID_REQUEST OidRequest,
    _In_ NDIS_STATUS Status);

typedef
VOID
(NTAPI NDIS_HOOK_INDICATE_STATUS)(
    _In_ PVOID ProviderBindingContext,
    _In_ NDIS_HANDLE HookAdapterHandle,
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_STATUS_INDICATION StatusIndication);

typedef struct _NDIS_HOOK_PROVIDER_DISPATCH
{
    NDIS_HOOK_REGISTER_WDI_DRIVER *RegisterWdiDriver;
    NDIS_HOOK_DEREGISTER_WDI_DRIVER *DeregisterWdiDriver;
    NDIS_HOOK_OID_REQUEST_COMPLETE *OidRequestComplete;
    NDIS_HOOK_OID_REQUEST_COMPLETE *DirectOidRequestComplete;
    NDIS_HOOK_INDICATE_STATUS *IndicateStatus;
} NDIS_HOOK_PROVIDER_DISPATCH, *PNDIS_HOOK_PROVIDER_DISPATCH;

/* NDIS's dispatch */

typedef
VOID
(NTAPI NDIS_HOOK_SET_DRIVER_CONTEXT)(
    _In_ NDIS_HANDLE NdisMiniportDriverHandle,
    _In_ NDIS_HANDLE HookDriverHandle);

typedef
VOID
(NTAPI NDIS_HOOK_SET_ADAPTER_CONTEXT)(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_HANDLE HookAdapterHandle);

typedef
NDIS_HANDLE
(NTAPI NDIS_HOOK_GET_DRIVER_HANDLE)(
    _In_ NDIS_HANDLE MiniportDriverContext);

typedef
NDIS_HANDLE
(NTAPI NDIS_HOOK_GET_ADAPTER_HANDLE)(
    _In_ NDIS_HANDLE MiniportAdapterContext);

typedef
VOID
(NTAPI NDIS_HOOK_RAW_OID_REQUEST_COMPLETE)(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_OID_REQUEST OidRequest,
    _In_ NDIS_STATUS Status);

typedef
VOID
(NTAPI NDIS_HOOK_RAW_INDICATE_STATUS)(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_STATUS_INDICATION StatusIndication);

typedef
NDIS_STATUS
(NTAPI NDIS_HOOK_INVOKE_OID_REQUEST)(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_OID_REQUEST OidRequest);

typedef
VOID
(NTAPI NDIS_HOOK_INVOKE_CANCEL_OID_REQUEST)(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PVOID RequestId);

typedef struct _NDIS_HOOK_CLIENT_DISPATCH
{
    NDIS_HOOK_SET_DRIVER_CONTEXT *SetDriverContext;
    NDIS_HOOK_SET_ADAPTER_CONTEXT *SetAdapterContext;
    NDIS_HOOK_GET_DRIVER_HANDLE *GetHookDriverHandle;
    NDIS_HOOK_GET_ADAPTER_HANDLE *GetHookAdapterHandle;
    NDIS_HOOK_RAW_OID_REQUEST_COMPLETE *RawOidRequestComplete;
    NDIS_HOOK_RAW_OID_REQUEST_COMPLETE *RawDirectOidRequestComplete;
    NDIS_HOOK_RAW_INDICATE_STATUS *RawIndicateStatus;
    NDIS_HOOK_INVOKE_OID_REQUEST *InvokeOidRequest;
    NDIS_HOOK_INVOKE_CANCEL_OID_REQUEST *InvokeCancelOidRequest;
    NDIS_HOOK_INVOKE_OID_REQUEST *InvokeDirectOidRequest;
    NDIS_HOOK_INVOKE_CANCEL_OID_REQUEST *InvokeCancelDirectOidRequest;
} NDIS_HOOK_CLIENT_DISPATCH, *PNDIS_HOOK_CLIENT_DISPATCH;

#ifdef __cplusplus
}
#endif
