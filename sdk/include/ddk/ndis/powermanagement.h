/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Power management capabilities, wake patterns and protocol offloads
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define NDIS_PM_CAPABILITIES_REVISION_1             1
#define NDIS_PM_CAPABILITIES_REVISION_2             2
#define NDIS_PM_PARAMETERS_REVISION_1               1
#define NDIS_PM_PARAMETERS_REVISION_2               2
#define NDIS_PM_WOL_PATTERN_REVISION_1              1
#define NDIS_PM_PROTOCOL_OFFLOAD_REVISION_1         1
#define NDIS_PM_WAKE_REASON_REVISION_1              1
#define NDIS_PM_WAKE_PACKET_REVISION_1              1

/* NDIS_PM_CAPABILITIES::Flags */
#define NDIS_PM_WAKE_PACKET_INDICATION_SUPPORTED    0x00000001
#define NDIS_PM_SELECTIVE_SUSPEND_SUPPORTED         0x00000002
#define NDIS_PM_S0_IDLE_SUPPORTED                   0x00000004

/* NDIS_PM_CAPABILITIES::SupportedWoLPacketPatterns */
#define NDIS_PM_WOL_BITMAP_PATTERN_SUPPORTED            0x00000001
#define NDIS_PM_WOL_MAGIC_PACKET_SUPPORTED              0x00000002
#define NDIS_PM_WOL_IPV4_TCP_SYN_SUPPORTED              0x00000004
#define NDIS_PM_WOL_IPV6_TCP_SYN_SUPPORTED              0x00000008
#define NDIS_PM_WOL_EAPOL_REQUEST_ID_MESSAGE_SUPPORTED  0x00000010

/* NDIS_PM_CAPABILITIES::SupportedProtocolOffloads */
#define NDIS_PM_PROTOCOL_OFFLOAD_ARP_SUPPORTED              0x00000001
#define NDIS_PM_PROTOCOL_OFFLOAD_NS_SUPPORTED               0x00000002
#define NDIS_PM_PROTOCOL_OFFLOAD_80211_RSN_REKEY_SUPPORTED  0x00000080

/* NDIS_PM_CAPABILITIES::SupportedWakeUpEvents */
#define NDIS_PM_WAKE_ON_MEDIA_CONNECT_SUPPORTED     0x00000001
#define NDIS_PM_WAKE_ON_MEDIA_DISCONNECT_SUPPORTED  0x00000002

/* NDIS_PM_PARAMETERS::EnabledWoLPacketPatterns */
#define NDIS_PM_WOL_BITMAP_PATTERN_ENABLED              0x00000001
#define NDIS_PM_WOL_MAGIC_PACKET_ENABLED                0x00000002
#define NDIS_PM_WOL_IPV4_TCP_SYN_ENABLED                0x00000004
#define NDIS_PM_WOL_IPV6_TCP_SYN_ENABLED                0x00000008
#define NDIS_PM_WOL_EAPOL_REQUEST_ID_MESSAGE_ENABLED    0x00000010

/* NDIS_PM_PARAMETERS::EnabledProtocolOffloads */
#define NDIS_PM_PROTOCOL_OFFLOAD_ARP_ENABLED                0x00000001
#define NDIS_PM_PROTOCOL_OFFLOAD_NS_ENABLED                 0x00000002
#define NDIS_PM_PROTOCOL_OFFLOAD_80211_RSN_REKEY_ENABLED    0x00000080

/* NDIS_PM_PARAMETERS::WakeUpFlags */
#define NDIS_PM_WAKE_ON_LINK_CHANGE_ENABLED         0x00000001
#define NDIS_PM_WAKE_ON_MEDIA_DISCONNECT_ENABLED    0x00000002
#define NDIS_PM_SELECTIVE_SUSPEND_ENABLED           0x00000010
#define NDIS_PM_AOAC_NAPS_ENABLED                   0x00000020

/* Media specific wake up events, reported alongside the generic ones. */
#define NDIS_WLAN_WAKE_ON_NLO_DISCOVERY_SUPPORTED           0x00000001
#define NDIS_WLAN_WAKE_ON_AP_ASSOCIATION_LOST_SUPPORTED     0x00000002
#define NDIS_WLAN_WAKE_ON_GTK_HANDSHAKE_ERROR_SUPPORTED     0x00000004
#define NDIS_WLAN_WAKE_ON_4WAY_HANDSHAKE_REQUEST_SUPPORTED  0x00000008

#define NDIS_WWAN_WAKE_ON_REGISTER_STATE_SUPPORTED  0x00000001
#define NDIS_WWAN_WAKE_ON_SMS_RECEIVE_SUPPORTED     0x00000002
#define NDIS_WWAN_WAKE_ON_USSD_RECEIVE_SUPPORTED    0x00000004
#define NDIS_WWAN_WAKE_ON_PACKET_STATE_SUPPORTED    0x00000008
#define NDIS_WWAN_WAKE_ON_UICC_CHANGE_SUPPORTED     0x00000010

#define NDIS_PM_MAX_STRING_SIZE                     64

typedef enum _NDIS_PM_WOL_PACKET
{
    NdisPMWoLPacketUnspecified = 0,
    NdisPMWoLPacketBitmapPattern,
    NdisPMWoLPacketMagicPacket,
    NdisPMWoLPacketIPv4TcpSyn,
    NdisPMWoLPacketIPv6TcpSyn,
    NdisPMWoLPacketEapolRequestIdMessage,
    NdisPMWoLPacketMaximum
} NDIS_PM_WOL_PACKET, *PNDIS_PM_WOL_PACKET;

typedef enum _NDIS_PM_PROTOCOL_OFFLOAD_TYPE
{
    NdisPMProtocolOffloadIdUnspecified = 0,
    NdisPMProtocolOffloadIdIPv4ARP,
    NdisPMProtocolOffloadIdIPv6NS,
    NdisPMProtocolOffload80211RSNRekey,
    NdisPMProtocolOffload80211RSNRekeyV2,
    NdisPMProtocolOffloadIdMaximum
} NDIS_PM_PROTOCOL_OFFLOAD_TYPE, *PNDIS_PM_PROTOCOL_OFFLOAD_TYPE;

typedef enum _NDIS_PM_WAKE_REASON_TYPE
{
    NdisWakeReasonUnspecified = 0,
    NdisWakeReasonPacket,
    NdisWakeReasonMediaDisconnect,
    NdisWakeReasonMediaConnect,
    NdisWakeReasonWlanNLODiscovery = 0x1000,
    NdisWakeReasonWlanAPAssociationLost,
    NdisWakeReasonWlanGTKHandshakeError,
    NdisWakeReasonWlan4WayHandshakeRequest,
    NdisWakeReasonWlanIncomingActionFrame,
    NdisWakeReasonWlanClientDriverDiagnostic,
    NdisWakeReasonWwanRegisterState = 0x2000,
    NdisWakeReasonWwanSMSReceive,
    NdisWakeReasonWwanUSSDReceive,
    NdisWakeReasonWwanPacketState = 0x2004,
    NdisWakeReasonWwanUiccChange
} NDIS_PM_WAKE_REASON_TYPE, *PNDIS_PM_WAKE_REASON_TYPE;

typedef struct _NDIS_PM_COUNTED_STRING
{
    USHORT Length;
    WCHAR String[NDIS_PM_MAX_STRING_SIZE + 1];
} NDIS_PM_COUNTED_STRING, *PNDIS_PM_COUNTED_STRING;

typedef struct _NDIS_PM_CAPABILITIES
{
    NDIS_OBJECT_HEADER Header;
    ULONG Flags;
    ULONG SupportedWoLPacketPatterns;
    ULONG NumTotalWoLPatterns;
    ULONG MaxWoLPatternSize;
    ULONG MaxWoLPatternOffset;
    ULONG MaxWoLPacketSaveBuffer;
    ULONG SupportedProtocolOffloads;
    ULONG NumArpOffloadIPv4Addresses;
    ULONG NumNSOffloadIPv6Addresses;
    NDIS_DEVICE_POWER_STATE MinMagicPacketWakeUp;
    NDIS_DEVICE_POWER_STATE MinPatternWakeUp;
    NDIS_DEVICE_POWER_STATE MinLinkChangeWakeUp;
    ULONG SupportedWakeUpEvents;
    ULONG MediaSpecificWakeUpEvents;
} NDIS_PM_CAPABILITIES, *PNDIS_PM_CAPABILITIES;

#define NDIS_SIZEOF_NDIS_PM_CAPABILITIES_REVISION_2 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_PM_CAPABILITIES, MediaSpecificWakeUpEvents)

typedef struct _NDIS_PM_PARAMETERS
{
    NDIS_OBJECT_HEADER Header;
    ULONG EnabledWoLPacketPatterns;
    ULONG EnabledProtocolOffloads;
    ULONG WakeUpFlags;
    ULONG MediaSpecificWakeUpEvents;
} NDIS_PM_PARAMETERS, *PNDIS_PM_PARAMETERS;

#define NDIS_SIZEOF_NDIS_PM_PARAMETERS_REVISION_2 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_PM_PARAMETERS, MediaSpecificWakeUpEvents)

typedef struct _NDIS_PM_WOL_PATTERN
{
    NDIS_OBJECT_HEADER Header;
    ULONG Flags;
    ULONG Priority;
    NDIS_PM_WOL_PACKET WoLPacketType;
    NDIS_PM_COUNTED_STRING FriendlyName;
    ULONG PatternId;
    ULONG NextWoLPatternOffset;
    union _WOL_PATTERN
    {
        struct _IPV4_TCP_SYN_WOL_PACKET_PARAMETERS
        {
            ULONG Flags;
            UCHAR IPv4SourceAddress[4];
            UCHAR IPv4DestAddress[4];
            USHORT TCPSourcePortNumber;
            USHORT TCPDestPortNumber;
        } IPv4TcpSynParameters;
        struct _IPV6_TCP_SYN_WOL_PACKET_PARAMETERS
        {
            ULONG Flags;
            UCHAR IPv6SourceAddress[16];
            UCHAR IPv6DestAddress[16];
            USHORT TCPSourcePortNumber;
            USHORT TCPDestPortNumber;
        } IPv6TcpSynParameters;
        struct _EAPOL_REQUEST_ID_MESSAGE_WOL_PACKET_PARAMETERS
        {
            ULONG Flags;
        } EapolRequestIdMessageParameters;
        struct _WOL_BITMAP_PATTERN
        {
            ULONG Flags;
            ULONG MaskOffset;
            ULONG MaskSize;
            ULONG PatternOffset;
            ULONG PatternSize;
        } WoLBitMapPattern;
    } WoLPattern;
} NDIS_PM_WOL_PATTERN, *PNDIS_PM_WOL_PATTERN;

#define NDIS_SIZEOF_NDIS_PM_WOL_PATTERN_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_PM_WOL_PATTERN, WoLPattern)

typedef struct _NDIS_PM_PROTOCOL_OFFLOAD
{
    NDIS_OBJECT_HEADER Header;
    ULONG Flags;
    ULONG Priority;
    NDIS_PM_PROTOCOL_OFFLOAD_TYPE ProtocolOffloadType;
    NDIS_PM_COUNTED_STRING FriendlyName;
    ULONG ProtocolOffloadId;
    ULONG NextProtocolOffloadOffset;
    union _PROTOCOL_OFFLOAD_PARAMETERS
    {
        struct _IPV4_ARP_PARAMETERS
        {
            ULONG Flags;
            UCHAR RemoteIPv4Address[4];
            UCHAR HostIPv4Address[4];
            UCHAR MacAddress[6];
        } IPv4ARPParameters;
        struct _IPV6_NS_PARAMETERS
        {
            ULONG Flags;
            UCHAR RemoteIPv6Address[16];
            UCHAR SolicitedNodeIPv6Address[16];
            UCHAR MacAddress[6];
            UCHAR TargetIPv6Addresses[2][16];
        } IPv6NSParameters;
        struct _DOT11_RSN_REKEY_PARAMETERS
        {
            ULONG Flags;
            UCHAR KCK[16];
            UCHAR KEK[16];
            ULONG64 KeyReplayCounter;
        } Dot11RSNRekeyParameters;
        struct _DOT11_RSN_REKEY_PARAMETERS_V2
        {
            ULONG Flags;
            ULONG64 KeyReplayCounter;
            ULONG AuthAlgo;
            ULONG KCKLength;
            ULONG KEKLength;
            UCHAR KCK[32];
            UCHAR KEK[32];
        } Dot11RSNRekeyParametersV2;
    } ProtocolOffloadParameters;
} NDIS_PM_PROTOCOL_OFFLOAD, *PNDIS_PM_PROTOCOL_OFFLOAD;

#define NDIS_SIZEOF_NDIS_PM_PROTOCOL_OFFLOAD_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_PM_PROTOCOL_OFFLOAD, ProtocolOffloadParameters)

typedef struct _NDIS_PM_WAKE_REASON
{
    NDIS_OBJECT_HEADER Header;
    ULONG Flags;
    NDIS_PM_WAKE_REASON_TYPE WakeReason;
    ULONG InfoBufferOffset;
    ULONG InfoBufferSize;
} NDIS_PM_WAKE_REASON, *PNDIS_PM_WAKE_REASON;

#define NDIS_SIZEOF_PM_WAKE_REASON_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_PM_WAKE_REASON, InfoBufferSize)

typedef struct _NDIS_PM_WAKE_PACKET
{
    NDIS_OBJECT_HEADER Header;
    ULONG Flags;
    ULONG PatternId;
    NDIS_PM_COUNTED_STRING PatternFriendlyName;
    ULONG OriginalPacketSize;
    ULONG SavedPacketSize;
    ULONG SavedPacketOffset;
} NDIS_PM_WAKE_PACKET, *PNDIS_PM_WAKE_PACKET;

#define NDIS_SIZEOF_PM_WAKE_PACKET_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_PM_WAKE_PACKET, SavedPacketOffset)

#ifdef __cplusplus
}
#endif
