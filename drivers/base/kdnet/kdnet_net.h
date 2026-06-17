

#ifndef _KDNET_NET_H_
#define _KDNET_NET_H_

#include "kdnet_crypto.h"

/* ------------------------------------------------------------------ wire ---*/
#include <pshpack1.h>

typedef struct _ETHERNET_ADDRESS
{
    UCHAR Address[6];
} ETHERNET_ADDRESS, *PETHERNET_ADDRESS;

typedef struct _ETHERNET_HEADER
{
    ETHERNET_ADDRESS Destination;
    ETHERNET_ADDRESS Source;
    USHORT EtherType;          /* big-endian on the wire */
} ETHERNET_HEADER, *PETHERNET_HEADER;

typedef struct _ETHERNET_PACKET
{
    ETHERNET_HEADER Header;
    UCHAR Data[1];
} ETHERNET_PACKET, *PETHERNET_PACKET;

/* IP header: bitfields laid out to match the decompiled struct on x86 LE. */
typedef struct _IP_HEADER
{
    UCHAR  HeaderLength : 4;
    UCHAR  Version : 4;
    UCHAR  ServiceType;
    USHORT Length;             /* big-endian */
    USHORT ID;
    USHORT Offset : 13;
    USHORT Flags : 3;
    UCHAR  TimeToLive;
    UCHAR  Protocol;
    USHORT HeaderChecksum;
    ULONG  Source;             /* big-endian */
    ULONG  Destination;
} IP_HEADER, *PIP_HEADER;

typedef struct _UDP_HEADER
{
    USHORT SourcePort;         /* big-endian */
    USHORT DestinationPort;
    USHORT Length;
    USHORT Checksum;
} UDP_HEADER, *PUDP_HEADER;

typedef struct _ARP
{
    USHORT Hardware;
    USHORT Protocol;
    UCHAR  AddressLength;
    UCHAR  ProtocolLength;
    USHORT OpCode;
    UCHAR  Data[1];            /* SenderMac, SenderIP, TargetMac, TargetIP */
} ARP, *PARP;

typedef struct _ETHERNET_IP   /* ARP payload body */
{
    ETHERNET_ADDRESS SourceMac;
    ULONG  SourceIP;
    ETHERNET_ADDRESS TargetMac;
    ULONG  TargetIP;
} ETHERNET_IP, *PETHERNET_IP;

typedef struct _DHCP
{
    UCHAR  Opcode;
    UCHAR  HardwareAddressType;
    UCHAR  HardwareAddressLength;
    UCHAR  Hops;
    ULONG  TransactionID;
    USHORT Seconds;
    USHORT Flags;
    ULONG  ClientIpAddress;
    ULONG  YourIpAddress;
    ULONG  ServerIpAddress;
    ULONG  RelayAgentIpAddress;
    UCHAR  ClientHardwareAddress[16];
    UCHAR  ServerName[64];
    UCHAR  BootFileName[128];
    UCHAR  Options[1];
} DHCP, *PDHCP;

/* KD net packet header, V1 (cleartext link) and V2 (encrypted, in kdnet_crypto.h). */
typedef struct _KD_NET_HEADER
{
    USHORT    Signature;
    UCHAR     Version;
    UCHAR     Flags;
    UCHAR     Reserved;
    UCHAR     PaddingByteCount;
    ULONGLONG SequenceNumber;
} KD_NET_HEADER, *PKD_NET_HEADER;

#include <poppack.h>

/* EtherTypes (host order; byte-swapped onto the wire). */
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

#define IP_PROTOCOL_UDP 17

/* --------------------------------------------------------------- adapter ---*/

typedef struct _DHCP_STATE
{
    ULONG DhcpTransactionID;
    ULONG DhcpSeconds;
    ULONG DhcpServer;
    ULONG DhcpIPAddress;
    ULONG DhcpSubnetMask;
    ULONG DhcpRouterIP;
    ULONG DhcpState;
    ULONG DhcpRenewTime;
    ULONG DhcpRebindTime;
    ULONG DhcpLeaseTime;
    ULONG DhcpTimer;
    ULONG DhcpLeaseRenewed;
    ETHERNET_ADDRESS DhcpServerMac;
} DHCP_STATE, *PDHCP_STATE;


typedef struct _DEBUG_NET_PARAMETERS
{
    ULONG  TargetIP;
    USHORT TargetPort;
    ULONG  AssignedHostIP;
    USHORT AssignedHostPort;
    ULONG  HostIP;
    USHORT HostPort;
    ETHERNET_ADDRESS HostMac;
    UCHAR  DebuggerActive;
    UCHAR  EncryptedLink;
    UCHAR  Dhcp;
    UCHAR  VerifyHostMac;
    ULONG  Connected;
    volatile LONGLONG TargetSequenceNumber;
    ULONGLONG LastValidHostSequenceNumber;
    UCHAR  Key[32];
    UCHAR  SessionKey[32];
    ULONGLONG OfferTimer;
    UCHAR  TargetRandom[32];
    UCHAR  HostConnectionInfo[256];
    UCHAR  MachineId[32];
    UCHAR  OffersSendStatus;
    DHCP_STATE DhcpState;
} DEBUG_NET_PARAMETERS, *PDEBUG_NET_PARAMETERS;

typedef struct _DEBUG_NET_DATA
{
    KDNET_SHARED_DATA     KdNet;
    PDEBUG_NET_PARAMETERS Parameters;
    KDNET_CRYPTO_CONTEXT  Crypto;       /* HMacKey + Target/Session AES keys */
    ETHERNET_ADDRESS      TargetMac;
    ULONG                 TargetIP;
    USHORT                Vendor;
    ULONGLONG             InterruptTime;
    ULONGLONG             AccumulatedTimeout;
} DEBUG_NET_DATA, *PDEBUG_NET_DATA;

/* ----------------------------------------------------------- net helpers ---*/

ULONG KdpComputeChecksum(const UCHAR *Buffer, ULONG Length);

extern PDEBUG_NET_DATA      KdNetData;
extern DEBUG_NET_DATA       KdNetDataStorage;
extern DEBUG_NET_PARAMETERS KdNetParameters;

/* Byteswap an ethernet frame's headers between host and network order. */
VOID KdNetSwapPacket(PETHERNET_PACKET Packet, BOOLEAN ToNetwork);

/* Framing send path (host-order fields in; SwapPacket converts on the way out). */
NTSTATUS KdNetSendEthernetPacket(PDEBUG_NET_DATA Adapter, ULONG Handle, ULONG Length,
                                 PETHERNET_ADDRESS Source, PETHERNET_ADDRESS Destination, USHORT EtherType);
NTSTATUS KdNetSendIPPacket(PDEBUG_NET_DATA Adapter, ULONG Handle,
                           PETHERNET_ADDRESS SourceAddress, PETHERNET_ADDRESS DestinationAddress,
                           ULONG Length, ULONG SourceIP, ULONG DestinationIP,
                           UCHAR Protocol, UCHAR TypeOfService, UCHAR TimeToLive);
NTSTATUS KdNetSendUDPPacketEx(PDEBUG_NET_DATA Adapter, ULONG Handle,
                              PETHERNET_ADDRESS SourceAddress, PETHERNET_ADDRESS DestinationAddress,
                              ULONG SourceIP, ULONG DestinationIP, UCHAR TypeOfService, UCHAR TimeToLive,
                              ULONG Length, USHORT SourcePort, USHORT DestinationPort);
/* Build+encrypt+send a KD packet; returns >=0 (TX handle) on success. */
NTSTATUS KdNetSendKdPacket(PDEBUG_NET_DATA Adapter, ULONG Handle, ULONG Length,
                           USHORT SourcePort, USHORT DestinationPort);

/* Link-local (169.254.0.0/16) network base, host order. */
#define KDNET_LINKLOCAL_NET 0xA9FE0000

/* ARP reply handler (responds to host ARP-who-has-target-IP). */
NTSTATUS KdNetHandleArp(PDEBUG_NET_DATA Adapter, PETHERNET_PACKET Packet);

NTSTATUS KdNetGetNodeMacAddress(PDEBUG_NET_DATA Adapter, ULONG SourceIP, ULONG NodeIP,
                                PETHERNET_ADDRESS NodeAddress, ULONG Retries);

/* Bring up the network: assign target IP + resolve the host MAC (ARP). */
NTSTATUS KdNetInitializeNetwork(VOID);

/* Connection handshake. */
NTSTATUS KdNetSendOfferPacket(PDEBUG_NET_DATA Adapter, ULONG Handle,
                              PETHERNET_ADDRESS DestinationAddress, ULONG DestinationIP, USHORT DestinationPort);
NTSTATUS KdNetSendPingPacket(PDEBUG_NET_DATA Adapter);
BOOLEAN  KdNetEnableHostReconnect(PDEBUG_NET_DATA Adapter, ULONG Timeout);
/* Handle a decrypted control-channel packet (offer poke / connect response). */
NTSTATUS KdNetProcessControlChannelPacket(PDEBUG_NET_DATA Adapter, PUCHAR Packet, ULONG Length, ULONGLONG SequenceNumber);

extern ULONG     KdNetReconnectRunningTimeout;
extern ULONGLONG KdNetReconnectTimestamp;

/* Filtered receive: returns the UDP payload matching the given peers/ports.
 * On success *Packet -> UDP payload, *Length = payload length, and *SourcePort/
 * *DestinationPort are updated to the received (host-order) ports. */
NTSTATUS KdNetWaitForSpecificRxUdpPacketEx(PDEBUG_NET_DATA Adapter, PULONG Handle, PVOID *Packet,
                                           PULONG Length, PULONG Timeout,
                                           PETHERNET_ADDRESS SourceAddress, PETHERNET_ADDRESS DestinationAddress,
                                           ULONG SourceIP, ULONG DestinationIP,
                                           PUSHORT SourcePort, PUSHORT DestinationPort);

/* NIC accessors routed through the extensibility exports. */
NTSTATUS KdNetGetTxPacket(PDEBUG_NET_DATA Adapter, PULONG Handle);
NTSTATUS KdNetSendTxPacket(PDEBUG_NET_DATA Adapter, ULONG Handle, ULONG Length);
NTSTATUS KdNetGetRxPacket(PDEBUG_NET_DATA Adapter, PULONG Handle, PVOID *Packet, PULONG Length);
VOID     KdNetReleaseRxPacket(PDEBUG_NET_DATA Adapter, ULONG Handle);
PETHERNET_PACKET KdNetGetPacketAddress(PDEBUG_NET_DATA Adapter, ULONG Handle);
ULONG    KdNetGetPacketLength(PDEBUG_NET_DATA Adapter, ULONG Handle);
PUCHAR   KdNetGetPacketKdData(PDEBUG_NET_DATA Adapter, ULONG Handle);

#endif /* _KDNET_NET_H_ */
