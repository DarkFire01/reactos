/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client side of the host router: parent interfaces, root router target, config access
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4dr.h"

#define NDEBUG
#include <debug.h>

/* Below this much stack an asynchronous send is worth a warning */
#define USB4DR_LOW_STACK_BYTES          (12 * 1024)

/** Output size the host router writes for a config IOCTL. */
static
ULONG
NTAPI
Usb4DrConfigOutputSize(
    _In_ ULONG IoControlCode)
{
    if (IoControlCode == IOCTL_USB4HR_READ_CONFIG_EX)
        return sizeof(USB4HR_CONFIG_EX_OUTPUT);

    return sizeof(USB4HR_CONFIG_OUTPUT);
}

NTSTATUS
Usb4DrHostLink::QueryInterface(
    _In_ const GUID* InterfaceType,
    _Out_writes_bytes_(Size) PINTERFACE Interface,
    _In_ USHORT Size,
    _In_ USHORT Version)
{
    NTSTATUS Status;

    RtlZeroMemory(Interface, Size);

    Status = WdfFdoQueryForInterface(m_Fdo->Device(), InterfaceType, Interface, Size, Version, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Interface->Size < Size || Interface->Version != Version)
    {
        DPRINT1("Parent answered with interface size %u version %u\n", Interface->Size, Interface->Version);
        return STATUS_NOT_SUPPORTED;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrHostLink::Create(
    _In_ Usb4DrFdo* Fdo)
{
    WDF_IO_TARGET_OPEN_PARAMS OpenParams;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE Device = Fdo->Device();
    NTSTATUS Status;

    m_Fdo = Fdo;

    Status = QueryInterface(&GUID_USB4HR_PARENT_INTERFACE,
                            (PINTERFACE)&m_Parent,
                            sizeof(m_Parent),
                            USB4HR_PARENT_INTERFACE_VERSION);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Parent interface query failed 0x%lx\n", Status);
        return Status;
    }

    DPRINT("Router at depth %lu, ports %u:%u:%u:%u:%u:%u, domain 0x%lx\n",
           m_Parent.TopologyId.Depth,
           m_Parent.TopologyId.Port[0],
           m_Parent.TopologyId.Port[1],
           m_Parent.TopologyId.Port[2],
           m_Parent.TopologyId.Port[3],
           m_Parent.TopologyId.Port[4],
           m_Parent.TopologyId.Port[5],
           m_Parent.DomainId);

    Status = QueryInterface(&GUID_USB4HR_HARDWARE_SERVICES,
                            (PINTERFACE)&m_Services,
                            sizeof(m_Services),
                            USB4HR_HARDWARE_SERVICES_VERSION);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Hardware services query failed 0x%lx\n", Status);
        return Status;
    }

    if (m_Parent.RootRouterPdo == NULL)
    {
        DPRINT1("Parent interface has no root router PDO\n");
        return STATUS_INVALID_DEVICE_STATE;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;

    Status = WdfIoTargetCreate(Device, &Attributes, &m_RootTarget);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Root router target creation failed 0x%lx\n", Status);
        m_RootTarget = NULL;
        return Status;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_EXISTING_DEVICE(&OpenParams, m_Parent.RootRouterPdo);
    Status = WdfIoTargetOpen(m_RootTarget, &OpenParams);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Opening the root router target failed 0x%lx\n", Status);
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;

    Status = WdfRequestCreate(&Attributes, m_RootTarget, &m_ConfigRequest);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Config request creation failed 0x%lx\n", Status);
        m_ConfigRequest = NULL;
        return Status;
    }

    /* The request owns its buffers so they live exactly as long as it does */
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = m_ConfigRequest;

    Status = WdfMemoryCreatePreallocated(&Attributes,
                                         &m_ConfigInput,
                                         sizeof(m_ConfigInput),
                                         &m_ConfigInputMemory);
    if (NT_SUCCESS(Status))
    {
        Status = WdfMemoryCreatePreallocated(&Attributes,
                                             &m_ConfigOutput,
                                             sizeof(m_ConfigOutput),
                                             &m_ConfigOutputMemory);
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Config buffer creation failed 0x%lx\n", Status);
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;

    Status = WdfWaitLockCreate(&Attributes, &m_ConfigLock);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Config lock creation failed 0x%lx\n", Status);
        m_ConfigLock = NULL;
        return Status;
    }

    return STATUS_SUCCESS;
}

VOID
Usb4DrHostLink::Cleanup()
{
    /* The target, the request and the lock are children of the device and are gone by now */
    m_RootTarget = NULL;
    m_ConfigRequest = NULL;
    m_ConfigInputMemory = NULL;
    m_ConfigOutputMemory = NULL;
    m_ConfigLock = NULL;
}

NTSTATUS
Usb4DrHostLink::RefreshParentInterface()
{
    USB4HR_PARENT_INTERFACE Parent;
    NTSTATUS Status;

    Status = QueryInterface(&GUID_USB4HR_PARENT_INTERFACE,
                            (PINTERFACE)&Parent,
                            sizeof(Parent),
                            USB4HR_PARENT_INTERFACE_VERSION);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Parent interface requery failed 0x%lx\n", Status);
        return Status;
    }

    if (Parent.RouterHandle != m_Parent.RouterHandle)
        DPRINT("Router handle %p replaces %p\n", Parent.RouterHandle, m_Parent.RouterHandle);

    RtlCopyMemory(&m_Parent, &Parent, sizeof(m_Parent));
    return STATUS_SUCCESS;
}

const USB4HR_PARENT_INTERFACE*
Usb4DrHostLink::Parent() const
{
    return &m_Parent;
}

const USB4HR_HARDWARE_SERVICES*
Usb4DrHostLink::Services() const
{
    return &m_Services;
}

USB4HR_HANDLE
Usb4DrHostLink::RouterHandle() const
{
    return m_Parent.RouterHandle;
}

ULONG
Usb4DrHostLink::Depth() const
{
    return m_Parent.TopologyId.Depth;
}

WDFIOTARGET
Usb4DrHostLink::RootTarget() const
{
    return m_RootTarget;
}

BOOLEAN
Usb4DrHostLink::ClaimConfigRequest()
{
    return InterlockedCompareExchange(&m_ConfigBusy, 1, 0) == 0;
}

VOID
Usb4DrHostLink::ReleaseConfigRequest()
{
    InterlockedExchange(&m_ConfigBusy, 0);
}

NTSTATUS
Usb4DrHostLink::FormatConfig(
    _In_ ULONG IoControlCode,
    _In_ USB4HR_HANDLE Handle,
    _In_ ULONG Space,
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _In_ PULONG Buffer)
{
    WDF_REQUEST_REUSE_PARAMS ReuseParams;
    WDFMEMORY_OFFSET OutputRange;
    NTSTATUS Status;

    WDF_REQUEST_REUSE_PARAMS_INIT(&ReuseParams, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    Status = WdfRequestReuse(m_ConfigRequest, &ReuseParams);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Config request reuse failed 0x%lx\n", Status);
        return Status;
    }

    RtlZeroMemory(&m_ConfigInput, sizeof(m_ConfigInput));
    m_ConfigInput.Handle = Handle;
    m_ConfigInput.Space = Space;
    m_ConfigInput.DwordOffset = DwordOffset;
    m_ConfigInput.DwordCount = DwordCount;
    m_ConfigInput.Buffer = Buffer;

    /* A host router that never writes the output reads as a failure */
    RtlZeroMemory(&m_ConfigOutput, sizeof(m_ConfigOutput));
    m_ConfigOutput.Status = USB4HR_STATUS_FAILURE;

    OutputRange.BufferOffset = 0;
    OutputRange.BufferLength = Usb4DrConfigOutputSize(IoControlCode);

    Status = WdfIoTargetFormatRequestForInternalIoctl(m_RootTarget,
                                                      m_ConfigRequest,
                                                      IoControlCode,
                                                      m_ConfigInputMemory,
                                                      NULL,
                                                      m_ConfigOutputMemory,
                                                      &OutputRange);
    if (!NT_SUCCESS(Status))
        DPRINT1("Config IOCTL 0x%lx format failed 0x%lx\n", IoControlCode, Status);

    return Status;
}

NTSTATUS
Usb4DrHostLink::SendConfigSync(
    _In_ ULONG IoControlCode,
    _In_ USB4HR_HANDLE Handle,
    _In_ ULONG Space,
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _In_ PULONG Buffer,
    _Out_opt_ PUSB4HR_STATUS Usb4Status)
{
    WDF_REQUEST_SEND_OPTIONS Options;
    USB4HR_STATUS Result = USB4HR_STATUS_FAILURE;
    NTSTATUS Status;

    if (Usb4Status)
        *Usb4Status = USB4HR_STATUS_FAILURE;

    if (DwordCount == 0 || DwordCount > USB4HR_MAX_CONFIG_DWORDS)
    {
        DPRINT1("Config access of %lu dwords\n", DwordCount);
        return STATUS_INVALID_PARAMETER;
    }

    if (m_ConfigRequest == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    Usb4DrWaitLockGuard Guard(m_ConfigLock);

    if (!ClaimConfigRequest())
    {
        DPRINT1("Synchronous config access while an asynchronous one is in flight\n");
        return STATUS_DEVICE_BUSY;
    }

    Status = FormatConfig(IoControlCode, Handle, Space, DwordOffset, DwordCount, Buffer);
    if (NT_SUCCESS(Status))
    {
        WDF_REQUEST_SEND_OPTIONS_INIT(&Options,
                                      WDF_REQUEST_SEND_OPTION_SYNCHRONOUS | WDF_REQUEST_SEND_OPTION_TIMEOUT);
        Options.Timeout = Usb4DrWdfTimeoutMs(USB4DR_CONFIG_TIMEOUT_MS);

        if (!WdfRequestSend(m_ConfigRequest, m_RootTarget, &Options))
        {
            Status = WdfRequestGetStatus(m_ConfigRequest);
            if (NT_SUCCESS(Status))
                Status = STATUS_UNSUCCESSFUL;
        }
        else
        {
            Status = WdfRequestGetStatus(m_ConfigRequest);
        }

        if (NT_SUCCESS(Status))
            Result = m_ConfigOutput.Status;
    }

    ReleaseConfigRequest();

    if (Usb4Status)
        *Usb4Status = Result;

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Config IOCTL 0x%lx space %lu offset %lu failed 0x%lx\n", IoControlCode, Space, DwordOffset, Status);
        return Status;
    }

    if (Result != USB4HR_STATUS_SUCCESS)
    {
        DPRINT("Config IOCTL 0x%lx space %lu offset %lu: USB4 status %lu\n", IoControlCode, Space, DwordOffset, Result);
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrHostLink::SendConfigAsync(
    _In_ ULONG IoControlCode,
    _In_ USB4HR_HANDLE Handle,
    _In_ ULONG Space,
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _In_ PULONG Buffer,
    _In_ PUSB4DR_CONFIG_COMPLETION Completion,
    _In_opt_ PVOID Context)
{
    WDF_REQUEST_SEND_OPTIONS Options;
    NTSTATUS Status;

    if (DwordCount == 0 || DwordCount > USB4HR_MAX_CONFIG_DWORDS || Completion == NULL)
    {
        DPRINT1("Asynchronous config access of %lu dwords\n", DwordCount);
        return STATUS_INVALID_PARAMETER;
    }

    if (m_ConfigRequest == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    if (!ClaimConfigRequest())
    {
        DPRINT1("Config access already in flight\n");
        return STATUS_DEVICE_BUSY;
    }

    Status = FormatConfig(IoControlCode, Handle, Space, DwordOffset, DwordCount, Buffer);
    if (!NT_SUCCESS(Status))
    {
        ReleaseConfigRequest();
        return Status;
    }

    /* Windows moves the send to a work item here; nothing in this driver recurses through completions */
    if (IoGetRemainingStackSize() < USB4DR_LOW_STACK_BYTES)
        DPRINT1("Config access sent with %Iu bytes of stack left\n", IoGetRemainingStackSize());

    m_AsyncCompletion = Completion;
    m_AsyncContext = Context;
    m_AsyncHandle = Handle;

    WdfRequestSetCompletionRoutine(m_ConfigRequest, EvtConfigCompleted, this);

    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = Usb4DrWdfTimeoutMs(USB4DR_CONFIG_TIMEOUT_MS);

    if (!WdfRequestSend(m_ConfigRequest, m_RootTarget, &Options))
    {
        Status = WdfRequestGetStatus(m_ConfigRequest);
        if (NT_SUCCESS(Status))
            Status = STATUS_UNSUCCESSFUL;

        DPRINT1("Config IOCTL 0x%lx send failed 0x%lx\n", IoControlCode, Status);
        m_AsyncCompletion = NULL;
        ReleaseConfigRequest();
        return Status;
    }

    return STATUS_SUCCESS;
}

VOID
NTAPI
Usb4DrHostLink::EvtConfigCompleted(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context)
{
    Usb4DrHostLink* Link = (Usb4DrHostLink*)Context;
    PUSB4DR_CONFIG_COMPLETION Completion;
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    USB4HR_HANDLE Handle;
    PVOID CallerContext;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);

    Status = Params->IoStatus.Status;
    if (NT_SUCCESS(Status))
        Usb4Status = Link->m_ConfigOutput.Status;
    else
        DPRINT1("Asynchronous config IOCTL 0x%lx failed 0x%lx\n", Params->Parameters.Ioctl.IoControlCode, Status);

    Completion = Link->m_AsyncCompletion;
    CallerContext = Link->m_AsyncContext;
    Handle = Link->m_AsyncHandle;
    Link->m_AsyncCompletion = NULL;

    /* The caller may start its next access from the callback */
    Link->ReleaseConfigRequest();

    if (Completion)
        Completion(CallerContext, Handle, Status, Usb4Status);
}

NTSTATUS
Usb4DrHostLink::ReadConfig(
    _In_ USB4HR_HANDLE Handle,
    _In_ ULONG Space,
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _Out_writes_(DwordCount) PULONG Buffer,
    _Out_opt_ PUSB4HR_STATUS Usb4Status)
{
    if (DwordCount && DwordCount <= USB4HR_MAX_CONFIG_DWORDS)
        RtlZeroMemory(Buffer, DwordCount * sizeof(*Buffer));

    return SendConfigSync(IOCTL_USB4HR_READ_CONFIG, Handle, Space, DwordOffset, DwordCount, Buffer, Usb4Status);
}

NTSTATUS
Usb4DrHostLink::WriteConfig(
    _In_ USB4HR_HANDLE Handle,
    _In_ ULONG Space,
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _In_reads_(DwordCount) const ULONG* Buffer,
    _Out_opt_ PUSB4HR_STATUS Usb4Status)
{
    /* The host router only reads a write source */
    return SendConfigSync(IOCTL_USB4HR_WRITE_CONFIG,
                          Handle,
                          Space,
                          DwordOffset,
                          DwordCount,
                          const_cast<PULONG>(Buffer),
                          Usb4Status);
}

NTSTATUS
Usb4DrHostLink::ReadConfigAsync(
    _In_ USB4HR_HANDLE Handle,
    _In_ ULONG Space,
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _Out_writes_(DwordCount) PULONG Buffer,
    _In_ PUSB4DR_CONFIG_COMPLETION Completion,
    _In_opt_ PVOID Context)
{
    return SendConfigAsync(IOCTL_USB4HR_READ_CONFIG,
                           Handle,
                           Space,
                           DwordOffset,
                           DwordCount,
                           Buffer,
                           Completion,
                           Context);
}

NTSTATUS
Usb4DrHostLink::WriteConfigAsync(
    _In_ USB4HR_HANDLE Handle,
    _In_ ULONG Space,
    _In_ ULONG DwordOffset,
    _In_ ULONG DwordCount,
    _In_reads_(DwordCount) const ULONG* Buffer,
    _In_ PUSB4DR_CONFIG_COMPLETION Completion,
    _In_opt_ PVOID Context)
{
    return SendConfigAsync(IOCTL_USB4HR_WRITE_CONFIG,
                           Handle,
                           Space,
                           DwordOffset,
                           DwordCount,
                           const_cast<PULONG>(Buffer),
                           Completion,
                           Context);
}

NTSTATUS
Usb4DrHostLink::UpdateConfig(
    _In_ USB4HR_HANDLE Handle,
    _In_ ULONG Space,
    _In_ ULONG DwordOffset,
    _In_ ULONG Value,
    _In_ ULONG Mask)
{
    ULONG Dword;
    NTSTATUS Status;

    Status = ReadConfig(Handle, Space, DwordOffset, 1, &Dword, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Dword = (Dword & ~Mask) | (Value & Mask);
    return WriteConfig(Handle, Space, DwordOffset, 1, &Dword, NULL);
}

NTSTATUS
Usb4DrHostLink::SendIoctl(
    _In_ WDFIOTARGET Target,
    _In_ ULONG IoControlCode,
    _In_reads_bytes_opt_(InputLength) PVOID Input,
    _In_ ULONG InputLength,
    _Out_writes_bytes_opt_(OutputLength) PVOID Output,
    _In_ ULONG OutputLength,
    _In_ ULONG TimeoutMs,
    _Out_opt_ PULONG_PTR Information)
{
    WDF_MEMORY_DESCRIPTOR InputDescriptor;
    WDF_MEMORY_DESCRIPTOR OutputDescriptor;
    PWDF_MEMORY_DESCRIPTOR InputPointer = NULL;
    PWDF_MEMORY_DESCRIPTOR OutputPointer = NULL;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Transferred = 0;
    NTSTATUS Status;

    if (Information)
        *Information = 0;

    if (Target == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    if (Input && InputLength)
    {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InputDescriptor, Input, InputLength);
        InputPointer = &InputDescriptor;
    }

    if (Output && OutputLength)
    {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&OutputDescriptor, Output, OutputLength);
        OutputPointer = &OutputDescriptor;
    }

    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
    if (TimeoutMs)
    {
        Options.Flags |= WDF_REQUEST_SEND_OPTION_TIMEOUT;
        Options.Timeout = Usb4DrWdfTimeoutMs(TimeoutMs);
    }

    Status = WdfIoTargetSendInternalIoctlSynchronously(Target,
                                                       NULL,
                                                       IoControlCode,
                                                       InputPointer,
                                                       OutputPointer,
                                                       &Options,
                                                       &Transferred);
    if (!NT_SUCCESS(Status))
        DPRINT1("IOCTL 0x%lx failed 0x%lx\n", IoControlCode, Status);

    if (Information)
        *Information = Transferred;

    return Status;
}

NTSTATUS
Usb4DrHostLink::ResetHostRouter()
{
    USB4HR_RESET_INPUT Input;

    if (Depth() != 0)
    {
        DPRINT1("Host router reset requested below the root router\n");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    RtlZeroMemory(&Input, sizeof(Input));
    Input.RouterHandle = RouterHandle();

    return SendIoctl(m_RootTarget,
                     IOCTL_USB4HR_HOST_ROUTER_RESET,
                     &Input,
                     sizeof(Input),
                     NULL,
                     0,
                     USB4DR_CONFIG_TIMEOUT_MS,
                     NULL);
}
