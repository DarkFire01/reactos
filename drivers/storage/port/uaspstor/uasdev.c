/*
 * PROJECT:     ReactOS USB Attached SCSI Miniport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Bringing the device up
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "uaspstor.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

/**
 * @brief Reads the device descriptor and the whole configuration descriptor.
 *
 * The configuration descriptor is read twice, because its real length is only
 * known once the first nine bytes of it are in hand.
 */
static
NTSTATUS
UaspGetDescriptors(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    PUSB_CONFIGURATION_DESCRIPTOR Partial;
    USHORT TotalLength;
    NTSTATUS Status;

    Status = UaspGetDescriptor(Adapter,
                               USB_DEVICE_DESCRIPTOR_TYPE,
                               0,
                               sizeof(USB_DEVICE_DESCRIPTOR),
                               (PVOID *)&Adapter->DeviceDescriptor);
    if (!NT_SUCCESS(Status))
        return Status;

    DPRINT1("USB device %04x:%04x, USB %x.%02x\n",
            Adapter->DeviceDescriptor->idVendor,
            Adapter->DeviceDescriptor->idProduct,
            Adapter->DeviceDescriptor->bcdUSB >> 8,
            Adapter->DeviceDescriptor->bcdUSB & 0xFF);

    Status = UaspGetDescriptor(Adapter,
                               USB_CONFIGURATION_DESCRIPTOR_TYPE,
                               0,
                               sizeof(USB_CONFIGURATION_DESCRIPTOR),
                               (PVOID *)&Partial);
    if (!NT_SUCCESS(Status))
        return Status;

    TotalLength = Partial->wTotalLength;
    ExFreePoolWithTag(Partial, TAG_UASP);

    if (TotalLength < sizeof(USB_CONFIGURATION_DESCRIPTOR))
        return STATUS_DEVICE_DATA_ERROR;

    return UaspGetDescriptor(Adapter,
                             USB_CONFIGURATION_DESCRIPTOR_TYPE,
                             0,
                             TotalLength,
                             (PVOID *)&Adapter->ConfigurationDescriptor);
}

/**
 * @brief Picks the UAS interface out of the configuration and reads its pipes.
 *
 * A device that also speaks bulk only offers both from the same interface, one
 * alternate setting each, so the protocol byte is what tells them apart. The
 * endpoint addresses carry no meaning of their own: each is followed by a pipe
 * usage descriptor saying what the protocol uses it for.
 */
static
NTSTATUS
UaspFindInterface(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    /* One entry per endpoint of the interface being walked */
    UCHAR MaxStreams[16];
    PUSB_INTERFACE_DESCRIPTOR Interface = NULL;
    PUSB_ENDPOINT_DESCRIPTOR Endpoint = NULL;
    PUSB_COMMON_DESCRIPTOR Descriptor;
    PUAS_PIPE_USAGE_DESCRIPTOR Usage;
    PUSB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR Companion;
    PUCHAR Current, End;
    UCHAR EndpointIndex = 0;
    UCHAR PipeIndex[5];
    USHORT Offered;
    UCHAR Index;

    RtlFillMemory(PipeIndex, sizeof(PipeIndex), 0xFF);
    RtlZeroMemory(MaxStreams, sizeof(MaxStreams));

    Current = (PUCHAR)Adapter->ConfigurationDescriptor;
    End = Current + Adapter->ConfigurationDescriptor->wTotalLength;

    while (Current + sizeof(USB_COMMON_DESCRIPTOR) <= End)
    {
        Descriptor = (PUSB_COMMON_DESCRIPTOR)Current;

        if (Descriptor->bLength == 0 || Current + Descriptor->bLength > End)
            break;

        switch (Descriptor->bDescriptorType)
        {
            case USB_INTERFACE_DESCRIPTOR_TYPE:
            {
                PUSB_INTERFACE_DESCRIPTOR Candidate = (PUSB_INTERFACE_DESCRIPTOR)Descriptor;

                if (Interface != NULL)
                {
                    /* The interface we took is over, and so is the walk */
                    Current = End;
                    continue;
                }

                if (Descriptor->bLength >= sizeof(USB_INTERFACE_DESCRIPTOR) &&
                    Candidate->bInterfaceClass == USB_DEVICE_CLASS_STORAGE &&
                    Candidate->bInterfaceSubClass == 0x06 &&
                    Candidate->bInterfaceProtocol == UAS_INTERFACE_PROTOCOL)
                {
                    Interface = Candidate;
                    EndpointIndex = 0;
                    Endpoint = NULL;
                }
                break;
            }

            case USB_ENDPOINT_DESCRIPTOR_TYPE:
                if (Interface == NULL)
                    break;

                Endpoint = (PUSB_ENDPOINT_DESCRIPTOR)Descriptor;
                EndpointIndex++;
                break;

            case UAS_PIPE_USAGE_DESCRIPTOR_TYPE:
                if (Interface == NULL || Endpoint == NULL ||
                    Descriptor->bLength < sizeof(UAS_PIPE_USAGE_DESCRIPTOR))
                {
                    break;
                }

                Usage = (PUAS_PIPE_USAGE_DESCRIPTOR)Descriptor;

                if (Usage->bPipeId >= UAS_PIPE_ID_COMMAND &&
                    Usage->bPipeId <= UAS_PIPE_ID_DATA_OUT)
                {
                    PipeIndex[Usage->bPipeId] = EndpointIndex - 1;
                }
                break;

            case USB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR_TYPE:
                if (Interface == NULL || Endpoint == NULL ||
                    Descriptor->bLength < sizeof(USB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR) ||
                    EndpointIndex > RTL_NUMBER_OF(MaxStreams))
                {
                    break;
                }

                Companion = (PUSB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR)Descriptor;
                MaxStreams[EndpointIndex - 1] = Companion->bmAttributes.Bulk.MaxStreams;
                break;

            default:
                break;
        }

        Current += Descriptor->bLength;
    }

    if (Interface == NULL)
    {
        DPRINT1("No UAS interface in the configuration\n");
        return STATUS_NO_SUCH_DEVICE;
    }

    if (PipeIndex[UAS_PIPE_ID_COMMAND] == 0xFF || PipeIndex[UAS_PIPE_ID_STATUS] == 0xFF ||
        PipeIndex[UAS_PIPE_ID_DATA_IN] == 0xFF || PipeIndex[UAS_PIPE_ID_DATA_OUT] == 0xFF)
    {
        DPRINT1("The UAS interface is missing a pipe (%u %u %u %u)\n",
                PipeIndex[UAS_PIPE_ID_COMMAND], PipeIndex[UAS_PIPE_ID_STATUS],
                PipeIndex[UAS_PIPE_ID_DATA_IN], PipeIndex[UAS_PIPE_ID_DATA_OUT]);
        return STATUS_NO_SUCH_DEVICE;
    }

    Adapter->InterfaceDescriptor = Interface;
    Adapter->CommandPipeIndex = PipeIndex[UAS_PIPE_ID_COMMAND];
    Adapter->StatusPipeIndex = PipeIndex[UAS_PIPE_ID_STATUS];
    Adapter->DataInPipeIndex = PipeIndex[UAS_PIPE_ID_DATA_IN];
    Adapter->DataOutPipeIndex = PipeIndex[UAS_PIPE_ID_DATA_OUT];

    /*
     * Data and status travel on streams, one per command. The command pipe is
     * shared and needs none, so it is left out of the count. A pipe offering
     * no streams takes the whole device back to one command at a time.
     */
    Offered = 0xFFFF;
    for (Index = 0; Index < 3; Index++)
    {
        static const UCHAR Streamed[3] =
        {
            UAS_PIPE_ID_STATUS, UAS_PIPE_ID_DATA_IN, UAS_PIPE_ID_DATA_OUT
        };
        UCHAR Shift = MaxStreams[PipeIndex[Streamed[Index]]];
        USHORT Count;

        if (Shift == 0)
        {
            Offered = 0;
            break;
        }

        /* Stream identifiers run from one, so one of them is never usable */
        Count = (Shift >= 16) ? 0xFFFF : (USHORT)((1 << Shift) - 1);
        if (Count < Offered)
            Offered = Count;
    }

    Adapter->StreamsOffered = (Offered == 0xFFFF) ? 0 : Offered;

    DPRINT1("UAS interface %u alternate %u, pipes cmd %u status %u in %u out %u, %u streams\n",
            Interface->bInterfaceNumber, Interface->bAlternateSetting,
            Adapter->CommandPipeIndex, Adapter->StatusPipeIndex,
            Adapter->DataInPipeIndex, Adapter->DataOutPipeIndex,
            Adapter->StreamsOffered);

    return STATUS_SUCCESS;
}

/**
 * @brief Puts the device into the configuration that holds the UAS interface.
 *
 * What comes back names every pipe of the interface, which is what later
 * transfers are addressed to, so it is kept.
 */
static
NTSTATUS
UaspSelectConfiguration(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    USBD_INTERFACE_LIST_ENTRY InterfaceList[2];
    PUSBD_INTERFACE_INFORMATION Information;
    NTSTATUS Status;
    PURB Urb;

    RtlZeroMemory(InterfaceList, sizeof(InterfaceList));
    InterfaceList[0].InterfaceDescriptor = Adapter->InterfaceDescriptor;

    /* The device is asked for this one interface and nothing else */
    Adapter->ConfigurationDescriptor->bNumInterfaces = 1;

    Status = USBD_SelectConfigUrbAllocateAndBuild(Adapter->UsbdHandle,
                                                  Adapter->ConfigurationDescriptor,
                                                  InterfaceList,
                                                  &Urb);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Building the select configuration request failed (Status 0x%08lx)\n", Status);
        return Status;
    }

    Status = UaspSendUrbSynchronously(Adapter, Urb, TRUE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Selecting the configuration failed (Status 0x%08lx, USBD 0x%08lx)\n",
                Status, Urb->UrbHeader.Status);
        USBD_UrbFree(Adapter->UsbdHandle, Urb);
        return Status;
    }

    Adapter->ConfigurationHandle = Urb->UrbSelectConfiguration.ConfigurationHandle;

    Information = InterfaceList[0].Interface;

    Adapter->InterfaceInformation = ExAllocatePoolWithTag(NonPagedPool,
                                                          Information->Length,
                                                          TAG_UASP);
    if (Adapter->InterfaceInformation == NULL)
    {
        USBD_UrbFree(Adapter->UsbdHandle, Urb);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(Adapter->InterfaceInformation, Information, Information->Length);

    USBD_UrbFree(Adapter->UsbdHandle, Urb);

    return STATUS_SUCCESS;
}

/**
 * @brief Matches the pipes the stack opened against the roles we found.
 */
static
NTSTATUS
UaspResolvePipes(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    PUSBD_INTERFACE_INFORMATION Information = Adapter->InterfaceInformation;
    PUSBD_PIPE_INFORMATION Pipe;

    if (Adapter->CommandPipeIndex >= Information->NumberOfPipes ||
        Adapter->StatusPipeIndex >= Information->NumberOfPipes ||
        Adapter->DataInPipeIndex >= Information->NumberOfPipes ||
        Adapter->DataOutPipeIndex >= Information->NumberOfPipes)
    {
        DPRINT1("The interface opened with %u pipes, fewer than the descriptors named\n",
                Information->NumberOfPipes);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Pipe = &Information->Pipes[Adapter->CommandPipeIndex];
    if (Pipe->PipeType != UsbdPipeTypeBulk || USB_ENDPOINT_DIRECTION_IN(Pipe->EndpointAddress))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    Adapter->CommandPipe = Pipe->PipeHandle;

    Pipe = &Information->Pipes[Adapter->StatusPipeIndex];
    if (Pipe->PipeType != UsbdPipeTypeBulk || !USB_ENDPOINT_DIRECTION_IN(Pipe->EndpointAddress))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    Adapter->StatusPipe = Pipe->PipeHandle;

    Pipe = &Information->Pipes[Adapter->DataInPipeIndex];
    if (Pipe->PipeType != UsbdPipeTypeBulk || !USB_ENDPOINT_DIRECTION_IN(Pipe->EndpointAddress))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    Adapter->DataInPipe = Pipe->PipeHandle;

    Pipe = &Information->Pipes[Adapter->DataOutPipeIndex];
    if (Pipe->PipeType != UsbdPipeTypeBulk || USB_ENDPOINT_DIRECTION_IN(Pipe->EndpointAddress))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    Adapter->DataOutPipe = Pipe->PipeHandle;

    return STATUS_SUCCESS;
}

/**
 * @brief Settles how many commands may be outstanding at once.
 *
 * Streams are what lets a device answer several commands at a time, and the
 * controller has a say in how many. Without them the driver keeps one command
 * in flight, since nothing else would tell the answers apart.
 */
static
VOID
UaspQueryStreamSupport(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    USHORT Granted = 0;
    NTSTATUS Status;

    Adapter->StreamCount = 0;

    if (Adapter->StreamsOffered == 0)
    {
        DPRINT1("The device offers no streams, one command at a time\n");
        Adapter->RequestCount = UASP_NO_STREAM_REQUESTS;
        return;
    }

    Status = USBD_QueryUsbCapability(Adapter->UsbdHandle,
                                     &GUID_USB_CAPABILITY_STATIC_STREAMS,
                                     sizeof(Granted),
                                     (PUCHAR)&Granted,
                                     NULL);
    if (!NT_SUCCESS(Status) || Granted == 0)
    {
        DPRINT1("The controller grants no streams (Status 0x%08lx), one command at a time\n",
                Status);
        Adapter->RequestCount = UASP_NO_STREAM_REQUESTS;
        return;
    }

    Adapter->StreamCount = min(Granted, Adapter->StreamsOffered);
    Adapter->StreamCount = min(Adapter->StreamCount, UASP_MAX_REQUESTS);
    Adapter->RequestCount = Adapter->StreamCount;

    DPRINT1("Device offers %u streams, controller grants %u, using %u\n",
            Adapter->StreamsOffered, Granted, Adapter->StreamCount);
}

/**
 * @brief Opens one stream per command on a pipe and hands the pipes out.
 *
 * @param Offset Where in a request the stream pipe of this pipe is kept.
 */
static
NTSTATUS
UaspOpenStreamsOnPipe(
    _In_ PUASP_ADAPTER_EXTENSION Adapter,
    _In_ USBD_PIPE_HANDLE Pipe,
    _In_ ULONG Offset)
{
    PUSBD_STREAM_INFORMATION Streams;
    ULONG Length;
    NTSTATUS Status;
    USHORT Index;
    PURB Urb;

    Length = Adapter->StreamCount * sizeof(USBD_STREAM_INFORMATION);

    Streams = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_UASP);
    if (Streams == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Streams, Length);

    Status = USBD_UrbAllocate(Adapter->UsbdHandle, &Urb);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Streams, TAG_UASP);
        return Status;
    }

    RtlZeroMemory(Urb, sizeof(struct _URB_OPEN_STATIC_STREAMS));

    Urb->UrbOpenStaticStreams.Hdr.Length = sizeof(struct _URB_OPEN_STATIC_STREAMS);
    Urb->UrbOpenStaticStreams.Hdr.Function = URB_FUNCTION_OPEN_STATIC_STREAMS;
    Urb->UrbOpenStaticStreams.PipeHandle = Pipe;
    Urb->UrbOpenStaticStreams.NumberOfStreams = Adapter->StreamCount;
    Urb->UrbOpenStaticStreams.StreamInfoVersion = 1;
    Urb->UrbOpenStaticStreams.StreamInfoSize = sizeof(USBD_STREAM_INFORMATION);
    Urb->UrbOpenStaticStreams.Streams = Streams;

    Status = UaspSendUrbSynchronously(Adapter, Urb, TRUE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Opening streams on pipe %p failed (Status 0x%08lx, USBD 0x%08lx)\n",
                Pipe, Status, Urb->UrbHeader.Status);
        USBD_UrbFree(Adapter->UsbdHandle, Urb);
        ExFreePoolWithTag(Streams, TAG_UASP);
        return Status;
    }

    if (Urb->UrbOpenStaticStreams.NumberOfStreams != Adapter->StreamCount)
    {
        DPRINT1("Asked for %u streams and got %lu\n",
                Adapter->StreamCount, Urb->UrbOpenStaticStreams.NumberOfStreams);
        USBD_UrbFree(Adapter->UsbdHandle, Urb);
        ExFreePoolWithTag(Streams, TAG_UASP);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Stream n answers the command holding tag n, so they line up in order */
    for (Index = 0; Index < Adapter->StreamCount; Index++)
    {
        *(USBD_PIPE_HANDLE *)((PUCHAR)&Adapter->Requests[Index] + Offset) =
            Streams[Index].PipeHandle;
    }

    USBD_UrbFree(Adapter->UsbdHandle, Urb);
    ExFreePoolWithTag(Streams, TAG_UASP);

    return STATUS_SUCCESS;
}

static
NTSTATUS
UaspOpenStreams(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    NTSTATUS Status;

    Status = UaspOpenStreamsOnPipe(Adapter,
                                   Adapter->DataInPipe,
                                   FIELD_OFFSET(UASP_REQUEST, DataInStream));
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UaspOpenStreamsOnPipe(Adapter,
                                   Adapter->DataOutPipe,
                                   FIELD_OFFSET(UASP_REQUEST, DataOutStream));
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UaspOpenStreamsOnPipe(Adapter,
                                   Adapter->StatusPipe,
                                   FIELD_OFFSET(UASP_REQUEST, StatusStream));
    if (!NT_SUCCESS(Status))
        return Status;

    Adapter->StreamsOpen = TRUE;

    return STATUS_SUCCESS;
}

/**
 * @brief Brings the device from freshly enumerated to ready for commands.
 */
NTSTATUS
UaspStartDevice(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    NTSTATUS Status;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    Status = USBD_CreateHandle(Adapter->DeviceObject,
                               Adapter->LowerDeviceObject,
                               USBD_CLIENT_CONTRACT_VERSION_602,
                               TAG_UASP,
                               &Adapter->UsbdHandle);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("USBD_CreateHandle() failed (Status 0x%08lx)\n", Status);
        return Status;
    }

    Status = UaspGetDescriptors(Adapter);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UaspFindInterface(Adapter);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UaspSelectConfiguration(Adapter);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UaspResolvePipes(Adapter);
    if (!NT_SUCCESS(Status))
        return Status;

    UaspQueryStreamSupport(Adapter);

    Status = UaspCreateRequests(Adapter);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Adapter->StreamCount != 0)
    {
        Status = UaspOpenStreams(Adapter);
        if (!NT_SUCCESS(Status))
        {
            /*
             * The device said it had streams and then would not open them.
             * One command at a time still works, so fall back rather than
             * refuse the device.
             */
            DPRINT1("Falling back to one command at a time\n");

            UaspDeleteRequests(Adapter);

            Adapter->StreamCount = 0;
            Adapter->RequestCount = UASP_NO_STREAM_REQUESTS;

            Status = UaspCreateRequests(Adapter);
            if (!NT_SUCCESS(Status))
                return Status;
        }
    }

    Adapter->QueueState = UaspQueueRunning;
    Adapter->Started = TRUE;

    DPRINT1("UAS device ready, %u command(s) at a time\n", Adapter->RequestCount);

    return STATUS_SUCCESS;
}

/**
 * @brief Lets go of everything the device brought up.
 */
VOID
UaspStopDevice(
    _In_ PUASP_ADAPTER_EXTENSION Adapter)
{
    Adapter->Removing = TRUE;

    UaspFreezeQueue(Adapter);
    UaspDeleteRequests(Adapter);

    if (Adapter->UsbdHandle != NULL)
    {
        USBD_CloseHandle(Adapter->UsbdHandle);
        Adapter->UsbdHandle = NULL;
    }

    if (Adapter->InterfaceInformation != NULL)
    {
        ExFreePoolWithTag(Adapter->InterfaceInformation, TAG_UASP);
        Adapter->InterfaceInformation = NULL;
    }

    if (Adapter->ConfigurationDescriptor != NULL)
    {
        ExFreePoolWithTag(Adapter->ConfigurationDescriptor, TAG_UASP);
        Adapter->ConfigurationDescriptor = NULL;
        Adapter->InterfaceDescriptor = NULL;
    }

    if (Adapter->DeviceDescriptor != NULL)
    {
        ExFreePoolWithTag(Adapter->DeviceDescriptor, TAG_UASP);
        Adapter->DeviceDescriptor = NULL;
    }

    Adapter->Started = FALSE;
}
