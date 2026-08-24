/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/net/afd/afd/info.c
 * PURPOSE:          Ancillary functions driver
 * PROGRAMMER:       Art Yerkes (ayerkes@speakeasy.net)
 * UPDATE HISTORY:
 * 20040708 Created
 */

#include "afd.h"

typedef struct _TCP_KEEPALIVE_VALS {
    ULONG onoff;
    ULONG keepalivetime;
    ULONG keepaliveinterval;
} TCP_KEEPALIVE_VALS;

/* Push a TDI connection option down the socket's own connection object.
 *
 * IOCTL_TCP_SET_INFORMATION_EX used to reach tcpip only from wshtcpip, on the
 * \Device\Tcp control channel, carrying an entity ID picked out of the driver's
 * *global* entity list. That list cannot tell one socket's address file from
 * another's, so the option landed on whichever address file happened to be last
 * - a different socket, or a UDP/ICMP one with no connection at all, which
 * failed the request and surfaced as WSAEINVAL. Sending it down
 * FCB->Connection lets tcpip resolve the target from the file object instead. */
static
NTSTATUS
AfdSetTcpConnectionInfo(PAFD_FCB FCB,
                        ULONG TdiId,
                        PVOID Buffer,
                        ULONG BufferSize)
{
    PTCP_REQUEST_SET_INFORMATION_EX Info;
    PDEVICE_OBJECT DeviceObject;
    IO_STATUS_BLOCK Iosb;
    KEVENT Event;
    PIRP Irp;
    NTSTATUS Status;
    ULONG Size;

    if (!FCB->Connection.Object)
        return STATUS_INVALID_CONNECTION;

    DeviceObject = IoGetRelatedDeviceObject(FCB->Connection.Object);
    if (!DeviceObject)
        return STATUS_INVALID_CONNECTION;

    Size = FIELD_OFFSET(TCP_REQUEST_SET_INFORMATION_EX, Buffer) + BufferSize;

    Info = ExAllocatePoolWithTag(NonPagedPool, Size, TAG_AFD_TCP_SET_INFO);
    if (!Info)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Info, Size);
    Info->ID.toi_entity.tei_entity = CO_TL_ENTITY;
    Info->ID.toi_entity.tei_instance = 0;
    Info->ID.toi_class = INFO_CLASS_PROTOCOL;
    Info->ID.toi_type = INFO_TYPE_CONNECTION;
    Info->ID.toi_id = TdiId;
    Info->BufferSize = BufferSize;
    RtlCopyMemory(Info->Buffer, Buffer, BufferSize);

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    Irp = IoBuildDeviceIoControlRequest(IOCTL_TCP_SET_INFORMATION_EX,
                                        DeviceObject,
                                        Info,
                                        Size,
                                        NULL,
                                        0,
                                        FALSE,
                                        &Event,
                                        &Iosb);
    if (!Irp)
    {
        ExFreePoolWithTag(Info, TAG_AFD_TCP_SET_INFO);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* IoBuildDeviceIoControlRequest() does not fill this in, and it is the
       whole point of going through our own object. */
    IoGetNextIrpStackLocation(Irp)->FileObject = FCB->Connection.Object;

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }

    ExFreePoolWithTag(Info, TAG_AFD_TCP_SET_INFO);

    AFD_DbgPrint(MID_TRACE,("TdiId %u returned 0x%x\n", TdiId, Status));

    return Status;
}

/* Applied once the socket actually has a connection to carry them. */
NTSTATUS
AfdApplyPendingTcpOptions(PAFD_FCB FCB)
{
    TCP_KEEPALIVE_VALS Vals;
    NTSTATUS Status = STATUS_SUCCESS;

    if (FCB->KeepAliveValid)
    {
        Status = AfdSetTcpConnectionInfo(FCB,
                                         TCP_SOCKET_KEEPALIVE,
                                         &FCB->KeepAlive,
                                         sizeof(FCB->KeepAlive));
    }

    if (NT_SUCCESS(Status) && FCB->KeepAliveValsValid)
    {
        Vals.onoff = FCB->KeepAliveValid ? FCB->KeepAlive : 1;
        Vals.keepalivetime = FCB->KeepAliveTime;
        Vals.keepaliveinterval = FCB->KeepAliveInterval;

        Status = AfdSetTcpConnectionInfo(FCB,
                                         TCP_SOCKET_KEEPALIVEVALS,
                                         &Vals,
                                         sizeof(Vals));
    }

    return Status;
}

NTSTATUS NTAPI
AfdGetInfo( PDEVICE_OBJECT DeviceObject, PIRP Irp,
            PIO_STACK_LOCATION IrpSp ) {
    NTSTATUS Status = STATUS_SUCCESS;
    PAFD_INFO InfoReq = LockRequest(Irp, IrpSp, TRUE, NULL);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PLIST_ENTRY CurrentEntry;

    UNREFERENCED_PARAMETER(DeviceObject);

    AFD_DbgPrint(MID_TRACE,("Called %p %x\n", InfoReq,
                            InfoReq ? InfoReq->InformationClass : 0));

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    if (!InfoReq)
        return UnlockAndMaybeComplete(FCB, STATUS_NO_MEMORY, Irp, 0);

    _SEH2_TRY {
        switch( InfoReq->InformationClass ) {
        case AFD_INFO_RECEIVE_WINDOW_SIZE:
            InfoReq->Information.Ulong = FCB->Recv.Size;
            break;

        case AFD_INFO_SEND_WINDOW_SIZE:
            InfoReq->Information.Ulong = FCB->Send.Size;
            AFD_DbgPrint(MID_TRACE,("Send window size %u\n", FCB->Send.Size));
            break;

        case AFD_INFO_GROUP_ID_TYPE:
            InfoReq->Information.LargeInteger.u.HighPart = FCB->GroupType;
            InfoReq->Information.LargeInteger.u.LowPart = FCB->GroupID;
            AFD_DbgPrint(MID_TRACE, ("Group ID: %u Group Type: %u\n", FCB->GroupID, FCB->GroupType));
            break;

        case AFD_INFO_BLOCKING_MODE:
            InfoReq->Information.Boolean = FCB->NonBlocking;
            break;

    case AFD_INFO_INLINING_MODE:
        InfoReq->Information.Boolean = FCB->OobInline;
        break;

    case AFD_INFO_RECEIVE_CONTENT_SIZE:
        InfoReq->Information.Ulong = FCB->Recv.Content - FCB->Recv.BytesUsed;
        break;

        case AFD_INFO_SENDS_IN_PROGRESS:
            InfoReq->Information.Ulong = 0;

            /* Count the queued sends */
            CurrentEntry = FCB->PendingIrpList[FUNCTION_SEND].Flink;
            while (CurrentEntry != &FCB->PendingIrpList[FUNCTION_SEND])
            {
                 InfoReq->Information.Ulong++;
                 CurrentEntry = CurrentEntry->Flink;
            }

        /* This needs to count too because when this is dispatched
         * the user-mode IRP has already been completed and therefore
         * will NOT be in our pending IRP list. We count this as one send
         * outstanding although it could be multiple since we batch sends
         * when waiting for the in flight request to return, so this number
         * may not be accurate but it really doesn't matter that much since
         * it's more or less a zero/non-zero comparison to determine whether
         * we can shutdown the socket
         */
        if (FCB->SendIrp.InFlightRequest)
            InfoReq->Information.Ulong++;
        break;

        default:
            AFD_DbgPrint(MIN_TRACE,("Unknown info id %x\n",
                                    InfoReq->InformationClass));
            Status = STATUS_INVALID_PARAMETER;
            break;
        }
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        AFD_DbgPrint(MIN_TRACE,("Exception executing GetInfo\n"));
        Status = STATUS_INVALID_PARAMETER;
    } _SEH2_END;

    AFD_DbgPrint(MID_TRACE,("Returning %x\n", Status));

    return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
}

NTSTATUS NTAPI
AfdSetInfo( PDEVICE_OBJECT DeviceObject, PIRP Irp,
            PIO_STACK_LOCATION IrpSp ) {
    NTSTATUS Status = STATUS_SUCCESS;
    PAFD_INFO InfoReq = LockRequest(Irp, IrpSp, FALSE, NULL);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PCHAR NewBuffer;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!SocketAcquireStateLock(FCB)) return LostSocket(Irp);

    if (!InfoReq)
        return UnlockAndMaybeComplete(FCB, STATUS_NO_MEMORY, Irp, 0);

    _SEH2_TRY {
        switch (InfoReq->InformationClass) {
            case AFD_INFO_BLOCKING_MODE:
                AFD_DbgPrint(MID_TRACE,("Blocking mode set to %u\n", InfoReq->Information.Boolean));
                FCB->NonBlocking = InfoReq->Information.Boolean;
                break;
            case AFD_INFO_INLINING_MODE:
                FCB->OobInline = InfoReq->Information.Boolean;
                break;
            case AFD_INFO_RECEIVE_WINDOW_SIZE:
                if (FCB->SharedData.State == SOCKET_STATE_CONNECTED ||
                    FCB->Flags & AFD_ENDPOINT_CONNECTIONLESS)
                {
                    /* FIXME: likely not right, check tcpip.sys for TDI_QUERY_MAX_DATAGRAM_INFO */
                    if (InfoReq->Information.Ulong > 0 && InfoReq->Information.Ulong < 0xFFFF &&
                        InfoReq->Information.Ulong != FCB->Recv.Size)
                    {
                        NewBuffer = ExAllocatePoolWithTag(PagedPool,
                                                          InfoReq->Information.Ulong,
                                                          TAG_AFD_DATA_BUFFER);

                        if (NewBuffer)
                        {
                            if (FCB->Recv.Content > InfoReq->Information.Ulong)
                                FCB->Recv.Content = InfoReq->Information.Ulong;

                            if (FCB->Recv.Window)
                            {
                                RtlCopyMemory(NewBuffer,
                                              FCB->Recv.Window,
                                              FCB->Recv.Content);

                                ExFreePoolWithTag(FCB->Recv.Window, TAG_AFD_DATA_BUFFER);
                            }

                            FCB->Recv.Size = InfoReq->Information.Ulong;
                            FCB->Recv.Window = NewBuffer;

                            Status = STATUS_SUCCESS;
                        }
                        else
                        {
                            Status = STATUS_NO_MEMORY;
                        }
                    }
                    else
                    {
                        Status = STATUS_SUCCESS;
                    }
                }
                else
                {
                    Status = STATUS_INVALID_PARAMETER;
                }
                break;
            case AFD_INFO_SEND_WINDOW_SIZE:
                if (FCB->SharedData.State == SOCKET_STATE_CONNECTED ||
                    FCB->Flags & AFD_ENDPOINT_CONNECTIONLESS)
                {
                    if (InfoReq->Information.Ulong > 0 && InfoReq->Information.Ulong < 0xFFFF &&
                        InfoReq->Information.Ulong != FCB->Send.Size)
                    {
                        NewBuffer = ExAllocatePoolWithTag(PagedPool,
                                                          InfoReq->Information.Ulong,
                                                          TAG_AFD_DATA_BUFFER);

                        if (NewBuffer)
                        {
                            if (FCB->Send.BytesUsed > InfoReq->Information.Ulong)
                                FCB->Send.BytesUsed = InfoReq->Information.Ulong;

                            if (FCB->Send.Window)
                            {
                                RtlCopyMemory(NewBuffer,
                                              FCB->Send.Window,
                                              FCB->Send.BytesUsed);

                                ExFreePoolWithTag(FCB->Send.Window, TAG_AFD_DATA_BUFFER);
                            }

                            FCB->Send.Size = InfoReq->Information.Ulong;
                            FCB->Send.Window = NewBuffer;

                            Status = STATUS_SUCCESS;
                        }
                        else
                        {
                            Status = STATUS_NO_MEMORY;
                        }
                    }
                    else
                    {
                        Status = STATUS_SUCCESS;
                    }
                }
                else
                {
                    Status = STATUS_INVALID_PARAMETER;
                }
                break;
            case AFD_INFO_KEEPALIVE:
                FCB->KeepAlive = InfoReq->Information.Ulong ? 1 : 0;
                FCB->KeepAliveValid = TRUE;

                /* Nothing to push it down yet; MakeSocketIntoConnection() will */
                if (FCB->Connection.Object)
                    Status = AfdApplyPendingTcpOptions(FCB);
                else
                    Status = STATUS_SUCCESS;
                break;

            case AFD_INFO_KEEPALIVE_VALS:
                FCB->KeepAliveTime = InfoReq->Information.LargeInteger.u.LowPart;
                FCB->KeepAliveInterval = InfoReq->Information.LargeInteger.u.HighPart;
                FCB->KeepAliveValsValid = TRUE;

                if (FCB->Connection.Object)
                    Status = AfdApplyPendingTcpOptions(FCB);
                else
                    Status = STATUS_SUCCESS;
                break;

            default:
                AFD_DbgPrint(MIN_TRACE,("Unknown request %u\n", InfoReq->InformationClass));
                break;
        }
    } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        AFD_DbgPrint(MIN_TRACE,("Exception executing SetInfo\n"));
        Status = STATUS_INVALID_PARAMETER;
    } _SEH2_END;

    AFD_DbgPrint(MID_TRACE,("Returning %x\n", Status));

    return UnlockAndMaybeComplete(FCB, Status, Irp, 0);
}

NTSTATUS NTAPI
AfdGetSockName( PDEVICE_OBJECT DeviceObject, PIRP Irp,
                PIO_STACK_LOCATION IrpSp ) {
    NTSTATUS Status = STATUS_SUCCESS;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;
    PMDL Mdl = NULL;

    UNREFERENCED_PARAMETER(DeviceObject);
    ASSERT(Irp->MdlAddress == NULL);

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    if( FCB->AddressFile.Object == NULL && FCB->Connection.Object == NULL ) {
         return UnlockAndMaybeComplete( FCB, STATUS_INVALID_PARAMETER, Irp, 0 );
    }

    Mdl = IoAllocateMdl( Irp->UserBuffer,
                         IrpSp->Parameters.DeviceIoControl.OutputBufferLength,
                         FALSE,
                         FALSE,
                         NULL );

    if( Mdl != NULL ) {
        _SEH2_TRY {
            MmProbeAndLockPages( Mdl, Irp->RequestorMode, IoModifyAccess );
        } _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
            AFD_DbgPrint(MIN_TRACE, ("MmProbeAndLockPages() failed.\n"));
            Status = _SEH2_GetExceptionCode();
        } _SEH2_END;

        if( NT_SUCCESS(Status) ) {
                Status = TdiQueryInformation( FCB->Connection.Object
                                                ? FCB->Connection.Object
                                                : FCB->AddressFile.Object,
                                              TDI_QUERY_ADDRESS_INFO,
                                              Mdl );
        }

        /* Check if MmProbeAndLockPages or TdiQueryInformation failed and
         * clean up Mdl */
        if (!NT_SUCCESS(Status) && Irp->MdlAddress != Mdl)
            IoFreeMdl(Mdl);
    } else
        Status = STATUS_INSUFFICIENT_RESOURCES;

    return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
}

NTSTATUS NTAPI
AfdGetPeerName( PDEVICE_OBJECT DeviceObject, PIRP Irp,
                PIO_STACK_LOCATION IrpSp ) {
    NTSTATUS Status;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PAFD_FCB FCB = FileObject->FsContext;

    UNREFERENCED_PARAMETER(DeviceObject);

    if( !SocketAcquireStateLock( FCB ) ) return LostSocket( Irp );

    if (FCB->RemoteAddress == NULL) {
        AFD_DbgPrint(MIN_TRACE,("Invalid parameter\n"));
        return UnlockAndMaybeComplete( FCB, STATUS_INVALID_PARAMETER, Irp, 0 );
    }

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength >= TaLengthOfTransportAddress(FCB->RemoteAddress))
    {
        RtlCopyMemory(Irp->UserBuffer, FCB->RemoteAddress, TaLengthOfTransportAddress(FCB->RemoteAddress));
        Status = STATUS_SUCCESS;
    }
    else
    {
        AFD_DbgPrint(MIN_TRACE,("Buffer too small\n"));
        Status = STATUS_BUFFER_TOO_SMALL;
    }

    return UnlockAndMaybeComplete( FCB, Status, Irp, 0 );
}
