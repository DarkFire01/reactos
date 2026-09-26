/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The NDIS 6 OID request
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define NDIS_OBJECT_TYPE_OID_REQUEST                0x96

#define NDIS_OID_REQUEST_REVISION_1                 1
#define NDIS_OID_REQUEST_REVISION_2                 2

#define NDIS_OID_REQUEST_NDIS_RESERVED_SIZE         16
#define NDIS_OID_REQUEST_MINIPORT_RESERVED_SIZE     2
#define NDIS_OID_REQUEST_SOURCE_RESERVED_SIZE       2

typedef struct _NDIS_OID_REQUEST
{
    NDIS_OBJECT_HEADER Header;
    NDIS_REQUEST_TYPE RequestType;
    NDIS_PORT_NUMBER PortNumber;
    UINT Timeout;
    PVOID RequestId;
    NDIS_HANDLE RequestHandle;
    union _REQUEST_DATA
    {
        struct _QUERY
        {
            NDIS_OID Oid;
            PVOID InformationBuffer;
            UINT InformationBufferLength;
            UINT BytesWritten;
            UINT BytesNeeded;
        } QUERY_INFORMATION;
        struct _SET
        {
            NDIS_OID Oid;
            PVOID InformationBuffer;
            UINT InformationBufferLength;
            UINT BytesRead;
            UINT BytesNeeded;
        } SET_INFORMATION;
        struct _METHOD
        {
            NDIS_OID Oid;
            PVOID InformationBuffer;
            ULONG InputBufferLength;
            ULONG OutputBufferLength;
            ULONG MethodId;
            UINT BytesWritten;
            UINT BytesRead;
            UINT BytesNeeded;
        } METHOD_INFORMATION;
    } DATA;
    UCHAR NdisReserved[NDIS_OID_REQUEST_NDIS_RESERVED_SIZE * sizeof(PVOID)];
    UCHAR MiniportReserved[NDIS_OID_REQUEST_MINIPORT_RESERVED_SIZE * sizeof(PVOID)];
    UCHAR SourceReserved[NDIS_OID_REQUEST_SOURCE_RESERVED_SIZE * sizeof(PVOID)];
    UCHAR SupportedRevision;
    UCHAR Reserved1;
    USHORT Reserved2;
#if (NDIS_SUPPORT_NDIS650)
    ULONG SwitchId;
    ULONG VPortId;
    ULONG Flags;
#endif
} NDIS_OID_REQUEST, *PNDIS_OID_REQUEST;

/* NDIS_OID_REQUEST::Flags */
#define NDIS_OID_REQUEST_FLAGS_VPORT_ID_VALID       0x0001

#define NDIS_SIZEOF_OID_REQUEST_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_OID_REQUEST, Reserved2)

#if (NDIS_SUPPORT_NDIS650)
#define NDIS_SIZEOF_OID_REQUEST_REVISION_2 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_OID_REQUEST, Flags)
#endif

_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_STATUS
NTAPI
NdisAllocateCloneOidRequest(
    _In_ NDIS_HANDLE SourceHandle,
    _In_ PNDIS_OID_REQUEST OidRequest,
    _In_ UINT PoolTag,
    _Out_ PNDIS_OID_REQUEST *ClonedOidRequest);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisFreeCloneOidRequest(
    _In_ NDIS_HANDLE SourceHandle,
    _In_ PNDIS_OID_REQUEST Request);

#ifdef __cplusplus
}
#endif
