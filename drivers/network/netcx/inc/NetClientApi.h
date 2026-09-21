/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Top level interface between the adapter half and the
 *              translator half of the class extension
 *
 * The adapter half hands the translator NET_CLIENT_DISPATCH and an adapter
 * with its NET_CLIENT_ADAPTER_DISPATCH. The translator answers with
 * NET_CLIENT_CONTROL_DISPATCH, which is how the adapter half drives it.
 */

#pragma once

#include <NetClientAdapter.h>
#include <NetClientDriverConfigurationConstants.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _NET_CLIENT_DISPATCH
{
    ULONG Size;

    NTSTATUS
    (*NetClientCreateBufferPool)(
        _In_ NET_CLIENT_BUFFER_POOL_CONFIG *BufferPoolConfig,
        _Out_ NET_CLIENT_BUFFER_POOL *Pool,
        _Out_ NET_CLIENT_BUFFER_POOL_DISPATCH const **BufferPoolDispatch);

    ULONG
    (*NetClientQueryDriverConfigurationUlong)(
        _In_ DRIVER_CONFIG_ENUM ConfigurationEnum);

    BOOLEAN
    (*NetClientQueryDriverConfigurationBoolean)(
        _In_ DRIVER_CONFIG_ENUM ConfigurationEnum);
} NET_CLIENT_DISPATCH;

typedef struct _NET_CLIENT_CONTROL_DISPATCH
{
    ULONG Size;

    VOID
    (*CreateDatapath)(
        _In_ PVOID ClientContext);

    VOID
    (*DestroyDatapath)(
        _In_ PVOID ClientContext);

    VOID
    (*StartDatapath)(
        _In_ PVOID ClientContext);

    VOID
    (*StopDatapath)(
        _In_ PVOID ClientContext);

    /* All three return TRUE when the translator consumed the request. */
    BOOLEAN
    (*NdisOidRequestHandler)(
        _In_ PVOID ClientContext,
        _In_ NDIS_OID_REQUEST *Request,
        _Out_ NTSTATUS *Status);

    BOOLEAN
    (*NdisDirectOidRequestHandler)(
        _In_ PVOID ClientContext,
        _In_ NDIS_OID_REQUEST *Request,
        _Out_ NTSTATUS *Status);

    BOOLEAN
    (*NdisSynchronousOidRequestHandler)(
        _In_ PVOID ClientContext,
        _In_ NDIS_OID_REQUEST *Request,
        _Out_ NTSTATUS *Status);

    NTSTATUS
    (*OffloadInitialize)(
        _In_ PVOID ClientContext);

    VOID
    (*WifiDestroyPeerAddressDatapath)(
        _In_ PVOID ClientContext,
        _In_ SIZE_T Demux);

    VOID
    (*PauseOffloadCapabilities)(
        _In_ PVOID ClientContext);

    VOID
    (*ResumeOffloadCapabilities)(
        _In_ PVOID ClientContext);
} NET_CLIENT_CONTROL_DISPATCH;

#ifdef __cplusplus
}
#endif
