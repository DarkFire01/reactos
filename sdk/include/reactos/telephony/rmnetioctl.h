/*
 * PROJECT:     ReactOS Telephony
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Control interface of the cellular data adapter (rmnet.sys)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* The RIL owns the data session, the adapter only carries its packets */
#define RMNET_CONTROL_DEVICE        L"\\\\.\\RmNet"

#define RMNET_IOCTL_TYPE            0x8000
#define IOCTL_RMNET_QUERY           CTL_CODE(RMNET_IOCTL_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RMNET_SET_LINK        CTL_CODE(RMNET_IOCTL_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define RMNET_STATE_NO_IPA          0
#define RMNET_STATE_READY           1
#define RMNET_STATE_CONNECTED       2

typedef struct _RMNET_QUERY
{
    ULONG Version;
    ULONG State;
    /* What the modem must bind the data session to, the IPA client id */
    ULONG MuxId;
    ULONG EndpointType;
    ULONG InterfaceId;
    ULONG Reserved;
    ULONG64 SentPackets;
    ULONG64 ReceivedPackets;
    ULONG64 DroppedPackets;
} RMNET_QUERY, *PRMNET_QUERY;

/* Addresses are in network byte order, as they go on the wire */
typedef struct _RMNET_LINK
{
    ULONG Version;
    ULONG Up;
    ULONG Address;
    ULONG SubnetMask;
    ULONG Gateway;
    ULONG PrimaryDns;
    ULONG SecondaryDns;
    ULONG Mtu;
} RMNET_LINK, *PRMNET_LINK;

#define RMNET_INTERFACE_VERSION     1
