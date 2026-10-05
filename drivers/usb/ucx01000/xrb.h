/*
 * PROJECT:     ReactOS USB Host Controller Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Extended URBs (XRBs) and the shared URB completion helpers
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/*
 * An XRB is a URB UCX allocated for a client through the USBD client
 * interface. This header sits right in front of the URB the client sees;
 * its last field is the signature so a client underrun destroys it.
 */
#define UCX_XRB_SIGNATURE           0x2E425258

#define UCX_XRB_TYPE_GENERAL        0xAABB0000
#define UCX_XRB_TYPE_SELECT_CONFIG  0xAABB0001
#define UCX_XRB_TYPE_SELECT_IFACE   0xAABB0002
#define UCX_XRB_TYPE_ISOCH          0xAABB0003

#define UCX_XRB_STATE_INACTIVE      0xABCD0000
#define UCX_XRB_STATE_ACTIVE        0xABCD0001

/* BUGCODE_USB3_DRIVER and the reasons this driver raises */
#define UCX_BUGCHECK_USB3           0x144
#define UCX_USB3_ACTIVE_URB_REUSED  0x1
#define UCX_USB3_XRB_CORRUPTED      0x3
#define UCX_USB3_STREAMS_IRQL       0x800
#define UCX_USB3_STREAMS_NOT_GRANTED 0x801
#define UCX_USB3_STREAMS_BAD_COUNT  0x802
#define UCX_USB3_STREAMS_OPEN       0x803
#define UCX_USB3_HANDLE_LEAKED      0x804
#define UCX_USB3_CHAINED_MDL        0x806

struct UcxXrbHeader
{
    ULONG TotalSize;
    ULONG ContractVersion;
    ULONG Type;
    ULONG State;
    PIRP Irp;
    UcxUsbdHandle* Handle;
    WDFMEMORY Memory;
    WDFREQUEST Request;
    LIST_ENTRY TrackingLink;
    PVOID Reserved;
#ifdef _WIN64
    ULONG Pad;
#endif
    ULONG Signature;
};

#ifdef _WIN64
C_ASSERT(sizeof(UcxXrbHeader) == 0x50);
#else
C_ASSERT(sizeof(UcxXrbHeader) == 0x30);
#endif
C_ASSERT(FIELD_OFFSET(UcxXrbHeader, Signature) == sizeof(UcxXrbHeader) - sizeof(ULONG));

FORCEINLINE
UcxXrbHeader*
UcxXrbFromUrb(
    _In_ PURB Urb)
{
    return (UcxXrbHeader*)Urb - 1;
}

FORCEINLINE
PURB
UcxUrbFromXrb(
    _In_ UcxXrbHeader* Xrb)
{
    return (PURB)(Xrb + 1);
}

/*
 * A URB is an XRB exactly when the client's usbd library put the URB into
 * the stack location's FileObject as well as Argument1.
 */
FORCEINLINE
BOOLEAN
UcxIsXrbIrp(
    _In_ PIRP Irp,
    _In_ PURB Urb)
{
    return IoGetCurrentIrpStackLocation(Irp)->FileObject == (PFILE_OBJECT)Urb;
}

/* Every path that completes an XRB lets the client reuse it again */
FORCEINLINE
VOID
UcxXrbMarkInactive(
    _In_ PURB Urb)
{
    UcxXrbFromUrb(Urb)->State = UCX_XRB_STATE_INACTIVE;
}

/* IOCTL_INTERNAL_USB_SUBMIT_URB at the root hub PDO */
NTSTATUS
NTAPI
UcxProcessSubmitUrb(
    _In_ WDFDEVICE RootHubPdo,
    _In_ PIRP Irp);

/* Synchronous completion helpers shared by the URB, endpoint and controller code */

/** Sets both statuses and completes at the current IRQL; returns Status. */
NTSTATUS
NTAPI
UcxCompleteUrb(
    _In_ PIRP Irp,
    _In_ PURB Urb,
    _In_ NTSTATUS Status,
    _In_ USBD_STATUS UsbdStatus);

/** Same, but plain URBs complete at DISPATCH_LEVEL as they did under USBPORT. */
VOID
NTAPI
UcxCompleteUrbAtDispatch(
    _In_ PIRP Irp,
    _In_ PURB Urb,
    _In_ NTSTATUS Status,
    _In_ USBD_STATUS UsbdStatus);

/** Completes an IRP held after STATUS_MORE_PROCESSING_REQUIRED, statuses as they are. */
VOID
NTAPI
UcxCompleteHeldUrbIrp(
    _In_ PIRP Irp);

/* Static streams forwarding to the device management queue */
NTSTATUS
NTAPI
UcxForwardStreamsUrb(
    _In_ WDFDEVICE RootHubPdo,
    _In_ PIRP Irp,
    _In_ PURB Urb,
    _In_ UcxController* Controller);
