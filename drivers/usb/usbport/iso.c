/*
 * PROJECT:     ReactOS USB Port Driver
 * LICENSE:     MIT License (MIT)
 * PURPOSE:     USBPort isochronous transfer functions
 * COPYRIGHT:   Copyright 2017 Vadim Galyant <vgal@rambler.ru>
 */

#include "usbport.h"

#define NDEBUG
#include <debug.h>

/*
 * USBPORT_LookupSgPhysicalAddr - Walk the scatter/gather table
 * to translate a byte offset within the transfer buffer into
 * the corresponding physical address.  Also returns which SG
 * entry the offset falls in through *OutEntry.
 */
static
PHYSICAL_ADDRESS
USBPORT_LookupSgPhysicalAddr(
    IN PUSBPORT_SCATTER_GATHER_LIST SgTable,
    IN ULONG ByteOffset,
    OUT PULONG OutEntry)
{
    PHYSICAL_ADDRESS Result;
    ULONG Idx = 0;
    ULONG Count = SgTable->SgElementCount;

    while (Idx < Count)
    {
        ULONG EntryStart = SgTable->SgElement[Idx].SgOffset;
        ULONG EntryEnd   = EntryStart + SgTable->SgElement[Idx].SgTransferLength;

        if (ByteOffset >= EntryStart && ByteOffset < EntryEnd)
        {
            Result = SgTable->SgElement[Idx].SgPhysicalAddress;
            Result.LowPart += (ByteOffset - EntryStart);
            *OutEntry = Idx;
            return Result;
        }
        Idx++;
    }

    /* No matching entry - return zero (caller guarantees valid offsets) */
    Result.QuadPart = 0;
    *OutEntry = 0;
    return Result;
}

USBD_STATUS
NTAPI
USBPORT_InitializeIsoTransfer(PDEVICE_OBJECT FdoDevice,
                              struct _URB_ISOCH_TRANSFER * Urb,
                              PUSBPORT_TRANSFER Transfer)
{
    PUSBPORT_ENDPOINT Endpoint;
    PUSBPORT_ISO_TRANSFER_DATA IsoBlock;
    PUSBPORT_SCATTER_GATHER_LIST SgTable;
    ULONG TotalPackets, Idx;
    ULONG Period;
    BOOLEAN IsHighSpeed;
    PUSBPORT_DEVICE_EXTENSION FdoExtension;
    PUSBPORT_REGISTRATION_PACKET Packet;

    DPRINT("USBPORT_InitializeIsoTransfer: FdoDevice - %p, Urb - %p Irp - %p\n", FdoDevice, Urb, Transfer->Irp);

    Endpoint = Transfer->Endpoint;

    if (!Urb || !Transfer || !Endpoint)
    {
        DPRINT1("USBPORT_InitializeIsoTransfer: Invalid parameters\n");
        return USBD_STATUS_INVALID_PARAMETER;
    }

    if (Endpoint->EndpointProperties.TransferType != USBPORT_TRANSFER_TYPE_ISOCHRONOUS)
    {
        DPRINT1("USBPORT_InitializeIsoTransfer: Not an isochronous endpoint\n");
        return USBD_STATUS_INVALID_PIPE_HANDLE;
    }

    TotalPackets = Urb->NumberOfPackets;
    if (TotalPackets == 0 || TotalPackets > 1024)
    {
        DPRINT1("USBPORT_InitializeIsoTransfer: Invalid NumberOfPackets: %lu\n", TotalPackets);
        return USBD_STATUS_INVALID_PARAMETER;
    }

    IsoBlock = (PUSBPORT_ISO_TRANSFER_DATA)Transfer->IsoBlockPtr;
    if (!IsoBlock)
    {
        DPRINT1("USBPORT_InitializeIsoTransfer: No IsoBlock allocated\n");
        return USBD_STATUS_INSUFFICIENT_RESOURCES;
    }

    SgTable = &Transfer->SgList;
    Period = Endpoint->EndpointProperties.Period;
    IsHighSpeed = (Endpoint->EndpointProperties.DeviceSpeed == UsbHighSpeed);

    IsoBlock->TotalPackets = TotalPackets;
    IsoBlock->MappedBuffer = (PVOID)SgTable->MappedSystemVa;

    if (Urb->TransferFlags & USBD_START_ISO_TRANSFER_ASAP)
    {
        FdoExtension = FdoDevice->DeviceExtension;
        Packet = &FdoExtension->MiniPortInterface->Packet;
        Urb->StartFrame = (Packet->Get32BitFrameNumber(FdoExtension->MiniPortExt) + 64) & 0xFFFFFFF0;
        DPRINT("Urb StartFrame %u\n", Urb->StartFrame);
    }

    /*
     * Walk each URB packet descriptor, compute its actual byte length
     * from the offset array, resolve the physical scatter/gather mapping,
     * and populate the per-packet data for the miniport.
     */
    for (Idx = 0; Idx < TotalPackets; Idx++)
    {
        USBD_ISO_PACKET_DESCRIPTOR *UrbPkt = &Urb->IsoPacket[Idx];
        PUSBPORT_ISO_PACKET_DATA PktData = &IsoBlock->Packets[Idx];
        ULONG PktBytes;
        ULONG MaxPkt = Endpoint->EndpointProperties.TotalMaxPacketSize;

        /*
         * The URB uses offset-based packet boundaries.  Derive
         * each packet's byte count from the gap between consecutive
         * offsets.  The final packet runs to the end of the buffer.
         */
        if (Idx < TotalPackets - 1)
            PktBytes = Urb->IsoPacket[Idx + 1].Offset - UrbPkt->Offset;
        else
            PktBytes = Urb->TransferBufferLength - UrbPkt->Offset;

        if (PktBytes > MaxPkt)
            PktBytes = MaxPkt;

        UrbPkt->Status = USBD_STATUS_NOT_ACCESSED;

        /* Fill in the miniport packet data */
        PktData->PacketLength = PktBytes;
        PktData->BytesTransferred = 0;
        PktData->CompletionStatus = USBD_STATUS_NOT_ACCESSED;

        /* Assign USB frame/microframe indices based on bus speed */
        if (IsHighSpeed)
        {
            ULONG SlotsPerFrame = 8 / Period;
            PktData->FrameNumber = Urb->StartFrame + (Idx / SlotsPerFrame);
            PktData->MicroFrameNumber = Idx % SlotsPerFrame;
        }
        else
        {
            PktData->FrameNumber = Urb->StartFrame + Idx;
            PktData->MicroFrameNumber = 0;
        }

        /*
         * Map the packet's byte range onto physical scatter/gather
         * segments.  Most packets occupy a single segment; packets
         * straddling a page boundary need two.
         */
        if (PktBytes > 0 && SgTable->SgElementCount > 0)
        {
            PHYSICAL_ADDRESS HeadPhys;
            ULONG HeadEntry, TailEntry;

            HeadPhys = USBPORT_LookupSgPhysicalAddr(SgTable,
                                                     UrbPkt->Offset,
                                                     &HeadEntry);
            PktData->Segment0Addr = HeadPhys;
            PktData->Segment0Length = PktBytes;
            PktData->SegmentCount = 1;

            /* Does this packet span into another SG entry? */
            if (PktBytes > 1)
            {
                USBPORT_LookupSgPhysicalAddr(SgTable,
                                              UrbPkt->Offset + PktBytes - 1,
                                              &TailEntry);

                if (TailEntry != HeadEntry)
                {
                    ULONG BytesToPageEnd = PAGE_SIZE - (HeadPhys.LowPart & (PAGE_SIZE - 1));

                    PktData->Segment0Length = BytesToPageEnd;
                    PktData->Segment1Addr = SgTable->SgElement[TailEntry].SgPhysicalAddress;
                    PktData->Segment1Length = PktBytes - BytesToPageEnd;
                    PktData->SegmentCount = 2;
                }
            }
        }
        else
        {
            PktData->PacketLength = 0;
            PktData->Segment0Addr.QuadPart = 0;
            PktData->Segment0Length = 0;
            PktData->SegmentCount = 0;
        }
    }

    /* Enqueue on the endpoint's transfer list for the DMA worker to pick up */
    KeAcquireSpinLock(&Endpoint->EndpointSpinLock,
                      &Endpoint->EndpointOldIrql);

    InsertTailList(&Endpoint->TransferList, &Transfer->TransferLink);

    KeReleaseSpinLock(&Endpoint->EndpointSpinLock,
                      Endpoint->EndpointOldIrql);

    DPRINT("USBPORT_InitializeIsoTransfer: Prepared %lu packets for ISO transfer\n",
           TotalPackets);
    return USBD_STATUS_SUCCESS;
}

ULONG
NTAPI
USBPORT_CompleteIsoTransfer(IN PVOID MiniPortExtension,
                            IN PVOID MiniPortEndpoint,
                            IN PVOID TransferParameters,
                            IN ULONG TransferLength)
{
    PUSBPORT_ENDPOINT Endpoint;
    PUSBPORT_TRANSFER Transfer;
    struct _URB_ISOCH_TRANSFER *IsoUrb;
    USBD_ISO_PACKET_DESCRIPTOR *PacketDescriptor;
    ULONG i;
    ULONG CompletedLength = 0;
    ULONG RemainingLength = TransferLength;

    DPRINT("USBPORT_CompleteIsoTransfer: TransferLength - %lu\n", TransferLength);

    if (!MiniPortEndpoint)
    {
        DPRINT1("USBPORT_CompleteIsoTransfer: Invalid MiniPortEndpoint\n");
        return 0;
    }

    Endpoint = (PUSBPORT_ENDPOINT)((ULONG_PTR)MiniPortEndpoint -
                                   sizeof(USBPORT_ENDPOINT));

    Transfer = CONTAINING_RECORD(TransferParameters,
                                 USBPORT_TRANSFER,
                                 TransferParameters);

    if (!Transfer || !Transfer->Urb)
    {
        DPRINT1("USBPORT_CompleteIsoTransfer: Invalid Transfer or URB\n");
        return 0;
    }

    IsoUrb = (struct _URB_ISOCH_TRANSFER *)Transfer->Urb;

    // Validate this is actually an ISO transfer
    if (IsoUrb->Hdr.Function != URB_FUNCTION_ISOCH_TRANSFER)
    {
        DPRINT1("USBPORT_CompleteIsoTransfer: Not an ISO transfer URB\n");
        return 0;
    }

    // Update packet descriptors with completion status
    i = 0;
    do
    {
        PacketDescriptor = &IsoUrb->IsoPacket[i];

        if (RemainingLength >= Endpoint->EndpointProperties.MaxPacketSize)
        {
            PacketDescriptor->Status = USBD_STATUS_SUCCESS;
            PacketDescriptor->Length = Endpoint->EndpointProperties.MaxPacketSize;
            CompletedLength += PacketDescriptor->Length;
            RemainingLength -= PacketDescriptor->Length;
        }
        else
        {
            PacketDescriptor->Status = USBD_STATUS_SUCCESS;
            PacketDescriptor->Length = RemainingLength;
            CompletedLength += RemainingLength;
            RemainingLength = 0;
            i++;
            break;
        }
    }while(i++ < IsoUrb->NumberOfPackets);

    // Mark remaining packets as not processed if any
    for (; i < IsoUrb->NumberOfPackets; i++)
    {
        PacketDescriptor = &IsoUrb->IsoPacket[i];
        PacketDescriptor->Status = USBD_STATUS_NOT_ACCESSED;
        PacketDescriptor->Length = 0;
    }

    // Complete the transfer
    USBPORT_MiniportCompleteTransfer(MiniPortExtension,
                                     MiniPortEndpoint,
                                     TransferParameters,
                                     USBD_STATUS_SUCCESS,
                                     CompletedLength);

    DPRINT("USBPORT_CompleteIsoTransfer: Completed %lu bytes\n", CompletedLength);
    UNREFERENCED_PARAMETER(Endpoint);
    return CompletedLength;
}

