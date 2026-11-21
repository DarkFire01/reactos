/*
 * PROJECT:     ReactOS MSRil Broker
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntifs.h>
#include <initguid.h>
#include <telephony/rilwire.h>

#define TAG_MSRIL               'liRM'

/* Bytes a client may have queued before the broker gives up on it */
#define MSRIL_CLIENT_BUDGET     0x100000

/* Broker devices are numbered MSRil1 to MSRil32 */
#define MSRIL_MAX_INSTANCES     32

typedef enum _MSRIL_FILE_ROLE
{
    MsRilRoleClient,
    MsRilRoleHost
} MSRIL_FILE_ROLE;

typedef enum _MSRIL_CLIENT_STATE
{
    MsRilClientLive,
    MsRilClientDisconnected,
    MsRilClientOverBudget
} MSRIL_CLIENT_STATE;

typedef struct _MSRIL_RECORD
{
    LIST_ENTRY Link;
    ULONG Length;
    MSRIL_HOST_EVENT Event;
    UCHAR Data[ANYSIZE_ARRAY];
} MSRIL_RECORD, *PMSRIL_RECORD;

typedef struct _MSRIL_FILE
{
    MSRIL_FILE_ROLE Role;
    LIST_ENTRY Link;
    ULONG ClientId;
    ULONG ProcessId;
    MSRIL_CLIENT_STATE State;

    /* Client: records waiting to be read. Host: events waiting to be fetched. */
    LIST_ENTRY Records;
    ULONG RecordCount;
    ULONG QueuedBytes;

    /* Client: pending reads. Host: pending event fetches. */
    LIST_ENTRY Irps;

    /* Client: close event for the host, allocated up front so cleanup cannot fail */
    PMSRIL_RECORD Closed;

    /* Client: one flag per notification code it enabled */
    UCHAR Enabled[RIL_RECORD_NOTIFY_COUNT];
} MSRIL_FILE, *PMSRIL_FILE;

typedef struct _MSRIL_EXTENSION
{
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT Lower;
    PDEVICE_OBJECT Pdo;
    ULONG Instance;
    UNICODE_STRING LinkName;
    WCHAR LinkNameBuffer[32];
    LONG OpenHandles;

    /* Serializes interface state changes, which must run at PASSIVE_LEVEL */
    KEVENT InterfaceLock;
    UNICODE_STRING InterfaceName;
    BOOLEAN InterfaceEnabled;
    BOOLEAN Started;
    BOOLEAN Removed;

    /* Guards everything below and the per-file lists */
    KSPIN_LOCK Lock;
    PMSRIL_FILE Host;
    LIST_ENTRY Clients;
    ULONG NextClientId;

    /* The network sends its time once per registration, a later client gets this copy */
    PMSRIL_RECORD LastNitz;
} MSRIL_EXTENSION, *PMSRIL_EXTENSION;
