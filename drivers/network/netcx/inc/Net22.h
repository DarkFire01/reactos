/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Structure layouts as adapter 2.2 clients were built with them
 *
 * The execution context member of the packet queue configuration was only
 * ever in the preview headers, so a released 2.2 client still passes the
 * 2.0 sized structure.
 */

#pragma once

typedef struct _NET_ADAPTER_NDIS_PM_CAPABILITIES_V2_2
{
    ULONG Size;
    ULONG MediaSpecificWakeUpEvents;
    ULONG SupportedProtocolOffloads;
} NET_ADAPTER_NDIS_PM_CAPABILITIES_V2_2;

typedef struct _NET_PACKET_QUEUE_CONFIG_V2_2
{
    ULONG Size;
    PFN_PACKET_QUEUE_START EvtStart;
    PFN_PACKET_QUEUE_STOP EvtStop;
    PFN_PACKET_QUEUE_ADVANCE EvtAdvance;
    PFN_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtSetNotificationEnabled;
    PFN_PACKET_QUEUE_CANCEL EvtCancel;
} NET_PACKET_QUEUE_CONFIG_V2_2;
