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

/* Records a client may hold before deliveries to it start failing */
#define MSRIL_MAX_CLIENT_QUEUE  512

typedef enum _MSRIL_FILE_ROLE
{
    MsRilRoleClient,
    MsRilRoleHost
} MSRIL_FILE_ROLE;

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

    /* Client: the host that served it went away */
    BOOLEAN Orphaned;

    /* Client: records waiting to be read. Host: events waiting to be fetched. */
    LIST_ENTRY Records;
    ULONG RecordCount;

    /* Client: pending reads. Host: pending event fetches. */
    LIST_ENTRY Irps;
} MSRIL_FILE, *PMSRIL_FILE;

typedef struct _MSRIL_EXTENSION
{
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT Lower;
    PDEVICE_OBJECT Pdo;
    ULONG Instance;
    UNICODE_STRING LinkName;
    WCHAR LinkNameBuffer[32];
    UNICODE_STRING InterfaceName;
    BOOLEAN InterfaceEnabled;
    BOOLEAN Removed;

    /* Guards everything below and the per-file lists */
    KSPIN_LOCK Lock;
    PMSRIL_FILE Host;
    LIST_ENTRY Clients;
    ULONG NextClientId;
} MSRIL_EXTENSION, *PMSRIL_EXTENSION;
