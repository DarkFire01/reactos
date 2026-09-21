/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Structure layouts as adapter 2.1 clients were built with them
 *
 * 2.1 moved packet filtering out to its own capabilities and let extensions
 * report protocol offloads.
 */

#pragma once

typedef struct _NET_ADAPTER_LINK_LAYER_CAPABILITIES_V2_1
{
    ULONG Size;
    ULONG64 MaxTxLinkSpeed;
    ULONG64 MaxRxLinkSpeed;
} NET_ADAPTER_LINK_LAYER_CAPABILITIES_V2_1;

typedef struct _NET_ADAPTER_NDIS_PM_CAPABILITIES_V2_1
{
    ULONG Size;
    ULONG MediaSpecificWakeUpEvents;
    ULONG SupportedProtocolOffloads;
} NET_ADAPTER_NDIS_PM_CAPABILITIES_V2_1;

typedef struct _NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_V2_1
{
    ULONG Size;
    NET_PACKET_FILTER_FLAGS SupportedPacketFilters;
    SIZE_T MaximumMulticastAddresses;
    PFN_NET_ADAPTER_SET_RECEIVE_FILTER EvtSetReceiveFilter;
} NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_V2_1;

typedef struct _NET_PACKET_QUEUE_CONFIG_V2_1
{
    ULONG Size;
    PFN_PACKET_QUEUE_START EvtStart;
    PFN_PACKET_QUEUE_STOP EvtStop;
    PFN_PACKET_QUEUE_ADVANCE EvtAdvance;
    PFN_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtSetNotificationEnabled;
    PFN_PACKET_QUEUE_CANCEL EvtCancel;
} NET_PACKET_QUEUE_CONFIG_V2_1;
