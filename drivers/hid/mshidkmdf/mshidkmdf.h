/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pass-through HID to KMDF filter driver
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include <ntddk.h>
#include <hidport.h>

/* {FFAD15A2-A6F8-4E60-99C7-2B92624DDC25}, answered by the KMDF framework below us */
DEFINE_GUID(GUID_HIDKMDF_PRESENCE_INTERFACE,
            0xFFAD15A2, 0xA6F8, 0x4E60, 0x99, 0xC7, 0x2B, 0x92, 0x62, 0x4D, 0xDC, 0x25);

#define HIDKMDF_PRESENCE_INTERFACE_VERSION 1

typedef
NTSTATUS
NTAPI
HIDKMDF_NOTIFY_PRESENCE(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ BOOLEAN IsPresent);
typedef HIDKMDF_NOTIFY_PRESENCE *PHIDKMDF_NOTIFY_PRESENCE;

/**
 * @brief Interface block, also used as the hidclass minidriver extension.
 *
 * The responder fills in Interface; NotifyPresence and HidClassDevice are ours.
 */
typedef struct _HIDKMDF_PRESENCE_INTERFACE
{
    INTERFACE Interface;
    PHIDKMDF_NOTIFY_PRESENCE NotifyPresence;
    PDEVICE_OBJECT HidClassDevice;
} HIDKMDF_PRESENCE_INTERFACE, *PHIDKMDF_PRESENCE_INTERFACE;
