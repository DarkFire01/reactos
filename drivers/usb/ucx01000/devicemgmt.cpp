/*
 * PROJECT:     ReactOS USB Host Controller Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device management IOCTLs from the hub and the operations they drive
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * The hub's own IRP reaches the HCD: UCX never builds a request here. Some
 * operations first walk the device's endpoint machines; the request is then
 * forwarded to the HCD or completed once every endpoint acknowledged.
 */

#include "ucx01000.h"

#define NDEBUG
#include <debug.h>

/** The device a device management request is about. */
static
UcxUsbDevice*
NTAPI
UcxDeviceFromManagementRequest(
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode)
{
    PVOID Arg1 = UcxRequestArgs(Request).Arg1;

    if (IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB)
        return UcxUsbDevice::FromHandle((UCXUSBDEVICE)((PURB)Arg1)->UrbHeader.UsbdDeviceHandle);

    return UcxUsbDevice::FromHandle(((PUSBDEVICE_MGMT_HEADER)Arg1)->UsbDevice);
}

VOID
NTAPI
UcxEvtDeviceMgmtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    UcxUsbDevice* Device = UcxDeviceFromManagementRequest(Request, IoControlCode);
    UcxController* Controller = Device->m_Controller;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (!Controller->AcquireResetReference())
    {
        Device->FailManagement(Request, IoControlCode);
        return;
    }

    /* Only an enable may bring back a device the last controller reset deprogrammed */
    if (Device->m_DeprogrammedByControllerReset)
    {
        if (IoControlCode != IOCTL_UCXHUB_DEVICE_ENABLE)
        {
            Device->FailManagement(Request, IoControlCode);
            Controller->ReleaseResetReference();
            return;
        }
        Device->m_DeprogrammedByControllerReset = FALSE;
    }

    Device->DispatchManagement(Request, IoControlCode);

    /* The operation usually continues; it does not need the reference */
    Controller->ReleaseResetReference();
}

VOID
UcxUsbDevice::DispatchManagement(
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode)
{
    UCXCONTROLLER Controller = m_Controller->m_Handle;
    const UCX_USBDEVICE_EVENT_CALLBACKS* Hcd = &m_Callbacks.Public;
    PVOID Arg1 = UcxRequestArgs(Request).Arg1;
    PURB Urb;

    KeQuerySystemTime((PLARGE_INTEGER)&m_Timestamp);

    switch (IoControlCode)
    {
        case IOCTL_UCXHUB_DEVICE_PURGE_IO:
            PurgeFromHub(Request, ((PUSBDEVICE_PURGEIO)Arg1)->OnSuspend);
            break;

        case IOCTL_UCXHUB_DEVICE_ABORT_IO:
            AbortFromHub(Request);
            break;

        case IOCTL_UCXHUB_DEVICE_TREE_PURGE_IO:
            TreePurgeFromHub(Request);
            break;

        case IOCTL_UCXHUB_DEVICE_START_IO:
            StartFromHub(Request);
            break;

        case IOCTL_INTERNAL_USB_SUBMIT_URB:
            Urb = (PURB)Arg1;
            if (Urb->UrbHeader.Function == URB_FUNCTION_OPEN_STATIC_STREAMS)
                UcxEndpoint::FromPipe(Urb->UrbOpenStaticStreams.PipeHandle)->StaticStreamsEnableFromClient(Request);
            else if (Urb->UrbHeader.Function == URB_FUNCTION_CLOSE_STATIC_STREAMS)
                UcxEndpoint::FromPipe(Urb->UrbPipeRequest.PipeHandle)->StaticStreamsDisableFromClient(Request);
            else
                WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
            break;

        case IOCTL_UCXHUB_DEVICE_RESET:
            ResetFromHub(Request, FALSE);
            break;

        case IOCTL_UCXHUB_DEVICE_ADDRESS:
            Hcd->EvtUsbDeviceAddress(Controller, Request);
            break;

        case IOCTL_UCXHUB_DEVICE_UPDATE:
            UpdateFromHub(Request);
            break;

        case IOCTL_UCXHUB_DEVICE_HUB_INFO:
            Hcd->EvtUsbDeviceHubInfo(Controller, Request);
            break;

        /* Goes to the device's default endpoint, not the one in the payload */
        case IOCTL_UCXHUB_DEFAULT_ENDPOINT_UPDATE:
            m_DefaultEndpoint->m_Callbacks.DefaultEndpointUpdate(Controller, Request);
            break;

        case IOCTL_UCXHUB_ENDPOINT_RESET:
            UcxEndpoint::FromHandle(((PENDPOINT_RESET)Arg1)->Endpoint)->ResetFromHub(Request);
            break;

        case IOCTL_UCXHUB_DEVICE_ENABLE:
            Hcd->EvtUsbDeviceEnable(Controller, Request);
            break;

        case IOCTL_UCXHUB_DEVICE_DISABLE:
            DisableFromHub(Request, FALSE);
            break;

        case IOCTL_UCXHUB_ENDPOINTS_CONFIGURE:
            EndpointsConfigureFromHub(Request, FALSE);
            break;

        default:
            WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
            break;
    }
}

/*
 * A controller reset is in progress, or wiped the device. Operations the hub
 * cannot see fail still run through the endpoints; the rest fail at once.
 */
VOID
UcxUsbDevice::FailManagement(
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode)
{
    PVOID Arg1 = UcxRequestArgs(Request).Arg1;
    PURB Urb;

    switch (IoControlCode)
    {
        /* The suspend hint is dropped on this path */
        case IOCTL_UCXHUB_DEVICE_PURGE_IO:
            PurgeFromHub(Request, FALSE);
            break;

        case IOCTL_UCXHUB_DEVICE_ABORT_IO:
            AbortFromHub(Request);
            break;

        case IOCTL_UCXHUB_DEVICE_TREE_PURGE_IO:
            TreePurgeFromHub(Request);
            break;

        case IOCTL_INTERNAL_USB_SUBMIT_URB:
            Urb = (PURB)Arg1;
            if (Urb->UrbHeader.Function == URB_FUNCTION_OPEN_STATIC_STREAMS)
            {
                UcxEndpoint* Endpoint = UcxEndpoint::FromPipe(Urb->UrbOpenStaticStreams.PipeHandle);

                NT_ASSERT(!Endpoint->m_OpenFailedOnReset);
                Endpoint->m_OpenFailedOnReset = TRUE;
                WdfRequestComplete(Request, STATUS_NO_SUCH_DEVICE);
            }
            else if (Urb->UrbHeader.Function == URB_FUNCTION_CLOSE_STATIC_STREAMS)
            {
                UcxEndpoint::FromPipe(Urb->UrbPipeRequest.PipeHandle)->StaticStreamsDisableFromClient(Request);
            }
            else
            {
                WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
            }
            break;

        case IOCTL_UCXHUB_DEVICE_RESET:
            NT_ASSERT(!m_ResetFailedByControllerReset);
            m_ResetFailedByControllerReset = TRUE;
            ResetFromHub(Request, TRUE);
            break;

        case IOCTL_UCXHUB_DEVICE_DISABLE:
            DisableFromHub(Request, TRUE);
            break;

        case IOCTL_UCXHUB_ENDPOINTS_CONFIGURE:
            EndpointsConfigureFromHub(Request, TRUE);
            break;

        case IOCTL_UCXHUB_ENDPOINT_RESET:
            NT_ASSERT(!m_EndpointResetFailedByControllerReset);
            m_EndpointResetFailedByControllerReset = TRUE;
            WdfRequestComplete(Request, STATUS_NO_SUCH_DEVICE);
            break;

        default:
            WdfRequestComplete(Request, STATUS_NO_SUCH_DEVICE);
            break;
    }
}

/* Requests parked while a controller reset ran come back here when it is over */
VOID
NTAPI
UcxEvtPendDuringResetIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PVOID Arg1 = UcxRequestArgs(Request).Arg1;
    NTSTATUS Status;
    UcxUsbDevice* Device;
    PURB Urb;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode)
    {
        case IOCTL_UCXHUB_DEVICE_RESET:
        case IOCTL_UCXHUB_DEVICE_DISABLE:
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_UCXHUB_ENDPOINTS_CONFIGURE:
            Status = (((PENDPOINTS_CONFIGURE)Arg1)->EndpointsToEnableCount != 0) ? STATUS_NO_SUCH_DEVICE
                                                                                  : STATUS_SUCCESS;
            break;

        case IOCTL_INTERNAL_USB_SUBMIT_URB:
            Urb = (PURB)Arg1;
            if (Urb->UrbHeader.Function != URB_FUNCTION_CLOSE_STATIC_STREAMS)
            {
                NT_ASSERT(FALSE);
                Status = STATUS_NO_SUCH_DEVICE;
                break;
            }

            Device = UcxUsbDevice::FromHandle((UCXUSBDEVICE)Urb->UrbHeader.UsbdDeviceHandle);
            if (Device->m_FailCloseStaticStreams)
            {
                Device->m_FailCloseStaticStreams = FALSE;
                Status = STATUS_NO_SUCH_DEVICE;
            }
            else
            {
                Status = STATUS_SUCCESS;
            }
            break;

        default:
            NT_ASSERT(FALSE);
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    WdfRequestComplete(Request, Status);
}

/* One tree purge at a time: the sequential queue stays stopped until it is done */
VOID
NTAPI
UcxEvtTreePurgeIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    UcxController* Controller = UcxController::FromFdo(WdfIoQueueGetDevice(Queue));

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode != IOCTL_UCXHUB_DEVICE_TREE_PURGE_IO)
    {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    WdfIoQueueStop(Queue, NULL, NULL);

    /* This IOCTL must not fail */
    if (!NT_SUCCESS(WdfRequestForwardToIoQueue(Request, Controller->m_DeviceMgmtQueue)))
    {
        WdfRequestComplete(Request, STATUS_SUCCESS);
        WdfIoQueueStart(Queue);
    }
}

/* Pending operations */

/* The count is set by the caller or the fan out, always before the first post */
VOID
UcxUsbDevice::StartOperation(
    _In_ PVOID Pending,
    _In_ PFN_UCX_OPERATION_DONE Done,
    _In_ LONG Count)
{
    NT_ASSERT(m_PendingOperation == NULL && m_PendingOperationDone == NULL);

    m_PendingOperation = Pending;
    m_PendingOperationDone = Done;
    m_PendingOperationCount = Count;
}

VOID
UcxUsbDevice::CompleteHubOperation()
{
    PFN_UCX_OPERATION_DONE Done;

    if (InterlockedDecrement(&m_PendingOperationCount) != 0)
        return;

    Done = m_PendingOperationDone;
    m_PendingOperationDone = NULL;
    (this->*Done)();
}

/* With no endpoints nothing is posted and the hub's request never completes, as on Windows */
VOID
UcxUsbDevice::FanOutToEndpoints(
    _In_ EpEvent Event)
{
    LIST_ENTRY Endpoints;
    PLIST_ENTRY Entry;
    UcxEndpoint* Endpoint;
    LONG Count = 0;

    InitializeListHead(&Endpoints);

    {
        SpinLockGuard Guard(&m_Controller->m_TopologyLock);

        for (Entry = m_EndpointList.Flink; Entry != &m_EndpointList; Entry = Entry->Flink)
        {
            Endpoint = CONTAINING_RECORD(Entry, UcxEndpoint, m_DeviceLink);

            WdfObjectReference(Endpoint->m_Handle);
            InsertTailList(&Endpoints, &Endpoint->m_OperationLink);
            Count++;
        }
    }

    m_PendingOperationCount = Count;

    while (!IsListEmpty(&Endpoints))
    {
        Entry = RemoveHeadList(&Endpoints);
        UcxClearListEntry(Entry);
        Endpoint = CONTAINING_RECORD(Entry, UcxEndpoint, m_OperationLink);

        Endpoint->Post(Event);
        WdfObjectDereference(Endpoint->m_Handle);
    }
}

VOID
UcxUsbDevice::PurgeFromHub(
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN OnSuspend)
{
    if (OnSuspend && m_Callbacks.Public.EvtUsbDeviceSuspend != NULL)
        m_Callbacks.Public.EvtUsbDeviceSuspend(m_Controller->m_Handle, m_Handle);

    StartOperation(Request, &UcxUsbDevice::CompleteHubRequest, 0);
    FanOutToEndpoints(EpEvent::HubPurge);
}

/* The hub only sends STARTIO on resume from suspend */
VOID
UcxUsbDevice::StartFromHub(
    _In_ WDFREQUEST Request)
{
    if (m_Callbacks.Public.EvtUsbDeviceResume != NULL)
        m_Callbacks.Public.EvtUsbDeviceResume(m_Controller->m_Handle, m_Handle);

    StartOperation(Request, &UcxUsbDevice::CompleteHubRequest, 0);
    FanOutToEndpoints(EpEvent::HubStartRequest);
}

VOID
UcxUsbDevice::AbortFromHub(
    _In_ WDFREQUEST Request)
{
    StartOperation(Request, &UcxUsbDevice::CompleteHubRequest, 0);
    FanOutToEndpoints(EpEvent::HubAbort);
}

/* Disconnected devices are included: their endpoints may not have reached purge yet */
static
VOID
NTAPI
UcxTreePurgeVisitor(
    _In_ UcxUsbDevice* Device,
    _In_opt_ PVOID Context)
{
    PLIST_ENTRY Endpoints = (PLIST_ENTRY)Context;
    PLIST_ENTRY Entry;
    UcxEndpoint* Endpoint;

    Device->m_AllowChildrenToExitTreePurge = FALSE;
    Device->m_AllowEndpointsToExitTreePurge = FALSE;

    for (Entry = Device->m_EndpointList.Flink; Entry != &Device->m_EndpointList; Entry = Entry->Flink)
    {
        Endpoint = CONTAINING_RECORD(Entry, UcxEndpoint, m_DeviceLink);

        WdfObjectReference(Endpoint->m_Handle);
        InsertTailList(Endpoints, &Endpoint->m_TreePurgeLink);
    }
}

VOID
UcxUsbDevice::TreePurgeFromHub(
    _In_ WDFREQUEST Request)
{
    UcxController* Controller = m_Controller;
    LIST_ENTRY Endpoints;
    PLIST_ENTRY Entry;
    UcxEndpoint* Endpoint;
    LONG Count = 0;

    InitializeListHead(&Endpoints);

    {
        SpinLockGuard Guard(&Controller->m_TopologyLock);

        WalkSubtree(UcxTreePurgeVisitor, &Endpoints, TRUE);
    }

    for (Entry = Endpoints.Flink; Entry != &Endpoints; Entry = Entry->Flink)
        Count++;

    NT_ASSERT(Controller->m_PendingTreePurge == NULL && Controller->m_PendingTreePurgeEndpoints == 0);
    Controller->m_PendingTreePurge = Request;
    Controller->m_PendingTreePurgeEndpoints = Count;

    while (!IsListEmpty(&Endpoints))
    {
        Entry = RemoveHeadList(&Endpoints);
        UcxClearListEntry(Entry);
        Endpoint = CONTAINING_RECORD(Entry, UcxEndpoint, m_TreePurgeLink);

        Endpoint->Post(EpEvent::HubTreePurge);
        WdfObjectDereference(Endpoint->m_Handle);
    }
}

VOID
UcxUsbDevice::ResetFromHub(
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN AfterReset)
{
    PUSBDEVICE_RESET Reset = (PUSBDEVICE_RESET)UcxRequestArgs(Request).Arg1;
    ULONG Count = Reset->EndpointsToDisableCount;
    UCXENDPOINT* ToDisable = Reset->EndpointsToDisable;
    UCXENDPOINT DefaultEndpoint = Reset->DefaultEndpoint;
    ULONG Index;

    {
        SpinLockGuard Guard(&m_Controller->m_TopologyLock);

        /* A tree purged device may leave tree purge once its parent has */
        if (!m_AllowEndpointsToExitTreePurge && m_ParentHub->m_AllowChildrenToExitTreePurge)
            m_AllowEndpointsToExitTreePurge = TRUE;
    }

    StartOperation(Request,
                   AfterReset ? &UcxUsbDevice::PendReset : &UcxUsbDevice::ForwardResetToHcd,
                   Count + 1);

    /* The payload may be gone once the last post finishes the operation */
    for (Index = 0; Index < Count; Index++)
        UcxEndpoint::FromHandle(ToDisable[Index])->Post(EpEvent::HubDeviceReset);

    UcxEndpoint::FromHandle(DefaultEndpoint)->Post(EpEvent::HubDeviceReset);
}

/* Only the default endpoint is told; configure disabled the others first */
VOID
UcxUsbDevice::DisableFromHub(
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN AfterReset)
{
    PUSBDEVICE_DISABLE Disable = (PUSBDEVICE_DISABLE)UcxRequestArgs(Request).Arg1;
    UCXENDPOINT DefaultEndpoint = Disable->DefaultEndpoint;

    m_Enabled = FALSE;
    StartOperation(Request,
                   AfterReset ? &UcxUsbDevice::PendDisable : &UcxUsbDevice::ForwardDisableToHcd,
                   1);

    UcxEndpoint::FromHandle(DefaultEndpoint)->Post(EpEvent::HubDisable);
}

VOID
UcxUsbDevice::EndpointsConfigureFromHub(
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN AfterReset)
{
    PENDPOINTS_CONFIGURE Configure = (PENDPOINTS_CONFIGURE)UcxRequestArgs(Request).Arg1;
    ULONG Count = Configure->EndpointsToDisableCount;
    UCXENDPOINT* ToDisable = Configure->EndpointsToDisable;
    ULONG Index;

    if (Count == 0)
    {
        /* Unlike the parked path, nothing to disable after a reset is a failure here */
        if (AfterReset)
            WdfRequestComplete(Request, STATUS_NO_SUCH_DEVICE);
        else
            m_Callbacks.Public.EvtUsbDeviceEndpointsConfigure(m_Controller->m_Handle, Request);
        return;
    }

    StartOperation(Request,
                   AfterReset ? &UcxUsbDevice::PendEndpointsConfigure
                              : &UcxUsbDevice::ForwardEndpointsConfigureToHcd,
                   Count);

    for (Index = 0; Index < Count; Index++)
        UcxEndpoint::FromHandle(ToDisable[Index])->Post(EpEvent::HubDisable);
}

/* Applied before the HCD sees the request, whatever it then answers */
VOID
UcxUsbDevice::UpdateFromHub(
    _In_ WDFREQUEST Request)
{
    PUSBDEVICE_UPDATE Update = (PUSBDEVICE_UPDATE)UcxRequestArgs(Request).Arg1;

    if (Update->Flags.UpdateIsHub && Update->IsHub)
        m_Type |= UCX_DEVICE_TYPE_HUB;

    if (Update->Flags.UpdateAllowIoOnInvalidPipeHandles)
        EnableInvalidHandleTracking();

    if (Update->Flags.UpdateDeviceDescriptor)
        m_BcdUsb = Update->DeviceDescriptor->bcdUSB;

    m_Callbacks.Public.EvtUsbDeviceUpdate(m_Controller->m_Handle, Request);
}

/* Completions run when the last endpoint acknowledged */

VOID
UcxUsbDevice::ForwardResetToHcd()
{
    m_Callbacks.Public.EvtUsbDeviceReset(m_Controller->m_Handle, (WDFREQUEST)TakeOperation());
}

VOID
UcxUsbDevice::ForwardDisableToHcd()
{
    m_Callbacks.Public.EvtUsbDeviceDisable(m_Controller->m_Handle, (WDFREQUEST)TakeOperation());
}

VOID
UcxUsbDevice::ForwardEndpointsConfigureToHcd()
{
    m_Callbacks.Public.EvtUsbDeviceEndpointsConfigure(m_Controller->m_Handle, (WDFREQUEST)TakeOperation());
}

/* The parked request completes once the controller reset machine releases it */
VOID
UcxUsbDevice::PendReset()
{
    WdfRequestForwardToIoQueue((WDFREQUEST)TakeOperation(), m_Controller->m_PendDuringResetQueue);
}

VOID
UcxUsbDevice::PendDisable()
{
    WdfRequestForwardToIoQueue((WDFREQUEST)TakeOperation(), m_Controller->m_PendDuringResetQueue);
}

VOID
UcxUsbDevice::PendEndpointsConfigure()
{
    WdfRequestForwardToIoQueue((WDFREQUEST)TakeOperation(), m_Controller->m_PendDuringResetQueue);
}

VOID
UcxUsbDevice::CompleteHubRequest()
{
    WdfRequestComplete((WDFREQUEST)TakeOperation(), STATUS_SUCCESS);
}

static
VOID
NTAPI
UcxResumeIrpCompletion(
    _In_ PIRP Irp,
    _In_ NTSTATUS Status)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/* A controller reset that hit while the default endpoint finished still fails the enable */
VOID
UcxUsbDevice::CompleteEnableIrp()
{
    PIRP Irp = (PIRP)TakeOperation();

    UcxResumeIrpCompletion(Irp, m_DeprogrammedByControllerReset ? STATUS_NO_SUCH_DEVICE : STATUS_SUCCESS);
}

VOID
UcxUsbDevice::CompleteEndpointsConfigureIrp()
{
    UcxResumeIrpCompletion((PIRP)TakeOperation(), STATUS_SUCCESS);
}

/* A reset device lets its own children leave tree purge again; the HCD's status is not used */
VOID
UcxUsbDevice::CompleteResetIrp()
{
    PIRP Irp;

    {
        SpinLockGuard Guard(&m_Controller->m_TopologyLock);

        if (m_AllowEndpointsToExitTreePurge && !m_AllowChildrenToExitTreePurge)
            m_AllowChildrenToExitTreePurge = TRUE;
    }

    Irp = (PIRP)TakeOperation();
    UcxResumeIrpCompletion(Irp, STATUS_SUCCESS);
}

/* Up paths from the device management completion routine */

NTSTATUS
UcxUsbDevice::EnableCompleteFromHcd(
    _In_ PIRP Irp,
    _In_ PUSBDEVICE_ENABLE Enable)
{
    /* Completed again right here; the hub's routine runs nested */
    if (!NT_SUCCESS(Irp->IoStatus.Status))
    {
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    StartOperation(Irp, &UcxUsbDevice::CompleteEnableIrp, 1);
    m_Enabled = TRUE;
    UcxEndpoint::FromHandle(Enable->DefaultEndpoint)->Post(EpEvent::ConfigureDone);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* The endpoints that were disabled stay disabled until a later configure */
NTSTATUS
UcxUsbDevice::ResetCompleteFromHcd(
    _In_ PIRP Irp,
    _In_ PUSBDEVICE_RESET Reset)
{
    NT_ASSERT(NT_SUCCESS(Irp->IoStatus.Status));

    StartOperation(Irp, &UcxUsbDevice::CompleteResetIrp, 1);
    UcxEndpoint::FromHandle(Reset->DefaultEndpoint)->Post(EpEvent::DeviceResetDone);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
UcxUsbDevice::EndpointsConfigureCompleteFromHcd(
    _In_ PIRP Irp,
    _In_ PENDPOINTS_CONFIGURE Configure)
{
    ULONG Count = Configure->EndpointsToEnableCount;
    UCXENDPOINT* ToEnable = Configure->EndpointsToEnable;
    ULONG Index;

    if (!NT_SUCCESS(Irp->IoStatus.Status))
    {
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    if (Count == 0)
    {
        UcxResumeIrpCompletion(Irp, STATUS_SUCCESS);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    StartOperation(Irp, &UcxUsbDevice::CompleteEndpointsConfigureIrp, Count);

    for (Index = 0; Index < Count; Index++)
        UcxEndpoint::FromHandle(ToEnable[Index])->Post(EpEvent::ConfigureDone);

    return STATUS_MORE_PROCESSING_REQUIRED;
}
