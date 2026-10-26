/*
 * PROJECT:     ReactOS PSDK
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Bluetooth radio IOCTLs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifndef __BTHIOCTL_H__
#define __BTHIOCTL_H__

#include <bthdef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BTH_IOCTL_BASE 0

#define BTH_CTL(id) CTL_CODE(FILE_DEVICE_BLUETOOTH, (id), METHOD_BUFFERED, FILE_ANY_ACCESS)
#define BTH_KERNEL_CTL(id) CTL_CODE(FILE_DEVICE_BLUETOOTH, (id), METHOD_NEITHER, FILE_ANY_ACCESS)

#define IOCTL_BTH_GET_LOCAL_INFO    BTH_CTL(BTH_IOCTL_BASE + 0x00)
#define IOCTL_BTH_GET_RADIO_INFO    BTH_CTL(BTH_IOCTL_BASE + 0x01)
#define IOCTL_BTH_GET_DEVICE_INFO   BTH_CTL(BTH_IOCTL_BASE + 0x02)
#define IOCTL_BTH_DISCONNECT_DEVICE BTH_CTL(BTH_IOCTL_BASE + 0x03)

#define LOCAL_RADIO_DISCOVERABLE 0x0001
#define LOCAL_RADIO_CONNECTABLE  0x0002

#include <pshpack1.h>

typedef struct _BTH_RADIO_INFO {
    ULONGLONG lmpSupportedFeatures;
    USHORT mfg;
    USHORT lmpSubversion;
    UCHAR lmpVersion;
} BTH_RADIO_INFO, *PBTH_RADIO_INFO;

typedef struct _BTH_LOCAL_RADIO_INFO {
    BTH_DEVICE_INFO localInfo;
    ULONG flags;
    USHORT hciRevision;
    UCHAR hciVersion;
    BTH_RADIO_INFO radioInfo;
} BTH_LOCAL_RADIO_INFO, *PBTH_LOCAL_RADIO_INFO;

#include <poppack.h>

#ifdef __cplusplus
}
#endif

#endif /* __BTHIOCTL_H__ */
