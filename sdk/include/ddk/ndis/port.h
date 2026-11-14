/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS ports and miniport initiated PnP events
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef NET_IF_MEDIA_CONNECT_STATE NDIS_MEDIA_CONNECT_STATE, *PNDIS_MEDIA_CONNECT_STATE;

typedef ULONG NDIS_NIC_SWITCH_ID, *PNDIS_NIC_SWITCH_ID;
typedef ULONG NDIS_NIC_SWITCH_VPORT_ID, *PNDIS_NIC_SWITCH_VPORT_ID;

/* Ports */

#define NDIS_OBJECT_TYPE_PORT_CHARACTERISTICS                        0x9C

typedef enum _NDIS_PORT_TYPE
{
    NdisPortTypeUndefined,
    NdisPortTypeBridge,
    NdisPortTypeRasConnection,
    NdisPortType8021xSupplicant,
#if (NDIS_SUPPORT_NDIS630)
    NdisPortTypeNdisImPlatform,
#endif
    NdisPortTypeMax,
} NDIS_PORT_TYPE, *PNDIS_PORT_TYPE;

typedef enum _NDIS_PORT_AUTHORIZATION_STATE
{
    NdisPortAuthorizationUnknown,
    NdisPortAuthorized,
    NdisPortUnauthorized,
    NdisPortReauthorizing
} NDIS_PORT_AUTHORIZATION_STATE, *PNDIS_PORT_AUTHORIZATION_STATE;

typedef enum _NDIS_PORT_CONTROL_STATE
{
    NdisPortControlStateUnknown,
    NdisPortControlStateControlled,
    NdisPortControlStateUncontrolled
} NDIS_PORT_CONTROL_STATE, *PNDIS_PORT_CONTROL_STATE;

typedef NDIS_PORT_CONTROL_STATE NDIS_PORT_CONTROLL_STATE;
typedef PNDIS_PORT_CONTROL_STATE PNDIS_PORT_CONTROLL_STATE;

/* NDIS_PORT_CHARACTERISTICS::Flags */
#define NDIS_PORT_CHAR_USE_DEFAULT_AUTH_SETTINGS                     0x00000001

#define NDIS_PORT_CHARACTERISTICS_REVISION_1                         1

typedef struct _NDIS_PORT_CHARACTERISTICS
{
    NDIS_OBJECT_HEADER Header;
    NDIS_PORT_NUMBER PortNumber;
    ULONG Flags;
    NDIS_PORT_TYPE Type;
    NDIS_MEDIA_CONNECT_STATE MediaConnectState;
    ULONG64 XmitLinkSpeed;
    ULONG64 RcvLinkSpeed;
    NET_IF_DIRECTION_TYPE Direction;
    NDIS_PORT_CONTROL_STATE SendControlState;
    NDIS_PORT_CONTROL_STATE RcvControlState;
    NDIS_PORT_AUTHORIZATION_STATE SendAuthorizationState;
    NDIS_PORT_AUTHORIZATION_STATE RcvAuthorizationState;
} NDIS_PORT_CHARACTERISTICS, *PNDIS_PORT_CHARACTERISTICS;

#define NDIS_SIZEOF_PORT_CHARACTERISTICS_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_PORT_CHARACTERISTICS, RcvAuthorizationState)

/* The chain NetEventPortActivation carries */
typedef struct _NDIS_PORT NDIS_PORT, *PNDIS_PORT;

struct _NDIS_PORT
{
    PNDIS_PORT Next;
    PVOID NdisReserved;
    PVOID MiniportReserved;
    PVOID ProtocolReserved;
    NDIS_PORT_CHARACTERISTICS PortCharacteristics;
};

_Must_inspect_result_
_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_STATUS
NTAPI
NdisMAllocatePort(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _Inout_ PNDIS_PORT_CHARACTERISTICS PortCharacteristics);

_IRQL_requires_max_(DISPATCH_LEVEL)
NDIS_STATUS
NTAPI
NdisMFreePort(
    _In_ NDIS_HANDLE NdisMiniportHandle,
    _In_ NDIS_PORT_NUMBER PortNumber);

/* Miniport initiated PnP events */

#define NET_EVENT_FLAGS_VPORT_ID_VALID                               0x00000002

#define NET_PNP_EVENT_NOTIFICATION_REVISION_1                        1
#if (NDIS_SUPPORT_NDIS650)
#define NET_PNP_EVENT_NOTIFICATION_REVISION_2                        2
#endif

typedef struct _NET_PNP_EVENT_NOTIFICATION
{
    NDIS_OBJECT_HEADER Header;
    NDIS_PORT_NUMBER PortNumber;
    NET_PNP_EVENT NetPnPEvent;
    ULONG Flags;
#if (NDIS_SUPPORT_NDIS650)
    NDIS_NIC_SWITCH_ID SwitchId;
    NDIS_NIC_SWITCH_VPORT_ID VPortId;
#endif
} NET_PNP_EVENT_NOTIFICATION, *PNET_PNP_EVENT_NOTIFICATION;

#define NDIS_SIZEOF_NET_PNP_EVENT_NOTIFICATION_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NET_PNP_EVENT_NOTIFICATION, Flags)
#if (NDIS_SUPPORT_NDIS650)
#define NDIS_SIZEOF_NET_PNP_EVENT_NOTIFICATION_REVISION_2 \
    RTL_SIZEOF_THROUGH_FIELD(NET_PNP_EVENT_NOTIFICATION, VPortId)
#endif

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
NDIS_STATUS
NTAPI
NdisMNetPnPEvent(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNET_PNP_EVENT_NOTIFICATION NetPnPEventNotification);

#if (NDIS_SUPPORT_NDIS61)
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
NdisMDirectOidRequestComplete(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_OID_REQUEST OidRequest,
    _In_ NDIS_STATUS Status);
#endif

#ifdef __cplusplus
}
#endif
