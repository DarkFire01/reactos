/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pass-through HID to KMDF filter driver
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include <ntddk.h>
#include <hidport.h>

/*
 * The private contract between mshidkmdf and the KMDF framework underneath it.
 * It is published by no WDK header.
 *
 * GUID_WDF_HID_INTERFACE_STANDARD is {FFAD15A2-A6F8-4E60-99C7-2B92624DDC25}.
 */
DEFINE_GUID(GUID_WDF_HID_INTERFACE_STANDARD,
            0xFFAD15A2, 0xA6F8, 0x4E60, 0x99, 0xC7, 0x2B, 0x92, 0x62, 0x4D, 0xDC, 0x25);

typedef NTSTATUS
(NTAPI *PHID_KMDF_NOTIFY_PRESENCE)(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ BOOLEAN IsPresent);

/*
 * The block exchanged over IRP_MN_QUERY_INTERFACE, and simultaneously this
 * minidriver's HIDCLASS device extension. HidKmdfAddDevice hands hidclass's
 * MiniDeviceExtension straight to the responder as the interface buffer.
 *
 * Layout is fixed by the x64 offsets HidKmdfAddDevice and
 * HidKmdfPnp (:473) use, noted per member.  The reference hardcodes the total
 * as 48; sizeof() is the same number there and the right one on i386.
 *
 * INTERFACE proper travels *up*: the responder fills in Context and the
 * reference/dereference pair.  The two trailing members travel *down*: we
 * publish hidclass's HidNotifyPresence entry point along with the hidclass FDO
 * to call it on, so the KMDF driver below can report its device coming or going.
 */
typedef struct _WDF_HID_INTERFACE
{
    INTERFACE Interface;                        /* +0x00  Size/Version/Context/Ref/Deref */
    PHID_KMDF_NOTIFY_PRESENCE NotifyPresence;   /* +0x20  ours, read by the responder */
    PDEVICE_OBJECT HidClassDeviceObject;        /* +0x28  ours, read by the responder */
} WDF_HID_INTERFACE, *PWDF_HID_INTERFACE;

/* Version stamped into Interface.Version and the query. */
#define WDF_HID_INTERFACE_VERSION 1
