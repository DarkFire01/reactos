/*
 * PROJECT:     ReactOS USB Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     xHCI Debug Capability backend
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdusb.h"

/* xHCI capability registers */
#define XHCI_CAPLENGTH              0x00
#define XHCI_HCSPARAMS1             0x04
#define XHCI_HCSPARAMS3             0x0C
#define XHCI_HCCPARAMS1             0x10
#define XHCI_DBOFF                  0x14
#define XHCI_RTSOFF                 0x18

#define XHCI_HCCPARAMS1_AC64        0x00000001
#define XHCI_HCCPARAMS1_PSA_MASK    0x0000F000

/* xHCI operational registers */
#define XHCI_USBCMD                 0x00
#define XHCI_USBSTS                 0x04
#define XHCI_PAGESIZE               0x08

#define XHCI_USBCMD_RS              0x00000001
#define XHCI_USBCMD_HCRST           0x00000002
#define XHCI_USBSTS_HCH             0x00000001
#define XHCI_USBSTS_CNR             0x00000800

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
#define DBC_DCID                    0x00
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
#define DBC_DCPORTSC_CCS            0x00000001

#define DBC_DOORBELL_OUT            0x000
#define DBC_DOORBELL_IN             0x100

/* Identity the host side driver binds to */
#define DBC_DCDDI1_VALUE            0x349500FF
#define DBC_DCDDI2_VALUE            0x000000E0

/* TRB fields */
#define DBC_TRB_CYCLE                    0x00000001
#define DBC_TRB_TOGGLE_CYCLE             0x00000002
#define DBC_TRB_ISP                      0x00000004
#define DBC_TRB_IOC                      0x00000020
#define DBC_TRB_TYPE_MASK                0x0000FC00
#define DBC_TRB_TYPE(Type)               ((ULONG)(Type) << 10)
#define DBC_TRB_TYPE_NORMAL              1
#define DBC_TRB_TYPE_LINK                6
#define DBC_TRB_TYPE_TRANSFER_EVENT      32

#define DBC_TRB_LENGTH_MASK              0x0001FFFF
#define DBC_TRB_RESULT_MASK              0x00FFFFFF
#define DBC_TRB_COMPLETION(Status)       ((Status) >> 24)
#define DBC_TRB_COMPLETION_SUCCESS       1
#define DBC_TRB_COMPLETION_SHORT_PACKET  13

#define ENDPOINT_TYPE_BULK_OUT      2
#define ENDPOINT_TYPE_BULK_IN       6
#define ENDPOINT_MAX_PACKET         1024

#define XHCI_EVENT_RING_SIZE        1024
#define XHCI_OUT_TRANSFERS          30
#define XHCI_IN_TRANSFERS           31
#define XHCI_OUT_PAGES              33
#define XHCI_IN_PAGES               34

#define DBC_STRING_DESCRIPTOR_TYPE  3

typedef struct _XHCI_TRB
{
    ULONG64 Parameter;
    ULONG Status;
    ULONG Control;
} XHCI_TRB, *PXHCI_TRB;

typedef struct _XHCI_SEGMENT_ENTRY
{
    ULONG64 Base;
    ULONG Size;
    ULONG Reserved;
} XHCI_SEGMENT_ENTRY, *PXHCI_SEGMENT_ENTRY;

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

/** Everything the controller reads or writes, laid over the HAL's memory. */
typedef struct _XHCI_DMA
{
    XHCI_TRB EventRing[XHCI_EVENT_RING_SIZE];
    XHCI_SEGMENT_ENTRY Segment;
    XHCI_TRB OutRing[XHCI_OUT_TRANSFERS + 1];
    XHCI_TRB InRing[XHCI_IN_TRANSFERS + 1];
    DBC_CONTEXT Context;
    DBC_STRING String0;
    DBC_STRING Manufacturer;
    DBC_STRING Product;
    DBC_STRING Serial;
    DECLSPEC_ALIGN(PAGE_SIZE) UCHAR OutPages[XHCI_OUT_PAGES][PAGE_SIZE];
    UCHAR InPages[XHCI_IN_PAGES][PAGE_SIZE];
} XHCI_DMA, *PXHCI_DMA;

C_ASSERT(sizeof(XHCI_TRB) == 16);
C_ASSERT(sizeof(DBC_INFO_CONTEXT) == 64);
C_ASSERT(sizeof(DBC_ENDPOINT_CONTEXT) == 64);
C_ASSERT(sizeof(DBC_STRING) == 64);
C_ASSERT(FIELD_OFFSET(XHCI_DMA, Segment) % 64 == 0);
C_ASSERT(FIELD_OFFSET(XHCI_DMA, OutRing) % 16 == 0);
C_ASSERT(FIELD_OFFSET(XHCI_DMA, Context) % 64 == 0);
C_ASSERT(FIELD_OFFSET(XHCI_DMA, OutPages) % PAGE_SIZE == 0);

/** Driver side state of the debug capability. */
typedef struct _XHCI_DEBUG
{
    PXHCI_DMA Dma;
    ULONG64 DmaAddress;
    ULONG DmaLength;
    PUCHAR Capabilities;
    ULONG CapabilitiesLength;
    PUCHAR Operational;
    PUCHAR Dbc;
    ULONG EventIndex;
    ULONG EventCycle;
    ULONG OutIndex;
    ULONG OutCycle;
    ULONG InIndex;
    ULONG InCycle;
    UCHAR OutPage[XHCI_OUT_TRANSFERS];
    UCHAR InPage[XHCI_IN_TRANSFERS];
    BOOLEAN ResetController;
} XHCI_DEBUG, *PXHCI_DEBUG;

static XHCI_DEBUG XhciDebug = { .ResetController = TRUE };

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
 * Converts an address inside the DMA block to the bus address the controller
 * uses. The HAL hands out one physically contiguous block.
 *
 * @param[in] Address
 * An address inside XhciDebug.Dma.
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
    return XhciDebug.DmaAddress + ((PUCHAR)Address - (PUCHAR)XhciDebug.Dma);
}

/**
 * @brief
 * Sets the cycle bit of a TRB without touching its other control bits.
 *
 * @param[in,out] Trb
 * The TRB to update.
 *
 * @param[in] Cycle
 * The new cycle bit.
 */
static
VOID
NTAPI
XhciSetCycle(
    _Inout_ PXHCI_TRB Trb,
    _In_ ULONG Cycle)
{
    Trb->Control = (Trb->Control & ~DBC_TRB_CYCLE) | (Cycle & DBC_TRB_CYCLE);
}

/* CONTROLLER *****************************************************************/

/**
 * @brief
 * Waits for the controller to clear Controller Not Ready.
 *
 * @return
 * STATUS_SUCCESS, STATUS_PORT_DISCONNECTED when the controller is gone, or
 * STATUS_UNSUCCESSFUL after a second.
 */
static
NTSTATUS
NTAPI
XhciWaitForReady(VOID)
{
    ULONG Status;
    ULONG Try;

    for (Try = 0; ; Try++)
    {
        Status = READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_USBSTS));
        if (!(Status & XHCI_USBSTS_CNR))
            return STATUS_SUCCESS;

        if (Status == 0xFFFFFFFF)
            return STATUS_PORT_DISCONNECTED;

        if (Try + 1 >= 10000)
            return STATUS_UNSUCCESSFUL;

        KdUsbStall(100);
    }
}

/**
 * @brief
 * Resets the host controller when asked to, and waits for it to settle.
 *
 * A controller that is still running belongs to someone else, so it is left
 * alone and the reset fails.
 *
 * @param[in,out] Reset
 * TRUE to reset the controller. Cleared once the reset is issued.
 *
 * @return
 * STATUS_SUCCESS when the controller is ready.
 */
static
NTSTATUS
NTAPI
XhciResetController(
    _Inout_ PBOOLEAN Reset)
{
    NTSTATUS Status;
    ULONG Try;

    Status = XhciWaitForReady();
    if (!NT_SUCCESS(Status))
        return Status;

    if (*Reset)
    {
        if (!(READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_USBSTS)) & XHCI_USBSTS_HCH))
        {
            KdUsbTrace("controller is running, not resetting it\n");
            return STATUS_UNSUCCESSFUL;
        }

        WRITE_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_USBCMD), XHCI_USBCMD_HCRST);
        KeMemoryBarrier();
        *Reset = FALSE;
    }

    for (Try = 0;
         READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_USBCMD)) & XHCI_USBCMD_HCRST;
         Try++)
    {
        if (Try + 1 >= 20000)
        {
            KdUsbTrace("HCRST never cleared\n");
            return STATUS_UNSUCCESSFUL;
        }

        KdUsbStall(50);
    }

    return XhciWaitForReady();
}

/**
 * @brief
 * Walks the extended capability list for one capability ID.
 *
 * @param[in] Offset
 * Byte offset of the first extended capability.
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
    PUCHAR Capabilities = XhciDebug.Capabilities;
    PUCHAR Extended = Capabilities + Offset;
    ULONG Header;

    for (;;)
    {
        Header = READ_REGISTER_ULONG((PULONG)Extended);
        if ((Header & 0xFF) == Id)
            return Extended;

        Offset = ((Header >> 8) & 0xFF) * 4;
        Extended += Offset;
        if (Offset == 0 || Extended >= Capabilities + XhciDebug.CapabilitiesLength)
            return NULL;
    }
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
    ULONG Try;

    Legacy = XhciFindExtendedCapability(Offset, XHCI_EXT_CAP_LEGACY);
    if (Legacy != NULL)
    {
        Value = READ_REGISTER_ULONG((PULONG)(Legacy + XHCI_USBLEGSUP));
        KdUsbTrace("USBLEGSUP 0x%08lx USBLEGCTLSTS 0x%08lx\n",
                   Value, READ_REGISTER_ULONG((PULONG)(Legacy + XHCI_USBLEGCTLSTS)));

        if (Value & XHCI_USBLEGSUP_BIOS_OWNED)
        {
            WRITE_REGISTER_ULONG((PULONG)(Legacy + XHCI_USBLEGSUP), Value | XHCI_USBLEGSUP_OS_OWNED);

            for (Try = 0; Try < 10000; Try++)
            {
                Value = READ_REGISTER_ULONG((PULONG)(Legacy + XHCI_USBLEGSUP));
                if (!(Value & XHCI_USBLEGSUP_BIOS_OWNED))
                    break;

                KdUsbStall(100);
            }

            /* A firmware that never answers loses the controller anyway */
            if (Value & XHCI_USBLEGSUP_BIOS_OWNED)
            {
                KdUsbTrace("BIOS kept ownership, forcing it\n");
                WRITE_REGISTER_ULONG((PULONG)(Legacy + XHCI_USBLEGSUP),
                                     (Value & ~XHCI_USBLEGSUP_BIOS_OWNED) | XHCI_USBLEGSUP_OS_OWNED);
            }
        }

        Value = READ_REGISTER_ULONG((PULONG)(Legacy + XHCI_USBLEGCTLSTS));
        WRITE_REGISTER_ULONG((PULONG)(Legacy + XHCI_USBLEGCTLSTS),
                             (Value & ~XHCI_USBLEGCTLSTS_SMI_ENABLES) | XHCI_USBLEGCTLSTS_SMI_EVENTS);
        KeMemoryBarrier();
    }

    Value = READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_USBSTS));
    if (Value & XHCI_USBSTS_HCH)
        return;

    Value = READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_USBCMD));
    WRITE_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_USBCMD), Value & ~XHCI_USBCMD_RS);
    KeMemoryBarrier();

    for (Try = 0; Try < 200; Try++)
    {
        if (READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_USBSTS)) & XHCI_USBSTS_HCH)
            break;

        KdUsbStall(100);
    }

    KdUsbTrace("stopped controller, USBSTS 0x%08lx\n",
               READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_USBSTS)));
}

/**
 * @brief
 * Checks the capability registers and finds the Debug Capability.
 *
 * @return
 * STATUS_SUCCESS when the controller is usable and has a Debug Capability.
 */
static
NTSTATUS
NTAPI
XhciLocateDebugCapability(VOID)
{
    PUCHAR Capabilities = XhciDebug.Capabilities;
    PUCHAR Extended;
    ULONG Params1;
    ULONG Params3;
    ULONG CapParams;
    ULONG Offset;

    Params1 = READ_REGISTER_ULONG((PULONG)(Capabilities + XHCI_HCSPARAMS1));
    Params3 = READ_REGISTER_ULONG((PULONG)(Capabilities + XHCI_HCSPARAMS3));
    CapParams = READ_REGISTER_ULONG((PULONG)(Capabilities + XHCI_HCCPARAMS1));
    KdUsbTrace("HCSPARAMS1 0x%08lx HCSPARAMS3 0x%08lx HCCPARAMS1 0x%08lx\n",
               Params1, Params3, CapParams);

    if ((Params1 & 0xFF) == 0 ||
        ((Params1 >> 8) & 0x3FF) == 0 ||
        (Params1 >> 24) == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((Params3 & 0xFF) > 10 || (Params3 >> 16) > 0x7FF)
        return STATUS_INVALID_PARAMETER;

    /* The context and ring addresses are 64-bit, so AC64 is required */
    if (!(CapParams & XHCI_HCCPARAMS1_AC64) || !(CapParams & XHCI_HCCPARAMS1_PSA_MASK))
        return STATUS_INVALID_PARAMETER;

    Offset = (CapParams >> 16) * 4;
    if (Offset == 0 || Offset >= XhciDebug.CapabilitiesLength)
        return STATUS_INVALID_PARAMETER;

    if (READ_REGISTER_ULONG((PULONG)(Capabilities + XHCI_DBOFF)) == 0 ||
        READ_REGISTER_ULONG((PULONG)(Capabilities + XHCI_RTSOFF)) == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    XhciDebug.Operational = Capabilities + READ_REGISTER_UCHAR(Capabilities + XHCI_CAPLENGTH);
    if (READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_PAGESIZE)) != 1)
    {
        KdUsbTrace("PAGESIZE 0x%lx is not 4K\n",
                   READ_REGISTER_ULONG((PULONG)(XhciDebug.Operational + XHCI_PAGESIZE)));
        return STATUS_INVALID_PARAMETER;
    }

    Extended = XhciFindExtendedCapability(Offset, XHCI_EXT_CAP_DEBUG);
    if (Extended == NULL)
    {
        KdUsbTrace("no Debug Capability on this controller\n");
        return STATUS_NO_SUCH_DEVICE;
    }

    XhciDebug.Dbc = Extended;
    KdUsbTrace("DbC at +0x%lx, DCCTRL 0x%08lx\n",
               (ULONG)(Extended - Capabilities), XhciReadDbc(DBC_DCCTRL));

    /* Only the boot time reset takes the controller from the firmware */
    if (XhciDebug.ResetController)
        XhciClaimFromFirmware(Offset);

    return XhciResetController(&XhciDebug.ResetController);
}

/**
 * @brief
 * Assigns data pages to the transfers of one ring.
 *
 * A TRB buffer may not cross a 64 KB boundary, so pages that would put a
 * transfer across one are skipped. The rings own a few spare pages for that.
 *
 * @param[in] Pages
 * The candidate pages.
 *
 * @param[in] PageCount
 * How many candidate pages there are.
 *
 * @param[out] Assigned
 * Receives the page index for each transfer.
 *
 * @param[in] Transfers
 * How many transfers need a page.
 *
 * @return
 * TRUE when every transfer got a page.
 */
static
BOOLEAN
NTAPI
XhciAssignPages(
    _In_ UCHAR (*Pages)[PAGE_SIZE],
    _In_ ULONG PageCount,
    _Out_writes_(Transfers) PUCHAR Assigned,
    _In_ ULONG Transfers)
{
    ULONG64 Address;
    ULONG Transfer = 0;
    ULONG Page;

    for (Page = 0; Page < PageCount && Transfer < Transfers; Page++)
    {
        Address = XhciBusAddress(Pages[Page]);
        if ((Address >> 16) == ((Address + KDUSB_TRANSFER_SIZE) >> 16))
            Assigned[Transfer++] = (UCHAR)Page;
    }

    return Transfer == Transfers;
}

/**
 * @brief
 * Fills a string descriptor for the DbC info context.
 *
 * @param[out] Descriptor
 * The descriptor to fill.
 *
 * @param[in] Text
 * The string, NUL terminated.
 *
 * @param[in] Size
 * The size of Text in bytes, terminator included.
 */
static
VOID
NTAPI
XhciSetString(
    _Out_ PDBC_STRING Descriptor,
    _In_ PCWSTR Text,
    _In_ ULONG Size)
{
    Descriptor->Length = (UCHAR)(FIELD_OFFSET(DBC_STRING, Characters) + Size);
    Descriptor->DescriptorType = DBC_STRING_DESCRIPTOR_TYPE;
    RtlCopyMemory(Descriptor->Characters, Text, Size);
}

/**
 * @brief
 * Builds the rings and the DbC context, and enables the Debug Capability.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INSUFFICIENT_RESOURCES when the data pages
 * cannot hold every transfer.
 */
static
NTSTATUS
NTAPI
XhciEnableDebugCapability(VOID)
{
    static const WCHAR LanguageId[] = { 0x0409 };
    static const WCHAR Manufacturer[] = L"ReactOS";
    static const WCHAR Product[] = L"ReactOS USB3 Debug";
    static const WCHAR Serial[] = L"123456789";
    PXHCI_DMA Dma = XhciDebug.Dma;
    PDBC_CONTEXT Context = &Dma->Context;
    PXHCI_TRB Trb;
    ULONG Burst;
    ULONG Index;

    XhciDebug.EventIndex = 0;
    XhciDebug.EventCycle = 1;
    XhciDebug.OutCycle = 0;
    XhciDebug.InCycle = 0;

    Burst = (XhciReadDbc(DBC_DCCTRL) >> 16) & 0xFF;

    /* Event ring, one segment */
    RtlZeroMemory(Dma->EventRing, sizeof(Dma->EventRing));
    Dma->Segment.Base = XhciBusAddress(Dma->EventRing);
    Dma->Segment.Size = XHCI_EVENT_RING_SIZE;
    Dma->Segment.Reserved = 0;

    XhciWriteDbc(DBC_DCERSTSZ, 1);
    XhciWriteDbc64(DBC_DCERSTBA, XhciBusAddress(&Dma->Segment));
    XhciWriteDbc64(DBC_DCERDP, XhciBusAddress(Dma->EventRing));

    /* OUT ring: transfers stay with the driver until a packet is queued */
    if (!XhciAssignPages(Dma->OutPages, XHCI_OUT_PAGES, XhciDebug.OutPage, XHCI_OUT_TRANSFERS))
        return STATUS_INSUFFICIENT_RESOURCES;

    for (Index = 0; Index < XHCI_OUT_TRANSFERS; Index++)
    {
        Trb = &Dma->OutRing[Index];
        Trb->Parameter = XhciBusAddress(Dma->OutPages[XhciDebug.OutPage[Index]]);
        Trb->Status = KDUSB_TRANSFER_SIZE;
        Trb->Control = DBC_TRB_IOC | (XhciDebug.OutCycle ^ DBC_TRB_CYCLE);
    }

    Trb = &Dma->OutRing[XHCI_OUT_TRANSFERS];
    Trb->Parameter = XhciBusAddress(Dma->OutRing);
    Trb->Status = 0;
    Trb->Control = DBC_TRB_TYPE(DBC_TRB_TYPE_LINK) | DBC_TRB_TOGGLE_CYCLE | (XhciDebug.OutCycle ^ DBC_TRB_CYCLE);

    /* IN ring: every transfer is handed to the controller up front */
    if (!XhciAssignPages(Dma->InPages, XHCI_IN_PAGES, XhciDebug.InPage, XHCI_IN_TRANSFERS))
        return STATUS_INSUFFICIENT_RESOURCES;

    for (Index = 0; Index < XHCI_IN_TRANSFERS; Index++)
    {
        Trb = &Dma->InRing[Index];
        Trb->Parameter = XhciBusAddress(Dma->InPages[XhciDebug.InPage[Index]]);
        Trb->Status = PAGE_SIZE;
        Trb->Control = DBC_TRB_TYPE(DBC_TRB_TYPE_NORMAL) | DBC_TRB_ISP | DBC_TRB_IOC | XhciDebug.InCycle;
    }

    Trb = &Dma->InRing[XHCI_IN_TRANSFERS];
    Trb->Parameter = XhciBusAddress(Dma->InRing);
    Trb->Status = 0;
    Trb->Control = DBC_TRB_TYPE(DBC_TRB_TYPE_LINK) | DBC_TRB_TOGGLE_CYCLE | XhciDebug.InCycle;

    /* Strings the host shows for the debug device */
    XhciSetString(&Dma->String0, LanguageId, sizeof(LanguageId));
    XhciSetString(&Dma->Manufacturer, Manufacturer, sizeof(Manufacturer));
    XhciSetString(&Dma->Product, Product, sizeof(Product));
    XhciSetString(&Dma->Serial, Serial, sizeof(Serial));

    RtlZeroMemory(Context, sizeof(*Context));
    Context->Info.String0 = XhciBusAddress(&Dma->String0);
    Context->Info.Manufacturer = XhciBusAddress(&Dma->Manufacturer);
    Context->Info.Product = XhciBusAddress(&Dma->Product);
    Context->Info.Serial = XhciBusAddress(&Dma->Serial);
    Context->Info.String0Length = Dma->String0.Length;
    Context->Info.ManufacturerLength = Dma->Manufacturer.Length;
    Context->Info.ProductLength = Dma->Product.Length;
    Context->Info.SerialLength = Dma->Serial.Length;

    Context->Out.Info = (ENDPOINT_TYPE_BULK_OUT << 3) | (Burst << 8) | (ENDPOINT_MAX_PACKET << 16);
    Context->Out.Dequeue = XhciBusAddress(Dma->OutRing) | XhciDebug.OutCycle;
    Context->Out.AverageLength = KDUSB_TRANSFER_SIZE;

    Context->In.Info = (ENDPOINT_TYPE_BULK_IN << 3) | (Burst << 8) | (ENDPOINT_MAX_PACKET << 16);
    Context->In.Dequeue = XhciBusAddress(Dma->InRing) | XhciDebug.InCycle;
    Context->In.AverageLength = PAGE_SIZE;

    XhciWriteDbc64(DBC_DCCP, XhciBusAddress(Context));
    XhciWriteDbc(DBC_DCDDI1, DBC_DCDDI1_VALUE);
    XhciWriteDbc(DBC_DCDDI2, DBC_DCDDI2_VALUE);
    XhciWriteDbc(DBC_DCCTRL, DBC_DCCTRL_DCE | DBC_DCCTRL_DRC | DBC_DCCTRL_LSE);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Counts down a timeout in 10 microsecond steps.
 *
 * @param[in,out] Timeout
 * The microseconds left.
 *
 * @return
 * FALSE when no time was left.
 */
static
BOOLEAN
NTAPI
XhciStallTick(
    _Inout_ PULONG Timeout)
{
    if (*Timeout == 0)
        return FALSE;

    KdUsbStall(10);
    *Timeout = (*Timeout > 10) ? (*Timeout - 10) : 0;
    return TRUE;
}

/**
 * @brief
 * Waits for the host to configure the debug device.
 *
 * @param[in,out] Timeout
 * Microseconds to wait; receives what is left.
 *
 * @return
 * STATUS_SUCCESS once the device is configured, otherwise
 * STATUS_PORT_DISCONNECTED.
 */
static
NTSTATUS
NTAPI
XhciWaitForConnection(
    _Inout_ PULONG Timeout)
{
    ULONG Control;

    while (!(XhciReadDbc(DBC_DCPORTSC) & DBC_DCPORTSC_CCS))
    {
        if (!XhciStallTick(Timeout))
        {
            KdUsbTrace("no cable: DCPORTSC 0x%08lx DCCTRL 0x%08lx\n",
                       XhciReadDbc(DBC_DCPORTSC), XhciReadDbc(DBC_DCCTRL));
            return STATUS_PORT_DISCONNECTED;
        }
    }

    for (Control = XhciReadDbc(DBC_DCCTRL);
         !(Control & DBC_DCCTRL_DCR);
         Control = XhciReadDbc(DBC_DCCTRL))
    {
        if (!XhciStallTick(Timeout))
        {
            KdUsbTrace("host never configured us: DCPORTSC 0x%08lx DCCTRL 0x%08lx\n",
                       XhciReadDbc(DBC_DCPORTSC), Control);
            return STATUS_PORT_DISCONNECTED;
        }
    }

    if (Control & DBC_DCCTRL_DRC)
        XhciWriteDbc(DBC_DCCTRL, DBC_DCCTRL_DCE | DBC_DCCTRL_DRC | DBC_DCCTRL_LSE);

    /* Let the controller start on the IN transfers queued at setup */
    XhciWriteDbc(DBC_DCDB, DBC_DOORBELL_IN);
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Recovers the Debug Capability after something outside the debugger reset
 * the controller, for example the xHCI driver taking it over.
 *
 * @return
 * STATUS_SUCCESS when the host is connected.
 */
static
NTSTATUS
NTAPI
XhciRecoverFromReset(VOID)
{
    BOOLEAN Reset = FALSE;
    NTSTATUS Status;
    ULONG Timeout;

    if (XhciReadDbc(DBC_DCCTRL) & DBC_DCCTRL_DCE)
    {
        Timeout = 0;
        return XhciWaitForConnection(&Timeout);
    }

    Status = XhciResetController(&Reset);
    if (!NT_SUCCESS(Status))
        return Status;

    XhciEnableDebugCapability();

    Timeout = 2500000;
    return XhciWaitForConnection(&Timeout);
}

/**
 * @brief
 * Drains the event ring, recording each completed transfer in its own TRB.
 *
 * A completed TRB keeps its buffer and gets the Transfer Event type, the
 * completion code in the top byte of Status, and the bytes moved below it.
 *
 * @return
 * STATUS_SUCCESS when events were handled, STATUS_IO_TIMEOUT when there
 * were none, or STATUS_CONNECTION_RESET when the capability was disabled.
 */
static
NTSTATUS
NTAPI
XhciProcessEvents(VOID)
{
    PXHCI_DMA Dma = XhciDebug.Dma;
    NTSTATUS Status = STATUS_IO_TIMEOUT;
    PXHCI_TRB Event;
    PXHCI_TRB Trb;
    ULONG Index = XhciDebug.EventIndex;
    ULONG Cycle = XhciDebug.EventCycle;
    ULONG Moved;
    ULONG Completion;

    if (!(XhciReadDbc(DBC_DCCTRL) & DBC_DCCTRL_DCE))
        return STATUS_CONNECTION_RESET;

    for (Event = &Dma->EventRing[Index];
         (Event->Control & DBC_TRB_CYCLE) == Cycle;
         Event = &Dma->EventRing[Index])
    {
        Status = STATUS_SUCCESS;

        if ((Event->Control & DBC_TRB_TYPE_MASK) == DBC_TRB_TYPE(DBC_TRB_TYPE_TRANSFER_EVENT) &&
            Event->Parameter >= XhciDebug.DmaAddress &&
            Event->Parameter < XhciDebug.DmaAddress + XhciDebug.DmaLength)
        {
            Trb = (PXHCI_TRB)((PUCHAR)Dma + (ULONG_PTR)(Event->Parameter - XhciDebug.DmaAddress));

            /* The event reports what was left over, keep what was moved */
            Moved = ((Trb->Status & DBC_TRB_LENGTH_MASK) - Event->Status) & DBC_TRB_RESULT_MASK;
            Completion = DBC_TRB_COMPLETION(Event->Status);
            if (Completion == DBC_TRB_COMPLETION_SHORT_PACKET)
                Completion = DBC_TRB_COMPLETION_SUCCESS;

            Trb->Status = Moved | (Completion << 24);
            Trb->Control = (Trb->Control & ~DBC_TRB_TYPE_MASK) | DBC_TRB_TYPE(DBC_TRB_TYPE_TRANSFER_EVENT);
        }

        Index = (Index + 1) % XHCI_EVENT_RING_SIZE;
        if (Index == 0)
            Cycle ^= DBC_TRB_CYCLE;
    }

    if (NT_SUCCESS(Status))
    {
        XhciDebug.EventIndex = Index;
        XhciDebug.EventCycle = Cycle;
        XhciWriteDbc64(DBC_DCERDP, XhciBusAddress(&Dma->EventRing[Index]));
    }

    if (!(XhciReadDbc(DBC_DCCTRL) & DBC_DCCTRL_DCE))
        Status = STATUS_CONNECTION_RESET;

    return Status;
}

/**
 * @brief
 * Claims the next OUT transfer.
 *
 * @param[out] Index
 * Receives the claimed transfer.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_IO_TIMEOUT while that transfer is still in
 * flight.
 */
static
NTSTATUS
NTAPI
XhciClaimOutTransfer(
    _Out_ PULONG Index)
{
    PXHCI_TRB Trb = &XhciDebug.Dma->OutRing[XhciDebug.OutIndex];

    if ((Trb->Control & DBC_TRB_TYPE_MASK) == DBC_TRB_TYPE(DBC_TRB_TYPE_NORMAL))
        return STATUS_IO_TIMEOUT;

    Trb->Control = (Trb->Control & ~DBC_TRB_TYPE_MASK) | DBC_TRB_TYPE(DBC_TRB_TYPE_NORMAL);
    Trb->Status = (Trb->Status & ~DBC_TRB_LENGTH_MASK) | KDUSB_TRANSFER_SIZE;

    *Index = XhciDebug.OutIndex;
    XhciDebug.OutIndex = (XhciDebug.OutIndex + 1) % XHCI_OUT_TRANSFERS;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Queues a claimed OUT transfer and waits for it to finish.
 *
 * @param[in] Index
 * The transfer from XhciClaimOutTransfer.
 *
 * @param[in] Length
 * How many bytes of its page to send.
 *
 * @return
 * STATUS_SUCCESS when the host took the data.
 */
static
NTSTATUS
NTAPI
XhciSubmitOutTransfer(
    _In_ ULONG Index,
    _In_ ULONG Length)
{
    PXHCI_DMA Dma = XhciDebug.Dma;
    PXHCI_TRB Trb;
    NTSTATUS Status;
    ULONG Spin;
    ULONG Control;
    BOOLEAN Wraps;

    if (Index >= XHCI_OUT_TRANSFERS || Length > KDUSB_TRANSFER_SIZE)
        return STATUS_INVALID_PARAMETER;

    /* A length on a packet boundary would need a zero length packet after it */
    if ((Length % ENDPOINT_MAX_PACKET) == 0)
        Dma->OutPages[XhciDebug.OutPage[Index]][Length++] = 0;

    Trb = &Dma->OutRing[Index];
    Trb->Status = Length;
    Trb->Control = (Trb->Control & (DBC_TRB_CYCLE | DBC_TRB_TYPE_MASK)) | DBC_TRB_IOC;

    Wraps = (Index == XHCI_OUT_TRANSFERS - 1);
    if (Wraps)
        XhciSetCycle(&Dma->OutRing[XHCI_OUT_TRANSFERS], XhciDebug.OutCycle);

    KeMemoryBarrier();
    KdUsbStall(1);
    XhciSetCycle(Trb, XhciDebug.OutCycle);
    XhciWriteDbc(DBC_DCDB, DBC_DOORBELL_OUT);

    if (Wraps)
        XhciDebug.OutCycle ^= DBC_TRB_CYCLE;

    for (Spin = 25000; ; )
    {
        Control = Trb->Control;
        if ((Control & DBC_TRB_TYPE_MASK) != DBC_TRB_TYPE(DBC_TRB_TYPE_NORMAL))
            break;

        Status = XhciProcessEvents();
        if (Status == STATUS_IO_TIMEOUT)
        {
            if (Spin-- == 0)
                return Status;

            KdUsbStall(10);
        }
        else if (!NT_SUCCESS(Status))
        {
            return Status;
        }
    }

    Status = (DBC_TRB_COMPLETION(Trb->Status) == DBC_TRB_COMPLETION_SUCCESS) ? STATUS_SUCCESS
                                                                      : STATUS_UNSUCCESSFUL;
    Trb->Control = Control & ~DBC_TRB_TYPE_MASK;
    return Status;
}

/**
 * @brief
 * Hands an IN transfer back to the controller after its data was read.
 *
 * @param[in] Index
 * The transfer to requeue.
 */
static
VOID
NTAPI
XhciRequeueInTransfer(
    _In_ ULONG Index)
{
    PXHCI_DMA Dma = XhciDebug.Dma;
    PXHCI_TRB Trb = &Dma->InRing[Index];

    /* Starting a new lap of the ring, so the link TRB goes back first */
    if (Index == 0)
    {
        XhciSetCycle(&Dma->InRing[XHCI_IN_TRANSFERS], XhciDebug.InCycle);
        XhciDebug.InCycle ^= DBC_TRB_CYCLE;
    }

    Trb->Status = KDUSB_TRANSFER_SIZE;
    Trb->Control = (Trb->Control & DBC_TRB_CYCLE) | DBC_TRB_TYPE(DBC_TRB_TYPE_NORMAL) | DBC_TRB_ISP | DBC_TRB_IOC;

    KeMemoryBarrier();
    KdUsbStall(1);
    XhciSetCycle(Trb, XhciDebug.InCycle);
    XhciWriteDbc(DBC_DCDB, DBC_DOORBELL_IN);
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
    NTSTATUS Status;
    PUCHAR Page;
    ULONG Index;
    ULONG Length = 0;
    ULONG Piece;

    if (!NT_SUCCESS(XhciRecoverFromReset()))
    {
        KdUsbSetHostPresent(FALSE);
        return;
    }

    if (!NT_SUCCESS(XhciClaimOutTransfer(&Index)))
        return;

    Page = XhciDebug.Dma->OutPages[XhciDebug.OutPage[Index]];
    for (; Count != 0; Count--, Buffers++)
    {
        Piece = min(Buffers->Length, KDUSB_TRANSFER_SIZE - Length);
        RtlCopyMemory(Page + Length, Buffers->Data, Piece);
        Length += Piece;
    }

    Status = XhciSubmitOutTransfer(Index, Length);
    if (Status == STATUS_IO_TIMEOUT)
        KdUsbSetHostPresent(FALSE);
    else if (NT_SUCCESS(Status))
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
    static CHAR Prefix[] = "NAME=";
    static CHAR Terminator[2] = { 0, 0 };
    KDUSB_BUFFER Reply[3];

    Reply[0].Data = Prefix;
    Reply[0].Length = sizeof(Prefix) - 1;
    Reply[1].Data = KdUsbTargetName;
    Reply[1].Length = KdUsbTargetNameLength;
    Reply[2].Data = Terminator;
    Reply[2].Length = sizeof(Terminator);

    XhciSendBuffers(Reply, RTL_NUMBER_OF(Reply));
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
    PXHCI_TRB Trb;
    NTSTATUS Status;
    PUCHAR Page;
    ULONG Index;
    ULONG Length;
    ULONG Spin;

    if (!NT_SUCCESS(XhciRecoverFromReset()))
    {
        KdUsbSetHostPresent(FALSE);
        return KdPacketTimedOut;
    }

    for (;;)
    {
        for (Spin = 400000; ; )
        {
            Status = XhciProcessEvents();
            if (!NT_SUCCESS(Status) && Status != STATUS_IO_TIMEOUT)
                return KdPacketNeedsResend;

            Trb = &XhciDebug.Dma->InRing[XhciDebug.InIndex];
            if ((Trb->Control & DBC_TRB_TYPE_MASK) == DBC_TRB_TYPE(DBC_TRB_TYPE_TRANSFER_EVENT))
                break;

            if (PacketType == PACKET_TYPE_KD_POLL_BREAKIN)
                return KdPacketTimedOut;

            KdUsbStall(5);
            if (--Spin == 0)
                return KdPacketTimedOut;
        }

        Index = XhciDebug.InIndex;
        Length = 0;
        if (DBC_TRB_COMPLETION(Trb->Status) == DBC_TRB_COMPLETION_SUCCESS)
        {
            Length = Trb->Status & DBC_TRB_RESULT_MASK;
            if (Length > KDUSB_TRANSFER_SIZE)
                Length = 0;
        }

        Page = XhciDebug.Dma->InPages[XhciDebug.InPage[Index]];
        RtlZeroMemory(Page + Length, KDUSB_TRANSFER_SIZE - Length);
        XhciDebug.InIndex = (Index + 1) % XHCI_IN_TRANSFERS;

        KdUsbSetHostPresent(TRUE);
        RtlCopyMemory(KdUsbReceiveBuffer, Page, Length);
        XhciRequeueInTransfer(Index);

        if (!KdUsbIsNameQuery(Length))
            break;

        XhciSendTargetName();
        if (!NT_SUCCESS(XhciRecoverFromReset()))
        {
            KdUsbSetHostPresent(FALSE);
            return KdPacketTimedOut;
        }
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
 * Finds the register BAR, makes sure the controller decodes it and can
 * master the bus, then starts the Debug Capability.
 *
 * @param[in] LoaderBlock
 * The loader block at boot, or NULL when re-initializing.
 *
 * @return
 * STATUS_SUCCESS once the host is connected, STATUS_PORT_DISCONNECTED when
 * the capability is up but no host answered yet.
 */
static
NTSTATUS
NTAPI
XhciStartController(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PDEBUG_DEVICE_ADDRESS Bar = NULL;
    NTSTATUS Status;
    USHORT Command;
    ULONG Timeout;
    ULONG Index;

    for (Index = 0; Index < MAXIMUM_DEBUG_BARS; Index++)
    {
        Bar = &KdUsbDevice.BaseAddress[Index];
        if (Bar->Valid && Bar->Type == CmResourceTypeMemory && Bar->TranslatedAddress != NULL)
            break;
    }

    if (Index == MAXIMUM_DEBUG_BARS)
    {
        KdUsbTrace("no mapped memory BAR\n");
        return STATUS_UNSUCCESSFUL;
    }

    XhciDebug.Capabilities = Bar->TranslatedAddress;
    XhciDebug.CapabilitiesLength = Bar->Length;
    KdUsbTrace("BAR%lu at %p, 0x%lx bytes\n", Index, Bar->TranslatedAddress, Bar->Length);

    /* The HAL's PCI config routines are discarded once boot is over */
    if (LoaderBlock != NULL &&
        KdUsbDevice.NameSpace == KdNameSpacePCI &&
        KdGetPciDataByOffset != NULL &&
        KdGetPciDataByOffset(KdUsbDevice.Bus,
                             KdUsbDevice.Slot,
                             &Command,
                             FIELD_OFFSET(PCI_COMMON_HEADER, Command),
                             sizeof(Command)) == sizeof(Command))
    {
        KdUsbTrace("PCI command 0x%04x\n", Command);
        if ((Command & (PCI_ENABLE_MEMORY_SPACE | PCI_ENABLE_BUS_MASTER)) !=
            (PCI_ENABLE_MEMORY_SPACE | PCI_ENABLE_BUS_MASTER))
        {
            Command |= PCI_ENABLE_MEMORY_SPACE | PCI_ENABLE_BUS_MASTER;
            if (KdSetPciDataByOffset(KdUsbDevice.Bus,
                                     KdUsbDevice.Slot,
                                     &Command,
                                     FIELD_OFFSET(PCI_COMMON_HEADER, Command),
                                     sizeof(Command)) != sizeof(Command))
            {
                return STATUS_UNSUCCESSFUL;
            }
        }
    }

    Status = XhciLocateDebugCapability();
    if (!NT_SUCCESS(Status))
    {
        KdUsbTrace("locate/reset failed 0x%lx\n", Status);
        return Status;
    }

    Status = XhciEnableDebugCapability();
    if (!NT_SUCCESS(Status))
    {
        KdUsbTrace("enabling DbC failed 0x%lx\n", Status);
        return Status;
    }

    KdUsbTrace("DbC enabled, waiting for host\n");

    Timeout = 2500000;
    return XhciWaitForConnection(&Timeout);
}

/**
 * @brief
 * Checks whether the debug device descriptor names an xHCI controller.
 *
 * @return
 * TRUE for an xHCI programming interface.
 */
static
BOOLEAN
NTAPI
XhciIsXhciDevice(VOID)
{
    return KdUsbDevice.BaseClass == PCI_CLASS_SERIAL_BUS_CTLR &&
           KdUsbDevice.SubClass == PCI_SUBCLASS_SB_USB &&
           KdUsbDevice.ProgIf == KDUSB_PROGIF_XHCI;
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

    if (KdUsbDevice.BaseClass != PCI_CLASS_SERIAL_BUS_CTLR ||
        KdUsbDevice.SubClass != PCI_SUBCLASS_SB_USB ||
        (KdUsbDevice.ProgIf != KDUSB_PROGIF_XHCI && KdUsbDevice.ProgIf != 0xFF))
    {
        return FALSE;
    }

    if (LoaderBlock != NULL)
    {
        KdUsbDevice.Memory.MaxEnd.QuadPart = (LONGLONG)-1;
        KdUsbDevice.Memory.Length = sizeof(*XhciDebug.Dma);

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

        if (!XhciIsXhciDevice() || (KdUsbDevice.Memory.Start.QuadPart & (PAGE_SIZE - 1)) != 0)
        {
            KdReleasePciDeviceforDebugging(&KdUsbDevice);
            return FALSE;
        }
    }

    if (KdUsbDevice.Memory.VirtualAddress == NULL)
        return FALSE;

    XhciDebug.Dma = KdUsbDevice.Memory.VirtualAddress;
    XhciDebug.DmaAddress = KdUsbDevice.Memory.Start.QuadPart;
    XhciDebug.DmaLength = KdUsbDevice.Memory.Length;
    XhciDebug.EventIndex = 0;
    XhciDebug.OutIndex = 0;
    XhciDebug.InIndex = 0;
    RtlZeroMemory(XhciDebug.Dma, sizeof(*XhciDebug.Dma));

    Status = XhciStartController(LoaderBlock);
    KdUsbTrace("xHCI start 0x%lx\n", Status);
    if (Status == STATUS_PORT_DISCONNECTED)
        return TRUE;

    if (!NT_SUCCESS(Status))
    {
        if (LoaderBlock != NULL)
            KdReleasePciDeviceforDebugging(&KdUsbDevice);

        return FALSE;
    }

    return TRUE;
}

/**
 * @brief
 * Asks for a host controller reset on the next initialization.
 */
VOID
NTAPI
KdUsbXhciRequestReset(VOID)
{
    XhciDebug.ResetController = TRUE;
}

const KDUSB_CONTROLLER KdUsbXhciController =
{
    XhciInitialize,
    XhciReadPacket,
    XhciSendBuffers,
    XhciShutdown
};
