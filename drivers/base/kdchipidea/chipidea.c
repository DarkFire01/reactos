/*
 * PROJECT:     ReactOS ChipIdea Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ChipIdea device controller driven by polling, presenting a CDC-ACM serial port
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdci.h"

#define KDCI_RESET_TIMEOUT_US           100000
#define KDCI_FLUSH_TIMEOUT_US           10000
#define KDCI_ULPI_TIMEOUT_US            10000

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

#define USB_DESC_DEVICE                 0x01
#define USB_DESC_CONFIGURATION          0x02
#define USB_DESC_STRING                 0x03
#define USB_DESC_QUALIFIER              0x06

/* Offsets of the two bulk packet sizes inside the configuration descriptor */
#define KDCI_CONFIG_OUT_MPS             65
#define KDCI_CONFIG_IN_MPS              72

/* How often a live controller is checked for still being ours, and a dead one retried */
#define KDCI_CHECK_US                   100000
#define KDCI_REVIVE_US                  2000000

/* How long power taken away by someone else is left alone before the debugger restores it */
#define KDCI_RECLAIM_US                 10000000

typedef enum _KDCI_EP0_STATE
{
    Ep0Idle,
    Ep0DataIn,
    Ep0DataOut,
    Ep0StatusIn,
    Ep0StatusOut
} KDCI_EP0_STATE;

typedef struct _KDCI_CONTROLLER
{
    PUCHAR Base;
    PUCHAR Op;
    BOOLEAN Lpm;
    PKDCI_DMA Dma;
    ULONG64 DmaAddress;

    KDCI_EP0_STATE Ep0State;
    UCHAR PendingRequest;
    USHORT BulkPacket;

    UCHAR Configuration;
    BOOLEAN Configured;
    BOOLEAN Listening;
    BOOLEAN RxArmed;
    BOOLEAN TxBusy;

    UCHAR LineCoding[7];

    UCHAR Input[KDCI_INPUT_BYTES];
    ULONG InputHead;
    ULONG InputTail;

    const KDCI_PLATFORM* Platform;
    BOOLEAN Up;
    ULONG64 DownSince;
    ULONG64 NextRevival;
    ULONG64 NextCheck;
} KDCI_CONTROLLER;

static KDCI_CONTROLLER Ci;

/* Pages taken from the loader for the DMA block, marked so memory management keeps off them */
static MEMORY_ALLOCATION_DESCRIPTOR KdCiDmaDescriptor;

/* DESCRIPTORS ****************************************************************/

/* pid.codes test identifiers, never shipped on a product */
static const UCHAR KdCiDeviceDescriptor[18] =
{
    18, USB_DESC_DEVICE, 0x00, 0x02,
    0xEF, 0x02, 0x01, KDCI_EP0_PACKET,
    0x09, 0x12, 0x01, 0x00,
    0x00, 0x01, 1, 2, 3, 1
};

static const UCHAR KdCiQualifierDescriptor[10] =
{
    10, USB_DESC_QUALIFIER, 0x00, 0x02,
    0xEF, 0x02, 0x01, KDCI_EP0_PACKET, 1, 0
};

/* One function with a communication and a data interface, grouped so Windows binds usbser */
static UCHAR KdCiConfigDescriptor[75] =
{
    9, USB_DESC_CONFIGURATION, 75, 0, 2, 1, 0, 0x80, 250,
    8, 0x0B, 0, 2, 0x02, 0x02, 0x01, 0,
    9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    5, 0x24, 0x00, 0x10, 0x01,
    5, 0x24, 0x01, 0x00, 0x01,
    4, 0x24, 0x02, 0x02,
    5, 0x24, 0x06, 0, 1,
    7, 0x05, KDCI_USB_NOTIFY_IN, 0x03, KDCI_NOTIFY_PACKET, 0, 9,
    9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    7, 0x05, KDCI_USB_DATA_OUT, 0x02, 0x00, 0x02, 0,
    7, 0x05, KDCI_USB_DATA_IN, 0x02, 0x00, 0x02, 0
};

C_ASSERT(KDCI_CONFIG_IN_MPS + 2 < sizeof(KdCiConfigDescriptor));

static const PCWSTR KdCiStrings[] =
{
    L"ReactOS",
    L"ReactOS Kernel Debugger",
    L"CI-0"
};

/* REGISTER ACCESS ************************************************************/

ULONG
NTAPI
KdCiRead(
    _In_ PUCHAR Base,
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((volatile ULONG*)(Base + Offset));
}

VOID
NTAPI
KdCiWrite(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((volatile ULONG*)(Base + Offset), Value);
}

VOID
NTAPI
KdCiModify(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Clear,
    _In_ ULONG Set)
{
    KdCiWrite(Base, Offset, (KdCiRead(Base, Offset) & ~Clear) | Set);
}

/* Waits for (register & Mask) == Value, TRUE when it got there in time */
BOOLEAN
NTAPI
KdCiWaitFor(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Mask,
    _In_ ULONG Value,
    _In_ ULONG TimeoutUs)
{
    ULONG64 Start = KdCiMicroseconds();

    while ((KdCiRead(Base, Offset) & Mask) != Value)
    {
        if (KdCiMicroseconds() - Start > TimeoutUs)
            return FALSE;
        YieldProcessor();
    }

    return TRUE;
}

#define KDCI_MAPPED_PAGES               16

static ULONG64 KdCiMappedPhysical[KDCI_MAPPED_PAGES];
static PUCHAR KdCiMappedVirtual[KDCI_MAPPED_PAGES];
static ULONG KdCiMappedCount;

/* Maps the page that holds a physical register once and returns the register's address */
PUCHAR
NTAPI
KdCiMapRegister(
    _In_ ULONG64 PhysicalAddress)
{
    PHYSICAL_ADDRESS Page;
    PUCHAR Base;
    ULONG i;

    Page.QuadPart = PhysicalAddress & ~(ULONG64)(PAGE_SIZE - 1);
    for (i = 0; i < KdCiMappedCount; i++)
    {
        if (KdCiMappedPhysical[i] == (ULONG64)Page.QuadPart)
            return KdCiMappedVirtual[i] + (PhysicalAddress & (PAGE_SIZE - 1));
    }

    if (KdCiMappedCount == KDCI_MAPPED_PAGES)
        return NULL;

    Base = KdMapPhysicalMemory64(Page, 1);
    if (Base == NULL)
        return NULL;

    KdCiMappedPhysical[KdCiMappedCount] = Page.QuadPart;
    KdCiMappedVirtual[KdCiMappedCount] = Base;
    KdCiMappedCount++;
    return Base + (PhysicalAddress & (PAGE_SIZE - 1));
}

/**
 * @brief Writes a PHY register through the controller's ULPI viewport, waking the PHY first when it sleeps.
 */
BOOLEAN
NTAPI
KdCiUlpiWrite(
    _In_ UCHAR Address,
    _In_ UCHAR Value)
{
    if (Ci.Op == NULL)
        return FALSE;

    if (!(KdCiRead(Ci.Op, CI_ULPI_VIEWPORT) & CI_ULPI_SYNC))
    {
        KdCiWrite(Ci.Op, CI_ULPI_VIEWPORT, CI_ULPI_WAKEUP);
        if (!KdCiWaitFor(Ci.Op, CI_ULPI_VIEWPORT, CI_ULPI_WAKEUP, 0, KDCI_ULPI_TIMEOUT_US))
            return FALSE;
    }

    KdCiWrite(Ci.Op, CI_ULPI_VIEWPORT, CI_ULPI_RUN | CI_ULPI_WRITE | CI_ULPI_ADDRESS(Address) | Value);
    return KdCiWaitFor(Ci.Op, CI_ULPI_VIEWPORT, CI_ULPI_RUN, 0, KDCI_ULPI_TIMEOUT_US);
}

/* DMA MEMORY *****************************************************************/

/* Takes pages for the DMA block from a free loader descriptor below 4 GB, the controller has 32 bit pointers */
static
ULONG64
KdCiTakePages(
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

        KdCiDmaDescriptor.MemoryType = LoaderFirmwarePermanent;
        KdCiDmaDescriptor.BasePage = Block->BasePage;
        KdCiDmaDescriptor.PageCount = PageCount;

        Block->BasePage += PageCount;
        Block->PageCount -= PageCount;
        InsertTailList(&Block->ListEntry, &KdCiDmaDescriptor.ListEntry);

        return (ULONG64)KdCiDmaDescriptor.BasePage << PAGE_SHIFT;
    }

    return 0;
}

static
ULONG
KdCiPhysical(
    _In_ const VOID* Pointer)
{
    return (ULONG)(Ci.DmaAddress + (ULONG64)((const UCHAR*)Pointer - (const UCHAR*)Ci.Dma));
}

/* The DMA block is device memory, so it is accessed one register at a time */
static
VOID
KdCiCopyToDma(
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
KdCiCopyFromDma(
    _Out_writes_bytes_(Length) UCHAR* Destination,
    _In_reads_bytes_(Length) volatile const UCHAR* Source,
    _In_ ULONG Length)
{
    ULONG i;

    for (i = 0; i < Length; i++)
        Destination[i] = READ_REGISTER_UCHAR((volatile UCHAR*)&Source[i]);
}

/* ENDPOINTS ******************************************************************/

/* The prime, flush and complete bit of a queue head slot */
static
ULONG
KdCiSlotBit(
    _In_ ULONG Slot)
{
    return (Slot & 1) ? CI_IN_BIT(Slot / 2) : CI_OUT_BIT(Slot / 2);
}

static
VOID
KdCiSetQueueHead(
    _In_ ULONG Slot,
    _In_ ULONG Capabilities)
{
    volatile CI_QUEUE_HEAD* Head = &Ci.Dma->QueueHeads[Slot];
    ULONG i;

    WRITE_REGISTER_ULONG(&Head->Capabilities, Capabilities);
    WRITE_REGISTER_ULONG(&Head->CurrentTd, 0);
    WRITE_REGISTER_ULONG(&Head->NextTd, CI_TD_TERMINATE);
    WRITE_REGISTER_ULONG(&Head->Token, 0);
    for (i = 0; i < RTL_NUMBER_OF(Head->Buffer); i++)
        WRITE_REGISTER_ULONG(&Head->Buffer[i], 0);
}

/**
 * @brief Hands one transfer descriptor to an endpoint and primes it.
 *
 * Every endpoint runs one transfer at a time, so the queue head overlay is
 * pointed straight at the descriptor before the prime.
 */
static
VOID
KdCiPrime(
    _In_ ULONG Slot,
    _Out_ volatile CI_TRANSFER* Transfer,
    _In_ const VOID* Buffer,
    _In_ ULONG Length)
{
    volatile CI_QUEUE_HEAD* Head = &Ci.Dma->QueueHeads[Slot];
    ULONG Address = KdCiPhysical(Buffer);
    ULONG i;

    WRITE_REGISTER_ULONG(&Transfer->NextTd, CI_TD_TERMINATE);
    WRITE_REGISTER_ULONG(&Transfer->Buffer[0], Address);
    for (i = 1; i < RTL_NUMBER_OF(Transfer->Buffer); i++)
        WRITE_REGISTER_ULONG(&Transfer->Buffer[i], (Address & ~(PAGE_SIZE - 1)) + i * PAGE_SIZE);
    WRITE_REGISTER_ULONG(&Transfer->Token, CI_TD_BYTES(Length) | CI_TD_INTERRUPT | CI_TD_ACTIVE);

    WRITE_REGISTER_ULONG(&Head->NextTd, KdCiPhysical((const VOID*)Transfer));
    WRITE_REGISTER_ULONG(&Head->Token, 0);

    KeMemoryBarrier();
    KdCiWrite(Ci.Op, CI_ENDPTPRIME(Ci.Lpm), KdCiSlotBit(Slot));
}

/* Drops whatever the given endpoints had primed */
static
VOID
KdCiFlush(
    _In_ ULONG Bits)
{
    KdCiWrite(Ci.Op, CI_ENDPTFLUSH(Ci.Lpm), Bits);
    KdCiWaitFor(Ci.Op, CI_ENDPTFLUSH(Ci.Lpm), Bits, 0, KDCI_FLUSH_TIMEOUT_US);
}

/* CONTROL ENDPOINT ***********************************************************/

static
VOID
KdCiStallControl(VOID)
{
    KdCiModify(Ci.Op, CI_ENDPTCTRL(Ci.Lpm, 0), 0, CI_ENDPTCTRL_RX_STALL | CI_ENDPTCTRL_TX_STALL);
    Ci.Ep0State = Ep0Idle;
}

/* Sends the data stage of an IN request, cut to what the host asked for, the OUT status follows */
static
VOID
KdCiControlIn(
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG Length,
    _In_ ULONG Requested)
{
    Length = min(Length, min(Requested, (ULONG)KDCI_EP0_BYTES));
    KdCiCopyToDma(Ci.Dma->Ep0Buffer, Data, Length);
    KdCiPrime(KDCI_QH_EP0_IN, &Ci.Dma->Ep0Td, Ci.Dma->Ep0Buffer, Length);
    Ci.Ep0State = Ep0DataIn;
}

static
VOID
KdCiControlOut(
    _In_ UCHAR Request,
    _In_ ULONG Length)
{
    Length = min(Length, (ULONG)KDCI_EP0_BYTES);
    KdCiPrime(KDCI_QH_EP0_OUT, &Ci.Dma->Ep0Td, Ci.Dma->Ep0Buffer, Length);
    Ci.PendingRequest = Request;
    Ci.Ep0State = Ep0DataOut;
}

/* A zero length packet in the direction opposite to the data closes the request */
static
VOID
KdCiControlStatus(
    _In_ BOOLEAN In)
{
    KdCiPrime(In ? KDCI_QH_EP0_IN : KDCI_QH_EP0_OUT, &Ci.Dma->Ep0Td, Ci.Dma->Ep0Buffer, 0);
    Ci.Ep0State = In ? Ep0StatusIn : Ep0StatusOut;
}

static
ULONG
KdCiStringDescriptor(
    _In_ UCHAR Index,
    _Out_writes_bytes_(KDCI_EP0_BYTES) PUCHAR Buffer)
{
    PCWSTR String;
    ULONG Length = 2;

    Buffer[1] = USB_DESC_STRING;

    if (Index == 0)
    {
        /* English (United States) */
        Buffer[0] = 4;
        Buffer[2] = 0x09;
        Buffer[3] = 0x04;
        return 4;
    }

    if (Index > RTL_NUMBER_OF(KdCiStrings))
        return 0;

    for (String = KdCiStrings[Index - 1]; *String != UNICODE_NULL && Length + 2 <= 254; String++)
    {
        Buffer[Length++] = (UCHAR)(*String & 0xFF);
        Buffer[Length++] = (UCHAR)(*String >> 8);
    }

    Buffer[0] = (UCHAR)Length;
    return Length;
}

static
VOID
KdCiStopDataPipes(VOID)
{
    KdCiFlush(CI_OUT_BIT(KDCI_EP_DATA) | CI_IN_BIT(KDCI_EP_DATA) | CI_IN_BIT(KDCI_EP_NOTIFY));
    KdCiWrite(Ci.Op, CI_ENDPTCTRL(Ci.Lpm, KDCI_EP_DATA), 0);
    KdCiWrite(Ci.Op, CI_ENDPTCTRL(Ci.Lpm, KDCI_EP_NOTIFY), 0);

    Ci.RxArmed = FALSE;
    Ci.TxBusy = FALSE;
    Ci.Configured = FALSE;
    Ci.Listening = FALSE;
    Ci.Configuration = 0;
}

static
VOID
KdCiArmReceive(VOID)
{
    KdCiPrime(KDCI_QH_DATA_OUT, &Ci.Dma->RxTd, Ci.Dma->RxBuffer, Ci.BulkPacket);
    Ci.RxArmed = TRUE;
}

static
BOOLEAN
KdCiSetConfiguration(
    _In_ UCHAR Value)
{
    KdCiStopDataPipes();
    if (Value == 0)
        return TRUE;
    if (Value != 1)
        return FALSE;

    KdCiSetQueueHead(KDCI_QH_DATA_OUT, CI_QH_MAX_PACKET(Ci.BulkPacket) | CI_QH_NO_ZERO_LENGTH);
    KdCiSetQueueHead(KDCI_QH_DATA_IN, CI_QH_MAX_PACKET(Ci.BulkPacket) | CI_QH_NO_ZERO_LENGTH);
    KdCiSetQueueHead(KDCI_QH_NOTIFY_IN, CI_QH_MAX_PACKET(KDCI_NOTIFY_PACKET) | CI_QH_NO_ZERO_LENGTH);

    KdCiWrite(Ci.Op,
              CI_ENDPTCTRL(Ci.Lpm, KDCI_EP_DATA),
              CI_ENDPTCTRL_RX_ENABLE | CI_ENDPTCTRL_RX_TOGGLE_RESET | CI_ENDPTCTRL_RX_TYPE(CI_ENDPOINT_BULK) |
              CI_ENDPTCTRL_TX_ENABLE | CI_ENDPTCTRL_TX_TOGGLE_RESET | CI_ENDPTCTRL_TX_TYPE(CI_ENDPOINT_BULK));

    /* The unused receive side gets a non control type, a control type there would disturb the transmit side */
    KdCiWrite(Ci.Op,
              CI_ENDPTCTRL(Ci.Lpm, KDCI_EP_NOTIFY),
              CI_ENDPTCTRL_RX_TYPE(CI_ENDPOINT_BULK) |
              CI_ENDPTCTRL_TX_ENABLE | CI_ENDPTCTRL_TX_TOGGLE_RESET | CI_ENDPTCTRL_TX_TYPE(CI_ENDPOINT_INTERRUPT));

    Ci.Configuration = Value;
    Ci.Configured = TRUE;
    KdCiArmReceive();
    return TRUE;
}

static
VOID
KdCiHandleSetup(
    _In_reads_(8) const UCHAR* Setup)
{
    UCHAR Reply[KDCI_EP0_BYTES];
    UCHAR RequestType = Setup[0], Request = Setup[1];
    USHORT Value = (USHORT)(Setup[2] | (Setup[3] << 8));
    USHORT Length = (USHORT)(Setup[6] | (Setup[7] << 8));
    ULONG Size;

    if ((RequestType & USB_TYPE_MASK) == USB_TYPE_CLASS)
    {
        switch (Request)
        {
            case CDC_REQ_SET_LINE_CODING:
                if (Length == 0)
                    break;
                KdCiControlOut(Request, Length);
                return;

            case CDC_REQ_GET_LINE_CODING:
                KdCiControlIn(Ci.LineCoding, sizeof(Ci.LineCoding), Length);
                return;

            case CDC_REQ_SET_CONTROL_LINE_STATE:
                /* DTR says a terminal has the port open and reads what is sent */
                Ci.Listening = (Value & 1) != 0;
                KdCiControlStatus(TRUE);
                return;

            case CDC_REQ_SEND_BREAK:
                KdCiControlStatus(TRUE);
                return;
        }

        KdCiStallControl();
        return;
    }

    if ((RequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD)
    {
        KdCiStallControl();
        return;
    }

    switch (Request)
    {
        case USB_REQ_GET_STATUS:
            Reply[0] = 0;
            Reply[1] = 0;
            KdCiControlIn(Reply, 2, Length);
            return;

        case USB_REQ_CLEAR_FEATURE:
        case USB_REQ_SET_FEATURE:
            KdCiControlStatus(TRUE);
            return;

        case USB_REQ_SET_ADDRESS:
            /* The advance bit holds the new address back until the status stage has gone out */
            KdCiWrite(Ci.Op, CI_DEVICEADDR, CI_DEVICEADDR_ADDRESS(Value & 0x7F) | CI_DEVICEADDR_ADVANCE);
            KdCiControlStatus(TRUE);
            return;

        case USB_REQ_GET_DESCRIPTOR:
            switch (Value >> 8)
            {
                case USB_DESC_DEVICE:
                    KdCiControlIn(KdCiDeviceDescriptor, sizeof(KdCiDeviceDescriptor), Length);
                    return;

                case USB_DESC_CONFIGURATION:
                    KdCiConfigDescriptor[KDCI_CONFIG_OUT_MPS] = (UCHAR)(Ci.BulkPacket & 0xFF);
                    KdCiConfigDescriptor[KDCI_CONFIG_OUT_MPS + 1] = (UCHAR)(Ci.BulkPacket >> 8);
                    KdCiConfigDescriptor[KDCI_CONFIG_IN_MPS] = (UCHAR)(Ci.BulkPacket & 0xFF);
                    KdCiConfigDescriptor[KDCI_CONFIG_IN_MPS + 1] = (UCHAR)(Ci.BulkPacket >> 8);
                    KdCiControlIn(KdCiConfigDescriptor, sizeof(KdCiConfigDescriptor), Length);
                    return;

                case USB_DESC_STRING:
                    Size = KdCiStringDescriptor((UCHAR)(Value & 0xFF), Reply);
                    if (Size == 0)
                        break;
                    KdCiControlIn(Reply, Size, Length);
                    return;

                case USB_DESC_QUALIFIER:
                    KdCiControlIn(KdCiQualifierDescriptor, sizeof(KdCiQualifierDescriptor), Length);
                    return;
            }
            break;

        case USB_REQ_GET_CONFIGURATION:
            Reply[0] = Ci.Configuration;
            KdCiControlIn(Reply, 1, Length);
            return;

        case USB_REQ_SET_CONFIGURATION:
            if (!KdCiSetConfiguration((UCHAR)Value))
                break;
            KdCiControlStatus(TRUE);
            return;

        case USB_REQ_GET_INTERFACE:
            Reply[0] = 0;
            KdCiControlIn(Reply, 1, Length);
            return;

        case USB_REQ_SET_INTERFACE:
            if (Value != 0)
                break;
            KdCiControlStatus(TRUE);
            return;
    }

    KdCiStallControl();
}

/*
 * Reads a setup packet out of the control queue head. The tripwire drops when
 * a newer setup lands during the copy, which is then made again.
 */
static
VOID
KdCiTakeSetup(VOID)
{
    UCHAR Setup[8];

    do
    {
        KdCiModify(Ci.Op, CI_USBCMD, 0, CI_USBCMD_SETUP_TRIPWIRE);
        KdCiCopyFromDma(Setup, Ci.Dma->QueueHeads[KDCI_QH_EP0_OUT].Setup, sizeof(Setup));
    } while (!(KdCiRead(Ci.Op, CI_USBCMD) & CI_USBCMD_SETUP_TRIPWIRE));

    KdCiModify(Ci.Op, CI_USBCMD, CI_USBCMD_SETUP_TRIPWIRE, 0);
    KdCiWrite(Ci.Op, CI_ENDPTSETUPSTAT(Ci.Lpm), CI_OUT_BIT(0));

    /* A new setup ends whatever the previous request still had primed */
    KdCiFlush(CI_OUT_BIT(0) | CI_IN_BIT(0));
    KdCiHandleSetup(Setup);
}

static
VOID
KdCiControlComplete(VOID)
{
    switch (Ci.Ep0State)
    {
        case Ep0DataIn:
            KdCiControlStatus(FALSE);
            break;

        case Ep0DataOut:
            if (Ci.PendingRequest == CDC_REQ_SET_LINE_CODING)
                KdCiCopyFromDma(Ci.LineCoding, Ci.Dma->Ep0Buffer, sizeof(Ci.LineCoding));
            KdCiControlStatus(TRUE);
            break;

        default:
            Ci.Ep0State = Ep0Idle;
            break;
    }
}

/* DATA PIPES *****************************************************************/

static
VOID
KdCiReceiveComplete(VOID)
{
    ULONG Token = READ_REGISTER_ULONG(&Ci.Dma->RxTd.Token);
    ULONG Remaining = CI_TD_REMAINING(Token);
    ULONG Received;
    ULONG Next;
    ULONG i;

    Ci.RxArmed = FALSE;
    if (Token & CI_TD_ACTIVE)
        return;

    Received = (!(Token & CI_TD_ERRORS) && Remaining < Ci.BulkPacket) ? Ci.BulkPacket - Remaining : 0;
    for (i = 0; i < Received; i++)
    {
        Next = (Ci.InputHead + 1) % KDCI_INPUT_BYTES;
        if (Next == Ci.InputTail)
            break;

        Ci.Input[Ci.InputHead] = READ_REGISTER_UCHAR(&Ci.Dma->RxBuffer[i]);
        Ci.InputHead = Next;
    }

    if (Ci.Configured)
        KdCiArmReceive();
}

/* The host reset the bus: everything primed is dropped and the device is unaddressed again */
static
VOID
KdCiBusReset(VOID)
{
    KdCiWrite(Ci.Op, CI_ENDPTSETUPSTAT(Ci.Lpm), KdCiRead(Ci.Op, CI_ENDPTSETUPSTAT(Ci.Lpm)));
    KdCiWrite(Ci.Op, CI_ENDPTCOMPLETE(Ci.Lpm), KdCiRead(Ci.Op, CI_ENDPTCOMPLETE(Ci.Lpm)));
    KdCiFlush(0xFFFFFFFF);
    KdCiWrite(Ci.Op, CI_DEVICEADDR, 0);
    KdCiStopDataPipes();
    Ci.Ep0State = Ep0Idle;
}

static
VOID
KdCiReadSpeed(VOID)
{
    ULONG Speed = Ci.Lpm ? CI_DEVLC_SPEED(KdCiRead(Ci.Op, CI_DEVLC)) : CI_PORTSC_SPEED(KdCiRead(Ci.Op, CI_PORTSC));

    Ci.BulkPacket = (Speed == CI_SPEED_HIGH) ? KDCI_BULK_PACKET_HIGH : KDCI_BULK_PACKET_FULL;
}

/* BRING-UP *******************************************************************/

static
VOID
KdCiForgetHost(VOID)
{
    Ci.Ep0State = Ep0Idle;
    Ci.BulkPacket = KDCI_BULK_PACKET_HIGH;
    Ci.Configuration = 0;
    Ci.Configured = FALSE;
    Ci.Listening = FALSE;
    Ci.RxArmed = FALSE;
    Ci.TxBusy = FALSE;
}

/**
 * @brief Powers the controller, resets it, brings the PHY up and starts it as a USB serial device.
 */
static
NTSTATUS
KdCiBringUp(VOID)
{
    const KDCI_PLATFORM* Platform = Ci.Platform;
    PUCHAR Cap = Ci.Base + CI_CAP_OFFSET;
    ULONG Mode;

    Ci.Up = FALSE;
    KdCiForgetHost();

    if (!Platform->PowerUp(Platform))
        return STATUS_DEVICE_NOT_READY;

    Ci.Op = Cap + (READ_REGISTER_UCHAR(Cap + CI_CAPLENGTH));
    Ci.Lpm = (KdCiRead(Cap, CI_HCCPARAMS) & CI_HCCPARAMS_LPM) != 0;
    if (!(KdCiRead(Cap, CI_DCCPARAMS) & CI_DCCPARAMS_DEVICE) ||
        CI_DCCPARAMS_ENDPOINTS(KdCiRead(Cap, CI_DCCPARAMS)) <= KDCI_EP_NOTIFY)
    {
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }

    /* Stop, drop what firmware left primed and reset the controller */
    KdCiWrite(Ci.Op, CI_ENDPTFLUSH(Ci.Lpm), 0xFFFFFFFF);
    KdCiModify(Ci.Op, CI_USBCMD, CI_USBCMD_RUN, 0);
    KdCiModify(Ci.Op, CI_USBCMD, 0, CI_USBCMD_RESET);
    if (!KdCiWaitFor(Ci.Op, CI_USBCMD, CI_USBCMD_RESET, 0, KDCI_RESET_TIMEOUT_US))
        return STATUS_DEVICE_NOT_READY;

    if (Platform->UlpiPhy)
    {
        if (Ci.Lpm)
            KdCiModify(Ci.Op, CI_DEVLC, CI_DEVLC_PTS_MASK, CI_DEVLC_PTS_ULPI);
        else
            KdCiModify(Ci.Op, CI_PORTSC, CI_PORTSC_PTS_MASK, CI_PORTSC_PTS_ULPI);
    }

    if (Platform->StartPhy != NULL && !Platform->StartPhy(Platform))
        return STATUS_DEVICE_NOT_READY;

    /* Idle first, then device mode, with setups not locked out and streaming off */
    KdCiModify(Ci.Op, CI_USBMODE(Ci.Lpm), CI_USBMODE_CM_MASK, 0);
    KdCiModify(Ci.Op,
               CI_USBMODE(Ci.Lpm),
               CI_USBMODE_CM_MASK,
               CI_USBMODE_CM_DEVICE | CI_USBMODE_SETUP_LOCKOUT_OFF | CI_USBMODE_STREAM_DISABLE);
    Mode = KdCiRead(Ci.Op, CI_USBMODE(Ci.Lpm));
    if ((Mode & CI_USBMODE_CM_MASK) != CI_USBMODE_CM_DEVICE)
        return STATUS_DEVICE_NOT_READY;

    /* Everything is polled */
    KdCiWrite(Ci.Op, CI_USBINTR, 0);
    KdCiWrite(Ci.Op, CI_USBSTS, 0xFFFFFFFF);

    RtlZeroMemory((PVOID)Ci.Dma, sizeof(*Ci.Dma));
    KdCiSetQueueHead(KDCI_QH_EP0_OUT,
                     CI_QH_MAX_PACKET(KDCI_EP0_PACKET) | CI_QH_INTERRUPT_ON_SETUP | CI_QH_NO_ZERO_LENGTH);
    KdCiSetQueueHead(KDCI_QH_EP0_IN, CI_QH_MAX_PACKET(KDCI_EP0_PACKET) | CI_QH_NO_ZERO_LENGTH);
    KdCiWrite(Ci.Op, CI_ENDPTLISTADDR, KdCiPhysical(Ci.Dma->QueueHeads));
    KdCiWrite(Ci.Op, CI_DEVICEADDR, 0);

    KeMemoryBarrier();
    KdCiModify(Ci.Op, CI_USBCMD, 0, CI_USBCMD_RUN | Platform->CommandFlags);

    Ci.Up = TRUE;
    Ci.NextCheck = KdCiMicroseconds() + KDCI_CHECK_US;
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
 * Physical base of the controller.
 */
NTSTATUS
NTAPI
KdCiStartController(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_ const KDCI_PLATFORM* Platform,
    _In_ ULONG64 CoreBase)
{
    PHYSICAL_ADDRESS DmaAddress;
    PFN_COUNT DmaPages = BYTES_TO_PAGES(sizeof(*Ci.Dma));
    NTSTATUS Status;

    RtlZeroMemory(&Ci, sizeof(Ci));
    Ci.Platform = Platform;

    /* 115200 8N1 until the host says otherwise */
    Ci.LineCoding[0] = 0x00;
    Ci.LineCoding[1] = 0xC2;
    Ci.LineCoding[2] = 0x01;
    Ci.LineCoding[6] = 8;

    DmaAddress.QuadPart = KdCiTakePages(LoaderBlock, DmaPages);
    if (DmaAddress.QuadPart == 0)
        return STATUS_INSUFFICIENT_RESOURCES;

    Ci.Dma = KdMapPhysicalMemory64(DmaAddress, DmaPages);
    Ci.DmaAddress = DmaAddress.QuadPart;
    Ci.Base = KdCiMapRegister(CoreBase);
    if (Ci.Dma == NULL || Ci.Base == NULL)
    {
        Ci.Base = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* A controller that is there but not ready yet is retried from the poll, a missing one is not */
    Status = KdCiBringUp();
    if (Status == STATUS_DEVICE_NOT_READY)
    {
        Ci.NextRevival = KdCiMicroseconds() + KDCI_REVIVE_US;
        return STATUS_SUCCESS;
    }

    return Status;
}

/* TRUE while the controller still holds the device setup this driver gave it */
static
BOOLEAN
KdCiControllerIsOurs(VOID)
{
    if ((KdCiRead(Ci.Op, CI_USBMODE(Ci.Lpm)) & CI_USBMODE_CM_MASK) != CI_USBMODE_CM_DEVICE)
        return FALSE;
    if (!(KdCiRead(Ci.Op, CI_USBCMD) & CI_USBCMD_RUN))
        return FALSE;

    return KdCiRead(Ci.Op, CI_ENDPTLISTADDR) == KdCiPhysical(Ci.Dma->QueueHeads);
}

/*
 * Decides whether the controller may be touched. Its clocks are read from the
 * clock controller, which always answers; the controller only once they run.
 */
static
BOOLEAN
KdCiCheckAlive(VOID)
{
    const KDCI_PLATFORM* Platform = Ci.Platform;
    ULONG64 Now;

    if (Platform == NULL || Ci.Base == NULL)
        return FALSE;

    Now = KdCiMicroseconds();
    if (!Ci.Up)
    {
        if (Now < Ci.NextRevival)
            return FALSE;

        Ci.NextRevival = Now + KDCI_REVIVE_US;
        if (Platform->IsPowered != NULL && !Platform->IsPowered(Platform) && Now - Ci.DownSince < KDCI_RECLAIM_US)
            return FALSE;

        return NT_SUCCESS(KdCiBringUp());
    }

    if (Platform->IsPowered != NULL && !Platform->IsPowered(Platform))
    {
        KdCiPowerDown();
        return FALSE;
    }

    if (Now >= Ci.NextCheck)
    {
        Ci.NextCheck = Now + KDCI_CHECK_US;
        if (!KdCiControllerIsOurs())
            return NT_SUCCESS(KdCiBringUp());
    }

    return TRUE;
}

/* EXPORTED TO THE TRANSPORT **************************************************/

/**
 * @brief Handles what the controller reports since the last call: bus resets, setups and finished transfers.
 */
VOID
NTAPI
KdCiService(VOID)
{
    ULONG Status, Complete;

    if (!KdCiCheckAlive())
        return;

    Status = KdCiRead(Ci.Op, CI_USBSTS);
    if (Status & (CI_USBSTS_RESET | CI_USBSTS_PORT_CHANGE | CI_USBSTS_SUSPEND))
    {
        KdCiWrite(Ci.Op, CI_USBSTS, Status & (CI_USBSTS_RESET | CI_USBSTS_PORT_CHANGE | CI_USBSTS_SUSPEND));
        KdCiLastEvent = KdCiMicroseconds();

        if (Status & CI_USBSTS_RESET)
            KdCiBusReset();
        if (Status & CI_USBSTS_PORT_CHANGE)
            KdCiReadSpeed();
    }

    Complete = KdCiRead(Ci.Op, CI_ENDPTCOMPLETE(Ci.Lpm));
    if (Complete != 0)
    {
        KdCiWrite(Ci.Op, CI_ENDPTCOMPLETE(Ci.Lpm), Complete);
        KdCiLastEvent = KdCiMicroseconds();

        if (Complete & (CI_OUT_BIT(0) | CI_IN_BIT(0)))
            KdCiControlComplete();
        if ((Complete & CI_OUT_BIT(KDCI_EP_DATA)) && Ci.RxArmed)
            KdCiReceiveComplete();
        if (Complete & CI_IN_BIT(KDCI_EP_DATA))
            Ci.TxBusy = FALSE;
    }

    /* Setups go last so that a request completed above does not undo the new one */
    if (KdCiRead(Ci.Op, CI_ENDPTSETUPSTAT(Ci.Lpm)) & CI_OUT_BIT(0))
    {
        KdCiLastEvent = KdCiMicroseconds();
        KdCiTakeSetup();
    }
}

/**
 * @brief Forgets the controller without touching it, its power is going away.
 */
VOID
NTAPI
KdCiPowerDown(VOID)
{
    if (Ci.Platform == NULL)
        return;

    if (Ci.Up)
        Ci.DownSince = KdCiMicroseconds();

    Ci.Up = FALSE;
    KdCiForgetHost();
    Ci.NextRevival = KdCiMicroseconds() + KDCI_REVIVE_US;
}

/**
 * @brief Brings the controller back right away, its power has returned.
 */
NTSTATUS
NTAPI
KdCiPowerUp(VOID)
{
    if (Ci.Platform == NULL)
        return STATUS_DEVICE_NOT_READY;

    if (Ci.Up)
        return STATUS_SUCCESS;

    return KdCiBringUp();
}

VOID
NTAPI
KdCiDmaRange(
    _Out_ PULONG64 Base,
    _Out_ PULONG Bytes)
{
    *Base = Ci.DmaAddress;
    *Bytes = sizeof(*Ci.Dma);
}

BOOLEAN
NTAPI
KdCiIsConfigured(VOID)
{
    return Ci.Configured;
}

BOOLEAN
NTAPI
KdCiIsHostListening(VOID)
{
    return Ci.Configured && Ci.Listening;
}

BOOLEAN
NTAPI
KdCiIsSending(VOID)
{
    return Ci.Up && Ci.TxBusy;
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
KdCiSend(
    _In_reads_bytes_(Length) const UCHAR* Buffer,
    _In_ ULONG Length)
{
    if (!Ci.Up || !Ci.Configured || Ci.TxBusy || Length == 0)
        return 0;

    Length = min(Length, (ULONG)KDCI_TX_BYTES);
    if (Length > 1 && (Length % Ci.BulkPacket) == 0)
        Length--;

    KdCiCopyToDma(Ci.Dma->TxBuffer, Buffer, Length);
    KdCiPrime(KDCI_QH_DATA_IN, &Ci.Dma->TxTd, Ci.Dma->TxBuffer, Length);
    Ci.TxBusy = TRUE;
    return Length;
}

BOOLEAN
NTAPI
KdCiTakeInput(
    _Out_ PUCHAR Byte)
{
    if (Ci.InputTail == Ci.InputHead)
        return FALSE;

    *Byte = Ci.Input[Ci.InputTail];
    Ci.InputTail = (Ci.InputTail + 1) % KDCI_INPUT_BYTES;
    return TRUE;
}
