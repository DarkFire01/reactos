/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The NDIS 6 status indication
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define NDIS_OBJECT_TYPE_STATUS_INDICATION          0x98

#define NDIS_STATUS_INDICATION_REVISION_1           1

/* NDIS_STATUS_INDICATION::Flags */
#define NDIS_STATUS_INDICATION_FLAGS_MEDIA_CONNECT_TO_CONNECT  0x00000001
#define NDIS_STATUS_INDICATION_FLAGS_MEDIA_DISCONNECT_TO_CONNECT 0x00000002
#define NDIS_STATUS_INDICATION_FLAGS_MEDIA_CONNECT_TO_DISCONNECT 0x00000004

typedef struct _NDIS_STATUS_INDICATION
{
    NDIS_OBJECT_HEADER Header;
    NDIS_HANDLE SourceHandle;
    NDIS_PORT_NUMBER PortNumber;
    NDIS_STATUS StatusCode;
    ULONG Flags;
    NDIS_HANDLE DestinationHandle;
    PVOID RequestId;
    PVOID StatusBuffer;
    ULONG StatusBufferSize;
    GUID Guid;
    PVOID NdisReserved[4];
} NDIS_STATUS_INDICATION, *PNDIS_STATUS_INDICATION;

#define NDIS_SIZEOF_STATUS_INDICATION_REVISION_1     RTL_SIZEOF_THROUGH_FIELD(NDIS_STATUS_INDICATION, NdisReserved)

#ifdef __cplusplus
}
#endif
