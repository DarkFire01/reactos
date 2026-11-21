/*
 * PROJECT:     ReactOS Cellular Data Adapter
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     \\.\RmNet, through which the RIL hands over its data session
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "rmnet.h"

#define NDEBUG
#include <debug.h>

static NDIS_HANDLE RmNetControlHandle;
static PDEVICE_OBJECT RmNetControlDevice;

static
ULONG
RmNetByteSwap(
    _In_ ULONG Value)
{
    return ((Value & 0xFF) << 24) | ((Value & 0xFF00) << 8) | ((Value >> 8) & 0xFF00) | (Value >> 24);
}

/* Widens the mask until the gateway is on link, carriers often hand out a /32 */
static
ULONG
RmNetOnLinkMask(
    _In_ ULONG Address,
    _In_ ULONG Gateway,
    _In_ ULONG Mask)
{
    ULONG HostAddress = RmNetByteSwap(Address);
    ULONG HostGateway = RmNetByteSwap(Gateway);
    ULONG HostMask = RmNetByteSwap(Mask);

    if (!Gateway)
        return Mask ? Mask : RmNetByteSwap(0xFFFFFF00);
    if (!HostMask)
        HostMask = 0xFFFFFFFC;

    while (HostMask && ((HostAddress ^ HostGateway) & HostMask))
        HostMask <<= 1;

    /* A point to point peer needs at least a /30 for tcpip to call it on link */
    if (HostMask > 0xFFFFFFFC)
        HostMask = 0xFFFFFFFC;
    while (HostMask && ((HostAddress ^ HostGateway) & HostMask))
        HostMask <<= 1;

    return RmNetByteSwap(HostMask);
}

static
NTSTATUS
RmNetSetLink(
    _In_ PRMNET_ADAPTER Adapter,
    _In_ const RMNET_LINK *Link)
{
    BOOLEAN WasConnected = Adapter->Connected;

    if (Link->Version != RMNET_INTERFACE_VERSION)
        return STATUS_REVISION_MISMATCH;

    if (Link->Up)
    {
        if (!Adapter->IpaRegistered)
            return STATUS_DEVICE_NOT_READY;
        if (!Link->Address)
            return STATUS_INVALID_PARAMETER;

        /* A session that came back with another address is a new link, tcpip must drop the old lease */
        if (WasConnected && Adapter->Link.Address != Link->Address)
        {
            DPRINT1("rmnet: data session replaced, the old address goes away first\n");
            Adapter->Connected = FALSE;
            RmNetIndicateLinkState(Adapter);
            WasConnected = FALSE;
        }

        Adapter->Link = *Link;
        Adapter->Link.SubnetMask = RmNetOnLinkMask(Link->Address, Link->Gateway, Link->SubnetMask);
        if (!Adapter->Link.Mtu || Adapter->Link.Mtu > RMNET_MAX_MTU)
            Adapter->Link.Mtu = RMNET_DEFAULT_MTU;

        Adapter->Connected = TRUE;
        DPRINT1("rmnet: data session up, %u.%u.%u.%u via %u.%u.%u.%u, MTU %lu\n",
                ((PUCHAR)&Link->Address)[0], ((PUCHAR)&Link->Address)[1],
                ((PUCHAR)&Link->Address)[2], ((PUCHAR)&Link->Address)[3],
                ((PUCHAR)&Link->Gateway)[0], ((PUCHAR)&Link->Gateway)[1],
                ((PUCHAR)&Link->Gateway)[2], ((PUCHAR)&Link->Gateway)[3],
                Adapter->Link.Mtu);
    }
    else
    {
        Adapter->Connected = FALSE;
        DPRINT1("rmnet: data session down\n");
    }

    if (WasConnected != Adapter->Connected)
        RmNetIndicateLinkState(Adapter);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
RmNetControlDispatch(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PRMNET_ADAPTER Adapter = RmNetAdapter;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG InLength, OutLength;
    PRMNET_QUERY Query;
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG_PTR Information = 0;

    UNREFERENCED_PARAMETER(DeviceObject);

    switch (Stack->MajorFunction)
    {
        case IRP_MJ_CREATE:
        case IRP_MJ_CLEANUP:
        case IRP_MJ_CLOSE:
            break;

        case IRP_MJ_DEVICE_CONTROL:
            InLength = Stack->Parameters.DeviceIoControl.InputBufferLength;
            OutLength = Stack->Parameters.DeviceIoControl.OutputBufferLength;

            if (!Adapter)
            {
                Status = STATUS_DEVICE_NOT_READY;
                break;
            }

            switch (Stack->Parameters.DeviceIoControl.IoControlCode)
            {
                case IOCTL_RMNET_QUERY:
                    if (OutLength < sizeof(*Query))
                    {
                        Status = STATUS_BUFFER_TOO_SMALL;
                        break;
                    }

                    Query = Buffer;
                    RtlZeroMemory(Query, sizeof(*Query));
                    Query->Version = RMNET_INTERFACE_VERSION;
                    Query->State = Adapter->Connected ? RMNET_STATE_CONNECTED :
                                   Adapter->IpaRegistered ? RMNET_STATE_READY : RMNET_STATE_NO_IPA;
                    Query->MuxId = Adapter->ClientId;
                    Query->EndpointType = RMNET_IPA_ENDPOINT_EMBEDDED;
                    Query->InterfaceId = 0;
                    Query->SentPackets = Adapter->SentPackets;
                    Query->ReceivedPackets = Adapter->ReceivedPackets;
                    Query->DroppedPackets = Adapter->DroppedPackets;
                    Information = sizeof(*Query);
                    break;

                case IOCTL_RMNET_SET_LINK:
                    if (InLength < sizeof(RMNET_LINK))
                        Status = STATUS_BUFFER_TOO_SMALL;
                    else
                        Status = RmNetSetLink(Adapter, Buffer);
                    break;

                default:
                    Status = STATUS_INVALID_DEVICE_REQUEST;
                    break;
            }
            break;

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NDIS_STATUS
NTAPI
RmNetControlCreate(
    _In_ PRMNET_ADAPTER Adapter)
{
    static PDRIVER_DISPATCH Dispatch[IRP_MJ_MAXIMUM_FUNCTION + 1];
    NDIS_DEVICE_OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(L"\\Device\\RmNet");
    UNICODE_STRING LinkName = RTL_CONSTANT_STRING(L"\\DosDevices\\RmNet");
    NDIS_STATUS Status;

    UNREFERENCED_PARAMETER(Adapter);

    Dispatch[IRP_MJ_CREATE] = RmNetControlDispatch;
    Dispatch[IRP_MJ_CLEANUP] = RmNetControlDispatch;
    Dispatch[IRP_MJ_CLOSE] = RmNetControlDispatch;
    Dispatch[IRP_MJ_DEVICE_CONTROL] = RmNetControlDispatch;

    NdisZeroMemory(&Attributes, sizeof(Attributes));
    Attributes.Header.Type = NDIS_OBJECT_TYPE_DEVICE_OBJECT_ATTRIBUTES;
    Attributes.Header.Revision = NDIS_DEVICE_OBJECT_ATTRIBUTES_REVISION_1;
    Attributes.Header.Size = NDIS_SIZEOF_DEVICE_OBJECT_ATTRIBUTES_REVISION_1;
    Attributes.DeviceName = &DeviceName;
    Attributes.SymbolicName = &LinkName;
    Attributes.MajorFunctions = Dispatch;

    Status = NdisRegisterDeviceEx(RmNetDriverHandle, &Attributes, &RmNetControlDevice, &RmNetControlHandle);
    if (Status != NDIS_STATUS_SUCCESS)
        DPRINT1("rmnet: control device failed 0x%lx\n", Status);
    return Status;
}

VOID
NTAPI
RmNetControlDelete(VOID)
{
    if (RmNetControlHandle)
    {
        NdisDeregisterDeviceEx(RmNetControlHandle);
        RmNetControlHandle = NULL;
        RmNetControlDevice = NULL;
    }
}
