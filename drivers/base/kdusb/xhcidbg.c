/*
 * PROJECT:     ReactOS USB Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     xHCI Debug Capability backend
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdusb.h"

/* xHCI capability registers */
#define XHCI_CAPLENGTH              0x00
#define XHCI_HCCPARAMS1             0x10

#define XHCI_HCCPARAMS1_AC64        0x00000001
#define XHCI_HCCPARAMS1_XECP(Value) (((Value) >> 16) * sizeof(ULONG))

/* xHCI operational registers */
#define XHCI_USBCMD                 0x00
#define XHCI_USBSTS                 0x04
#define XHCI_PAGESIZE               0x08

#define XHCI_USBCMD_RS              0x00000001
#define XHCI_USBCMD_HCRST           0x00000002
#define XHCI_USBSTS_HCH             0x00000001
#define XHCI_USBSTS_CNR             0x00000800
#define XHCI_PAGESIZE_4K            0x00000001

#define XHCI_EXT_CAP_LEGACY         1
#define XHCI_EXT_CAP_DEBUG          10

/* USB Legacy Support capability */
#define XHCI_USBLEGSUP                  0x00
#define XHCI_USBLEGCTLSTS               0x04
#define XHCI_USBLEGSUP_BIOS_OWNED       0x00010000
#define XHCI_USBLEGSUP_OS_OWNED         0x01000000
#define XHCI_USBLEGCTLSTS_SMI_ENABLES   0x0000E011
#define XHCI_USBLEGCTLSTS_SMI_EVENTS    0xE0000000

/* Debug Capability registers, relative to its extended capability */
#define DBC_DCDB                    0x04
#define DBC_DCERSTSZ                0x08
#define DBC_DCERSTBA                0x10
#define DBC_DCERDP                  0x18
#define DBC_DCCTRL                  0x20
#define DBC_DCPORTSC                0x28
#define DBC_DCCP                    0x30
#define DBC_DCDDI1                  0x38
#define DBC_DCDDI2                  0x3C

#define DBC_DCCTRL_DCR              0x00000001
#define DBC_DCCTRL_LSE              0x00000002
#define DBC_DCCTRL_DRC              0x00000010
#define DBC_DCCTRL_DCE              0x80000000
#define DBC_DCCTRL_ENABLE           (DBC_DCCTRL_DCE | DBC_DCCTRL_LSE)
#define DBC_DCCTRL_MAX_BURST(Value) (((Value) >> 16) & 0xFF)
#define DBC_DCPORTSC_CCS            0x00000001

#define DBC_DOORBELL_OUT            0x000
#define DBC_DOORBELL_IN             0x100

/* Vendor and product the Windows debugger host binds to */
#define DBC_DCDDI1_VALUE            0x349500FF
#define DBC_DCDDI2_VALUE            0x000000E0

/* TRB fields */
#define XHCI_TRB_CYCLE              0x00000001
#define XHCI_TRB_TOGGLE_CYCLE       0x00000002
#define XHCI_TRB_ISP                0x00000004
#define XHCI_TRB_IOC                0x00000020
#define XHCI_TRB_TYPE(Type)         ((ULONG)(Type) << 10)
#define XHCI_TRB_TYPE_OF(Control)   (((Control) >> 10) & 0x3F)

#define XHCI_TRB_NORMAL             1
#define XHCI_TRB_LINK               6
#define XHCI_TRB_TRANSFER_EVENT     32

#define XHCI_EVENT_RESIDUAL(Status)     ((Status) & 0x00FFFFFF)
#define XHCI_EVENT_COMPLETION(Status)   ((UCHAR)((Status) >> 24))
#define XHCI_COMPLETION_SUCCESS         1
#define XHCI_COMPLETION_SHORT_PACKET    13

#define ENDPOINT_TYPE_BULK_OUT      2
#define ENDPOINT_TYPE_BULK_IN       6
#define ENDPOINT_MAX_PACKET         1024

#define USB_STRING_DESCRIPTOR_TYPE  3
#define USB_LANGUAGE_EN_US          0x0409

/* One page of event TRBs; each transfer owns one page of data */
#define XHCI_EVENT_TRBS             (PAGE_SIZE / 16)
#define XHCI_RING_TRANSFERS         16

/* Timeouts, in microseconds */
#define XHCI_READY_TIMEOUT          1000000
#define XHCI_RESET_TIMEOUT          1000000
#define XHCI_HANDOFF_TIMEOUT        1000000
#define XHCI_HALT_TIMEOUT           20000
#define XHCI_HOST_TIMEOUT           2500000
#define XHCI_SEND_TIMEOUT           250000
#define XHCI_RECEIVE_TIMEOUT        2000000

typedef struct _XHCI_TRB
{
    ULONG64 Parameter;
    ULONG Status;
    ULONG Control;
} XHCI_TRB, *PXHCI_TRB;

typedef struct _XHCI_ERST_ENTRY
{
    ULONG64 Base;
    ULONG Size;
    ULONG Reserved;
} XHCI_ERST_ENTRY, *PXHCI_ERST_ENTRY;

typedef struct _DBC_INFO_CONTEXT
{
    ULONG64 String0;
    ULONG64 Manufacturer;
    ULONG64 Product;
    ULONG64 Serial;
    UCHAR String0Length;
    UCHAR ManufacturerLength;
    UCHAR ProductLength;
    UCHAR SerialLength;
    ULONG Reserved[7];
} DBC_INFO_CONTEXT, *PDBC_INFO_CONTEXT;

typedef struct _DBC_ENDPOINT_CONTEXT
{
    ULONG State;
    ULONG Info;
    ULONG64 Dequeue;
    ULONG AverageLength;
    ULONG Reserved[11];
} DBC_ENDPOINT_CONTEXT, *PDBC_ENDPOINT_CONTEXT;

typedef struct _DBC_CONTEXT
{
    DBC_INFO_CONTEXT Info;
    DBC_ENDPOINT_CONTEXT Out;
    DBC_ENDPOINT_CONTEXT In;
} DBC_CONTEXT, *PDBC_CONTEXT;

typedef struct _DBC_STRING
{
    UCHAR Length;
    UCHAR DescriptorType;
    WCHAR Characters[31];
} DBC_STRING, *PDBC_STRING;

/** Memory the controller reads and writes, laid over the block the HAL allocates. */
typedef struct _XHCI_SHARED
{
    XHCI_TRB EventRing[XHCI_EVENT_TRBS];
    XHCI_TRB OutRing[XHCI_RING_TRANSFERS + 1];
    XHCI_TRB InRing[XHCI_RING_TRANSFERS + 1];
    DECLSPEC_ALIGN(64) XHCI_ERST_ENTRY EventSegment;
    DECLSPEC_ALIGN(64) DBC_CONTEXT Context;
    DBC_STRING LanguageString;
    DBC_STRING ManufacturerString;
    DBC_STRING ProductString;
    DBC_STRING SerialString;
    DECLSPEC_ALIGN(PAGE_SIZE) UCHAR OutBuffer[XHCI_RING_TRANSFERS][PAGE_SIZE];
    UCHAR InBuffer[XHCI_RING_TRANSFERS][PAGE_SIZE];
} XHCI_SHARED, *PXHCI_SHARED;

C_ASSERT(sizeof(XHCI_TRB) == 16);
C_ASSERT(sizeof(DBC_INFO_CONTEXT) == 64);
C_ASSERT(sizeof(DBC_ENDPOINT_CONTEXT) == 64);
C_ASSERT(sizeof(DBC_STRING) == 64);
C_ASSERT(FIELD_OFFSET(XHCI_SHARED, OutRing) == PAGE_SIZE);
C_ASSERT(FIELD_OFFSET(XHCI_SHARED, OutBuffer) == 2 * PAGE_SIZE);
C_ASSERT(KDUSB_TRANSFER_SIZE < PAGE_SIZE);

/** Driver side view of one queued transfer. */
typedef struct _XHCI_TRANSFER
{
    ULONG Requested;
    ULONG Moved;
    UCHAR Completion;
    BOOLEAN Pending;
} XHCI_TRANSFER, *PXHCI_TRANSFER;

/** A transfer ring of XHCI_RING_TRANSFERS TRBs closed by a link TRB. */
typedef struct _XHCI_RING
{
    PXHCI_TRB Trb;
    PUCHAR Buffers;
    ULONG Enqueue;
    ULONG Cycle;
    XHCI_TRANSFER Transfer[XHCI_RING_TRANSFERS];
} XHCI_RING, *PXHCI_RING;

typedef struct _XHCI_DEBUG
{
    PXHCI_SHARED Shared;
    ULONG64 SharedAddress;
    PUCHAR Capabilities;
    ULONG CapabilitiesLength;
    PUCHAR Operational;
    PUCHAR Dbc;
    ULONG EventDequeue;
    ULONG EventCycle;
    XHCI_RING Out;
    XHCI_RING In;
    BOOLEAN ResetPending;
} XHCI_DEBUG, *PXHCI_DEBUG;

static XHCI_DEBUG XhciDebug = { .ResetPending = TRUE };

/* REGISTER ACCESS ************************************************************/

static
ULONG
NTAPI
XhciReadDbc(
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(XhciDebug.Dbc + Offset));
}

static
VOID
NTAPI
XhciWriteDbc(
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(XhciDebug.Dbc + Offset), Value);
    KeMemoryBarrier();
}

static
VOID
NTAPI
XhciWriteDbc64(
    _In_ ULONG Offset,
    _In_ ULONG64 Value)
{
    XhciWriteDbc(Offset, (ULONG)Value);
    XhciWriteDbc(Offset + 4, (ULONG)(Value >> 32));
}

/**
 * @brief
 * Polls a register until the masked bits match.
 *
 * @param[in] Register
 * The register to read.
 *
 * @param[in] Mask
 * The bits to compare.
 *
 * @param[in] Value
 * The value the masked bits must reach.
 *
 * @param[in] Timeout
 * Microseconds to wait.
 *
 * @param[in] Interval
 * Microseconds between reads.
 *
 * @return
 * TRUE when the bits matched in time.
 */
static
BOOLEAN
NTAPI
XhciPoll(
    _In_ PUCHAR Register,
    _In_ ULONG Mask,
    _In_ ULONG Value,
    _In_ ULONG Timeout,
    _In_ ULONG Interval)
{
    ULONG Waited;

    for (Waited = 0; ; Waited += Interval)
    {
        if ((READ_REGISTER_ULONG((PULONG)Register) & Mask) == Value)
            return TRUE;

        if (Waited >= Timeout)
            return FALSE;

        KdUsbStall(Interval);
    }
}

/**
 * @brief
 * Converts an address inside the shared block to the bus address the
 * controller uses. The HAL hands out one physically contiguous block.
 *
 * @param[in] Address
 * An address inside XhciDebug.Shared.
 *
 * @return
 * The matching physical address.
 */
static
ULONG64
NTAPI
XhciBusAddress(
    _In_ PVOID Address)
{
    return XhciDebug.SharedAddress + ((PUCHAR)Address - (PUCHAR)XhciDebug.Shared);
}

/* TRANSFER RINGS *************************************************************/

/**
 * @brief
 * Resets a transfer ring to empty. The TRBs must already be zeroed.
 *
 * @param[out] Ring
 * The ring to reset.
 *
 * @param[in] Trb
 * The ring's TRBs, link TRB included.
 *
 * @param[in] Buffers
 * The ring's data pages, one per transfer.
 */
static
VOID
NTAPI
XhciRingInitialize(
    _Out_ PXHCI_RING Ring,
    _In_ PXHCI_TRB Trb,
    _In_ PUCHAR Buffers)
{
    PXHCI_TRB Link = &Trb[XHCI_RING_TRANSFERS];

    RtlZeroMemory(Ring, sizeof(*Ring));
    Ring->Trb = Trb;
    Ring->Buffers = Buffers;
    Ring->Cycle = XHCI_TRB_CYCLE;

    /* Zeroed TRBs carry cycle 0, so the controller owns none of them yet */
    Link->Parameter = XhciBusAddress(Trb);
    Link->Control = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TOGGLE_CYCLE;
}

/**
 * @brief
 * Returns the data page of a ring slot.
 */
static
PUCHAR
NTAPI
XhciRingBuffer(
    _In_ PXHCI_RING Ring,
    _In_ ULONG Slot)
{
    return Ring->Buffers + Slot * PAGE_SIZE;
}

/**
 * @brief
 * Hands the next slot of a ring to the controller.
 *
 * The cycle bit is written last, in one store with the rest of the control
 * word, so the controller never sees a half built TRB.
 *
 * @param[in,out] Ring
 * The ring.
 *
 * @param[in] Length
 * Bytes to transfer.
 *
 * @param[in] Flags
 * Extra TRB control flags.
 *
 * @return
 * The slot that was queued.
 */
static
ULONG
NTAPI
XhciRingQueue(
    _Inout_ PXHCI_RING Ring,
    _In_ ULONG Length,
    _In_ ULONG Flags)
{
    ULONG Slot = Ring->Enqueue;
    PXHCI_TRB Trb = &Ring->Trb[Slot];
    PXHCI_TRANSFER Transfer = &Ring->Transfer[Slot];

    Transfer->Requested = Length;
    Transfer->Moved = 0;
    Transfer->Completion = 0;
    Transfer->Pending = TRUE;

    Trb->Parameter = XhciBusAddress(XhciRingBuffer(Ring, Slot));
    Trb->Status = Length;
    KeMemoryBarrier();
    *(volatile ULONG *)&Trb->Control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | Flags | Ring->Cycle;

    if (Slot + 1 < XHCI_RING_TRANSFERS)
    {
        Ring->Enqueue = Slot + 1;
        return Slot;
    }

    /* Last slot of the lap: pass the link TRB over too and start a new lap */
    KeMemoryBarrier();
    *(volatile ULONG *)&Ring->Trb[XHCI_RING_TRANSFERS].Control =
        XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TOGGLE_CYCLE | Ring->Cycle;

    Ring->Cycle ^= XHCI_TRB_CYCLE;
    Ring->Enqueue = 0;
    return Slot;
}

/**
 * @brief
 * Maps the TRB address in a transfer event to its transfer.
 *
 * @param[in] Address
 * The bus address of the completed TRB.
 *
 * @return
 * The transfer, or NULL when the address is not a data TRB of either ring.
 */
static
PXHCI_TRANSFER
NTAPI
XhciFindTransfer(
    _In_ ULONG64 Address)
{
    PXHCI_RING Rings[] = { &XhciDebug.Out, &XhciDebug.In };
    ULONG64 Offset;
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(Rings); Index++)
    {
        Offset = Address - XhciBusAddress(Rings[Index]->Trb);
        if (Offset < XHCI_RING_TRANSFERS * sizeof(XHCI_TRB) &&
            Offset % sizeof(XHCI_TRB) == 0)
        {
            return &Rings[Index]->Transfer[Offset / sizeof(XHCI_TRB)];
        }
    }

    return NULL;
}

/**
 * @brief
 * Drains the event ring and records each finished transfer.
 *
 * @return
 * FALSE when the Debug Capability has been disabled underneath us.
 */
static
BOOLEAN
NTAPI
XhciProcessEvents(VOID)
{
    PXHCI_SHARED Shared = XhciDebug.Shared;
    PXHCI_TRANSFER Transfer;
    PXHCI_TRB Event;
    ULONG Control;
    ULONG Residual;
    BOOLEAN Consumed = FALSE;

    if (!(XhciReadDbc(DBC_DCCTRL) & DBC_DCCTRL_DCE))
        return FALSE;

    for (;;)
    {
        Event = &Shared->EventRing[XhciDebug.EventDequeue];
        Control = *(volatile ULONG *)&Event->Control;
        if ((Control & XHCI_TRB_CYCLE) != XhciDebug.EventCycle)
            break;

        KeMemoryBarrier();

        Transfer = NULL;
        if (XHCI_TRB_TYPE_OF(Control) == XHCI_TRB_TRANSFER_EVENT)
            Transfer = XhciFindTransfer(Event->Parameter);

        if (Transfer != NULL)
        {
            /* The event reports what was left over */
            Residual = min(XHCI_EVENT_RESIDUAL(Event->Status), Transfer->Requested);
            Transfer->Moved = Transfer->Requested - Residual;
            Transfer->Completion = XHCI_EVENT_COMPLETION(Event->Status);
            if (Transfer->Completion == XHCI_COMPLETION_SHORT_PACKET)
                Transfer->Completion = XHCI_COMPLETION_SUCCESS;

            Transfer->Pending = FALSE;
        }

        XhciDebug.EventDequeue++;
        if (XhciDebug.EventDequeue == XHCI_EVENT_TRBS)
        {
            XhciDebug.EventDequeue = 0;
            XhciDebug.EventCycle ^= XHCI_TRB_CYCLE;
        }

        Consumed = TRUE;
    }

    if (Consumed)
        XhciWriteDbc64(DBC_DCERDP, XhciBusAddress(&Shared->EventRing[XhciDebug.EventDequeue]));

    return TRUE;
}

/* CONTROLLER *****************************************************************/

/**
 * @brief
 * Waits for the controller to clear Controller Not Ready.
 *
 * @return
 * STATUS_SUCCESS, STATUS_NO_SUCH_DEVICE when the registers read as all ones,
 * or STATUS_IO_TIMEOUT.
 */
static
NTSTATUS
NTAPI
XhciWaitForReady(VOID)
{
    PUCHAR Status = XhciDebug.Operational + XHCI_USBSTS;

    if (READ_REGISTER_ULONG((PULONG)Status) == 0xFFFFFFFF)
        return STATUS_NO_SUCH_DEVICE;

    if (!XhciPoll(Status, XHCI_USBSTS_CNR, 0, XHCI_READY_TIMEOUT, 100))
        return STATUS_IO_TIMEOUT;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Optionally resets the host controller, then waits for it to settle.
 *
 * A controller that is still running belongs to someone else, so it is left
 * alone and the reset fails.
 *
 * @param[in] Issue
 * TRUE to issue HCRST. FALSE only waits out a reset someone else started.
 *
 * @return
 * STATUS_SUCCESS when the controller is ready.
 */
static
NTSTATUS
NTAPI
XhciResetController(
    _In_ BOOLEAN Issue)
{
    PUCHAR Command = XhciDebug.Operational + XHCI_USBCMD;
    NTSTATUS Status;

    Status = XhciWaitForReady();
    if (!NT_SUCCESS(Status))
        return Status;

    if (Issue)
    {
        if (!(READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_USBSTS)) & XHCI_USBSTS_HCH))
        {
            KdUsbTrace("controller is running, not resetting it\n");
            return STATUS_DEVICE_BUSY;
        }

        WRITE_REGISTER_ULONG((PULONG)Command, XHCI_USBCMD_HCRST);
        KeMemoryBarrier();
        XhciDebug.ResetPending = FALSE;
    }

    if (!XhciPoll(Command, XHCI_USBCMD_HCRST, 0, XHCI_RESET_TIMEOUT, 50))
    {
        KdUsbTrace("HCRST never cleared\n");
        return STATUS_IO_TIMEOUT;
    }

    return XhciWaitForReady();
}

/**
 * @brief
 * Walks the extended capability list for one capability ID.
 *
 * @param[in] Offset
 * Byte offset of the first extended capability, or zero when there is none.
 *
 * @param[in] Id
 * The capability ID to find.
 *
 * @return
 * The capability's registers, or NULL when the list does not have it.
 */
static
PUCHAR
NTAPI
XhciFindExtendedCapability(
    _In_ ULONG Offset,
    _In_ UCHAR Id)
{
    ULONG Header;
    ULONG Next;

    while (Offset != 0 && Offset + sizeof(ULONG) <= XhciDebug.CapabilitiesLength)
    {
        Header = READ_REGISTER_ULONG((PULONG)(XhciDebug.Capabilities + Offset));
        if ((Header & 0xFF) == Id)
            return XhciDebug.Capabilities + Offset;

        Next = (Header >> 8) & 0xFF;
        if (Next == 0)
            break;

        Offset += Next * sizeof(ULONG);
    }

    return NULL;
}

/**
 * @brief
 * Takes the controller from the firmware and stops it.
 *
 * Under CSM the BIOS keeps the controller running for legacy keyboard
 * emulation. The ownership semaphore asks it to let go, its SMIs are turned
 * off, and the schedule is stopped so the controller can be reset.
 *
 * @param[in] Offset
 * Byte offset of the first extended capability.
 */
static
VOID
NTAPI
XhciClaimFromFirmware(
    _In_ ULONG Offset)
{
    PUCHAR Legacy;
    ULONG Value;

    Legacy = XhciFindExtendedCapability(Offset, XHCI_EXT_CAP_LEGACY);
    if (Legacy != NULL)
    {
        Value = READ_REGISTER_ULONG((PULONG)(Legacy + XHCI_USBLEGSUP));
        KdUsbTrace("USBLEGSUP 0x%08lx USBLEGCTLSTS 0x%08lx\n",
                   Value, READ_REGISTER_ULONG((PULONG)(Legacy + XHCI_USBLEGCTLSTS)));

        if (Value & XHCI_USBLEGSUP_BIOS_OWNED)
        {
            WRITE_REGISTER_ULONG((PULONG)(Legacy + XHCI_USBLEGSUP), Value | XHCI_USBLEGSUP_OS_OWNED);

            /* A firmware that never answers loses the controller anyway */
            if (!XhciPoll(Legacy + XHCI_USBLEGSUP, XHCI_USBLEGSUP_BIOS_OWNED, 0, XHCI_HANDOFF_TIMEOUT, 100))
            {
                KdUsbTrace("BIOS kept ownership, forcing it\n");
                Value = READ_REGISTER_ULONG((PULONG)(Legacy + XHCI_USBLEGSUP));
                WRITE_REGISTER_ULONG((PULONG)(Legacy + XHCI_USBLEGSUP),
                                     (Value & ~XHCI_USBLEGSUP_BIOS_OWNED) | XHCI_USBLEGSUP_OS_OWNED);
            }
        }

        Value = READ_REGISTER_ULONG((PULONG)(Legacy + XHCI_USBLEGCTLSTS));
        WRITE_REGISTER_ULONG((PULONG)(Legacy + XHCI_USBLEGCTLSTS),
                             (Value & ~XHCI_USBLEGCTLSTS_SMI_ENABLES) | XHCI_USBLEGCTLSTS_SMI_EVENTS);
        KeMemoryBarrier();
    }

    if (READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_USBSTS)) & XHCI_USBSTS_HCH)
        return;

    Value = READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_USBCMD));
    WRITE_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_USBCMD), Value & ~XHCI_USBCMD_RS);
    KeMemoryBarrier();

    XhciPoll(XhciDebug.Operational + XHCI_USBSTS, XHCI_USBSTS_HCH, XHCI_USBSTS_HCH, XHCI_HALT_TIMEOUT, 100);
    KdUsbTrace("stopped controller, USBSTS 0x%08lx\n",
               READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_USBSTS)));
}

/**
 * @brief
 * Finds the Debug Capability and brings the controller to a known state.
 *
 * @return
 * STATUS_SUCCESS when the controller is ready and has a Debug Capability.
 */
static
NTSTATUS
NTAPI
XhciLocateDebugCapability(VOID)
{
    PUCHAR Capabilities = XhciDebug.Capabilities;
    ULONG CapParams;
    ULONG Offset;

    CapParams = READ_REGISTER_ULONG((PULONG)(Capabilities + XHCI_HCCPARAMS1));
    KdUsbTrace("HCCPARAMS1 0x%08lx\n", CapParams);

    if (CapParams == 0xFFFFFFFF)
        return STATUS_NO_SUCH_DEVICE;

    /* Rings and contexts are handed over as 64-bit addresses */
    if (!(CapParams & XHCI_HCCPARAMS1_AC64) &&
        XhciDebug.SharedAddress + sizeof(XHCI_SHARED) > 0x100000000ULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    XhciDebug.Operational = Capabilities + READ_REGISTER_UCHAR(Capabilities + XHCI_CAPLENGTH);
    if (!(READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_PAGESIZE)) & XHCI_PAGESIZE_4K))
    {
        KdUsbTrace("PAGESIZE 0x%lx has no 4K\n",
                   READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_PAGESIZE)));
        return STATUS_NOT_SUPPORTED;
    }

    Offset = XHCI_HCCPARAMS1_XECP(CapParams);
    XhciDebug.Dbc = XhciFindExtendedCapability(Offset, XHCI_EXT_CAP_DEBUG);
    if (XhciDebug.Dbc == NULL)
    {
        KdUsbTrace("no Debug Capability on this controller\n");
        return STATUS_NOT_FOUND;
    }

    KdUsbTrace("DbC at +0x%lx, DCCTRL 0x%08lx\n",
               (ULONG)(XhciDebug.Dbc - Capabilities), XhciReadDbc(DBC_DCCTRL));

    /* Only a requested reset takes the controller from the firmware */
    if (XhciDebug.ResetPending)
        XhciClaimFromFirmware(Offset);

    return XhciResetController(XhciDebug.ResetPending);
}

/**
 * @brief
 * Fills a USB string descriptor.
 *
 * @param[out] Descriptor
 * The descriptor to fill.
 *
 * @param[in] Text
 * The characters, without a terminator.
 *
 * @param[in] Count
 * How many characters Text holds.
 *
 * @return
 * The descriptor length.
 */
static
UCHAR
NTAPI
XhciSetString(
    _Out_ PDBC_STRING Descriptor,
    _In_reads_(Count) PCWSTR Text,
    _In_ ULONG Count)
{
    Count = min(Count, RTL_NUMBER_OF(Descriptor->Characters));

    Descriptor->Length = (UCHAR)(FIELD_OFFSET(DBC_STRING, Characters) + Count * sizeof(WCHAR));
    Descriptor->DescriptorType = USB_STRING_DESCRIPTOR_TYPE;
    RtlCopyMemory(Descriptor->Characters, Text, Count * sizeof(WCHAR));

    return Descriptor->Length;
}

/**
 * @brief
 * Builds the rings, the strings and the DbC context, then enables the Debug
 * Capability.
 */
static
VOID
NTAPI
XhciEnableDebugCapability(VOID)
{
    static const WCHAR LanguageId[] = { USB_LANGUAGE_EN_US };
    static const WCHAR Manufacturer[] = L"ReactOS";
    static const WCHAR Product[] = L"ReactOS USB3 Debug";
    static const WCHAR Serial[] = L"1";
    PXHCI_SHARED Shared = XhciDebug.Shared;
    PDBC_CONTEXT Context = &Shared->Context;
    PDBC_INFO_CONTEXT Info = &Context->Info;
    ULONG EndpointInfo;
    ULONG Slot;

    /* Rings, context and strings; the data pages need no clearing */
    RtlZeroMemory(Shared, FIELD_OFFSET(XHCI_SHARED, OutBuffer));

    XhciDebug.EventDequeue = 0;
    XhciDebug.EventCycle = XHCI_TRB_CYCLE;
    Shared->EventSegment.Base = XhciBusAddress(Shared->EventRing);
    Shared->EventSegment.Size = XHCI_EVENT_TRBS;

    /* OUT transfers are queued as packets go out; IN transfers wait for the host */
    XhciRingInitialize(&XhciDebug.Out, Shared->OutRing, Shared->OutBuffer[0]);
    XhciRingInitialize(&XhciDebug.In, Shared->InRing, Shared->InBuffer[0]);

    for (Slot = 0; Slot < XHCI_RING_TRANSFERS; Slot++)
        XhciRingQueue(&XhciDebug.In, PAGE_SIZE, XHCI_TRB_ISP);

    Info->String0 = XhciBusAddress(&Shared->LanguageString);
    Info->String0Length = XhciSetString(&Shared->LanguageString, LanguageId, RTL_NUMBER_OF(LanguageId));
    Info->Manufacturer = XhciBusAddress(&Shared->ManufacturerString);
    Info->ManufacturerLength = XhciSetString(&Shared->ManufacturerString, Manufacturer, RTL_NUMBER_OF(Manufacturer) - 1);
    Info->Product = XhciBusAddress(&Shared->ProductString);
    Info->ProductLength = XhciSetString(&Shared->ProductString, Product, RTL_NUMBER_OF(Product) - 1);
    Info->Serial = XhciBusAddress(&Shared->SerialString);
    Info->SerialLength = XhciSetString(&Shared->SerialString, Serial, RTL_NUMBER_OF(Serial) - 1);

    EndpointInfo = (DBC_DCCTRL_MAX_BURST(XhciReadDbc(DBC_DCCTRL)) << 8) | (ENDPOINT_MAX_PACKET << 16);

    Context->Out.Info = EndpointInfo | (ENDPOINT_TYPE_BULK_OUT << 3);
    Context->Out.Dequeue = XhciBusAddress(Shared->OutRing) | XHCI_TRB_CYCLE;
    Context->Out.AverageLength = KDUSB_TRANSFER_SIZE;

    Context->In.Info = EndpointInfo | (ENDPOINT_TYPE_BULK_IN << 3);
    Context->In.Dequeue = XhciBusAddress(Shared->InRing) | XHCI_TRB_CYCLE;
    Context->In.AverageLength = PAGE_SIZE;

    KeMemoryBarrier();

    XhciWriteDbc(DBC_DCERSTSZ, 1);
    XhciWriteDbc64(DBC_DCERSTBA, XhciBusAddress(&Shared->EventSegment));
    XhciWriteDbc64(DBC_DCERDP, XhciBusAddress(Shared->EventRing));
    XhciWriteDbc64(DBC_DCCP, XhciBusAddress(Context));
    XhciWriteDbc(DBC_DCDDI1, DBC_DCDDI1_VALUE);
    XhciWriteDbc(DBC_DCDDI2, DBC_DCDDI2_VALUE);
    XhciWriteDbc(DBC_DCCTRL, DBC_DCCTRL_ENABLE | DBC_DCCTRL_DRC);
}

/**
 * @brief
 * Waits for the host to configure the debug device, then lets the IN
 * endpoint run.
 *
 * @param[in] Timeout
 * Microseconds to wait. Zero checks once.
 *
 * @return
 * STATUS_SUCCESS once the device is configured, otherwise
 * STATUS_PORT_DISCONNECTED.
 */
static
NTSTATUS
NTAPI
XhciWaitForHost(
    _In_ ULONG Timeout)
{
    ULONG Control;
    ULONG Waited;

    for (Waited = 0; ; Waited += 10)
    {
        Control = XhciReadDbc(DBC_DCCTRL);
        if ((Control & DBC_DCCTRL_DCR) && (XhciReadDbc(DBC_DCPORTSC) & DBC_DCPORTSC_CCS))
            break;

        if (Waited >= Timeout)
        {
            if (Timeout != 0)
            {
                KdUsbTrace("no host: DCPORTSC 0x%08lx DCCTRL 0x%08lx\n",
                           XhciReadDbc(DBC_DCPORTSC), Control);
            }

            return STATUS_PORT_DISCONNECTED;
        }

        KdUsbStall(10);
    }

    if (Control & DBC_DCCTRL_DRC)
        XhciWriteDbc(DBC_DCCTRL, DBC_DCCTRL_ENABLE | DBC_DCCTRL_DRC);

    XhciWriteDbc(DBC_DCDB, DBC_DOORBELL_IN);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Makes sure the link is usable before a transfer.
 *
 * When something outside the debugger reset the controller, for example the
 * xHCI driver taking it over, the Debug Capability is brought back up.
 *
 * @return
 * STATUS_SUCCESS when the host is connected.
 */
static
NTSTATUS
NTAPI
XhciCheckLink(VOID)
{
    NTSTATUS Status;

    if (XhciReadDbc(DBC_DCCTRL) & DBC_DCCTRL_DCE)
        return XhciWaitForHost(0);

    Status = XhciResetController(FALSE);
    if (!NT_SUCCESS(Status))
        return Status;

    XhciEnableDebugCapability();
    return XhciWaitForHost(XHCI_HOST_TIMEOUT);
}

/* BACKEND ROUTINES ***********************************************************/

/**
 * @brief
 * Sends a set of buffers to the host as one transfer.
 *
 * @param[in] Buffers
 * The pieces of the packet, in order. Anything past KDUSB_TRANSFER_SIZE
 * bytes is dropped.
 *
 * @param[in] Count
 * How many pieces there are.
 */
static
VOID
NTAPI
XhciSendBuffers(
    _In_reads_(Count) PKDUSB_BUFFER Buffers,
    _In_ ULONG Count)
{
    PXHCI_RING Ring = &XhciDebug.Out;
    PXHCI_TRANSFER Transfer;
    PUCHAR Data;
    ULONG Length = 0;
    ULONG Piece;
    ULONG Waited;
    ULONG Slot;

    if (!NT_SUCCESS(XhciCheckLink()))
    {
        KdUsbSetHostPresent(FALSE);
        return;
    }

    /* A slot still owed a completion from an earlier timeout cannot be reused */
    Slot = Ring->Enqueue;
    Transfer = &Ring->Transfer[Slot];
    if (Transfer->Pending)
        return;

    Data = XhciRingBuffer(Ring, Slot);
    while (Count-- != 0)
    {
        Piece = min(Buffers->Length, KDUSB_TRANSFER_SIZE - Length);
        RtlCopyMemory(Data + Length, Buffers->Data, Piece);
        Length += Piece;
        Buffers++;
    }

    /* The host only sees the end of a transfer that finishes on a short packet */
    if (Length % ENDPOINT_MAX_PACKET == 0)
        Data[Length++] = 0;

    XhciRingQueue(Ring, Length, 0);
    XhciWriteDbc(DBC_DCDB, DBC_DOORBELL_OUT);

    for (Waited = 0; Transfer->Pending; Waited += 10)
    {
        if (!XhciProcessEvents())
            return;

        if (!Transfer->Pending)
            break;

        if (Waited >= XHCI_SEND_TIMEOUT)
        {
            KdUsbSetHostPresent(FALSE);
            return;
        }

        KdUsbStall(10);
    }

    if (Transfer->Completion == XHCI_COMPLETION_SUCCESS)
        KdUsbSetHostPresent(TRUE);
}

/**
 * @brief
 * Answers the host's "NAME?" query with the TARGETNAME given at boot.
 */
static
VOID
NTAPI
XhciSendTargetName(VOID)
{
    static const CHAR Prefix[] = "NAME=";
    CHAR Reply[sizeof(Prefix) - 1 + KDUSB_TARGET_NAME_MAX + 2];
    KDUSB_BUFFER Buffer;
    ULONG Length = sizeof(Prefix) - 1;

    RtlCopyMemory(Reply, Prefix, Length);
    RtlCopyMemory(Reply + Length, KdUsbTargetName, KdUsbTargetNameLength);
    Length += KdUsbTargetNameLength;

    /* The host expects a two byte terminator */
    Reply[Length++] = ANSI_NULL;
    Reply[Length++] = ANSI_NULL;

    Buffer.Data = Reply;
    Buffer.Length = Length;
    XhciSendBuffers(&Buffer, 1);
}

/**
 * @brief
 * Waits up to two seconds for a transfer from the host.
 *
 * @param[in] PacketType
 * The packet type wanted. A break-in poll returns at once.
 *
 * @param[out] Packet
 * Receives the KD packet header.
 *
 * @param[in,out] MessageHeader
 * Receives the message header.
 *
 * @param[in,out] MessageData
 * Receives the message data.
 *
 * @return
 * KdPacketReceived, KdPacketTimedOut or KdPacketNeedsResend.
 */
static
KDSTATUS
NTAPI
XhciReadPacket(
    _In_ ULONG PacketType,
    _Out_ PKD_PACKET Packet,
    _Inout_opt_ PSTRING MessageHeader,
    _Inout_opt_ PSTRING MessageData)
{
    PXHCI_RING Ring = &XhciDebug.In;
    PXHCI_TRANSFER Transfer;
    ULONG Length;
    ULONG Waited;
    ULONG Slot;

    for (;;)
    {
        if (!NT_SUCCESS(XhciCheckLink()))
        {
            KdUsbSetHostPresent(FALSE);
            return KdPacketTimedOut;
        }

        /*
         * Every IN slot stays queued except while it is being read, so the
         * next one to complete is also the next one to be queued again.
         */
        Slot = Ring->Enqueue;
        Transfer = &Ring->Transfer[Slot];

        for (Waited = 0; ; Waited += 5)
        {
            if (!XhciProcessEvents())
                return KdPacketNeedsResend;

            if (!Transfer->Pending)
                break;

            if (PacketType == PACKET_TYPE_KD_POLL_BREAKIN || Waited >= XHCI_RECEIVE_TIMEOUT)
                return KdPacketTimedOut;

            KdUsbStall(5);
        }

        Length = 0;
        if (Transfer->Completion == XHCI_COMPLETION_SUCCESS && Transfer->Moved <= KDUSB_TRANSFER_SIZE)
            Length = Transfer->Moved;

        RtlCopyMemory(KdUsbReceiveBuffer, XhciRingBuffer(Ring, Slot), Length);
        RtlZeroMemory(KdUsbReceiveBuffer + Length, sizeof(KdUsbReceiveBuffer) - Length);
        KdUsbSetHostPresent(TRUE);

        XhciRingQueue(Ring, PAGE_SIZE, XHCI_TRB_ISP);
        XhciWriteDbc(DBC_DCDB, DBC_DOORBELL_IN);

        if (!KdUsbIsNameQuery(Length))
            break;

        XhciSendTargetName();
    }

    return KdUsbCompletePacket(PacketType, Length, Packet, MessageHeader, MessageData);
}

/**
 * @brief
 * Turns the Debug Capability off.
 */
static
VOID
NTAPI
XhciShutdown(VOID)
{
    XhciWriteDbc(DBC_DCCTRL, XhciReadDbc(DBC_DCCTRL) & ~DBC_DCCTRL_DCE);
}

/**
 * @brief
 * Points XhciDebug at the first mapped memory BAR.
 *
 * @return
 * TRUE when the HAL mapped one.
 */
static
BOOLEAN
NTAPI
XhciMapRegisters(VOID)
{
    PDEBUG_DEVICE_ADDRESS Bar;
    ULONG Index;

    for (Index = 0; Index < MAXIMUM_DEBUG_BARS; Index++)
    {
        Bar = &KdUsbDevice.BaseAddress[Index];
        if (!Bar->Valid || Bar->Type != CmResourceTypeMemory || Bar->TranslatedAddress == NULL)
            continue;

        XhciDebug.Capabilities = Bar->TranslatedAddress;
        XhciDebug.CapabilitiesLength = Bar->Length;
        KdUsbTrace("BAR%lu at %p, 0x%lx bytes\n", Index, Bar->TranslatedAddress, Bar->Length);
        return TRUE;
    }

    KdUsbTrace("no mapped memory BAR\n");
    return FALSE;
}

/**
 * @brief
 * Makes sure the controller decodes its registers and can master the bus.
 * Only usable at boot; the HAL's PCI config routines go away afterwards.
 *
 * @return
 * FALSE when the command register needed fixing and could not be written.
 */
static
BOOLEAN
NTAPI
XhciEnableDecoding(VOID)
{
    const USHORT Required = PCI_ENABLE_MEMORY_SPACE | PCI_ENABLE_BUS_MASTER;
    USHORT Command;

    if (KdUsbDevice.NameSpace != KdNameSpacePCI || KdGetPciDataByOffset == NULL)
        return TRUE;

    if (KdGetPciDataByOffset(KdUsbDevice.Bus,
                             KdUsbDevice.Slot,
                             &Command,
                             FIELD_OFFSET(PCI_COMMON_HEADER, Command),
                             sizeof(Command)) != sizeof(Command))
    {
        return TRUE;
    }

    KdUsbTrace("PCI command 0x%04x\n", Command);
    if ((Command & Required) == Required)
        return TRUE;

    Command |= Required;
    return KdSetPciDataByOffset(KdUsbDevice.Bus,
                                KdUsbDevice.Slot,
                                &Command,
                                FIELD_OFFSET(PCI_COMMON_HEADER, Command),
                                sizeof(Command)) == sizeof(Command);
}

/**
 * @brief
 * Brings the Debug Capability up on the controller KdUsbDevice describes.
 *
 * @param[in] Boot
 * TRUE on the first initialization.
 *
 * @return
 * STATUS_SUCCESS once the host is connected, STATUS_PORT_DISCONNECTED when
 * the capability is up but no host answered yet.
 */
static
NTSTATUS
NTAPI
XhciStart(
    _In_ BOOLEAN Boot)
{
    NTSTATUS Status;

    if (!XhciMapRegisters())
        return STATUS_UNSUCCESSFUL;

    if (Boot && !XhciEnableDecoding())
        return STATUS_UNSUCCESSFUL;

    Status = XhciLocateDebugCapability();
    if (!NT_SUCCESS(Status))
    {
        KdUsbTrace("locate/reset failed 0x%lx\n", Status);
        return Status;
    }

    XhciEnableDebugCapability();
    KdUsbTrace("DbC enabled, waiting for host\n");

    return XhciWaitForHost(XHCI_HOST_TIMEOUT);
}

/**
 * @brief
 * Claims the xHCI controller and its DMA memory, and starts the debug link.
 *
 * At boot the HAL finds the controller and allocates the memory. Later calls
 * reuse both.
 *
 * @param[in] LoaderBlock
 * The loader block at boot, or NULL when re-initializing.
 *
 * @return
 * TRUE when the Debug Capability is running. A missing host still counts:
 * the link comes up when the cable is plugged in.
 */
static
BOOLEAN
NTAPI
XhciInitialize(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    NTSTATUS Status;

    if (LoaderBlock != NULL)
    {
        KdUsbDevice.Memory.MaxEnd.QuadPart = (LONGLONG)-1;
        KdUsbDevice.Memory.Length = sizeof(XHCI_SHARED);

        Status = KdSetupPciDeviceForDebugging(LoaderBlock, &KdUsbDevice);
        KdUsbTrace("xHCI setup 0x%lx: %lu:0x%lx %04x:%04x class %02x/%02x/%02x\n",
                   Status, KdUsbDevice.Bus, KdUsbDevice.Slot,
                   KdUsbDevice.VendorID, KdUsbDevice.DeviceID,
                   KdUsbDevice.BaseClass, KdUsbDevice.SubClass, KdUsbDevice.ProgIf);
        if (!NT_SUCCESS(Status))
            return FALSE;

        KdUsbTrace("DMA PA 0x%I64x VA %p len 0x%lx\n",
                   KdUsbDevice.Memory.Start.QuadPart,
                   KdUsbDevice.Memory.VirtualAddress,
                   KdUsbDevice.Memory.Length);

        if (KdUsbDevice.BaseClass != PCI_CLASS_SERIAL_BUS_CTLR ||
            KdUsbDevice.SubClass != PCI_SUBCLASS_SB_USB ||
            KdUsbDevice.ProgIf != KDUSB_PROGIF_XHCI ||
            KdUsbDevice.Memory.VirtualAddress == NULL ||
            (KdUsbDevice.Memory.Start.QuadPart & (PAGE_SIZE - 1)) != 0)
        {
            KdReleasePciDeviceforDebugging(&KdUsbDevice);
            return FALSE;
        }
    }
    else if (KdUsbDevice.Memory.VirtualAddress == NULL)
    {
        return FALSE;
    }

    XhciDebug.Shared = KdUsbDevice.Memory.VirtualAddress;
    XhciDebug.SharedAddress = KdUsbDevice.Memory.Start.QuadPart;
    RtlZeroMemory(XhciDebug.Shared, sizeof(XHCI_SHARED));

    Status = XhciStart(LoaderBlock != NULL);
    KdUsbTrace("xHCI start 0x%lx\n", Status);
    if (NT_SUCCESS(Status) || Status == STATUS_PORT_DISCONNECTED)
        return TRUE;

    if (LoaderBlock != NULL)
        KdReleasePciDeviceforDebugging(&KdUsbDevice);

    return FALSE;
}

/**
 * @brief
 * Asks for a host controller reset on the next initialization.
 */
VOID
NTAPI
KdUsbXhciRequestReset(VOID)
{
    XhciDebug.ResetPending = TRUE;
}

const KDUSB_CONTROLLER KdUsbXhciController =
{
    XhciInitialize,
    XhciReadPacket,
    XhciSendBuffers,
    XhciShutdown
};
