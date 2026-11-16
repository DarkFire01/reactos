/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DWC3 device controller driven by polling, presenting a CDC-ACM serial port
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kddwc.h"

/* Most a controller command or reset may take */
#define KDDWC_COMMAND_TIMEOUT_US        10000
#define KDDWC_RESET_TIMEOUT_US          100000

/* USB requests */
#define USB_REQ_GET_STATUS              0x00
#define USB_REQ_CLEAR_FEATURE           0x01
#define USB_REQ_SET_FEATURE             0x03
#define USB_REQ_SET_ADDRESS             0x05
#define USB_REQ_GET_DESCRIPTOR          0x06
#define USB_REQ_GET_CONFIGURATION       0x08
#define USB_REQ_SET_CONFIGURATION       0x09
#define USB_REQ_GET_INTERFACE           0x0A
#define USB_REQ_SET_INTERFACE           0x0B
#define CDC_REQ_SET_LINE_CODING         0x20
#define CDC_REQ_GET_LINE_CODING         0x21
#define CDC_REQ_SET_CONTROL_LINE_STATE  0x22
#define CDC_REQ_SEND_BREAK              0x23

#define USB_TYPE_MASK                   0x60
#define USB_TYPE_STANDARD               0x00
#define USB_TYPE_CLASS                  0x20
#define USB_DIR_IN                      0x80

#define USB_DESC_DEVICE                 0x01
#define USB_DESC_CONFIGURATION          0x02
#define USB_DESC_STRING                 0x03
#define USB_DESC_QUALIFIER              0x06

/* Offsets of the two bulk packet sizes inside the configuration descriptor */
#define KDDWC_CONFIG_OUT_MPS            67
#define KDDWC_CONFIG_IN_MPS             74

typedef enum _KDDWC_EP0_STATE
{
    Ep0Setup,
    Ep0DataIn,
    Ep0DataOut,
    Ep0WaitStatusIn,
    Ep0WaitStatusOut,
    Ep0Status
} KDDWC_EP0_STATE;

typedef struct _KDDWC_CONTROLLER
{
    PUCHAR Core;
    PUCHAR Glue;
    PKDDWC_DMA Dma;
    ULONG64 DmaAddress;

    ULONG EventOffset;
    KDDWC_EP0_STATE Ep0State;
    BOOLEAN Ep0ThreeStage;
    UCHAR PendingRequest;
    USHORT BulkPacket;

    UCHAR Configuration;
    BOOLEAN Configured;
    BOOLEAN Listening;
    BOOLEAN RxArmed;
    BOOLEAN TxBusy;
    UCHAR RxResource;
    UCHAR TxResource;

    UCHAR LineCoding[7];

    UCHAR Input[KDDWC_INPUT_BYTES];
    ULONG InputHead;
    ULONG InputTail;

    /* The controller can lose its power to the operating system and come back */
    const KDDWC_PLATFORM* Platform;
    BOOLEAN Up;
    ULONG64 DownSince;
    ULONG64 NextRevival;
    ULONG64 NextCheck;
} KDDWC_CONTROLLER;

/* How often a live core is checked for still being ours, and a dead one retried */
#define KDDWC_CHECK_US                  100000
#define KDDWC_REVIVE_US                 2000000

/* How long power taken away by someone else is left alone before the debugger restores it */
#define KDDWC_RECLAIM_US                10000000

static KDDWC_CONTROLLER Dwc;

/* Pages taken from the loader for the DMA block, marked so memory management keeps off them */
static MEMORY_ALLOCATION_DESCRIPTOR KdDwcDmaDescriptor;

/* DESCRIPTORS ****************************************************************/

/* pid.codes test identifiers, never shipped on a product */
static const UCHAR KdDwcDeviceDescriptor[18] =
{
    18, USB_DESC_DEVICE, 0x00, 0x02,
    0xEF, 0x02, 0x01, KDDWC_EP0_PACKET,
    0x09, 0x12, 0x01, 0x00,
    0x00, 0x01, 1, 2, 3, 1
};

static const UCHAR KdDwcQualifierDescriptor[10] =
{
    10, USB_DESC_QUALIFIER, 0x00, 0x02,
    0xEF, 0x02, 0x01, KDDWC_EP0_PACKET, 1, 0
};

/* One function with a communication and a data interface, grouped so Windows binds usbser */
static UCHAR KdDwcConfigDescriptor[75] =
{
    /* Configuration */
    9, USB_DESC_CONFIGURATION, 75, 0, 2, 1, 0, 0x80, 250,
    /* Interface association */
    8, 0x0B, 0, 2, 0x02, 0x02, 0x01, 0,
    /* Communication interface */
    9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    /* Header, call management, abstract control and union functional descriptors */
    5, 0x24, 0x00, 0x10, 0x01,
    5, 0x24, 0x01, 0x00, 0x01,
    4, 0x24, 0x02, 0x02,
    5, 0x24, 0x06, 0, 1,
    /* Notification endpoint */
    7, 0x05, KDDWC_USB_NOTIFY_IN, 0x03, KDDWC_NOTIFY_PACKET, 0, 9,
    /* Data interface */
    9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    /* Bulk out and in, packet sizes patched for the link speed */
    7, 0x05, KDDWC_USB_DATA_OUT, 0x02, 0x00, 0x02, 0,
    7, 0x05, KDDWC_USB_DATA_IN, 0x02, 0x00, 0x02, 0
};

static const PCWSTR KdDwcStrings[] =
{
    L"ReactOS",
    L"ReactOS Kernel Debugger",
    L"DWC3-0"
};

/* REGISTER ACCESS ************************************************************/

ULONG
NTAPI
KdDwcRead(
    _In_ PUCHAR Base,
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((volatile ULONG*)(Base + Offset));
}

VOID
NTAPI
KdDwcWrite(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((volatile ULONG*)(Base + Offset), Value);
}

VOID
NTAPI
KdDwcModify(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Clear,
    _In_ ULONG Set)
{
    KdDwcWrite(Base, Offset, (KdDwcRead(Base, Offset) & ~Clear) | Set);
}

/* Waits for (register & Mask) == Value, TRUE when it got there in time */
BOOLEAN
NTAPI
KdDwcWaitFor(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Mask,
    _In_ ULONG Value,
    _In_ ULONG TimeoutUs)
{
    ULONG64 Start = KdDwcMicroseconds();

    while ((KdDwcRead(Base, Offset) & Mask) != Value)
    {
        if (KdDwcMicroseconds() - Start > TimeoutUs)
            return FALSE;
        YieldProcessor();
    }

    return TRUE;
}

/* Register pages stay mapped, bring up runs again whenever the controller lost its power */
#define KDDWC_MAPPED_PAGES              32

static ULONG64 KdDwcMappedPhysical[KDDWC_MAPPED_PAGES];
static PUCHAR KdDwcMappedVirtual[KDDWC_MAPPED_PAGES];
static ULONG KdDwcMappedCount;

/* Maps the page that holds a physical register once and returns the register's address */
PUCHAR
NTAPI
KdDwcMapRegister(
    _In_ ULONG64 PhysicalAddress)
{
    PHYSICAL_ADDRESS Page;
    PUCHAR Base;
    ULONG i;

    Page.QuadPart = PhysicalAddress & ~(ULONG64)(PAGE_SIZE - 1);
    for (i = 0; i < KdDwcMappedCount; i++)
    {
        if (KdDwcMappedPhysical[i] == (ULONG64)Page.QuadPart)
            return KdDwcMappedVirtual[i] + (PhysicalAddress & (PAGE_SIZE - 1));
    }

    if (KdDwcMappedCount == KDDWC_MAPPED_PAGES)
        return NULL;

    Base = KdMapPhysicalMemory64(Page, 1);
    if (Base == NULL)
        return NULL;

    KdDwcMappedPhysical[KdDwcMappedCount] = Page.QuadPart;
    KdDwcMappedVirtual[KdDwcMappedCount] = Base;
    KdDwcMappedCount++;
    return Base + (PhysicalAddress & (PAGE_SIZE - 1));
}

/* DMA MEMORY *****************************************************************/

/**
 * @brief Takes pages for the DMA block from a free loader descriptor below 4 GB.
 *
 * The pages are carved off the front of the block and listed under a descriptor
 * of their own, so memory management never hands them out.
 */
static
ULONG64
KdDwcTakePages(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ PFN_COUNT PageCount)
{
    PMEMORY_ALLOCATION_DESCRIPTOR Block;
    PLIST_ENTRY Entry;
    PFN_NUMBER Limit = (PFN_NUMBER)(0x100000000ULL >> PAGE_SHIFT);

    for (Entry = LoaderBlock->MemoryDescriptorListHead.Flink;
         Entry != &LoaderBlock->MemoryDescriptorListHead;
         Entry = Entry->Flink)
    {
        Block = CONTAINING_RECORD(Entry, MEMORY_ALLOCATION_DESCRIPTOR, ListEntry);
        if (Block->MemoryType != LoaderFree || Block->BasePage == 0)
            continue;
        if (Block->PageCount <= PageCount || Block->BasePage + PageCount >= Limit)
            continue;

        KdDwcDmaDescriptor.MemoryType = LoaderFirmwarePermanent;
        KdDwcDmaDescriptor.BasePage = Block->BasePage;
        KdDwcDmaDescriptor.PageCount = PageCount;

        Block->BasePage += PageCount;
        Block->PageCount -= PageCount;
        InsertTailList(&Block->ListEntry, &KdDwcDmaDescriptor.ListEntry);

        return (ULONG64)KdDwcDmaDescriptor.BasePage << PAGE_SHIFT;
    }

    return 0;
}

static
ULONG64
KdDwcPhysical(
    _In_ const VOID* Pointer)
{
    return Dwc.DmaAddress + (ULONG64)((const UCHAR*)Pointer - (const UCHAR*)Dwc.Dma);
}

/* The DMA block is device memory, so every access is aligned and done one register at a time */
static
VOID
KdDwcCopyToDma(
    _Out_writes_bytes_(Length) volatile UCHAR* Destination,
    _In_reads_bytes_(Length) const UCHAR* Source,
    _In_ ULONG Length)
{
    ULONG i;

    for (i = 0; i < Length; i++)
        WRITE_REGISTER_UCHAR(&Destination[i], Source[i]);
}

static
VOID
KdDwcCopyFromDma(
    _Out_writes_bytes_(Length) UCHAR* Destination,
    _In_reads_bytes_(Length) volatile const UCHAR* Source,
    _In_ ULONG Length)
{
    ULONG i;

    for (i = 0; i < Length; i++)
        Destination[i] = READ_REGISTER_UCHAR((volatile UCHAR*)&Source[i]);
}

static
VOID
KdDwcWriteTrb(
    _Out_ volatile DWC_TRB* Trb,
    _In_ const VOID* Buffer,
    _In_ ULONG Size,
    _In_ ULONG Control)
{
    ULONG64 Address = KdDwcPhysical(Buffer);

    WRITE_REGISTER_ULONG(&Trb->BufferLow, (ULONG)Address);
    WRITE_REGISTER_ULONG(&Trb->BufferHigh, (ULONG)(Address >> 32));
    WRITE_REGISTER_ULONG(&Trb->Size, Size & DWC_TRB_SIZE_MASK);

    /* The controller owns the TRB once HWO is seen, so that goes last */
    KeMemoryBarrier();
    WRITE_REGISTER_ULONG(&Trb->Control, Control | DWC_TRB_CTRL_HWO);
    KeMemoryBarrier();
}

/* ENDPOINT COMMANDS **********************************************************/

static
BOOLEAN
KdDwcEndpointCommand(
    _In_ ULONG Endpoint,
    _In_ ULONG Command,
    _In_ ULONG Parameter0,
    _In_ ULONG Parameter1,
    _Out_opt_ PUCHAR Resource)
{
    ULONG Value;

    KdDwcTraceUsb("endpoint %lu command 0x%lx 0x%lx 0x%lx\n", Endpoint, Command, Parameter0, Parameter1);
    KdDwcWrite(Dwc.Core, DWC_DEPCMDPAR2(Endpoint), 0);
    KdDwcWrite(Dwc.Core, DWC_DEPCMDPAR1(Endpoint), Parameter1);
    KdDwcWrite(Dwc.Core, DWC_DEPCMDPAR0(Endpoint), Parameter0);
    KdDwcWrite(Dwc.Core, DWC_DEPCMD(Endpoint), Command | DWC_DEPCMD_CMDACT);
    KdDwcTraceUsb("issued\n");

    if (!KdDwcWaitFor(Dwc.Core, DWC_DEPCMD(Endpoint), DWC_DEPCMD_CMDACT, 0, KDDWC_COMMAND_TIMEOUT_US))
    {
        KdDwcTrace("endpoint %lu command 0x%lx timed out\n", Endpoint, Command);
        return FALSE;
    }

    Value = KdDwcRead(Dwc.Core, DWC_DEPCMD(Endpoint));
    KdDwcTraceUsb("done 0x%lx\n", Value);
    if (Resource != NULL)
        *Resource = (UCHAR)DWC_DEPCMD_RESOURCE(Value);

    return DWC_DEPCMD_STATUS(Value) == 0;
}

static
BOOLEAN
KdDwcConfigureEndpoint(
    _In_ ULONG Endpoint,
    _In_ ULONG Type,
    _In_ ULONG PacketSize,
    _In_ ULONG Fifo,
    _In_ ULONG Events,
    _In_ BOOLEAN Modify)
{
    ULONG Parameter0;
    ULONG Parameter1;

    Parameter0 = DWC_EPCFG0_TYPE(Type) | DWC_EPCFG0_MPS(PacketSize) | DWC_EPCFG0_FIFO(Fifo);
    if (Modify)
        Parameter0 |= DWC_EPCFG0_ACTION_MODIFY;

    Parameter1 = Events | DWC_EPCFG1_NUMBER(Endpoint);

    if (!KdDwcEndpointCommand(Endpoint, DWC_DEPCMD_SETEPCONFIG, Parameter0, Parameter1, NULL))
        return FALSE;

    /* A modified endpoint keeps its transfer resource */
    if (Modify)
        return TRUE;

    return KdDwcEndpointCommand(Endpoint, DWC_DEPCMD_SETTRANSFRESOURCE, 1, 0, NULL);
}

static
BOOLEAN
KdDwcStartTransfer(
    _In_ ULONG Endpoint,
    _In_ volatile DWC_TRB* Trb,
    _Out_opt_ PUCHAR Resource)
{
    ULONG64 Address = KdDwcPhysical((const VOID*)Trb);

    return KdDwcEndpointCommand(Endpoint,
                                DWC_DEPCMD_STARTTRANSFER,
                                (ULONG)(Address >> 32),
                                (ULONG)Address,
                                Resource);
}

/* The core is not polled for an end of transfer it reports no completion for, it gets time instead */
static
VOID
KdDwcEndTransfer(
    _In_ ULONG Endpoint,
    _In_ UCHAR Resource)
{
    KdDwcTraceUsb("endpoint %lu end transfer %u\n", Endpoint, Resource);
    KdDwcModify(Dwc.Core, DWC_GUSB2PHYCFG, DWC_GUSB2PHYCFG_SUSPEND, 0);
    KdDwcWrite(Dwc.Core, DWC_DEPCMDPAR2(Endpoint), 0);
    KdDwcWrite(Dwc.Core, DWC_DEPCMDPAR1(Endpoint), 0);
    KdDwcWrite(Dwc.Core, DWC_DEPCMDPAR0(Endpoint), 0);
    KdDwcWrite(Dwc.Core,
               DWC_DEPCMD(Endpoint),
               DWC_DEPCMD_ENDTRANSFER | DWC_DEPCMD_FORCERM | DWC_DEPCMD_CMDACT | DWC_DEPCMD_PARAM(Resource));
    KdDwcStall(100);
}

/* CONTROL ENDPOINT ***********************************************************/

static
VOID
KdDwcArmSetup(VOID)
{
    KdDwcWriteTrb(&Dwc.Dma->Ep0Trb,
                  Dwc.Dma->Setup,
                  8,
                  DWC_TRB_TYPE_CONTROL_SETUP | DWC_TRB_CTRL_LST | DWC_TRB_CTRL_IOC | DWC_TRB_CTRL_ISP_IMI);

    KdDwcStartTransfer(KDDWC_EP0_OUT, &Dwc.Dma->Ep0Trb, NULL);
    Dwc.Ep0State = Ep0Setup;
}

static
VOID
KdDwcStallControl(VOID)
{
    KdDwcEndpointCommand(KDDWC_EP0_OUT, DWC_DEPCMD_SETSTALL, 0, 0, NULL);
    KdDwcArmSetup();
}

/* Sends the data stage of an IN request, cut to what the host asked for */
static
VOID
KdDwcControlIn(
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG Length,
    _In_ ULONG Requested)
{
    Length = min(Length, min(Requested, (ULONG)KDDWC_EP0_BYTES));
    KdDwcTraceUsb("control in %lu bytes from %p to %p\n", Length, Data, Dwc.Dma->Ep0Buffer);
    KdDwcCopyToDma(Dwc.Dma->Ep0Buffer, Data, Length);
    KdDwcTraceUsb("copied, TRB control 0x%lx\n", READ_REGISTER_ULONG(&Dwc.Dma->Ep0Trb.Control));

    KdDwcWriteTrb(&Dwc.Dma->Ep0Trb,
                  Dwc.Dma->Ep0Buffer,
                  Length,
                  DWC_TRB_TYPE_CONTROL_DATA | DWC_TRB_CTRL_LST | DWC_TRB_CTRL_IOC);

    KdDwcStartTransfer(KDDWC_EP0_IN, &Dwc.Dma->Ep0Trb, NULL);
    Dwc.Ep0ThreeStage = TRUE;
    Dwc.Ep0State = Ep0DataIn;
}

/* Receives the data stage of an OUT request, a whole packet as the controller wants */
static
VOID
KdDwcControlOut(
    _In_ UCHAR Request)
{
    KdDwcWriteTrb(&Dwc.Dma->Ep0Trb,
                  Dwc.Dma->Ep0Buffer,
                  KDDWC_EP0_PACKET,
                  DWC_TRB_TYPE_CONTROL_DATA | DWC_TRB_CTRL_LST | DWC_TRB_CTRL_IOC | DWC_TRB_CTRL_ISP_IMI);

    KdDwcStartTransfer(KDDWC_EP0_OUT, &Dwc.Dma->Ep0Trb, NULL);
    Dwc.PendingRequest = Request;
    Dwc.Ep0ThreeStage = TRUE;
    Dwc.Ep0State = Ep0DataOut;
}

/* A request without data waits for the controller to ask for the status stage */
static
VOID
KdDwcControlNoData(VOID)
{
    Dwc.Ep0ThreeStage = FALSE;
    Dwc.Ep0State = Ep0WaitStatusIn;
}

static
VOID
KdDwcStartStatus(
    _In_ ULONG Endpoint)
{
    ULONG Type = Dwc.Ep0ThreeStage ? DWC_TRB_TYPE_CONTROL_STATUS3 : DWC_TRB_TYPE_CONTROL_STATUS2;

    KdDwcWriteTrb(&Dwc.Dma->Ep0Trb,
                  Dwc.Dma->Ep0Buffer,
                  0,
                  Type | DWC_TRB_CTRL_LST | DWC_TRB_CTRL_IOC);

    KdDwcStartTransfer(Endpoint, &Dwc.Dma->Ep0Trb, NULL);
    Dwc.Ep0State = Ep0Status;
}

static
ULONG
KdDwcStringDescriptor(
    _In_ UCHAR Index,
    _Out_writes_bytes_(KDDWC_EP0_BYTES) PUCHAR Buffer)
{
    PCWSTR String;
    ULONG Length = 2;

    Buffer[1] = USB_DESC_STRING;

    if (Index == 0)
    {
        /* English (United States) */
        Buffer[2] = 0x09;
        Buffer[3] = 0x04;
        Buffer[0] = 4;
        return 4;
    }

    if (Index > RTL_NUMBER_OF(KdDwcStrings))
        return 0;

    for (String = KdDwcStrings[Index - 1]; *String != UNICODE_NULL && Length + 2 <= 254; String++)
    {
        Buffer[Length++] = (UCHAR)(*String & 0xFF);
        Buffer[Length++] = (UCHAR)(*String >> 8);
    }

    Buffer[0] = (UCHAR)Length;
    return Length;
}

static
VOID
KdDwcStopDataPipes(VOID)
{
    if (Dwc.RxArmed)
        KdDwcEndTransfer(KDDWC_EP_DATA_OUT, Dwc.RxResource);
    if (Dwc.TxBusy)
        KdDwcEndTransfer(KDDWC_EP_DATA_IN, Dwc.TxResource);

    KdDwcWrite(Dwc.Core, DWC_DALEPENA, (1 << KDDWC_EP0_OUT) | (1 << KDDWC_EP0_IN));

    Dwc.RxArmed = FALSE;
    Dwc.TxBusy = FALSE;
    Dwc.Configured = FALSE;
    Dwc.Listening = FALSE;
    Dwc.Configuration = 0;
}

static
VOID
KdDwcArmReceive(VOID)
{
    KdDwcWriteTrb(&Dwc.Dma->RxTrb,
                  Dwc.Dma->RxBuffer,
                  Dwc.BulkPacket,
                  DWC_TRB_TYPE_NORMAL | DWC_TRB_CTRL_LST | DWC_TRB_CTRL_IOC | DWC_TRB_CTRL_ISP_IMI);

    Dwc.RxArmed = KdDwcStartTransfer(KDDWC_EP_DATA_OUT, &Dwc.Dma->RxTrb, &Dwc.RxResource);
}

static
BOOLEAN
KdDwcSetConfiguration(
    _In_ UCHAR Value)
{
    KdDwcStopDataPipes();
    if (Value == 0)
        return TRUE;
    if (Value != 1)
        return FALSE;

    /* Transfer resources are handed out again from the first one after the control endpoint */
    KdDwcEndpointCommand(KDDWC_EP0_OUT, DWC_DEPCMD_DEPSTARTCFG | DWC_DEPCMD_PARAM(2), 0, 0, NULL);

    if (!KdDwcConfigureEndpoint(KDDWC_EP_DATA_OUT, 2, Dwc.BulkPacket, 0, DWC_EPCFG1_XFER_COMPLETE, FALSE) ||
        !KdDwcConfigureEndpoint(KDDWC_EP_DATA_IN, 2, Dwc.BulkPacket, 1, DWC_EPCFG1_XFER_COMPLETE, FALSE) ||
        !KdDwcConfigureEndpoint(KDDWC_EP_NOTIFY_IN, 3, KDDWC_NOTIFY_PACKET, 2, 0, FALSE))
    {
        KdDwcTrace("data endpoints did not configure\n");
        return FALSE;
    }

    KdDwcModify(Dwc.Core,
                DWC_DALEPENA,
                0,
                (1 << KDDWC_EP_DATA_OUT) | (1 << KDDWC_EP_DATA_IN) | (1 << KDDWC_EP_NOTIFY_IN));

    Dwc.Configuration = Value;
    Dwc.Configured = TRUE;
    KdDwcArmReceive();
    return TRUE;
}

static
VOID
KdDwcHandleSetup(VOID)
{
    UCHAR Setup[8];
    UCHAR Reply[KDDWC_EP0_BYTES];
    UCHAR RequestType, Request;
    USHORT Value, Length;
    ULONG Size;

    KdDwcCopyFromDma(Setup, Dwc.Dma->Setup, sizeof(Setup));
    RequestType = Setup[0];
    Request = Setup[1];
    Value = (USHORT)(Setup[2] | (Setup[3] << 8));
    Length = (USHORT)(Setup[6] | (Setup[7] << 8));
    KdDwcTraceUsb("setup %02x %02x value %04x length %u\n", RequestType, Request, Value, Length);

    if ((RequestType & USB_TYPE_MASK) == USB_TYPE_CLASS)
    {
        switch (Request)
        {
            case CDC_REQ_SET_LINE_CODING:
                if (Length == 0)
                    break;
                KdDwcControlOut(Request);
                return;

            case CDC_REQ_GET_LINE_CODING:
                KdDwcControlIn(Dwc.LineCoding, sizeof(Dwc.LineCoding), Length);
                return;

            case CDC_REQ_SET_CONTROL_LINE_STATE:
                /* DTR says a terminal has the port open and reads what is sent */
                Dwc.Listening = (Value & 1) != 0;
                KdDwcControlNoData();
                return;

            case CDC_REQ_SEND_BREAK:
                KdDwcControlNoData();
                return;
        }

        KdDwcStallControl();
        return;
    }

    if ((RequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD)
    {
        KdDwcStallControl();
        return;
    }

    switch (Request)
    {
        case USB_REQ_GET_STATUS:
            Reply[0] = 0;
            Reply[1] = 0;
            KdDwcControlIn(Reply, 2, Length);
            return;

        case USB_REQ_CLEAR_FEATURE:
        case USB_REQ_SET_FEATURE:
            KdDwcControlNoData();
            return;

        case USB_REQ_SET_ADDRESS:
            /* The address applies right away, the status stage still goes out on address 0 */
            KdDwcModify(Dwc.Core, DWC_DCFG, DWC_DCFG_ADDRESS_MASK, DWC_DCFG_ADDRESS(Value & 0x7F));
            KdDwcControlNoData();
            return;

        case USB_REQ_GET_DESCRIPTOR:
            switch (Value >> 8)
            {
                case USB_DESC_DEVICE:
                    KdDwcControlIn(KdDwcDeviceDescriptor, sizeof(KdDwcDeviceDescriptor), Length);
                    return;

                case USB_DESC_CONFIGURATION:
                    KdDwcConfigDescriptor[KDDWC_CONFIG_OUT_MPS] = (UCHAR)(Dwc.BulkPacket & 0xFF);
                    KdDwcConfigDescriptor[KDDWC_CONFIG_OUT_MPS + 1] = (UCHAR)(Dwc.BulkPacket >> 8);
                    KdDwcConfigDescriptor[KDDWC_CONFIG_IN_MPS] = (UCHAR)(Dwc.BulkPacket & 0xFF);
                    KdDwcConfigDescriptor[KDDWC_CONFIG_IN_MPS + 1] = (UCHAR)(Dwc.BulkPacket >> 8);
                    KdDwcControlIn(KdDwcConfigDescriptor, sizeof(KdDwcConfigDescriptor), Length);
                    return;

                case USB_DESC_STRING:
                    Size = KdDwcStringDescriptor((UCHAR)(Value & 0xFF), Reply);
                    if (Size == 0)
                        break;
                    KdDwcControlIn(Reply, Size, Length);
                    return;

                case USB_DESC_QUALIFIER:
                    KdDwcControlIn(KdDwcQualifierDescriptor, sizeof(KdDwcQualifierDescriptor), Length);
                    return;
            }
            break;

        case USB_REQ_GET_CONFIGURATION:
            Reply[0] = Dwc.Configuration;
            KdDwcControlIn(Reply, 1, Length);
            return;

        case USB_REQ_SET_CONFIGURATION:
            if (!KdDwcSetConfiguration((UCHAR)Value))
                break;
            KdDwcControlNoData();
            return;

        case USB_REQ_GET_INTERFACE:
            Reply[0] = 0;
            KdDwcControlIn(Reply, 1, Length);
            return;

        case USB_REQ_SET_INTERFACE:
            if (Value != 0)
                break;
            KdDwcControlNoData();
            return;
    }

    KdDwcStallControl();
}

static
VOID
KdDwcControlEvent(
    _In_ ULONG Endpoint,
    _In_ ULONG Type,
    _In_ ULONG Status)
{
    if (Type == DWC_ENDPOINT_EVENT_NOT_READY)
    {
        if (Status != DWC_NOT_READY_STATUS_PHASE)
            return;

        if ((Dwc.Ep0State == Ep0WaitStatusIn && Endpoint == KDDWC_EP0_IN) ||
            (Dwc.Ep0State == Ep0WaitStatusOut && Endpoint == KDDWC_EP0_OUT))
        {
            KdDwcStartStatus(Endpoint);
        }
        return;
    }

    if (Type != DWC_ENDPOINT_EVENT_COMPLETE)
        return;

    switch (Dwc.Ep0State)
    {
        case Ep0Setup:
            KdDwcHandleSetup();
            KdDwcTraceState();
            break;

        case Ep0DataIn:
            Dwc.Ep0State = Ep0WaitStatusOut;
            break;

        case Ep0DataOut:
            if (Dwc.PendingRequest == CDC_REQ_SET_LINE_CODING)
                KdDwcCopyFromDma(Dwc.LineCoding, Dwc.Dma->Ep0Buffer, sizeof(Dwc.LineCoding));
            Dwc.Ep0State = Ep0WaitStatusIn;
            break;

        case Ep0Status:
            KdDwcArmSetup();
            break;

        default:
            break;
    }
}

/* DATA PIPES *****************************************************************/

static
VOID
KdDwcReceiveComplete(VOID)
{
    ULONG Remaining;
    ULONG Received;
    ULONG i;
    ULONG Next;

    Remaining = READ_REGISTER_ULONG(&Dwc.Dma->RxTrb.Size) & DWC_TRB_SIZE_MASK;
    Received = (Remaining < Dwc.BulkPacket) ? Dwc.BulkPacket - Remaining : 0;

    for (i = 0; i < Received; i++)
    {
        Next = (Dwc.InputHead + 1) % KDDWC_INPUT_BYTES;
        if (Next == Dwc.InputTail)
            break;

        Dwc.Input[Dwc.InputHead] = READ_REGISTER_UCHAR(&Dwc.Dma->RxBuffer[i]);
        Dwc.InputHead = Next;
    }

    Dwc.RxArmed = FALSE;
    if (Dwc.Configured)
        KdDwcArmReceive();
}

static
VOID
KdDwcHandleEvent(
    _In_ ULONG Event)
{
    ULONG Endpoint;
    ULONG Speed;

    KdDwcTraceUsb("event 0x%08lx\n", Event);
    if (Event & DWC_EVENT_IS_DEVICE)
    {
        switch (DWC_EVENT_DEVICE_TYPE(Event))
        {
            case DWC_DEVICE_EVENT_DISCONNECT:
                KdDwcStopDataPipes();
                break;

            case DWC_DEVICE_EVENT_RESET:
                KdDwcStopDataPipes();
                KdDwcModify(Dwc.Core, DWC_DCFG, DWC_DCFG_ADDRESS_MASK, 0);
                if (Dwc.Ep0State != Ep0Setup)
                {
                    /* The control endpoints hold the first two transfer resources */
                    KdDwcEndTransfer(KDDWC_EP0_OUT, 0);
                    KdDwcEndTransfer(KDDWC_EP0_IN, 1);
                    KdDwcArmSetup();
                }
                break;

            case DWC_DEVICE_EVENT_CONNECT_DONE:
                Speed = KdDwcRead(Dwc.Core, DWC_DSTS) & DWC_DSTS_SPEED_MASK;
                Dwc.BulkPacket = (Speed == DWC_DSTS_SPEED_FULL) ? KDDWC_BULK_PACKET_FULL : KDDWC_BULK_PACKET_HIGH;
                KdDwcConfigureEndpoint(KDDWC_EP0_OUT, 0, KDDWC_EP0_PACKET, 0,
                                       DWC_EPCFG1_XFER_COMPLETE | DWC_EPCFG1_XFER_NOT_READY, TRUE);
                KdDwcConfigureEndpoint(KDDWC_EP0_IN, 0, KDDWC_EP0_PACKET, 0,
                                       DWC_EPCFG1_XFER_COMPLETE | DWC_EPCFG1_XFER_NOT_READY, TRUE);
                break;
        }
        return;
    }

    Endpoint = DWC_EVENT_ENDPOINT(Event);
    switch (Endpoint)
    {
        case KDDWC_EP0_OUT:
        case KDDWC_EP0_IN:
            KdDwcControlEvent(Endpoint, DWC_EVENT_ENDPOINT_TYPE(Event), DWC_EVENT_ENDPOINT_STATUS(Event));
            break;

        case KDDWC_EP_DATA_OUT:
            if (DWC_EVENT_ENDPOINT_TYPE(Event) == DWC_ENDPOINT_EVENT_COMPLETE)
                KdDwcReceiveComplete();
            break;

        case KDDWC_EP_DATA_IN:
            if (DWC_EVENT_ENDPOINT_TYPE(Event) == DWC_ENDPOINT_EVENT_COMPLETE)
                Dwc.TxBusy = FALSE;
            break;
    }
}

/* EXPORTED TO THE TRANSPORT **************************************************/

static
BOOLEAN
KdDwcCheckAlive(VOID);

/**
 * @brief Handles every event the controller has posted since the last call.
 */
VOID
NTAPI
KdDwcService(VOID)
{
    ULONG Count;
    ULONG Event;

    if (!KdDwcCheckAlive())
        return;

    Count = KdDwcRead(Dwc.Core, DWC_GEVNTCOUNT) & DWC_GEVNTCOUNT_MASK;
    while (Count != 0)
    {
        Event = READ_REGISTER_ULONG(&Dwc.Dma->Events[Dwc.EventOffset / sizeof(ULONG)]);
        Dwc.EventOffset = (Dwc.EventOffset + sizeof(ULONG)) % KDDWC_EVENT_BYTES;

        /* Hand the entry back before acting, so a long handler cannot overflow the buffer */
        KdDwcWrite(Dwc.Core, DWC_GEVNTCOUNT, sizeof(ULONG));
        Count -= sizeof(ULONG);

        KdDwcHandleEvent(Event);
    }
}

/**
 * @brief Prints the controller and control endpoint state on the loader screen.
 */
VOID
NTAPI
KdDwcTraceState(VOID)
{
    if (!Dwc.Up)
        return;

    KdDwcTraceUsb("state %lu, DSTS 0x%lx count 0x%lx EP0 commands 0x%lx 0x%lx TRB control 0x%lx\n",
               (ULONG)Dwc.Ep0State,
               KdDwcRead(Dwc.Core, DWC_DSTS),
               KdDwcRead(Dwc.Core, DWC_GEVNTCOUNT),
               KdDwcRead(Dwc.Core, DWC_DEPCMD(KDDWC_EP0_OUT)),
               KdDwcRead(Dwc.Core, DWC_DEPCMD(KDDWC_EP0_IN)),
               READ_REGISTER_ULONG(&Dwc.Dma->Ep0Trb.Control));
}

BOOLEAN
NTAPI
KdDwcIsConfigured(VOID)
{
    return Dwc.Configured;
}

BOOLEAN
NTAPI
KdDwcIsHostListening(VOID)
{
    return Dwc.Configured && Dwc.Listening;
}

BOOLEAN
NTAPI
KdDwcIsSending(VOID)
{
    return Dwc.Up && Dwc.TxBusy;
}

/**
 * @brief Starts sending up to one buffer of data to the host.
 *
 * A transfer that ends on a packet boundary would need a zero length packet
 * after it, so the last byte is held back for the next transfer instead.
 *
 * @return The number of bytes taken, 0 when the pipe is busy or not configured.
 */
ULONG
NTAPI
KdDwcSend(
    _In_reads_bytes_(Length) const UCHAR* Buffer,
    _In_ ULONG Length)
{
    if (!Dwc.Configured || Dwc.TxBusy || Length == 0)
        return 0;

    Length = min(Length, (ULONG)KDDWC_TX_BYTES);
    if (Length > 1 && (Length % Dwc.BulkPacket) == 0)
        Length--;

    KdDwcCopyToDma(Dwc.Dma->TxBuffer, Buffer, Length);
    KdDwcWriteTrb(&Dwc.Dma->TxTrb,
                  Dwc.Dma->TxBuffer,
                  Length,
                  DWC_TRB_TYPE_NORMAL | DWC_TRB_CTRL_LST | DWC_TRB_CTRL_IOC);

    Dwc.TxBusy = KdDwcStartTransfer(KDDWC_EP_DATA_IN, &Dwc.Dma->TxTrb, &Dwc.TxResource);
    return Dwc.TxBusy ? Length : 0;
}

BOOLEAN
NTAPI
KdDwcTakeInput(
    _Out_ PUCHAR Byte)
{
    if (Dwc.InputTail == Dwc.InputHead)
        return FALSE;

    *Byte = Dwc.Input[Dwc.InputTail];
    Dwc.InputTail = (Dwc.InputTail + 1) % KDDWC_INPUT_BYTES;
    return TRUE;
}

/* BRING-UP *******************************************************************/

/* The core has no SuperSpeed PHY here, so its pipe side runs from the UTMI clock. Done before any core access */
static
VOID
KdDwcSelectUtmiClock(VOID)
{
    KdDwcTrace("glue GENERAL_CFG 0x%lx\n", KdDwcRead(Dwc.Glue, QSCR_GENERAL_CFG));
    KdDwcModify(Dwc.Glue, QSCR_GENERAL_CFG, 0, QSCR_GENERAL_CFG_PIPE_CLK_GATE);
    KdDwcRead(Dwc.Glue, QSCR_GENERAL_CFG);
    KdDwcStall(100);
    KdDwcModify(Dwc.Glue, QSCR_GENERAL_CFG, 0, QSCR_GENERAL_CFG_UTMI_AS_PIPE | QSCR_GENERAL_CFG_PHYSTATUS_SW);
    KdDwcStall(100);
    KdDwcModify(Dwc.Glue, QSCR_GENERAL_CFG, QSCR_GENERAL_CFG_PIPE_CLK_GATE, 0);
}

/* The core may not suspend its PHYs or let the USB 2 one sleep while the PHY comes up */
static
VOID
KdDwcHoldPhysAwake(VOID)
{
    KdDwcModify(Dwc.Core, DWC_GUSB3PIPECTL, DWC_GUSB3PIPECTL_UX_EXIT_PX | DWC_GUSB3PIPECTL_SUSPHY, 0);
    KdDwcModify(Dwc.Core, DWC_GUSB2PHYCFG, DWC_GUSB2PHYCFG_SUSPEND, 0);
}

/**
 * @brief Resets the device logic and sets the core up for a high speed device, in the core driver's order.
 *
 * @param[in] Id
 * The core's GSNPSID, some settings depend on its revision.
 */
static
BOOLEAN
KdDwcResetCore(
    _In_ ULONG Id)
{
    ULONG Revision = DWC_GSNPSID_REVISION(Id);
    BOOLEAN Usb3 = DWC_GSNPSID_IS_USB3(Id);
    ULONG Control;
    ULONG Settings;

    /* The PIPE side has nothing behind it, only the USB 2 PHY is kept from suspending */
    KdDwcModify(Dwc.Core, DWC_GUSB3PIPECTL, 0, DWC_GUSB3PIPECTL_SUSPHY);

    KdDwcTrace("device soft reset, DCTL 0x%lx\n", KdDwcRead(Dwc.Core, DWC_DCTL));
    KdDwcModify(Dwc.Core, DWC_DCTL, DWC_DCTL_RUN_STOP, DWC_DCTL_CSFTRST);
    if (!KdDwcWaitFor(Dwc.Core, DWC_DCTL, DWC_DCTL_CSFTRST, 0, KDDWC_RESET_TIMEOUT_US))
    {
        KdDwcTrace("device soft reset did not finish\n");
        return FALSE;
    }

    /* Full speed timing and scrambling, clock gating where the core was built with it */
    Control = KdDwcRead(Dwc.Core, DWC_GCTL) & ~(DWC_GCTL_SCALEDOWN_MASK | DWC_GCTL_DISSCRAMBLE);
    if (DWC_GHWPARAMS1_PWROPT(KdDwcRead(Dwc.Core, DWC_GHWPARAMS1)) == DWC_GHWPARAMS1_PWROPT_CLOCK)
        Control &= ~DWC_GCTL_DSBLCLKGTNG;
    KdDwcWrite(Dwc.Core, DWC_GCTL, Control);

    if (Usb3 && Revision >= DWC_REVISION_310A)
        KdDwcModify(Dwc.Core, DWC_GUCTL2, 0, DWC_GUCTL2_RST_ACTBITLATER);

    /* At high speed only the device logic runs from the UTMI clock instead of a pipe clock */
    Settings = DWC_GUCTL1_PARKMODE_DISABLE_SS;
    if (Usb3 && Revision >= DWC_REVISION_290A)
        Settings |= DWC_GUCTL1_DEV_L1_EXIT_BY_HW | DWC_GUCTL1_DEV_FORCE_20_CLK;
    if (Usb3 && Revision >= DWC_REVISION_300A)
        Settings |= DWC_GUCTL1_DEV_DECOUPLE_L1L2;
    KdDwcModify(Dwc.Core, DWC_GUCTL1, 0, Settings);

    /* A change of role happens with the PHYs held awake */
    if ((KdDwcRead(Dwc.Core, DWC_GCTL) & DWC_GCTL_PRTCAPDIR_MASK) != DWC_GCTL_PRTCAPDIR_DEVICE)
        KdDwcHoldPhysAwake();
    KdDwcModify(Dwc.Core, DWC_GCTL, DWC_GCTL_PRTCAPDIR_MASK, DWC_GCTL_PRTCAPDIR_DEVICE);

    KdDwcTrace("core reset, GCTL 0x%lx GUCTL1 0x%lx GUSB2PHYCFG 0x%lx GUSB3PIPECTL 0x%lx\n",
               KdDwcRead(Dwc.Core, DWC_GCTL),
               KdDwcRead(Dwc.Core, DWC_GUCTL1),
               KdDwcRead(Dwc.Core, DWC_GUSB2PHYCFG),
               KdDwcRead(Dwc.Core, DWC_GUSB3PIPECTL));
    return TRUE;
}

/* Nothing reports VBUS to the core in device mode, so the glue says it is there */
static
VOID
KdDwcOverrideVbus(VOID)
{
    KdDwcModify(Dwc.Glue, QSCR_SS_PHY_CTRL, 0, QSCR_SS_PHY_CTRL_LANE0_PWR);
    KdDwcModify(Dwc.Glue, QSCR_HS_PHY_CTRL, 0, QSCR_HS_PHY_CTRL_VBUS_VALID | QSCR_HS_PHY_CTRL_SESSVLD_SEL);
}

/* How many packets the device may have in flight, from the size of its receive RAM */
static
ULONG
KdDwcPacketsInFlight(VOID)
{
    ULONG Depth = DWC_GHWPARAMS7_RAM2_DEPTH(KdDwcRead(Dwc.Core, DWC_GHWPARAMS7));
    ULONG Width = DWC_GHWPARAMS0_MDWIDTH(KdDwcRead(Dwc.Core, DWC_GHWPARAMS0));
    ULONG Bytes = Depth * Width / 8;

    /* Room the control endpoint and the header area keep for themselves */
    if (Bytes <= 4 * 2 + 3 * (3 + 1))
        return 1;

    return min((Bytes - 4 * 2 - 3 * (3 + 1)) / 16, 16UL);
}

/**
 * @brief Starts the device side and connects to the host, in the gadget driver's order.
 *
 * @param[in] Id
 * The core's GSNPSID.
 */
static
BOOLEAN
KdDwcStartDevice(
    _In_ ULONG Id)
{
    ULONG Count;
    ULONG Config;

    if (DWC_GSNPSID_IS_USB3(Id) && DWC_GSNPSID_REVISION(Id) >= DWC_REVISION_300A)
        KdDwcWrite(Dwc.Core, DWC_DEV_IMOD, 0);
    KdDwcModify(Dwc.Core, DWC_GRXTHRCFG, DWC_GRXTHRCFG_PKTCNTSEL, 0);

    /* One event buffer, masked: everything is polled */
    KdDwcWrite(Dwc.Core, DWC_GEVNTADRLO, (ULONG)KdDwcPhysical(Dwc.Dma->Events));
    KdDwcWrite(Dwc.Core, DWC_GEVNTADRHI, (ULONG)(KdDwcPhysical(Dwc.Dma->Events) >> 32));
    KdDwcWrite(Dwc.Core, DWC_GEVNTSIZ, DWC_GEVNTSIZ_INTMASK | KDDWC_EVENT_BYTES);
    Count = KdDwcRead(Dwc.Core, DWC_GEVNTCOUNT) & DWC_GEVNTCOUNT_MASK;
    if (Count != 0)
        KdDwcWrite(Dwc.Core, DWC_GEVNTCOUNT, Count);
    Dwc.EventOffset = 0;

    Config = KdDwcRead(Dwc.Core, DWC_DCFG);
    Config &= ~(DWC_DCFG_SPEED_MASK | DWC_DCFG_ADDRESS_MASK | DWC_DCFG_NUMP_MASK);
    Config |= DWC_DCFG_SPEED_HIGH | DWC_DCFG_IGNSTRMPP | (KdDwcPacketsInFlight() << DWC_DCFG_NUMP_SHIFT);
    KdDwcWrite(Dwc.Core, DWC_DCFG, Config);

    if (!KdDwcEndpointCommand(KDDWC_EP0_OUT, DWC_DEPCMD_DEPSTARTCFG, 0, 0, NULL) ||
        !KdDwcConfigureEndpoint(KDDWC_EP0_OUT, 0, KDDWC_EP0_PACKET, 0,
                                DWC_EPCFG1_XFER_COMPLETE | DWC_EPCFG1_XFER_NOT_READY, FALSE) ||
        !KdDwcConfigureEndpoint(KDDWC_EP0_IN, 0, KDDWC_EP0_PACKET, 0,
                                DWC_EPCFG1_XFER_COMPLETE | DWC_EPCFG1_XFER_NOT_READY, FALSE))
    {
        KdDwcTrace("control endpoint did not configure\n");
        return FALSE;
    }

    KdDwcWrite(Dwc.Core, DWC_DALEPENA, (1 << KDDWC_EP0_OUT) | (1 << KDDWC_EP0_IN));
    KdDwcArmSetup();

    KdDwcWrite(Dwc.Core,
               DWC_DEVTEN,
               DWC_DEVTEN_DISCONNECT | DWC_DEVTEN_RESET | DWC_DEVTEN_CONNECT_DONE | DWC_DEVTEN_WAKEUP |
               DWC_DEVTEN_SUSPEND | DWC_DEVTEN_ERRATIC | DWC_DEVTEN_COMMAND_DONE | DWC_DEVTEN_OVERFLOW);
    KdDwcModify(Dwc.Core, DWC_GUSB3PIPECTL, 0, DWC_GUSB3PIPECTL_SUSPHY);

    /* Run, with the USB 2 PHY awake and VBUS reported again right before */
    KdDwcTrace("control endpoint armed, setting run\n");
    KdDwcModify(Dwc.Core, DWC_GUSB2PHYCFG, DWC_GUSB2PHYCFG_SUSPEND, 0);
    KdDwcOverrideVbus();
    KdDwcModify(Dwc.Core, DWC_DCTL, DWC_DCTL_KEEP_CONNECT, DWC_DCTL_RUN_STOP);
    if (!KdDwcWaitFor(Dwc.Core, DWC_DSTS, DWC_DSTS_DEVCTRLHLT, 0, KDDWC_RESET_TIMEOUT_US))
    {
        KdDwcTrace("device controller did not start\n");
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief Powers the controller, takes it from firmware and connects to the host as a serial port.
 *
 * @param[in] LoaderBlock
 * The loader block, where the DMA block comes from.
 *
 * @param[in] Platform
 * The SoC, which knows how to power the controller and its PHY.
 *
 * @param[in] CoreBase
 * Physical base of the DWC3 controller.
 */
/* Clears what the host set up, a new bring up starts from an unconfigured device */
static
VOID
KdDwcForgetHost(VOID)
{
    Dwc.EventOffset = 0;
    Dwc.Ep0State = Ep0Setup;
    Dwc.Ep0ThreeStage = FALSE;
    Dwc.BulkPacket = KDDWC_BULK_PACKET_HIGH;
    Dwc.Configuration = 0;
    Dwc.Configured = FALSE;
    Dwc.Listening = FALSE;
    Dwc.RxArmed = FALSE;
    Dwc.TxBusy = FALSE;
}

/**
 * @brief Votes, powers and starts the controller as a USB serial device. Runs again after a power loss.
 */
static
NTSTATUS
KdDwcBringUp(VOID)
{
    const KDDWC_PLATFORM* Platform = Dwc.Platform;
    ULONG Id;

    Dwc.Up = FALSE;
    KdDwcForgetHost();

    /* Firmware dropped the controller's bus and supply votes at exit, DMA stalls without them */
    if (Platform->Rpmh != NULL && !KdDwcVoteRpmh(Platform->Rpmh))
        KdDwcTrace("some RPMh votes did not go through\n");

    /* Without its clocks the core would stall the bus on the first access */
    if (!Platform->PowerUp(Platform))
        return STATUS_DEVICE_NOT_READY;

    KdDwcSelectUtmiClock();

    KdDwcTrace("power up done, reading the core id\n");
    Id = KdDwcRead(Dwc.Core, DWC_GSNPSID);
    KdDwcTrace("core id 0x%lx\n", Id);
    if ((Id & 0xFFFF0000) != 0x55330000 && (Id & 0xFFFF0000) != 0x33310000)
        return STATUS_DEVICE_DOES_NOT_EXIST;

    /* The PHY comes up with the core holding it awake, as the core driver does it */
    KdDwcHoldPhysAwake();
    if (Platform->StartPhy != NULL && !Platform->StartPhy(Platform))
        return STATUS_DEVICE_NOT_READY;

    if (!KdDwcResetCore(Id) || !KdDwcStartDevice(Id))
        return STATUS_DEVICE_NOT_READY;

    Dwc.Up = TRUE;
    Dwc.NextCheck = KdDwcMicroseconds() + KDDWC_CHECK_US;
    KdDwcTrace("device controller running\n");
    return STATUS_SUCCESS;
}

/**
 * @brief Takes the controller from firmware and connects to the host as a serial port.
 *
 * @param[in] LoaderBlock
 * The loader block, where the DMA block comes from.
 *
 * @param[in] Platform
 * The SoC, which knows how to power the controller and its PHY.
 *
 * @param[in] CoreBase
 * Physical base of the DWC3 controller.
 */
NTSTATUS
NTAPI
KdDwcStartController(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ const KDDWC_PLATFORM* Platform,
    _In_ ULONG64 CoreBase)
{
    PHYSICAL_ADDRESS DmaAddress;
    PFN_COUNT DmaPages = BYTES_TO_PAGES(sizeof(*Dwc.Dma));
    NTSTATUS Status;

    RtlZeroMemory(&Dwc, sizeof(Dwc));
    Dwc.Platform = Platform;

    /* 115200 8N1 until the host says otherwise */
    Dwc.LineCoding[0] = 0x00;
    Dwc.LineCoding[1] = 0xC2;
    Dwc.LineCoding[2] = 0x01;
    Dwc.LineCoding[3] = 0x00;
    Dwc.LineCoding[6] = 8;

    DmaAddress.QuadPart = KdDwcTakePages(LoaderBlock, DmaPages);
    if (DmaAddress.QuadPart == 0)
    {
        KdDwcTrace("no memory for the DMA block\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Dwc.Dma = KdMapPhysicalMemory64(DmaAddress, DmaPages);
    Dwc.DmaAddress = DmaAddress.QuadPart;
    Dwc.Core = KdDwcMapRegister(CoreBase + KDDWC_CORE_PAGE_OFFSET);
    Dwc.Glue = KdDwcMapRegister(CoreBase + KDDWC_GLUE_PAGE_OFFSET);
    if (Dwc.Dma == NULL || Dwc.Core == NULL || Dwc.Glue == NULL)
    {
        Dwc.Core = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* The register pages start at a page boundary, the glue sits 0x800 into its page */
    Dwc.Core -= (CoreBase + KDDWC_CORE_PAGE_OFFSET) & (PAGE_SIZE - 1);
    Dwc.Glue -= (CoreBase + KDDWC_GLUE_PAGE_OFFSET) & (PAGE_SIZE - 1);

    KdDwcTrace("DMA PA 0x%I64x VA %p, core %p glue %p\n", Dwc.DmaAddress, Dwc.Dma, Dwc.Core, Dwc.Glue);
    KdDwcTrace("%s at 0x%I64x\n", Platform->Name, CoreBase);

    /* A controller that is there but not ready yet is retried from the poll, a missing one is not */
    Status = KdDwcBringUp();
    if (Status == STATUS_DEVICE_NOT_READY)
    {
        KdDwcTrace("controller not ready, retrying later\n");
        Dwc.NextRevival = KdDwcMicroseconds() + KDDWC_REVIVE_US;
        return STATUS_SUCCESS;
    }

    return Status;
}

/**
 * @brief Forgets the controller without touching it, its power is going away.
 */
VOID
NTAPI
KdDwcPowerDown(VOID)
{
    if (Dwc.Platform == NULL)
        return;

    if (Dwc.Up)
        Dwc.DownSince = KdDwcMicroseconds();

    Dwc.Up = FALSE;
    KdDwcForgetHost();
    Dwc.NextRevival = KdDwcMicroseconds() + KDDWC_REVIVE_US;
}

/**
 * @brief Brings the controller back right away, its power has returned.
 */
NTSTATUS
NTAPI
KdDwcPowerUp(VOID)
{
    if (Dwc.Platform == NULL)
        return STATUS_DEVICE_NOT_READY;

    if (Dwc.Up)
        return STATUS_SUCCESS;

    return KdDwcBringUp();
}

/* TRUE while the core still holds the device setup this driver gave it */
static
BOOLEAN
KdDwcCoreIsOurs(VOID)
{
    if ((KdDwcRead(Dwc.Core, DWC_GCTL) & DWC_GCTL_PRTCAPDIR_MASK) != DWC_GCTL_PRTCAPDIR_DEVICE)
        return FALSE;
    if (!(KdDwcRead(Dwc.Core, DWC_DCTL) & DWC_DCTL_RUN_STOP))
        return FALSE;

    return KdDwcRead(Dwc.Core, DWC_GEVNTADRLO) == (ULONG)KdDwcPhysical(Dwc.Dma->Events);
}

/*
 * Decides whether the controller may be touched. Power and clocks are read
 * from the clock controller, which always answers; the core is only read once
 * they are known to be on.
 */
static
BOOLEAN
KdDwcCheckAlive(VOID)
{
    const KDDWC_PLATFORM* Platform = Dwc.Platform;
    ULONG64 Now;

    if (Platform == NULL || Dwc.Core == NULL)
        return FALSE;

    Now = KdDwcMicroseconds();
    if (!Dwc.Up)
    {
        if (Now < Dwc.NextRevival)
            return FALSE;

        Dwc.NextRevival = Now + KDDWC_REVIVE_US;

        /* Whoever took the power gets a while to give it back before the debugger takes it again */
        if (Platform->IsPowered != NULL && !Platform->IsPowered(Platform) && Now - Dwc.DownSince < KDDWC_RECLAIM_US)
            return FALSE;

        return NT_SUCCESS(KdDwcBringUp());
    }

    if (Platform->IsPowered != NULL && !Platform->IsPowered(Platform))
    {
        KdDwcPowerDown();
        return FALSE;
    }

    if (Now >= Dwc.NextCheck)
    {
        Dwc.NextCheck = Now + KDDWC_CHECK_US;
        if (!KdDwcCoreIsOurs())
            return NT_SUCCESS(KdDwcBringUp());
    }

    return TRUE;
}
