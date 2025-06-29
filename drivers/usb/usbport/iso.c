/*
 * PROJECT:     ReactOS USB Port Driver
 * LICENSE:     MIT License (MIT)
 * PURPOSE:     USBPort isochronous transfer functions
 * COPYRIGHT:   Copyright 2017 Vadim Galyant <vgal@rambler.ru>
 */

#include "usbport.h"

#define NDEBUG
#include <debug.h>

USBD_STATUS
NTAPI
USBPORT_InitializeIsoTransfer(PDEVICE_OBJECT FdoDevice,
                              struct _URB_ISOCH_TRANSFER * Urb,
                              PUSBPORT_TRANSFER Transfer)
{
    PUSBPORT_DEVICE_EXTENSION FdoExtension;
    PUSBPORT_ENDPOINT Endpoint;
    PUSBPORT_REGISTRATION_PACKET Packet;
    USBD_ISO_PACKET_DESCRIPTOR *PacketDescriptor;
    ULONG i;
    ULONG TotalLength = 0;
    MPSTATUS MpStatus;

    DPRINT("USBPORT_InitializeIsoTransfer: FdoDevice - %p, Urb - %p\n", FdoDevice, Urb);

    FdoExtension = FdoDevice->DeviceExtension;
    Packet = &FdoExtension->MiniPortInterface->Packet;
    Endpoint = Transfer->Endpoint;

    // Validate parameters
    if (!Urb || !Transfer || !Endpoint)
    {
        DPRINT1("USBPORT_InitializeIsoTransfer: Invalid parameters\n");
        return USBD_STATUS_INVALID_PARAMETER;
    }

    // Check if this is an isochronous endpoint
    if (Endpoint->EndpointProperties.TransferType != USBPORT_TRANSFER_TYPE_ISOCHRONOUS)
    {
        DPRINT1("USBPORT_InitializeIsoTransfer: Not an isochronous endpoint\n");
        return USBD_STATUS_INVALID_PIPE_HANDLE;
    }

    // Validate number of packets
    if (Urb->NumberOfPackets == 0 || Urb->NumberOfPackets > 255)
    {
        DPRINT1("USBPORT_InitializeIsoTransfer: Invalid NumberOfPackets: %lu\n", Urb->NumberOfPackets);
        return USBD_STATUS_INVALID_PARAMETER;
    }

    // Initialize packet descriptors and calculate total length
    for (i = 0; i < Urb->NumberOfPackets; i++)
    {
        PacketDescriptor = &Urb->IsoPacket[i];
        
        // Validate packet length
        if (PacketDescriptor->Length > Endpoint->EndpointProperties.MaxPacketSize)
        {
            DPRINT1("USBPORT_InitializeIsoTransfer: Packet %lu length %lu exceeds max packet size %lu\n",
                    i, PacketDescriptor->Length, Endpoint->EndpointProperties.MaxPacketSize);
            return USBD_STATUS_INVALID_PARAMETER;
        }

        PacketDescriptor->Status = USBD_STATUS_NOT_ACCESSED;
        TotalLength += PacketDescriptor->Length;
    }

    // Validate total transfer length
    if (TotalLength > Urb->TransferBufferLength)
    {
        DPRINT1("USBPORT_InitializeIsoTransfer: Total packet length %lu exceeds buffer length %lu\n",
                TotalLength, Urb->TransferBufferLength);
        return USBD_STATUS_INVALID_PARAMETER;
    }

    // Set up transfer parameters for isochronous transfer
    Transfer->TransferParameters.TransferFlags = USBPORT_TRANSFER_FLAGS_ISO;
    Transfer->TransferParameters.TransferBufferLength = Urb->TransferBufferLength;
    Transfer->TransferParameters.TransferCounter = 0;
    Transfer->TransferParameters.IsTransferSplited = FALSE;

    // Submit to miniport if it supports ISO transfers
    if (Packet->SubmitIsoTransfer)
    {
        KIRQL OldIrql;
        
        KeAcquireSpinLock(&FdoExtension->MiniportSpinLock, &OldIrql);
        
        MpStatus = Packet->SubmitIsoTransfer(FdoExtension->MiniPortExt,
                                           Endpoint + 1,
                                           &Transfer->TransferParameters,
                                           Transfer->MiniportTransfer,
                                           Urb);
                                           
        KeReleaseSpinLock(&FdoExtension->MiniportSpinLock, OldIrql);

        if (MpStatus != MP_STATUS_SUCCESS)
        {
            DPRINT1("USBPORT_InitializeIsoTransfer: SubmitIsoTransfer failed with status %x\n", MpStatus);
            return USBD_STATUS_INTERNAL_HC_ERROR;
        }
    }
    else
    {
        DPRINT1("USBPORT_InitializeIsoTransfer: Miniport does not support ISO transfers\n");
        return USBD_STATUS_NOT_SUPPORTED;
    }

    DPRINT("USBPORT_InitializeIsoTransfer: Successfully initialized ISO transfer\n");
    return USBD_STATUS_SUCCESS;
}

ULONG
NTAPI
USBPORT_CompleteIsoTransfer(IN PVOID MiniPortExtension,
                            IN PVOID MiniPortEndpoint,
                            IN PVOID TransferParameters,
                            IN ULONG TransferLength)
{
    PUSBPORT_DEVICE_EXTENSION FdoExtension;
    PUSBPORT_ENDPOINT Endpoint;
    PUSBPORT_TRANSFER Transfer;
    struct _URB_ISOCH_TRANSFER *IsoUrb;
    USBD_ISO_PACKET_DESCRIPTOR *PacketDescriptor;
    ULONG i;
    ULONG CompletedLength = 0;
    ULONG RemainingLength = TransferLength;

    DPRINT("USBPORT_CompleteIsoTransfer: TransferLength - %lu\n", TransferLength);

    FdoExtension = (PUSBPORT_DEVICE_EXTENSION)((ULONG_PTR)MiniPortExtension -
                                               sizeof(USBPORT_DEVICE_EXTENSION));

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
    for (i = 0; i < IsoUrb->NumberOfPackets && RemainingLength > 0; i++)
    {
        PacketDescriptor = &IsoUrb->IsoPacket[i];
        
        if (RemainingLength >= PacketDescriptor->Length)
        {
            PacketDescriptor->Status = USBD_STATUS_SUCCESS;
            CompletedLength += PacketDescriptor->Length;
            RemainingLength -= PacketDescriptor->Length;
        }
        else
        {
            PacketDescriptor->Status = USBD_STATUS_SUCCESS;
            CompletedLength += RemainingLength;
            RemainingLength = 0;
        }
    }

    // Mark remaining packets as not processed if any
    for (; i < IsoUrb->NumberOfPackets; i++)
    {
        PacketDescriptor = &IsoUrb->IsoPacket[i];
        PacketDescriptor->Status = USBD_STATUS_NOT_ACCESSED;
    }

    // Complete the transfer
    USBPORT_MiniportCompleteTransfer(MiniPortExtension,
                                     MiniPortEndpoint,
                                     TransferParameters,
                                     USBD_STATUS_SUCCESS,
                                     CompletedLength);

    DPRINT("USBPORT_CompleteIsoTransfer: Completed %lu bytes\n", CompletedLength);
    return CompletedLength;
}

