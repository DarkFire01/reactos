/*
 * PROJECT:     ReactOS USB Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Definitions shared between the kdusb translation units
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define NOEXTAPI
#include <ntifs.h>
#include <windbgkd.h>
#include <arc/arc.h>
#include <kddll.h>
#include <ndk/haltypes.h>
#include <ndk/halfuncs.h>

/* Largest packet either controller moves in one transfer */
#define KDUSB_TRANSFER_SIZE         4016

/* Longest TARGETNAME the host can ask for */
#define KDUSB_TARGET_NAME_MAX       24

/* DBG2 port type for a USB debug controller */
#define KDUSB_DBG2_PORT_TYPE_USB    0x8002

/* Programming interfaces of the two supported host controllers */
#define KDUSB_PROGIF_EHCI           0x20
#define KDUSB_PROGIF_XHCI           0x30

/* SharedUserData->KdDebuggerEnabled bit that tracks a live host */
#define KDUSB_SHARED_HOST_CONNECTED 0x02

/** One piece of an outgoing packet. */
typedef struct _KDUSB_BUFFER
{
    PVOID Data;
    ULONG Length;
} KDUSB_BUFFER, *PKDUSB_BUFFER;

typedef
BOOLEAN
(NTAPI *PKDUSB_INITIALIZE)(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock);

typedef
KDSTATUS
(NTAPI *PKDUSB_READ_PACKET)(
    _In_ ULONG PacketType,
    _Out_ PKD_PACKET Packet,
    _Inout_opt_ PSTRING MessageHeader,
    _Inout_opt_ PSTRING MessageData);

typedef
VOID
(NTAPI *PKDUSB_SEND_BUFFERS)(
    _In_reads_(Count) PKDUSB_BUFFER Buffers,
    _In_ ULONG Count);

typedef
VOID
(NTAPI *PKDUSB_SHUTDOWN)(VOID);

/** The routines a controller backend provides to the packet layer. */
typedef struct _KDUSB_CONTROLLER
{
    PKDUSB_INITIALIZE Initialize;
    PKDUSB_READ_PACKET ReadPacket;
    PKDUSB_SEND_BUFFERS SendBuffers;
    PKDUSB_SHUTDOWN Shutdown;
} KDUSB_CONTROLLER, *PKDUSB_CONTROLLER;

typedef
ULONG
(*PKDUSB_BOOT_PRINT)(
    _In_ PCSTR Format,
    ...);

/* Set only while phase 0 runs with /FRLDRSCREENLOG; prints on the loader's screen */
extern PKDUSB_BOOT_PRINT KdUsbBootPrint;

#if 0
#define KdUsbTrace(...)                         \
    do                                          \
    {                                           \
        if (KdUsbBootPrint != NULL)             \
            KdUsbBootPrint("kdusb: " __VA_ARGS__); \
    } while (0)
#else
#define KdUsbTrace(...)                         \
    do                                          \
    {                                           \
        if (0)                                  \
            KdUsbBootPrint("kdusb: " __VA_ARGS__); \
    } while (0)
#endif

extern DEBUG_DEVICE_DESCRIPTOR KdUsbDevice;
extern UCHAR KdUsbReceiveBuffer[KDUSB_TRANSFER_SIZE];
extern CHAR KdUsbTargetName[KDUSB_TARGET_NAME_MAX + 3];
extern ULONG KdUsbTargetNameLength;

extern const KDUSB_CONTROLLER KdUsbXhciController;
extern const KDUSB_CONTROLLER KdUsbEhciController;

/* kdusb.c */
VOID
NTAPI
KdUsbSetHostPresent(
    _In_ BOOLEAN Present);

BOOLEAN
NTAPI
KdUsbIsNameQuery(
    _In_ ULONG Length);

KDSTATUS
NTAPI
KdUsbCompletePacket(
    _In_ ULONG PacketType,
    _In_ ULONG Length,
    _Out_ PKD_PACKET Packet,
    _Inout_opt_ PSTRING MessageHeader,
    _Inout_opt_ PSTRING MessageData);

/* kdusbhw.c */
VOID
NTAPI
KdUsbStall(
    _In_ ULONG Microseconds);

/* xhcidbg.c */
VOID
NTAPI
KdUsbXhciRequestReset(VOID);
