/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Ring zero: control packet transmit and receive with CRC
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4hr.h"

#define NDEBUG
#include <debug.h>

/* Ring zero registers; ring 0 sits at the start of every block */
#define RING0_TX_ADDRESS_LOW    (USB4HR_TX_RING_BASE + USB4HR_RING_ADDRESS_LOW)
#define RING0_TX_ADDRESS_HIGH   (USB4HR_TX_RING_BASE + USB4HR_RING_ADDRESS_HIGH)
#define RING0_TX_INDEXES        (USB4HR_TX_RING_BASE + USB4HR_RING_INDEXES)
#define RING0_TX_SIZE           (USB4HR_TX_RING_BASE + USB4HR_RING_SIZE)
#define RING0_RX_ADDRESS_LOW    (USB4HR_RX_RING_BASE + USB4HR_RING_ADDRESS_LOW)
#define RING0_RX_ADDRESS_HIGH   (USB4HR_RX_RING_BASE + USB4HR_RING_ADDRESS_HIGH)
#define RING0_RX_INDEXES        (USB4HR_RX_RING_BASE + USB4HR_RING_INDEXES)
#define RING0_RX_SIZE           (USB4HR_RX_RING_BASE + USB4HR_RING_SIZE)
#define RING0_TX_TABLE          USB4HR_TX_TABLE_BASE
#define RING0_TX_TIMESTAMP      (USB4HR_TX_TABLE_BASE + 4)
#define RING0_RX_TABLE          USB4HR_RX_TABLE_BASE

#define RING0_TX_DRAIN_MS       1000
#define RING0_TX_DRAIN_STEP_MS  10

/* Receive descriptors that are not done yet get this many 5 us polls */
#define RING0_DONE_POLLS        10
#define RING0_DONE_POLL_US      5

/* Bytes of a read response without data: route, header and CRC */
#define RING0_MIN_PACKET_BYTES  ((USB4HR_PACKET_HEADER_DWORDS * sizeof(ULONG)) + USB4HR_CRC_BYTES)

ULONG
NTAPI
Usb4HrCrc32c(
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG Length)
{
    ULONG Crc = 0xFFFFFFFF;
    ULONG Index;
    ULONG Bit;

    for (Index = 0; Index < Length; Index++)
    {
        Crc ^= Data[Index];
        for (Bit = 0; Bit < 8; Bit++)
        {
            if (Crc & 1)
                Crc = (Crc >> 1) ^ USB4HR_CRC32C_REFLECTED;
            else
                Crc >>= 1;
        }
    }

    return ~Crc;
}

VOID
NTAPI
Usb4HrRouteToPacket(
    _In_ const Usb4HrRoute* Route,
    _Out_writes_(2) PULONG Dwords)
{
    ULONG64 Packed = 0;
    ULONG Hop;
    ULONG Depth = min((ULONG)Route->Depth, (ULONG)USB4HR_MAX_DEPTH);

    /* Level n is byte n - 1 of the 64 bit route string; levels 5 and 6 land in the high dword */
    for (Hop = 0; Hop < Depth; Hop++)
        Packed |= (ULONG64)Route->Port[Hop] << (Hop * 8);

    Dwords[USB4HR_ROUTE_HIGH] = (ULONG)(Packed >> 32);
    Dwords[USB4HR_ROUTE_LOW] = (ULONG)Packed;
}

VOID
NTAPI
Usb4HrRouteFromPacket(
    _In_reads_(2) const ULONG* Dwords,
    _Out_ Usb4HrRoute* Route)
{
    ULONG64 Packed;
    ULONG Hop;

    Packed = ((ULONG64)(Dwords[USB4HR_ROUTE_HIGH] & ~USB4HR_ROUTE_CM) << 32) | Dwords[USB4HR_ROUTE_LOW];

    RtlZeroMemory(Route, sizeof(*Route));
    for (Hop = 0; Hop < USB4HR_MAX_DEPTH; Hop++)
    {
        Route->Port[Hop] = (UCHAR)(Packed >> (Hop * 8));
        if (Route->Port[Hop] != 0)
            Route->Depth = (UCHAR)(Hop + 1);
    }
}

ULONG
NTAPI
Usb4HrConfigRequestHeader(
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _In_ UCHAR Adapter,
    _In_ ULONG Space,
    _In_ UCHAR Sequence)
{
    ULONG Header;

    Header = DwordOffset & USB4HR_CFG_OFFSET_MASK;
    Header |= (DwordCount << USB4HR_CFG_LENGTH_SHIFT) & USB4HR_CFG_LENGTH_MASK;
    Header |= ((ULONG)Adapter << USB4HR_CFG_ADAPTER_SHIFT) & USB4HR_CFG_ADAPTER_MASK;
    Header |= (Space << USB4HR_CFG_SPACE_SHIFT) & USB4HR_CFG_SPACE_MASK;
    Header |= ((ULONG)Sequence << USB4HR_CFG_SEQUENCE_SHIFT) & USB4HR_CFG_SEQUENCE_MASK;
    return Header;
}

/* Reads a big endian dword out of a frame */
static
ULONG
NTAPI
Usb4HrFrameDword(
    _In_reads_bytes_(4) const UCHAR* Bytes)
{
    return ((ULONG)Bytes[0] << 24) | ((ULONG)Bytes[1] << 16) | ((ULONG)Bytes[2] << 8) | Bytes[3];
}

NTSTATUS
Usb4HrRingZero::AllocateBuffer(
    _In_ size_t Length,
    _Out_ WDFCOMMONBUFFER* Buffer,
    _Out_ PVOID* VirtualAddress,
    _Out_ PHYSICAL_ADDRESS* LogicalAddress)
{
    NTSTATUS Status;

    Status = WdfCommonBufferCreate(m_DmaEnabler, Length, WDF_NO_OBJECT_ATTRIBUTES, Buffer);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Ring zero buffer of %Iu bytes failed 0x%lx\n", Length, Status);
        return Status;
    }

    *VirtualAddress = WdfCommonBufferGetAlignedVirtualAddress(*Buffer);
    *LogicalAddress = WdfCommonBufferGetAlignedLogicalAddress(*Buffer);
    RtlZeroMemory(*VirtualAddress, Length);
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrRingZero::Create(
    _In_ Usb4HrHostRouter* HostRouter)
{
    WDF_DMA_ENABLER_CONFIG Config;
    WDFDEVICE Device = HostRouter->Device();
    PHYSICAL_ADDRESS TxFramesAddress;
    PHYSICAL_ADDRESS RxFramesAddress;
    PVOID Address;
    ULONG Index;
    NTSTATUS Status;

    m_HostRouter = HostRouter;
    m_Hardware = HostRouter->Hardware();
    KeInitializeSpinLock(&m_TxLock);
    KeInitializeSpinLock(&m_RxLock);

    /* Descriptors and frames are page aligned */
    if (WdfDeviceGetAlignmentRequirement(Device) < PAGE_SIZE - 1)
        WdfDeviceSetAlignmentRequirement(Device, PAGE_SIZE - 1);

    WDF_DMA_ENABLER_CONFIG_INIT(&Config, WdfDmaProfileScatterGather64Duplex, PAGE_SIZE);
    Status = WdfDmaEnablerCreate(Device, &Config, WDF_NO_OBJECT_ATTRIBUTES, &m_DmaEnabler);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WdfDmaEnablerCreate failed 0x%lx\n", Status);
        return Status;
    }

    Status = AllocateBuffer(USB4HR_RING_ZERO_ENTRIES * sizeof(USB4HR_BUFFER_DESCRIPTOR),
                            &m_TxDescBuffer,
                            &Address,
                            &m_TxDescAddress);
    if (!NT_SUCCESS(Status))
        return Status;
    m_TxDesc = (volatile USB4HR_BUFFER_DESCRIPTOR*)Address;

    Status = AllocateBuffer(USB4HR_RING_ZERO_ENTRIES * USB4HR_FRAME_SIZE,
                            &m_TxFrameBuffer,
                            &Address,
                            &TxFramesAddress);
    if (!NT_SUCCESS(Status))
        return Status;
    m_TxFrames = (PUCHAR)Address;

    Status = AllocateBuffer(USB4HR_RING_ZERO_ENTRIES * sizeof(USB4HR_BUFFER_DESCRIPTOR),
                            &m_RxDescBuffer,
                            &Address,
                            &m_RxDescAddress);
    if (!NT_SUCCESS(Status))
        return Status;
    m_RxDesc = (volatile USB4HR_BUFFER_DESCRIPTOR*)Address;

    Status = AllocateBuffer(USB4HR_RING_ZERO_ENTRIES * USB4HR_FRAME_SIZE,
                            &m_RxFrameBuffer,
                            &Address,
                            &RxFramesAddress);
    if (!NT_SUCCESS(Status))
        return Status;
    m_RxFrames = (PUCHAR)Address;

    /* Every descriptor owns one frame for the life of the device */
    for (Index = 0; Index < USB4HR_RING_ZERO_ENTRIES; Index++)
    {
        ULONG64 TxFrame = (ULONG64)TxFramesAddress.QuadPart + (ULONG64)Index * USB4HR_FRAME_SIZE;
        ULONG64 RxFrame = (ULONG64)RxFramesAddress.QuadPart + (ULONG64)Index * USB4HR_FRAME_SIZE;

        m_TxDesc[Index].AddressLow = (ULONG)TxFrame;
        m_TxDesc[Index].AddressHigh = (ULONG)(TxFrame >> 32);
        m_RxDesc[Index].AddressLow = (ULONG)RxFrame;
        m_RxDesc[Index].AddressHigh = (ULONG)(RxFrame >> 32);
    }

    m_RxCause = HostRouter->Interrupts()->RxCause(0);
    Status = HostRouter->Interrupts()->Connect(m_RxCause, OnRxInterrupt, this);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Ring zero receive interrupt connect failed 0x%lx\n", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrRingZero::RegisterHandler(
    _In_ ULONG Pdf,
    _In_ PUSB4HR_RX_HANDLER Handler,
    _In_ PVOID Context)
{
    ULONG Slot;

    if (Pdf >= USB4HR_PDF_COUNT)
    {
        DPRINT1("Receive handler for invalid PDF %lu\n", Pdf);
        return STATUS_INVALID_PARAMETER;
    }

    for (Slot = 0; Slot < USB4HR_RX_HANDLERS_PER_PDF; Slot++)
    {
        if (m_Handlers[Pdf][Slot].Handler == NULL)
        {
            m_Handlers[Pdf][Slot].Context = Context;
            m_Handlers[Pdf][Slot].Handler = Handler;
            return STATUS_SUCCESS;
        }
    }

    DPRINT1("No receive handler slot left for PDF %lu\n", Pdf);
    return STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS Usb4HrRingZero::StartTransmit()
{
    Usb4HrSpinLockGuard Guard(&m_TxLock);

    if (m_TxRunning)
        return STATUS_SUCCESS;

    if (!m_Hardware->MmioResponds())
    {
        DPRINT1("Transmit ring zero start with dead MMIO\n");
        return STATUS_DEVICE_NOT_READY;
    }

    m_Hardware->Write32(RING0_TX_ADDRESS_LOW, m_TxDescAddress.LowPart);
    m_Hardware->Write32(RING0_TX_ADDRESS_HIGH, (ULONG)m_TxDescAddress.HighPart);
    m_Hardware->Write32(RING0_TX_SIZE, USB4HR_RING_ZERO_ENTRIES);
    m_Hardware->Write32(RING0_TX_TIMESTAMP, 0);
    m_Hardware->Write32(RING0_TX_TABLE, USB4HR_TABLE_VALID | USB4HR_TABLE_RAW_MODE);

    m_TxRunning = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS Usb4HrRingZero::StartReceive()
{
    ULONG Index;

    if (!m_Hardware->MmioResponds())
    {
        DPRINT1("Receive ring zero start with dead MMIO\n");
        return STATUS_DEVICE_NOT_READY;
    }

    Usb4HrSpinLockGuard Guard(&m_RxLock);

    m_RxRunning = TRUE;
    m_HostRouter->Interrupts()->EnableCause(m_RxCause);

    /* Raw mode takes every PDF, so the PDF mask dword stays untouched */
    m_Hardware->Write32(RING0_RX_ADDRESS_LOW, m_RxDescAddress.LowPart);
    m_Hardware->Write32(RING0_RX_ADDRESS_HIGH, (ULONG)m_RxDescAddress.HighPart);
    m_Hardware->Write32(RING0_RX_SIZE,
                        USB4HR_RING_ZERO_ENTRIES |
                        ((USB4HR_FRAME_SIZE << USB4HR_RX_BUFFER_SIZE_SHIFT) & USB4HR_RX_BUFFER_SIZE_MASK));
    m_Hardware->Write32(RING0_RX_TABLE, USB4HR_TABLE_VALID | USB4HR_TABLE_RAW_MODE);

    for (Index = 0; Index < USB4HR_RING_ZERO_ENTRIES; Index++)
    {
        m_RxDesc[Index].Control = USB4HR_DESC_REQUEST_STATUS | USB4HR_DESC_INTERRUPT_ENABLE;
        m_RxDesc[Index].Timestamp = 0;
    }
    KeMemoryBarrier();

    /* The consumer index trails the producer: the last entry is the one software holds */
    m_Hardware->Write32(RING0_RX_INDEXES, USB4HR_RING_ZERO_ENTRIES - 1);
    return STATUS_SUCCESS;
}

NTSTATUS Usb4HrRingZero::Start()
{
    NTSTATUS Status;

    Status = StartTransmit();
    if (!NT_SUCCESS(Status))
        return Status;

    Status = StartReceive();
    if (!NT_SUCCESS(Status))
        DPRINT1("Receive ring zero start failed 0x%lx\n", Status);

    return Status;
}

VOID Usb4HrRingZero::StopTransmit()
{
    ULONG Waited;
    ULONG Indexes = 0;
    ULONG Table;

    {
        Usb4HrSpinLockGuard Guard(&m_TxLock);

        if (!m_TxRunning)
            return;
        m_TxRunning = FALSE;
    }

    if (!m_Hardware->MmioResponds())
    {
        DPRINT1("Transmit ring zero stop with dead MMIO\n");
        return;
    }

    Table = m_Hardware->Read32(RING0_TX_TABLE);
    m_Hardware->Write32(RING0_TX_TABLE, Table & ~USB4HR_TABLE_E2E_FLOW_CONTROL);

    /* Let the host interface fetch what is already queued */
    for (Waited = 0; Waited < RING0_TX_DRAIN_MS; Waited += RING0_TX_DRAIN_STEP_MS)
    {
        Indexes = m_Hardware->Read32(RING0_TX_INDEXES);
        if ((Indexes & USB4HR_RING_CONSUMER_MASK) == (Indexes >> USB4HR_RING_PRODUCER_SHIFT))
            break;
        Usb4HrSleepMs(RING0_TX_DRAIN_STEP_MS);
    }

    if (Waited >= RING0_TX_DRAIN_MS)
        DPRINT1("Transmit ring zero did not drain, indexes 0x%08lx\n", Indexes);

    Table = m_Hardware->Read32(RING0_TX_TABLE);
    m_Hardware->Write32(RING0_TX_TABLE, Table & ~USB4HR_TABLE_VALID);
    m_Hardware->Write32(RING0_TX_TABLE, 0);
}

VOID Usb4HrRingZero::StopReceive()
{
    ULONG Table;

    if (!m_RxRunning)
        return;
    m_RxRunning = FALSE;

    Usb4HrSpinLockGuard Guard(&m_RxLock);

    m_HostRouter->Interrupts()->DisableCause(m_RxCause);

    if (!m_Hardware->MmioResponds())
    {
        DPRINT1("Receive ring zero stop with dead MMIO\n");
        return;
    }

    Table = m_Hardware->Read32(RING0_RX_TABLE);
    if (!(Table & USB4HR_TABLE_RAW_MODE))
    {
        Table &= ~USB4HR_TABLE_E2E_FLOW_CONTROL;
        m_Hardware->Write32(RING0_RX_TABLE, Table);
    }
    m_Hardware->Write32(RING0_RX_TABLE, Table & ~USB4HR_TABLE_VALID);

    /* Windows writes the base back after disabling the ring */
    m_Hardware->Write32(RING0_RX_ADDRESS_LOW, m_Hardware->Read32(RING0_RX_ADDRESS_LOW));
    m_Hardware->Write32(RING0_RX_ADDRESS_HIGH, m_Hardware->Read32(RING0_RX_ADDRESS_HIGH));
}

VOID Usb4HrRingZero::Stop()
{
    StopTransmit();
    StopReceive();
}

BOOLEAN Usb4HrRingZero::IsRunning() const
{
    return m_TxRunning;
}

NTSTATUS
Usb4HrRingZero::Send(
    _In_ ULONG Pdf,
    _In_reads_(DwordCount) const ULONG* Dwords,
    _In_ ULONG DwordCount)
{
    ULONG Indexes;
    ULONG Producer;
    ULONG Consumer;
    ULONG Next;
    ULONG Index;
    ULONG Length;
    ULONG Crc;
    ULONG Control;
    PUCHAR Frame;

    if (Pdf >= USB4HR_PDF_COUNT || DwordCount == 0 || DwordCount >= USB4HR_PACKET_MAX_DWORDS)
    {
        DPRINT1("Bad control packet, PDF %lu, %lu dwords\n", Pdf, DwordCount);
        return STATUS_INVALID_PARAMETER;
    }

    Usb4HrSpinLockGuard Guard(&m_TxLock);

    if (!m_TxRunning)
    {
        DPRINT("Transmit ring zero is stopped\n");
        return STATUS_DEVICE_NOT_READY;
    }

    Indexes = m_Hardware->Read32(RING0_TX_INDEXES);
    Consumer = Indexes & USB4HR_RING_CONSUMER_MASK;
    Producer = Indexes >> USB4HR_RING_PRODUCER_SHIFT;
    if (Consumer >= USB4HR_RING_ZERO_ENTRIES || Producer >= USB4HR_RING_ZERO_ENTRIES)
    {
        DPRINT1("Transmit ring zero indexes out of range 0x%08lx\n", Indexes);
        return STATUS_INVALID_DEVICE_STATE;
    }

    Next = (Producer + 1) % USB4HR_RING_ZERO_ENTRIES;
    if (Next == Consumer)
    {
        DPRINT1("Transmit ring zero full\n");
        return STATUS_DEVICE_BUSY;
    }

    /* Notifications leave the last free slot to read and write requests */
    if (Pdf == USB4HR_PDF_NOTIFICATION && Producer != Consumer &&
        ((Consumer + USB4HR_RING_ZERO_ENTRIES - Producer) % USB4HR_RING_ZERO_ENTRIES) <= 2)
    {
        DPRINT1("Transmit ring zero nearly full, notification held back\n");
        return STATUS_DEVICE_BUSY;
    }

    Frame = m_TxFrames + Producer * USB4HR_FRAME_SIZE;
    for (Index = 0; Index < DwordCount; Index++)
    {
        Frame[Index * 4] = (UCHAR)(Dwords[Index] >> 24);
        Frame[Index * 4 + 1] = (UCHAR)(Dwords[Index] >> 16);
        Frame[Index * 4 + 2] = (UCHAR)(Dwords[Index] >> 8);
        Frame[Index * 4 + 3] = (UCHAR)Dwords[Index];
    }

    Length = DwordCount * sizeof(ULONG);
    Crc = Usb4HrCrc32c(Frame, Length);
    Frame[Length] = (UCHAR)(Crc >> 24);
    Frame[Length + 1] = (UCHAR)(Crc >> 16);
    Frame[Length + 2] = (UCHAR)(Crc >> 8);
    Frame[Length + 3] = (UCHAR)Crc;
    Length += USB4HR_CRC_BYTES;

    Control = m_TxDesc[Producer].Control;
    Control &= ~(USB4HR_DESC_LENGTH_MASK | USB4HR_DESC_EOF_PDF_MASK | USB4HR_DESC_DONE);
    Control |= Length | (Pdf << USB4HR_DESC_EOF_PDF_SHIFT);
    if (m_Hardware->HasShimFlag(USB4HR_SHIM_TX_REQUEST_STATUS))
        Control |= USB4HR_DESC_REQUEST_STATUS;
    m_TxDesc[Producer].Control = Control;
    KeMemoryBarrier();

    m_Hardware->Write32(RING0_TX_INDEXES, Next << USB4HR_RING_PRODUCER_SHIFT);
    return STATUS_SUCCESS;
}

VOID
NTAPI
Usb4HrRingZero::OnRxInterrupt(
    _In_ PVOID Context)
{
    static_cast<Usb4HrRingZero*>(Context)->ProcessReceived();
}

VOID Usb4HrRingZero::ProcessReceived()
{
    ULONG Indexes;
    ULONG Consumer;
    ULONG Producer;
    ULONG Index;
    ULONG Control;
    ULONG Polls;

    Usb4HrSpinLockGuard Guard(&m_RxLock);

    if (!m_RxRunning)
        return;

    Indexes = m_Hardware->Read32(RING0_RX_INDEXES);
    Consumer = Indexes & USB4HR_RING_CONSUMER_MASK;
    Producer = Indexes >> USB4HR_RING_PRODUCER_SHIFT;
    if (Consumer >= USB4HR_RING_ZERO_ENTRIES || Producer >= USB4HR_RING_ZERO_ENTRIES)
    {
        DPRINT1("Receive ring zero indexes out of range 0x%08lx, MMIO valid %u\n",
                Indexes, m_Hardware->MmioResponds());
        return;
    }

    /* Frames between the consumer and the producer are filled by the host interface */
    for (Index = (Consumer + 1) % USB4HR_RING_ZERO_ENTRIES;
         Index != Producer && m_RxRunning;
         Index = (Index + 1) % USB4HR_RING_ZERO_ENTRIES)
    {
        Control = m_RxDesc[Index].Control;
        for (Polls = 0; !(Control & USB4HR_DESC_DONE) && Polls < RING0_DONE_POLLS; Polls++)
        {
            KeStallExecutionProcessor(RING0_DONE_POLL_US);
            Control = m_RxDesc[Index].Control;
        }

        if (!(Control & USB4HR_DESC_DONE))
        {
            DPRINT1("Receive descriptor %lu not done\n", Index);
            break;
        }

        KeMemoryBarrier();
        DeliverFrame(Index, Control);

        m_RxDesc[Index].Control = USB4HR_DESC_REQUEST_STATUS | USB4HR_DESC_INTERRUPT_ENABLE;
        m_RxDesc[Index].Timestamp = 0;
        KeMemoryBarrier();

        if (m_RxRunning)
            m_Hardware->Write32(RING0_RX_INDEXES, Index);
    }
}

VOID
Usb4HrRingZero::DeliverFrame(
    _In_ ULONG Index,
    _In_ ULONG Control)
{
    const UCHAR* Frame = m_RxFrames + Index * USB4HR_FRAME_SIZE;
    ULONG Length = Control & USB4HR_DESC_LENGTH_MASK;
    ULONG Pdf = (Control & USB4HR_DESC_EOF_PDF_MASK) >> USB4HR_DESC_EOF_PDF_SHIFT;
    Usb4HrRxPacket Packet;
    ULONG Header;
    ULONG Payload;
    ULONG Slot;
    ULONG Dword;

    if (Length > USB4HR_FRAME_SIZE)
    {
        DPRINT1("Receive frame of %lu bytes is larger than a control packet\n", Length);
        return;
    }

    switch (Pdf)
    {
        case USB4HR_PDF_READ:
            if (Length < RING0_MIN_PACKET_BYTES || ((Length - RING0_MIN_PACKET_BYTES) % sizeof(ULONG)) != 0)
            {
                DPRINT1("Read response of %lu bytes dropped\n", Length);
                return;
            }

            Payload = (Length - RING0_MIN_PACKET_BYTES) / sizeof(ULONG);
            Header = Usb4HrFrameDword(Frame + 2 * sizeof(ULONG));
            if (Payload > USB4HR_MAX_CONFIG_DWORDS ||
                Payload != ((Header & USB4HR_CFG_LENGTH_MASK) >> USB4HR_CFG_LENGTH_SHIFT))
            {
                DPRINT1("Read response with %lu data dwords and header 0x%08lx dropped\n", Payload, Header);
                return;
            }
            break;

        case USB4HR_PDF_WRITE:
        case USB4HR_PDF_NOTIFICATION:
        case USB4HR_PDF_HOT_PLUG:
            if (Length < RING0_MIN_PACKET_BYTES)
            {
                DPRINT1("PDF %lu packet of %lu bytes dropped\n", Pdf, Length);
                return;
            }
            break;

        case USB4HR_PDF_XDOMAIN_REQUEST:
        case USB4HR_PDF_XDOMAIN_RESPONSE:
            DPRINT("Inter-domain packet dropped, not supported\n");
            return;

        default:
            DPRINT1("Packet with unexpected PDF %lu dropped\n", Pdf);
            return;
    }

    if (Usb4HrCrc32c(Frame, Length - USB4HR_CRC_BYTES) != Usb4HrFrameDword(Frame + Length - USB4HR_CRC_BYTES))
    {
        DPRINT1("PDF %lu packet with a bad CRC dropped\n", Pdf);
        return;
    }

    RtlZeroMemory(&Packet, sizeof(Packet));
    Packet.Pdf = Pdf;
    Packet.DwordCount = (Length - USB4HR_CRC_BYTES) / sizeof(ULONG);
    for (Dword = 0; Dword < Packet.DwordCount; Dword++)
        Packet.Dword[Dword] = Usb4HrFrameDword(Frame + Dword * sizeof(ULONG));

    for (Slot = 0; Slot < USB4HR_RX_HANDLERS_PER_PDF; Slot++)
    {
        if (m_Handlers[Pdf][Slot].Handler != NULL)
            m_Handlers[Pdf][Slot].Handler(m_Handlers[Pdf][Slot].Context, &Packet);
    }
}
