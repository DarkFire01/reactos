/*
 * PROJECT:     ReactOS SuperSpeed USB Hub Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Control transfers to a child device and the descriptor cache
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#include "hubid.h"

/* MS OS 1.0 and 2.0 feature descriptor indexes (wIndex of the vendor request) */
#define HUB_MSOS_INDEX_EXT_CONFIG           4
#define HUB_MSOS_INDEX_EXT_PROPERTIES       5
#define HUB_MSOS_INDEX_CONTAINER_ID         6
#define HUB_MSOS20_INDEX_SET                7
#define HUB_MSOS20_INDEX_ALT_ENUM           8

/* String index of the MS OS string descriptor */
#define HUB_MSOS_STRING_INDEX               0xEE

/* devxfer.cpp */

/**
 * Sends the setup packet already in Child->m_Control on the default pipe of
 * the device. FALSE when the send was refused; the caller posts TransferFailed.
 */
BOOLEAN
NTAPI
HubDeviceSend(
    _In_ HubChild* Child,
    _In_reads_bytes_opt_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _In_ BOOLEAN ShortTransferOk);

/** Bytes the last device transfer returned. */
FORCEINLINE
ULONG
NTAPI
HubDeviceBytesReturned(
    _In_ HubChild* Child)
{
    return Child->m_Control.Urb.TransferBufferLength;
}

/* devdesc.cpp */

/** Validator context for a device, with its bitmap and Win8 errata. */
VOID
NTAPI
HubDeviceDescContext(
    _In_ HubChild* Child,
    _Out_ HubDescContext* Context);

/* hubioctl.cpp */

/**
 * Completes a user mode descriptor request the DSM sent for the hub FDO,
 * Information = Size + 12 on success, then drops the "User Mode FDO Request"
 * reference the IOCTL handler took on the device.
 */
VOID
NTAPI
HubCompleteFdoDescriptorRequest(
    _In_ HubChild* Child,
    _In_ WDFREQUEST Request,
    _In_ NTSTATUS Status,
    _In_ ULONG Size);
