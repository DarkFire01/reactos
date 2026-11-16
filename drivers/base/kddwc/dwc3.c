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
} KDDWC_CONTROLLER;

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

/* Maps the page that holds a physical register and returns the register's address */
PUCHAR
NTAPI
KdDwcMapRegister(
    _In_ ULONG64 PhysicalAddress)
{
    PHYSICAL_ADDRESS Page;
    PUCHAR Base;

    Page.QuadPart = PhysicalAddress & ~(ULONG64)(PAGE_SIZE - 1);
    Base = KdMapPhysicalMemory64(Page, 1);
    if (Base == NULL)
        return NULL;

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

    KdDwcWrite(Dwc.Core, DWC_DEPCMDPAR2(Endpoint), 0);
    KdDwcWrite(Dwc.Core, DWC_DEPCMDPAR1(Endpoint), Parameter1);
    KdDwcWrite(Dwc.Core, DWC_DEPCMDPAR0(Endpoint), Parameter0);
    KdDwcWrite(Dwc.Core, DWC_DEPCMD(Endpoint), Command | DWC_DEPCMD_CMDACT);

    if (!KdDwcWaitFor(Dwc.Core, DWC_DEPCMD(Endpoint), DWC_DEPCMD_CMDACT, 0, KDDWC_COMMAND_TIMEOUT_US))
    {
        KdDwcTrace("endpoint %lu command 0x%lx timed out\n", Endpoint, Command);
        return FALSE;
    }

    Value = KdDwcRead(Dwc.Core, DWC_DEPCMD(Endpoint));
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

static
VOID
KdDwcEndTransfer(
    _In_ ULONG Endpoint,
    _In_ UCHAR Resource)
{
    KdDwcEndpointCommand(Endpoint,
                         DWC_DEPCMD_ENDTRANSFER | DWC_DEPCMD_FORCERM | DWC_DEPCMD_PARAM(Resource),
                         0,
                         0,
                         NULL);
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
    KdDwcCopyToDma(Dwc.Dma->Ep0Buffer, Data, Length);

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
                    KdDwcEndTransfer(KDDWC_EP0_OUT, 0);
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

/**
 * @brief Handles every event the controller has posted since the last call.
 */
VOID
NTAPI
KdDwcService(VOID)
{
    ULONG Count;
    ULONG Event;

    if (Dwc.Core == NULL)
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
    return Dwc.TxBusy;
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

/* Resets the core onto the high speed PHY alone, with UTMI standing in for the pipe */
static
BOOLEAN
KdDwcResetCore(
    _In_ BOOLEAN KeepPhy)
{
    KdDwcModify(Dwc.Core, DWC_GCTL, 0, DWC_GCTL_CORESOFTRESET);
    if (!KeepPhy)
    {
        KdDwcModify(Dwc.Core, DWC_GUSB2PHYCFG, 0, DWC_GUSB2PHYCFG_PHYSOFTRST);
        KdDwcStall(100);
        KdDwcModify(Dwc.Core, DWC_GUSB2PHYCFG, DWC_GUSB2PHYCFG_PHYSOFTRST, 0);
    }

    KdDwcModify(Dwc.Glue, QSCR_GENERAL_CFG, 0, QSCR_GENERAL_CFG_PIPE_CLK_GATE);
    KdDwcStall(100);
    KdDwcModify(Dwc.Glue, QSCR_GENERAL_CFG, 0, QSCR_GENERAL_CFG_UTMI_AS_PIPE | QSCR_GENERAL_CFG_PHYSTATUS_SW);
    KdDwcStall(100);
    KdDwcModify(Dwc.Glue, QSCR_GENERAL_CFG, QSCR_GENERAL_CFG_PIPE_CLK_GATE, 0);

    KdDwcModify(Dwc.Core, DWC_GCTL, DWC_GCTL_CORESOFTRESET, 0);
    KdDwcStall(100);

    /* The device logic gets its own reset after the role is set */
    KdDwcModify(Dwc.Core, DWC_GCTL, DWC_GCTL_PRTCAPDIR_MASK, DWC_GCTL_PRTCAPDIR_DEVICE);
    KdDwcModify(Dwc.Core, DWC_DCTL, DWC_DCTL_RUN_STOP, DWC_DCTL_CSFTRST);
    if (!KdDwcWaitFor(Dwc.Core, DWC_DCTL, DWC_DCTL_CSFTRST, 0, KDDWC_RESET_TIMEOUT_US))
    {
        KdDwcTrace("device soft reset did not finish\n");
        return FALSE;
    }

    /* This PHY misbehaves when the core suspends it or lets it sleep between transfers */
    KdDwcModify(Dwc.Core, DWC_GUSB2PHYCFG, DWC_GUSB2PHYCFG_SUSPEND, 0);
    KdDwcModify(Dwc.Core, DWC_GUCTL1, 0, DWC_GUCTL1_PARKMODE_DISABLE_SS);
    KdDwcModify(Dwc.Core, DWC_GCTL, DWC_GCTL_PRTCAPDIR_MASK, DWC_GCTL_PRTCAPDIR_DEVICE);

    /* Nothing reports VBUS to the core in device mode, so the glue says it is there */
    KdDwcModify(Dwc.Glue, QSCR_HS_PHY_CTRL, 0, QSCR_HS_PHY_CTRL_VBUS_VALID | QSCR_HS_PHY_CTRL_SESSVLD_SEL);
    KdDwcModify(Dwc.Glue, QSCR_SS_PHY_CTRL, 0, QSCR_SS_PHY_CTRL_LANE0_PWR);
    return TRUE;
}

static
BOOLEAN
KdDwcStartDevice(VOID)
{
    ULONG Count;

    /* One event buffer, masked: everything is polled */
    KdDwcWrite(Dwc.Core, DWC_GEVNTADRLO, (ULONG)KdDwcPhysical(Dwc.Dma->Events));
    KdDwcWrite(Dwc.Core, DWC_GEVNTADRHI, (ULONG)(KdDwcPhysical(Dwc.Dma->Events) >> 32));
    KdDwcWrite(Dwc.Core, DWC_GEVNTSIZ, DWC_GEVNTSIZ_INTMASK | KDDWC_EVENT_BYTES);
    Count = KdDwcRead(Dwc.Core, DWC_GEVNTCOUNT) & DWC_GEVNTCOUNT_MASK;
    if (Count != 0)
        KdDwcWrite(Dwc.Core, DWC_GEVNTCOUNT, Count);
    Dwc.EventOffset = 0;

    KdDwcModify(Dwc.Core, DWC_DCFG, DWC_DCFG_SPEED_MASK | DWC_DCFG_ADDRESS_MASK, DWC_DCFG_SPEED_HIGH);
    KdDwcWrite(Dwc.Core, DWC_DEVTEN, DWC_DEVTEN_DISCONNECT | DWC_DEVTEN_RESET | DWC_DEVTEN_CONNECT_DONE);

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

    KdDwcModify(Dwc.Core, DWC_DCTL, 0, DWC_DCTL_RUN_STOP);
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
NTSTATUS
NTAPI
KdDwcStartController(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ const KDDWC_PLATFORM* Platform,
    _In_ ULONG64 CoreBase)
{
    PHYSICAL_ADDRESS DmaAddress;
    PFN_COUNT DmaPages = BYTES_TO_PAGES(sizeof(*Dwc.Dma));
    ULONG Id;

    RtlZeroMemory(&Dwc, sizeof(Dwc));
    Dwc.BulkPacket = KDDWC_BULK_PACKET_HIGH;

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

    /* Without its clocks the core would stall the bus on the first access */
    KdDwcTrace("%s at 0x%I64x\n", Platform->Name, CoreBase);
    if (!Platform->PowerUp(Platform))
    {
        Dwc.Core = NULL;
        return STATUS_DEVICE_NOT_READY;
    }

    Id = KdDwcRead(Dwc.Core, DWC_GSNPSID);
    KdDwcTrace("core id 0x%lx\n", Id);
    if ((Id & 0xFFFF0000) != 0x55330000 && (Id & 0xFFFF0000) != 0x33310000)
    {
        Dwc.Core = NULL;
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }

    if (!KdDwcResetCore((Platform->Flags & KDDWC_PLATFORM_KEEP_PHY) != 0) || !KdDwcStartDevice())
    {
        Dwc.Core = NULL;
        return STATUS_DEVICE_NOT_READY;
    }

    KdDwcTrace("device controller running\n");
    return STATUS_SUCCESS;
}
