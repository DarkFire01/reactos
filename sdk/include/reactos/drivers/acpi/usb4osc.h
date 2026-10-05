/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USB4 _OSC request the ACPI root device answers for the USB4 host router
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/* Device interface the ACPI root device registers; the request goes to it */
DEFINE_GUID(GUID_DEVINTERFACE_UACPINT_ROOT,
    0x15c5e253, 0xb8c4, 0x452e, 0x99, 0xdf, 0x46, 0x1d, 0x0f, 0x50, 0x78, 0xc8);

/* \_SB._OSC UUID for USB4 capabilities */
DEFINE_GUID(GUID_UACPINT_USB4_OSC,
    0x23a0d13a, 0x26ab, 0x486c, 0x9c, 0x5f, 0x0f, 0xfa, 0x52, 0x5a, 0x57, 0x5a);

#define IOCTL_UACPINT_USB4_OSC \
    CTL_CODE(FILE_DEVICE_ACPI, 19, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define UACPINT_USB4_OSC_SIGNATURE      'UcEA'
#define UACPINT_USB4_OSC_REVISION       1

/* USB4 _OSC support field */
#define UACPINT_USB4_SUPPORT_VERSION_2  0x00000001

/* USB4 _OSC control field */
#define UACPINT_USB4_CONTROL_USB3       0x00000001
#define UACPINT_USB4_CONTROL_DP         0x00000002
#define UACPINT_USB4_CONTROL_PCIE       0x00000004
#define UACPINT_USB4_CONTROL_XDOMAIN    0x00000008
#define UACPINT_USB4_CONTROL_ALL        0x0000000F

/** Buffered in and out. Query TRUE asks without committing; Revision must be nonzero. */
typedef struct _UACPINT_USB4_OSC_REQUEST
{
    ULONG Signature;
    USHORT Revision;
    BOOLEAN Query;
    UCHAR Reserved0;
    ULONG Support;
    ULONG ControlRequested;
    ULONG ControlGranted;
    BOOLEAN Usb4Present;
    BOOLEAN ReEvaluated;
    BOOLEAN ControlRetained;
    UCHAR Reserved1;
} UACPINT_USB4_OSC_REQUEST, *PUACPINT_USB4_OSC_REQUEST;

C_ASSERT(sizeof(UACPINT_USB4_OSC_REQUEST) == 24);
