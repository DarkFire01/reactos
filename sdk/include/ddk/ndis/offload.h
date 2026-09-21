/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Task offload capabilities and configuration
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define NDIS_OBJECT_TYPE_OFFLOAD                             0xA7
#define NDIS_OBJECT_TYPE_OFFLOAD_ENCAPSULATION               0xA8
#define NDIS_OBJECT_TYPE_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES 0xA0

#define NDIS_OFFLOAD_REVISION_1                     1
#define NDIS_OFFLOAD_REVISION_2                     2
#define NDIS_OFFLOAD_REVISION_3                     3
#define NDIS_OFFLOAD_REVISION_4                     4
#define NDIS_OFFLOAD_REVISION_5                     5
#define NDIS_OFFLOAD_REVISION_6                     6

#define NDIS_OFFLOAD_PARAMETERS_REVISION_1          1
#define NDIS_OFFLOAD_PARAMETERS_REVISION_2          2
#define NDIS_OFFLOAD_PARAMETERS_REVISION_3          3

#define NDIS_OFFLOAD_ENCAPSULATION_REVISION_1       1

#define NDIS_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES_REVISION_1 1

/* Encapsulation the offload descriptions below are expressed against. */
#define NDIS_ENCAPSULATION_NOT_SUPPORTED             0x00000000
#define NDIS_ENCAPSULATION_NULL                      0x00000001
#define NDIS_ENCAPSULATION_IEEE_802_3                0x00000002
#define NDIS_ENCAPSULATION_IEEE_802_3_P_AND_Q        0x00000004
#define NDIS_ENCAPSULATION_IEEE_802_3_P_AND_Q_IN_OOB 0x00000008
#define NDIS_ENCAPSULATION_IEEE_LLC_SNAP_ROUTED      0x00000010

#define NDIS_OFFLOAD_NOT_SUPPORTED                  0
#define NDIS_OFFLOAD_SUPPORTED                      1

#define NDIS_OFFLOAD_SET_NO_CHANGE                  0
#define NDIS_OFFLOAD_SET_ON                         1
#define NDIS_OFFLOAD_SET_OFF                        2

/* NDIS_OFFLOAD_PARAMETERS checksum fields. */
#define NDIS_OFFLOAD_PARAMETERS_NO_CHANGE              0
#define NDIS_OFFLOAD_PARAMETERS_TX_RX_DISABLED         1
#define NDIS_OFFLOAD_PARAMETERS_TX_ENABLED_RX_DISABLED 2
#define NDIS_OFFLOAD_PARAMETERS_RX_ENABLED_TX_DISABLED 3
#define NDIS_OFFLOAD_PARAMETERS_TX_RX_ENABLED          4

#define NDIS_OFFLOAD_PARAMETERS_LSOV1_DISABLED             1
#define NDIS_OFFLOAD_PARAMETERS_LSOV1_ENABLED              2
#define NDIS_OFFLOAD_PARAMETERS_IPSECV1_DISABLED           1
#define NDIS_OFFLOAD_PARAMETERS_IPSECV1_AH_ENABLED         2
#define NDIS_OFFLOAD_PARAMETERS_IPSECV1_ESP_ENABLED        3
#define NDIS_OFFLOAD_PARAMETERS_IPSECV1_AH_AND_ESP_ENABLED 4
#define NDIS_OFFLOAD_PARAMETERS_LSOV2_DISABLED             1
#define NDIS_OFFLOAD_PARAMETERS_LSOV2_ENABLED              2
#define NDIS_OFFLOAD_PARAMETERS_RSC_DISABLED               1
#define NDIS_OFFLOAD_PARAMETERS_RSC_ENABLED                2
#define NDIS_OFFLOAD_PARAMETERS_USO_DISABLED               1
#define NDIS_OFFLOAD_PARAMETERS_USO_ENABLED                2

typedef struct _NDIS_TCP_IP_CHECKSUM_OFFLOAD
{
    struct
    {
        ULONG Encapsulation;
        ULONG IpOptionsSupported : 2;
        ULONG TcpOptionsSupported : 2;
        ULONG TcpChecksum : 2;
        ULONG UdpChecksum : 2;
        ULONG IpChecksum : 2;
    } IPv4Transmit;
    struct
    {
        ULONG Encapsulation;
        ULONG IpOptionsSupported : 2;
        ULONG TcpOptionsSupported : 2;
        ULONG TcpChecksum : 2;
        ULONG UdpChecksum : 2;
        ULONG IpChecksum : 2;
    } IPv4Receive;
    struct
    {
        ULONG Encapsulation;
        ULONG IpExtensionHeadersSupported : 2;
        ULONG TcpOptionsSupported : 2;
        ULONG TcpChecksum : 2;
        ULONG UdpChecksum : 2;
    } IPv6Transmit;
    struct
    {
        ULONG Encapsulation;
        ULONG IpExtensionHeadersSupported : 2;
        ULONG TcpOptionsSupported : 2;
        ULONG TcpChecksum : 2;
        ULONG UdpChecksum : 2;
    } IPv6Receive;
} NDIS_TCP_IP_CHECKSUM_OFFLOAD, *PNDIS_TCP_IP_CHECKSUM_OFFLOAD;

typedef struct _NDIS_TCP_LARGE_SEND_OFFLOAD_V1
{
    struct
    {
        ULONG Encapsulation;
        ULONG MaxOffLoadSize;
        ULONG MinSegmentCount;
        ULONG TcpOptions : 2;
        ULONG IpOptions : 2;
    } IPv4;
} NDIS_TCP_LARGE_SEND_OFFLOAD_V1, *PNDIS_TCP_LARGE_SEND_OFFLOAD_V1;

typedef struct _NDIS_TCP_LARGE_SEND_OFFLOAD_V2
{
    struct
    {
        ULONG Encapsulation;
        ULONG MaxOffLoadSize;
        ULONG MinSegmentCount;
    } IPv4;
    struct
    {
        ULONG Encapsulation;
        ULONG MaxOffLoadSize;
        ULONG MinSegmentCount;
        ULONG IpExtensionHeadersSupported : 2;
        ULONG TcpOptionsSupported : 2;
    } IPv6;
} NDIS_TCP_LARGE_SEND_OFFLOAD_V2, *PNDIS_TCP_LARGE_SEND_OFFLOAD_V2;

typedef struct _NDIS_TCP_RECV_SEG_COALESCE_OFFLOAD
{
    struct
    {
        BOOLEAN Enabled;
    } IPv4;
    struct
    {
        BOOLEAN Enabled;
    } IPv6;
} NDIS_TCP_RECV_SEG_COALESCE_OFFLOAD, *PNDIS_TCP_RECV_SEG_COALESCE_OFFLOAD;

typedef struct _NDIS_UDP_SEGMENTATION_OFFLOAD
{
    struct
    {
        ULONG Encapsulation;
        ULONG MaxOffLoadSize;
        ULONG MinSegmentCount : 6;
        ULONG SubMssFinalSegmentSupported : 1;
        ULONG Reserved : 25;
    } IPv4;
    struct
    {
        ULONG Encapsulation;
        ULONG MaxOffLoadSize;
        ULONG MinSegmentCount : 6;
        ULONG SubMssFinalSegmentSupported : 1;
        ULONG Reserved1 : 25;
        ULONG IpExtensionHeadersSupported : 2;
        ULONG Reserved2 : 30;
    } IPv6;
} NDIS_UDP_SEGMENTATION_OFFLOAD, *PNDIS_UDP_SEGMENTATION_OFFLOAD;

typedef struct _NDIS_UDP_RSC_OFFLOAD
{
    BOOLEAN Enabled;
} NDIS_UDP_RSC_OFFLOAD, *PNDIS_UDP_RSC_OFFLOAD;

typedef struct _NDIS_IPSEC_OFFLOAD_V1
{
    struct
    {
        ULONG Encapsulation;
        ULONG AhEspCombined;
        ULONG TransportTunnelCombined;
        ULONG IPv4Options;
        ULONG Flags;
    } Supported;
    struct
    {
        ULONG Md5 : 2;
        ULONG Sha_1 : 2;
        ULONG Transport : 2;
        ULONG Tunnel : 2;
        ULONG Send : 2;
        ULONG Receive : 2;
    } IPv4AH;
    struct
    {
        ULONG Des : 2;
        ULONG Reserved : 2;
        ULONG TripleDes : 2;
        ULONG NullEsp : 2;
        ULONG Transport : 2;
        ULONG Tunnel : 2;
        ULONG Send : 2;
        ULONG Receive : 2;
    } IPv4ESP;
} NDIS_IPSEC_OFFLOAD_V1, *PNDIS_IPSEC_OFFLOAD_V1;

typedef struct _NDIS_IPSEC_OFFLOAD_V2
{
    ULONG Encapsulation;
    BOOLEAN IPv6Supported;
    BOOLEAN IPv4Options;
    BOOLEAN IPv6NonIPsecExtensionHeaders;
    BOOLEAN Ah;
    BOOLEAN Esp;
    BOOLEAN AhEspCombined;
    BOOLEAN Transport;
    BOOLEAN Tunnel;
    BOOLEAN TransportTunnelCombined;
    BOOLEAN LsoSupported;
    BOOLEAN ExtendedSequenceNumbers;
    ULONG UdpEsp;
    ULONG AuthenticationAlgorithms;
    ULONG EncryptionAlgorithms;
    ULONG SaOffloadCapacity;
} NDIS_IPSEC_OFFLOAD_V2, *PNDIS_IPSEC_OFFLOAD_V2;

typedef struct _NDIS_ENCAPSULATED_PACKET_TASK_OFFLOAD
{
    ULONG TransmitChecksumOffloadSupported : 4;
    ULONG ReceiveChecksumOffloadSupported : 4;
    ULONG LsoV2Supported : 4;
    ULONG RssSupported : 4;
    ULONG VmqSupported : 4;
    ULONG UsoSupported : 4;
    ULONG Reserved : 8;
    ULONG MaxHeaderSizeSupported;
} NDIS_ENCAPSULATED_PACKET_TASK_OFFLOAD, *PNDIS_ENCAPSULATED_PACKET_TASK_OFFLOAD;

typedef struct _NDIS_ENCAPSULATED_PACKET_TASK_OFFLOAD_V2
{
    ULONG TransmitChecksumOffloadSupported : 4;
    ULONG ReceiveChecksumOffloadSupported : 4;
    ULONG LsoV2Supported : 4;
    ULONG RssSupported : 4;
    ULONG VmqSupported : 4;
    ULONG UsoSupported : 4;
    ULONG Reserved : 8;
    ULONG MaxHeaderSizeSupported;
    union _ENCAPSULATION_PROTOCOL_INFO
    {
        struct _VXLAN_INFO
        {
            USHORT VxlanUDPPortNumber;
            USHORT VxlanUDPPortNumberConfigurable : 1;
        } VxlanInfo;
        ULONG Value;
    } EncapsulationProtocolInfo;
    ULONG Reserved1;
    ULONG Reserved2;
} NDIS_ENCAPSULATED_PACKET_TASK_OFFLOAD_V2, *PNDIS_ENCAPSULATED_PACKET_TASK_OFFLOAD_V2;

typedef enum _NDIS_RFC6877_464XLAT_OFFLOAD_OPTIONS
{
    NDIS_RFC6877_464XLAT_OFFLOAD_NOT_SUPPORTED = 0,
    NDIS_RFC6877_464XLAT_OFFLOAD_SUPPORTED = 1,
    NDIS_RFC6877_464XLAT_OFFLOAD_ENABLED = 2
} NDIS_RFC6877_464XLAT_OFFLOAD_OPTIONS;

typedef struct _NDIS_RFC6877_464XLAT_OFFLOAD
{
    NDIS_RFC6877_464XLAT_OFFLOAD_OPTIONS XlatOffload;
} NDIS_RFC6877_464XLAT_OFFLOAD, *PNDIS_RFC6877_464XLAT_OFFLOAD;

typedef struct _NDIS_OFFLOAD
{
    NDIS_OBJECT_HEADER Header;
    NDIS_TCP_IP_CHECKSUM_OFFLOAD Checksum;
    NDIS_TCP_LARGE_SEND_OFFLOAD_V1 LsoV1;
    NDIS_IPSEC_OFFLOAD_V1 IPsecV1;
    NDIS_TCP_LARGE_SEND_OFFLOAD_V2 LsoV2;
    ULONG Flags;
    NDIS_IPSEC_OFFLOAD_V2 IPsecV2;
    NDIS_TCP_RECV_SEG_COALESCE_OFFLOAD Rsc;
    NDIS_ENCAPSULATED_PACKET_TASK_OFFLOAD EncapsulatedPacketTaskOffloadGre;
    NDIS_ENCAPSULATED_PACKET_TASK_OFFLOAD_V2 EncapsulatedPacketTaskOffloadVxlan;
    UCHAR EncapsulationTypes;
    NDIS_RFC6877_464XLAT_OFFLOAD Rfc6877Xlat;
    NDIS_UDP_SEGMENTATION_OFFLOAD UdpSegmentation;
    NDIS_UDP_RSC_OFFLOAD UdpRsc;
} NDIS_OFFLOAD, *PNDIS_OFFLOAD;

#define NDIS_SIZEOF_NDIS_OFFLOAD_REVISION_6 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_OFFLOAD, UdpRsc)

typedef struct _NDIS_OFFLOAD_PARAMETERS
{
    NDIS_OBJECT_HEADER Header;
    UCHAR IPv4Checksum;
    UCHAR TCPIPv4Checksum;
    UCHAR UDPIPv4Checksum;
    UCHAR TCPIPv6Checksum;
    UCHAR UDPIPv6Checksum;
    UCHAR LsoV1;
    UCHAR IPsecV1;
    UCHAR LsoV2IPv4;
    UCHAR LsoV2IPv6;
    UCHAR TcpConnectionIPv4;
    UCHAR TcpConnectionIPv6;
    ULONG Flags;
    UCHAR IPsecV2;
    UCHAR IPsecV2IPv4;
    UCHAR RscIPv4;
    UCHAR RscIPv6;
    UCHAR EncapsulatedPacketTaskOffload;
    UCHAR EncapsulationTypes;
    union _ENCAPSULATION_PROTOCOL_PARAMETERS
    {
        struct _VXLAN_PARAMETERS
        {
            USHORT VxlanUDPPortNumber;
        } VxlanParameters;
        ULONG Value;
    } EncapsulationProtocolParameters;
    struct
    {
        UCHAR IPv4;
        UCHAR IPv6;
    } UdpSegmentation;
    struct
    {
        UCHAR Enabled;
    } UdpRsc;
} NDIS_OFFLOAD_PARAMETERS, *PNDIS_OFFLOAD_PARAMETERS;

#define NDIS_SIZEOF_OFFLOAD_PARAMETERS_REVISION_3 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_OFFLOAD_PARAMETERS, UdpRsc)

typedef struct _NDIS_OFFLOAD_ENCAPSULATION
{
    NDIS_OBJECT_HEADER Header;
    struct
    {
        ULONG Enabled;
        ULONG EncapsulationType;
        ULONG HeaderSize;
    } IPv4;
    struct
    {
        ULONG Enabled;
        ULONG EncapsulationType;
        ULONG HeaderSize;
    } IPv6;
} NDIS_OFFLOAD_ENCAPSULATION, *PNDIS_OFFLOAD_ENCAPSULATION;

#define NDIS_SIZEOF_OFFLOAD_ENCAPSULATION_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_OFFLOAD_ENCAPSULATION, IPv6)

struct _NDIS_TCP_CONNECTION_OFFLOAD;

typedef struct _NDIS_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES
{
    NDIS_OBJECT_HEADER Header;
    PNDIS_OFFLOAD DefaultOffloadConfiguration;
    PNDIS_OFFLOAD HardwareOffloadCapabilities;
    struct _NDIS_TCP_CONNECTION_OFFLOAD *DefaultTcpConnectionOffloadConfiguration;
    struct _NDIS_TCP_CONNECTION_OFFLOAD *TcpConnectionOffloadHardwareCapabilities;
} NDIS_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES, *PNDIS_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES;

#define NDIS_SIZEOF_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES, \
                             TcpConnectionOffloadHardwareCapabilities)

#ifdef __cplusplus
}
#endif
