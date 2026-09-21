/*
 * PROJECT:     ReactOS NetAdapterCx support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Packet queue callbacks shared by the transmit and receive queues
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef
_Function_class_(EVT_PACKET_QUEUE_START)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_PACKET_QUEUE_START(
    _In_ NETPACKETQUEUE PacketQueue);

typedef EVT_PACKET_QUEUE_START *PFN_PACKET_QUEUE_START;

typedef
_Function_class_(EVT_PACKET_QUEUE_STOP)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_PACKET_QUEUE_STOP(
    _In_ NETPACKETQUEUE PacketQueue);

typedef EVT_PACKET_QUEUE_STOP *PFN_PACKET_QUEUE_STOP;

typedef
_Function_class_(EVT_PACKET_QUEUE_CANCEL)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_PACKET_QUEUE_CANCEL(
    _In_ NETPACKETQUEUE PacketQueue);

typedef EVT_PACKET_QUEUE_CANCEL *PFN_PACKET_QUEUE_CANCEL;

typedef
_Function_class_(EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED(
    _In_ NETPACKETQUEUE PacketQueue,
    _In_ BOOLEAN NotificationEnabled);

typedef EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED *PFN_PACKET_QUEUE_SET_NOTIFICATION_ENABLED;

/* Advance runs at DISPATCH_LEVEL, the rest are passive. */
typedef
_Function_class_(EVT_PACKET_QUEUE_ADVANCE)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
EVT_PACKET_QUEUE_ADVANCE(
    _In_ NETPACKETQUEUE PacketQueue);

typedef EVT_PACKET_QUEUE_ADVANCE *PFN_PACKET_QUEUE_ADVANCE;

typedef struct _NET_PACKET_QUEUE_CONFIG
{
    ULONG Size;
    PFN_PACKET_QUEUE_START EvtStart;
    PFN_PACKET_QUEUE_STOP EvtStop;
    PFN_PACKET_QUEUE_ADVANCE EvtAdvance;
    PFN_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtSetNotificationEnabled;
    PFN_PACKET_QUEUE_CANCEL EvtCancel;
} NET_PACKET_QUEUE_CONFIG;

/* Start and Stop are optional and stay null unless the caller sets them. */
FORCEINLINE
VOID
NET_PACKET_QUEUE_CONFIG_INIT(
    _Out_ NET_PACKET_QUEUE_CONFIG *Config,
    _In_ PFN_PACKET_QUEUE_ADVANCE EvtAdvance,
    _In_ PFN_PACKET_QUEUE_SET_NOTIFICATION_ENABLED EvtSetNotificationEnabled,
    _In_ PFN_PACKET_QUEUE_CANCEL EvtCancel)
{
    RtlZeroMemory(Config, sizeof(*Config));

    Config->Size = sizeof(*Config);
    Config->EvtAdvance = EvtAdvance;
    Config->EvtSetNotificationEnabled = EvtSetNotificationEnabled;
    Config->EvtCancel = EvtCancel;
}

#ifdef __cplusplus
}
#endif
