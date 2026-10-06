/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Ring zero: control packet transmit and receive with CRC
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#define USB4HR_PACKET_MAX_DWORDS        (USB4HR_FRAME_SIZE / sizeof(ULONG))
#define USB4HR_RX_HANDLERS_PER_PDF      2

/** A received control packet in host byte order, CRC checked and removed. */
struct Usb4HrRxPacket
{
    ULONG Pdf;
    ULONG DwordCount;   /**< route string included, CRC excluded */
    ULONG Dword[USB4HR_PACKET_MAX_DWORDS];
};

/** Receives every packet of one PDF, at DISPATCH_LEVEL from the ring DPC. The packet is only valid during the call. */
typedef VOID
(NTAPI USB4HR_RX_HANDLER)(
    _In_ PVOID Context,
    _In_ const Usb4HrRxPacket* Packet);
typedef USB4HR_RX_HANDLER *PUSB4HR_RX_HANDLER;

/** CRC-32C as control packets use it, over Length bytes in wire (big endian) order. */
ULONG
NTAPI
Usb4HrCrc32c(
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG Length);

/** Route string to the two leading packet dwords, high dword first, CM bit clear. */
VOID
NTAPI
Usb4HrRouteToPacket(
    _In_ const Usb4HrRoute* Route,
    _Out_writes_(2) PULONG Dwords);

/** Route of a received packet; the depth is the last nonzero hop. */
VOID
NTAPI
Usb4HrRouteFromPacket(
    _In_reads_(2) const ULONG* Dwords,
    _Out_ Usb4HrRoute* Route);

/** Dword 2 of a read or write request. */
ULONG
NTAPI
Usb4HrConfigRequestHeader(
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _In_ UCHAR Adapter,
    _In_ ULONG Space,
    _In_ UCHAR Sequence);

/** Transmit and receive ring 0 of the host interface. */
class Usb4HrRingZero
{
public:
    /** DMA enabler and the common buffers of both rings. Called from EvtDriverDeviceAdd. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Create(
        _In_ Usb4HrHostRouter* HostRouter);

    /** Adds a receive handler for Pdf. Before Start only. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    RegisterHandler(
        _In_ ULONG Pdf,
        _In_ PUSB4HR_RX_HANDLER Handler,
        _In_ PVOID Context);

    /** Programs both rings in raw mode, posts the receive buffers and unmasks their causes. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS Start();

    /** Waits up to 1 s for queued transmits, then disables both rings. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Stop();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    BOOLEAN IsRunning() const;

    /**
     * Queues one control packet. Dwords are in host order without the CRC;
     * STATUS_DEVICE_NOT_READY when stopped, STATUS_DEVICE_BUSY when the ring is full.
     */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    Send(
        _In_ ULONG Pdf,
        _In_reads_(DwordCount) const ULONG* Dwords,
        _In_ ULONG DwordCount);

private:
    /* The accessor drains the receive ring before it declares a response lost */
    friend class Usb4HrConfigAccessor;

    struct RxHandlerSlot
    {
        PUSB4HR_RX_HANDLER Handler;
        PVOID Context;
    };

    static USB4HR_INTERRUPT_HANDLER OnRxInterrupt;

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    AllocateBuffer(
        _In_ size_t Length,
        _Out_ WDFCOMMONBUFFER* Buffer,
        _Out_ PVOID* VirtualAddress,
        _Out_ PHYSICAL_ADDRESS* LogicalAddress);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS StartTransmit();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS StartReceive();

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID StopTransmit();

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID StopReceive();

    /** Walks completed receive descriptors and hands each packet to its handlers. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID ProcessReceived();

    _IRQL_requires_(DISPATCH_LEVEL)
    VOID
    DeliverFrame(
        _In_ ULONG Index,
        _In_ ULONG Control);

    Usb4HrHostRouter* m_HostRouter;
    Usb4HrHardware* m_Hardware;
    WDFDMAENABLER m_DmaEnabler;

    WDFCOMMONBUFFER m_TxDescBuffer;
    WDFCOMMONBUFFER m_TxFrameBuffer;
    volatile USB4HR_BUFFER_DESCRIPTOR* m_TxDesc;
    PUCHAR m_TxFrames;
    PHYSICAL_ADDRESS m_TxDescAddress;

    WDFCOMMONBUFFER m_RxDescBuffer;
    WDFCOMMONBUFFER m_RxFrameBuffer;
    volatile USB4HR_BUFFER_DESCRIPTOR* m_RxDesc;
    PUCHAR m_RxFrames;
    PHYSICAL_ADDRESS m_RxDescAddress;

    KSPIN_LOCK m_TxLock;
    KSPIN_LOCK m_RxLock;
    volatile BOOLEAN m_TxRunning;
    volatile BOOLEAN m_RxRunning;
    ULONG m_RxCause;

    RxHandlerSlot m_Handlers[USB4HR_PDF_COUNT][USB4HR_RX_HANDLERS_PER_PDF];
};
