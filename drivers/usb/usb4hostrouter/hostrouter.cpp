/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Host router FDO: PnP, power, queues, USB4 _OSC, properties and host router reset
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4hr.h"
#include <drivers/acpi/usb4osc.h>

#define NDEBUG
#include <debug.h>

/* USB4 _OSC grants this driver can run with */
#define USB4HR_OSC_ALL_TUNNELS          UACPINT_USB4_CONTROL_ALL
#define USB4HR_OSC_NO_PCIE              (UACPINT_USB4_CONTROL_ALL & ~UACPINT_USB4_CONTROL_PCIE)

/** Writes one property of GUID_USB4HR_PROPERTY_SET on the host router PDO. */
static
VOID
NTAPI
Usb4HrSetProperty(
    _In_ WDFDEVICE Device,
    _In_ ULONG Pid,
    _In_ DEVPROPTYPE Type,
    _In_ ULONG Size,
    _In_reads_bytes_(Size) PVOID Data)
{
    DEVPROPKEY Key;
    NTSTATUS Status;

    Key.fmtid = GUID_USB4HR_PROPERTY_SET;
    Key.pid = Pid;

    Status = IoSetDevicePropertyData(WdfDeviceWdmGetPhysicalDevice(Device), &Key, 0, 0, Type, Size, Data);
    if (!NT_SUCCESS(Status))
        DPRINT1("Setting host router property %lu failed 0x%lx\n", Pid, Status);
}

NTSTATUS
NTAPI
Usb4HrEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPower;
    WDF_CHILD_LIST_CONFIG ChildList;
    WDF_OBJECT_ATTRIBUTES Attributes;
    Usb4HrHostRouter* HostRouter;
    WDFDEVICE Device;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_CHILD_LIST_CONFIG_INIT(&ChildList, sizeof(Usb4HrChildDescription), Usb4HrHostRouter::EvtChildListCreateDevice);
    ChildList.EvtChildListDeviceReenumerated = Usb4HrHostRouter::EvtChildListDeviceReenumerated;
    WdfFdoInitSetDefaultChildListConfig(DeviceInit, &ChildList, WDF_NO_OBJECT_ATTRIBUTES);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPower);
    PnpPower.EvtDevicePrepareHardware = Usb4HrHostRouter::EvtPrepareHardware;
    PnpPower.EvtDeviceReleaseHardware = Usb4HrHostRouter::EvtReleaseHardware;
    PnpPower.EvtDeviceD0Entry = Usb4HrHostRouter::EvtD0Entry;
    PnpPower.EvtDeviceD0Exit = Usb4HrHostRouter::EvtD0Exit;
    PnpPower.EvtDeviceD0EntryPostInterruptsEnabled = Usb4HrHostRouter::EvtPostInterruptsEnabled;
    PnpPower.EvtDeviceD0ExitPreInterruptsDisabled = Usb4HrHostRouter::EvtPreInterruptsDisabled;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPower);

    /* The children go away before our hardware when a start fails */
    WdfDeviceInitSetReleaseHardwareOrderOnFailure(DeviceInit, WdfReleaseHardwareOrderOnFailureAfterDescendants);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, Usb4HrHostRouter);
    Attributes.EvtCleanupCallback = Usb4HrHostRouter::EvtCleanup;

    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("WdfDeviceCreate failed 0x%lx\n", Status);
        return Status;
    }

    HostRouter = new (Usb4HrGetHostRouter(Device)) Usb4HrHostRouter;
    Status = HostRouter->Initialize(Device);
    if (!NT_SUCCESS(Status))
        DPRINT1("Host router initialization failed 0x%lx\n", Status);

    return Status;
}

Usb4HrHostRouter*
Usb4HrHostRouter::FromDevice(
    _In_ WDFDEVICE Device)
{
    return Usb4HrGetHostRouter(Device);
}

WDFDEVICE Usb4HrHostRouter::Device() const
{
    return m_Device;
}

Usb4HrHardware* Usb4HrHostRouter::Hardware()
{
    return &m_Hardware;
}

Usb4HrInterrupts* Usb4HrHostRouter::Interrupts()
{
    return &m_Interrupts;
}

Usb4HrRingZero* Usb4HrHostRouter::Ring()
{
    return &m_Ring;
}

Usb4HrConfigAccessor* Usb4HrHostRouter::ConfigAccessor()
{
    return &m_ConfigAccessor;
}

Usb4HrTopology* Usb4HrHostRouter::Topology()
{
    return &m_Topology;
}

Usb4HrTunnelManager* Usb4HrHostRouter::Tunnels()
{
    return &m_Tunnels;
}

Usb4HrPowerPdo* Usb4HrHostRouter::PowerPdo()
{
    return m_PowerPdo;
}

ULONG Usb4HrHostRouter::DomainId() const
{
    return m_Hardware.DomainId();
}

const GUID* Usb4HrHostRouter::DomainUuid() const
{
    if (m_OwnDomainUuid)
        return &m_DomainUuid;

    return &Usb4HrDriver.SystemDomainUuid;
}

USB4HR_TUNNEL_PERMISSIONS Usb4HrHostRouter::TunnelPermissions() const
{
    return m_TunnelPermissions;
}

ULONG64 Usb4HrHostRouter::DomainFlags() const
{
    ULONG64 Flags = 0;

    if (m_Hardware.HasShimFlag(USB4HR_SHIM_NO_CLX) || Usb4HrDriver.Policy.DisableClxDomainWide)
        Flags |= USB4HR_DOMAIN_FLAG_NO_CLX;

    if (m_Hardware.HasShimFlag(USB4HR_SHIM_NO_XDOMAIN_E2E))
        Flags |= USB4HR_DOMAIN_FLAG_NO_XDOMAIN_E2E;

    /* USB4HR_DOMAIN_FLAG_LONG_DP_QUERY depends on a Windows feature that is off */
    return Flags;
}

BOOLEAN Usb4HrHostRouter::IsUsb4V2Enabled() const
{
    return m_Hardware.m_Usb4V2Enabled;
}

WDFQUEUE Usb4HrHostRouter::InternalIoctlQueue() const
{
    return m_InternalIoctlQueue;
}

NTSTATUS
Usb4HrHostRouter::ForwardFromRootRouter(
    _In_ WDFREQUEST Request)
{
    WDF_REQUEST_FORWARD_OPTIONS Options;
    NTSTATUS Status;

    WDF_REQUEST_FORWARD_OPTIONS_INIT(&Options);
    Status = WdfRequestForwardToParentDeviceIoQueue(Request, m_InternalIoctlQueue, &Options);
    if (!NT_SUCCESS(Status))
        DPRINT1("Forwarding a root router request failed 0x%lx\n", Status);

    return Status;
}

VOID Usb4HrHostRouter::OnRootRouterD0Entry()
{
    RefreshIdlePolicy(USB4HR_ROOT_ROUTER_IDLE_MS);
}

VOID
Usb4HrHostRouter::RefreshIdlePolicy(
    _In_ ULONG TimeoutMs)
{
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS Idle;
    NTSTATUS Status;

    if (m_IdleTimeoutMs == TimeoutMs)
        return;

    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&Idle, IdleCanWakeFromS0);
    Idle.DxState = PowerDeviceMaximum;
    Idle.IdleTimeout = TimeoutMs;
    Idle.UserControlOfIdleSettings = Usb4HrDriver.Policy.UserIdleControl ? IdleAllowUserControl :
                                                                           IdleDoNotAllowUserControl;
    Idle.Enabled = WdfUseDefault;
    Idle.PowerUpIdleDeviceOnSystemWake = WdfUseDefault;
    Idle.IdleTimeoutType = SystemManagedIdleTimeoutWithHint;
    Idle.ExcludeD3Cold = WdfFalse;

    Status = WdfDeviceAssignS0IdleSettings(m_Device, &Idle);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Idle timeout %lu ms not applied 0x%lx\n", TimeoutMs, Status);
        return;
    }

    m_IdleTimeoutMs = TimeoutMs;
}

NTSTATUS Usb4HrHostRouter::CreateQueues()
{
    WDF_IO_QUEUE_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;

    WDF_IO_QUEUE_CONFIG_INIT(&Config, WdfIoQueueDispatchParallel);
    Config.EvtIoInternalDeviceControl = EvtInternalIoctl;
    Status = WdfIoQueueCreate(m_Device, &Config, WDF_NO_OBJECT_ATTRIBUTES, &m_InternalIoctlQueue);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Internal IOCTL queue creation failed 0x%lx\n", Status);
        return Status;
    }

    /* Host router resets run one at a time at passive level */
    WDF_IO_QUEUE_CONFIG_INIT(&Config, WdfIoQueueDispatchSequential);
    Config.PowerManaged = WdfTrue;
    Config.EvtIoInternalDeviceControl = EvtResetIoctl;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;
    Status = WdfIoQueueCreate(m_Device, &Config, &Attributes, &m_ResetQueue);
    if (!NT_SUCCESS(Status))
        DPRINT1("Reset queue creation failed 0x%lx\n", Status);

    return Status;
}

NTSTATUS
Usb4HrHostRouter::Initialize(
    _In_ WDFDEVICE Device)
{
    WDF_DEVICE_STATE State;
    WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS Wake;
    Usb4HrChildDescription Child;
    static BOOLEAN GateReported;
    NTSTATUS Status;

    m_Device = Device;

    WDF_DEVICE_STATE_INIT(&State);
    State.NotDisableable = Usb4HrDriver.Policy.DriverDisableable ? WdfFalse : WdfTrue;
    WdfDeviceSetDeviceState(Device, &State);

    Status = m_Hardware.Initialize(this);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Hardware initialization failed 0x%lx\n", Status);
        return Status;
    }

    /* Windows refuses unvalidated hosts without test signing; ReactOS has no shim database */
    if (!m_Hardware.HasShimFlag(USB4HR_SHIM_VALIDATED_HOST) && !Usb4HrDriver.TestSigning && !GateReported)
    {
        GateReported = TRUE;
        DPRINT1("Host router not in a validated host list; loading anyway\n");
    }

    if (Usb4HrDriver.Policy.PerHostRouterDomainUuid)
    {
        Status = ExUuidCreate(&m_DomainUuid);
        if (NT_SUCCESS(Status))
            m_OwnDomainUuid = TRUE;
        else
            DPRINT1("Per host router domain UUID failed 0x%lx\n", Status);
    }

    RefreshIdlePolicy(USB4HR_FDO_IDLE_MS);

    WDF_DEVICE_POWER_POLICY_WAKE_SETTINGS_INIT(&Wake);
    Wake.DxState = PowerDeviceMaximum;
    Wake.UserControlOfWakeSettings = WakeDoNotAllowUserControl;
    Wake.Enabled = WdfTrue;
    Wake.ArmForWakeIfChildrenAreArmedForWake = TRUE;
    Wake.IndicateChildWakeOnParentWake = TRUE;
    Status = WdfDeviceAssignSxWakeSettings(Device, &Wake);
    if (!NT_SUCCESS(Status))
        DPRINT1("Wake settings not applied 0x%lx\n", Status);

    PublishDeviceProperties();

    Status = Usb4HrPowerPdo::Create(this, &m_PowerPdo);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Virtual power PDO creation failed 0x%lx\n", Status);
        return Status;
    }

    Status = m_Interrupts.Create(this);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = m_ConfigAccessor.Create(this);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Config accessor creation failed 0x%lx\n", Status);
        return Status;
    }

    Status = m_Topology.Create(this);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Topology creation failed 0x%lx\n", Status);
        return Status;
    }
    m_TopologyCreated = TRUE;

    Status = m_Tunnels.Create(this);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Tunnel manager creation failed 0x%lx\n", Status);
        return Status;
    }
    m_TunnelsCreated = TRUE;

    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&Child.Header, sizeof(Child));
    Child.Type = Usb4HrChildType::RootRouter;
    Status = WdfChildListAddOrUpdateChildDescriptionAsPresent(WdfFdoGetDefaultChildList(Device), &Child.Header, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Adding the root router failed 0x%lx\n", Status);
        return Status;
    }

    Status = EvaluateUsb4Osc(FALSE);
    if (!NT_SUCCESS(Status))
        return Status;

    return CreateQueues();
}

NTSTATUS
Usb4HrHostRouter::PrepareHardware(
    _In_ WDFCMRESLIST Raw,
    _In_ WDFCMRESLIST Translated)
{
    NTSTATUS Status;

    Status = m_Hardware.PrepareHardware(Translated);
    if (!NT_SUCCESS(Status))
        return Status;

    if (m_Hardware.IsHostRouterResetRequired())
    {
        DPRINT("USB4 version 2 host interface, resetting the host router\n");
        Status = m_Hardware.ResetHostRouter();
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Status = m_Interrupts.Prepare(Raw, Translated);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Interrupt preparation failed 0x%lx\n", Status);
        return Status;
    }

    /* The ring needs the path count, so it is built once the registers are mapped */
    if (!m_RingCreated)
    {
        Status = m_Ring.Create(this);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Ring zero creation failed 0x%lx\n", Status);
            return Status;
        }
        m_RingCreated = TRUE;
    }

    Status = m_Topology.ReportRouterFamily(m_Topology.RootRouterHandle(), Usb4HrRouterUsb4, m_Hardware.HostInterfaceVersion());
    if (!NT_SUCCESS(Status))
        DPRINT1("Setting the root router type failed 0x%lx\n", Status);

    return STATUS_SUCCESS;
}

NTSTATUS Usb4HrHostRouter::ReleaseHardware()
{
    m_Interrupts.Release();
    m_Hardware.ReleaseHardware();
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrHostRouter::D0Entry(
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    WDF_CHILD_LIST_ITERATOR Iterator;
    WDFCHILDLIST ChildList;
    POWER_ACTION Action = WdfDeviceGetSystemPowerAction(m_Device);
    WDFDEVICE Child;
    NTSTATUS Status;

    /* Firmware runs the USB4 _OSC again on resume from hibernate */
    if (Action == PowerActionHibernate)
    {
        Status = EvaluateUsb4Osc(TRUE);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("USB4 _OSC after hibernate failed 0x%lx\n", Status);
            WdfDeviceSetFailed(m_Device, WdfDeviceFailedNoRestart);
            return Status;
        }
    }

    Status = m_Hardware.ProgramHostInterface(PreviousState);
    if (NT_SUCCESS(Status))
        Status = m_Interrupts.ProgramHostInterface();

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Host interface configuration failed 0x%lx\n", Status);

        /* Nothing will power the domain up; do not leave power PDO waits hanging */
        m_Topology.SignalRootRouterStarted();
        return Status;
    }

    /* Children armed for wake learn that the host router woke them */
    ChildList = WdfFdoGetDefaultChildList(m_Device);
    WDF_CHILD_LIST_ITERATOR_INIT(&Iterator, WdfRetrievePresentChildren);
    WdfChildListBeginIteration(ChildList, &Iterator);
    for (;;)
    {
        Status = WdfChildListRetrieveNextDevice(ChildList, &Iterator, &Child, NULL);
        if (Status == STATUS_NO_MORE_ENTRIES || !NT_SUCCESS(Status))
            break;

        if (!NT_SUCCESS(WdfDeviceIndicateWakeStatus(Child, STATUS_SUCCESS)))
        {
            DPRINT("Child not armed for wake, releasing power waits\n");
            m_Topology.SignalRootRouterStarted();
        }
    }
    WdfChildListEndIteration(ChildList, &Iterator);

    if (Action >= PowerActionSleep)
        RefreshIdlePolicy(USB4HR_FDO_IDLE_MS);

    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrHostRouter::PostInterruptsEnabled(
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(PreviousState);

    Status = m_Ring.Start();
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Ring zero start failed 0x%lx\n", Status);
        return Status;
    }

    m_ConfigAccessor.Resume();
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrHostRouter::PreInterruptsDisabled(
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);

    m_ConfigAccessor.Pause();
    m_Ring.Stop();
    KeFlushQueuedDpcs();
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrHostRouter::D0Exit(
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    BOOLEAN Connected;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(TargetState);

    m_Tunnels.HandleDomainSleep();
    m_Topology.ResetRootRouterStarted();

    Connected = m_Topology.AreDevicesConnected();
    Status = m_Interrupts.QuiesceHostInterface();
    if (NT_SUCCESS(Status))
        Status = m_Hardware.QuiesceHostInterface(Connected);

    if (!NT_SUCCESS(Status))
        DPRINT1("Host interface teardown failed 0x%lx\n", Status);

    return Status;
}

VOID Usb4HrHostRouter::Cleanup()
{
    if (m_TunnelsCreated)
        m_Tunnels.Cleanup();

    if (m_TopologyCreated)
        m_Topology.Cleanup();

    m_Hardware.Cleanup();
}

VOID
Usb4HrHostRouter::DispatchInternalIoctl(
    _In_ WDFREQUEST Request,
    _In_ ULONG IoControlCode)
{
    NTSTATUS Status;

    switch (IoControlCode)
    {
        case IOCTL_USB4HR_READ_CONFIG:
            m_Topology.OnReadConfig(Request, FALSE);
            return;

        case IOCTL_USB4HR_READ_CONFIG_EX:
            m_Topology.OnReadConfig(Request, TRUE);
            return;

        case IOCTL_USB4HR_WRITE_CONFIG:
            m_Topology.OnWriteConfig(Request);
            return;

        case IOCTL_USB4HR_WAIT_ROUTER_EVENT:
            m_Topology.OnWaitRouterEvent(Request);
            return;

        case IOCTL_USB4HR_WAIT_ADAPTER_EVENT:
            m_Topology.OnWaitAdapterEvent(Request);
            return;

        case IOCTL_USB4HR_CREATE_TUNNEL:
        case IOCTL_USB4HR_DESTROY_TUNNEL:
        case IOCTL_USB4HR_REBUILD_TUNNEL:
            Status = m_Tunnels.QueueRequest(Request);
            if (!NT_SUCCESS(Status))
                WdfRequestComplete(Request, Status);
            return;

        case IOCTL_USB4HR_HOST_ROUTER_RESET:
            Status = WdfRequestForwardToIoQueue(Request, m_ResetQueue);
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("Forwarding the host router reset failed 0x%lx\n", Status);
                WdfRequestComplete(Request, Status);
            }
            return;

        default:
            DPRINT1("Internal IOCTL 0x%lx not supported\n", IoControlCode);
            WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
            return;
    }
}

NTSTATUS Usb4HrHostRouter::ReprogramHostInterface()
{
    NTSTATUS Status;

    Status = m_Hardware.ProgramHostInterface(WdfPowerDeviceD0);
    if (NT_SUCCESS(Status))
        Status = m_Interrupts.ProgramHostInterface();
    if (NT_SUCCESS(Status))
        Status = m_Ring.Start();
    if (!NT_SUCCESS(Status))
        return Status;

    m_ConfigAccessor.Resume();
    return STATUS_SUCCESS;
}

VOID
Usb4HrHostRouter::ServiceResetRequest(
    _In_ WDFREQUEST Request)
{
    PUSB4HR_RESET_INPUT Input;
    Usb4HrHandleInfo Info;
    NTSTATUS Status;

    Status = WdfRequestRetrieveInputBuffer(Request, sizeof(*Input), (PVOID*)&Input, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Host router reset without an input buffer 0x%lx\n", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    if (!m_Topology.IsHandleValid(Input->RouterHandle) ||
        !NT_SUCCESS(m_Topology.QueryHandle(Input->RouterHandle, &Info)) ||
        !Info.IsRouter)
    {
        DPRINT1("Host router reset with an invalid router handle\n");
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    if (Info.Route.Depth != 0)
    {
        DPRINT1("Host router reset requested for a router at depth %u\n", Info.Route.Depth);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    /* Quiesce: nothing may touch ring 0 while the router resets */
    m_ConfigAccessor.Pause();
    m_Ring.Stop();
    KeFlushQueuedDpcs();

    Status = m_Interrupts.QuiesceHostInterface();
    if (NT_SUCCESS(Status))
        Status = m_Hardware.QuiesceHostInterface(m_Topology.AreDevicesConnected());
    if (!NT_SUCCESS(Status))
        DPRINT1("Host interface teardown before reset failed 0x%lx\n", Status);

    Status = m_Hardware.ResetHostRouter();
    if (NT_SUCCESS(Status))
    {
        Status = ReprogramHostInterface();
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Host interface reprogram after reset failed 0x%lx\n", Status);
            WdfDeviceSetFailed(m_Device, WdfDeviceFailedAttemptRestart);
        }
    }
    else
    {
        DPRINT1("Host router reset failed 0x%lx\n", Status);
    }

    WdfRequestComplete(Request, Status);
}

NTSTATUS
Usb4HrHostRouter::ApplyOscGrant(
    _In_ ULONG Granted)
{
    NTSTATUS Status = STATUS_SUCCESS;

    if (Granted == USB4HR_OSC_ALL_TUNNELS)
    {
        m_TunnelPermissions.PcieTunnelingDisabled = FALSE;
    }
    else if (Granted == USB4HR_OSC_NO_PCIE)
    {
        DPRINT1("Firmware disabled PCIe tunneling\n");
        m_TunnelPermissions.PcieTunnelingDisabled = TRUE;
    }
    else
    {
        DPRINT1("Unusable USB4 _OSC grant 0x%lx\n", Granted);
        Status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* The grant is published even when it is refused */
    Usb4HrSetProperty(m_Device, USB4HR_PROPERTY_OSC_CONTROL, DEVPROP_TYPE_UINT32, sizeof(Granted), &Granted);
    return Status;
}

NTSTATUS
Usb4HrHostRouter::EvaluateUsb4Osc(
    _In_ BOOLEAN Reevaluate)
{
    WDF_IO_TARGET_OPEN_PARAMS OpenParams;
    UACPINT_USB4_OSC_REQUEST Request;
    UACPINT_USB4_OSC_REQUEST Reply;
    WDF_MEMORY_DESCRIPTOR Input;
    WDF_MEMORY_DESCRIPTOR Output;
    UNICODE_STRING TargetName;
    WDFIOTARGET Target = NULL;
    PZZWSTR Links = NULL;
    NTSTATUS Status;

    Status = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_UACPINT_ROOT, NULL, 0, &Links);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("No ACPI root interface for the USB4 _OSC 0x%lx\n", Status);
        return Status;
    }

    RtlInitUnicodeString(&TargetName, Links);
    if (TargetName.Length == 0)
    {
        DPRINT1("ACPI root interface list is empty\n");
        Status = STATUS_OBJECT_NAME_NOT_FOUND;
        goto Exit;
    }

    Status = WdfIoTargetCreate(m_Device, WDF_NO_OBJECT_ATTRIBUTES, &Target);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ACPI root target creation failed 0x%lx\n", Status);
        Target = NULL;
        goto Exit;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&OpenParams, &TargetName, GENERIC_READ | GENERIC_WRITE);
    Status = WdfIoTargetOpen(Target, &OpenParams);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Opening the ACPI root failed 0x%lx\n", Status);
        goto Exit;
    }

    RtlZeroMemory(&Request, sizeof(Request));
    Request.Signature = UACPINT_USB4_OSC_SIGNATURE;
    Request.Revision = UACPINT_USB4_OSC_REVISION;
    Request.Query = TRUE;
    Request.Support = m_Hardware.m_Usb4V2Enabled ? UACPINT_USB4_SUPPORT_VERSION_2 : 0;
    Request.ControlRequested = UACPINT_USB4_CONTROL_ALL;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, &Request, sizeof(Request));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Output, &Reply, sizeof(Reply));

    /* Query first */
    RtlZeroMemory(&Reply, sizeof(Reply));
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_UACPINT_USB4_OSC, &Input, &Output, NULL, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("USB4 _OSC query failed 0x%lx\n", Status);
        goto Exit;
    }

    if (!Reply.ControlRetained)
    {
        DPRINT1("Firmware kept USB4 control\n");
        Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }

    if (Reevaluate && Reply.ControlGranted != m_OscGranted)
    {
        DPRINT1("USB4 _OSC grant changed from 0x%lx to 0x%lx\n", m_OscGranted, Reply.ControlGranted);
        Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto Exit;
    }

    Status = ApplyOscGrant(Reply.ControlGranted);
    if (!NT_SUCCESS(Status))
        goto Exit;

    /* Then commit */
    Request.Query = FALSE;
    RtlZeroMemory(&Reply, sizeof(Reply));
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_UACPINT_USB4_OSC, &Input, &Output, NULL, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("USB4 _OSC commit failed 0x%lx\n", Status);
        goto Exit;
    }

    Status = ApplyOscGrant(Reply.ControlGranted);
    if (NT_SUCCESS(Status))
    {
        m_OscGranted = Reply.ControlGranted;
        DPRINT("USB4 _OSC granted 0x%lx\n", m_OscGranted);
    }

Exit:
    if (Target)
        WdfObjectDelete(Target);
    ExFreePool(Links);
    return Status;
}

VOID Usb4HrHostRouter::PublishDeviceProperties()
{
    BOOLEAN Value;

    Value = m_Hardware.QueryD3ColdSupport() ? DEVPROP_TRUE : DEVPROP_FALSE;
    Usb4HrSetProperty(m_Device, USB4HR_PROPERTY_D3COLD_SUPPORT, DEVPROP_TYPE_BOOLEAN, sizeof(Value), &Value);

    Value = m_Hardware.QueryResetSupport() ? DEVPROP_TRUE : DEVPROP_FALSE;
    Usb4HrSetProperty(m_Device, USB4HR_PROPERTY_RESET_SUPPORT, DEVPROP_TYPE_BOOLEAN, sizeof(Value), &Value);
}

/* KMDF callbacks *************************************************************/

NTSTATUS
NTAPI
Usb4HrHostRouter::EvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    return FromDevice(Device)->PrepareHardware(ResourcesRaw, ResourcesTranslated);
}

NTSTATUS
NTAPI
Usb4HrHostRouter::EvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    return FromDevice(Device)->ReleaseHardware();
}

NTSTATUS
NTAPI
Usb4HrHostRouter::EvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    return FromDevice(Device)->D0Entry(PreviousState);
}

NTSTATUS
NTAPI
Usb4HrHostRouter::EvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    return FromDevice(Device)->D0Exit(TargetState);
}

NTSTATUS
NTAPI
Usb4HrHostRouter::EvtPostInterruptsEnabled(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    return FromDevice(Device)->PostInterruptsEnabled(PreviousState);
}

NTSTATUS
NTAPI
Usb4HrHostRouter::EvtPreInterruptsDisabled(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    return FromDevice(Device)->PreInterruptsDisabled(TargetState);
}

VOID
NTAPI
Usb4HrHostRouter::EvtCleanup(
    _In_ WDFOBJECT Object)
{
    FromDevice((WDFDEVICE)Object)->Cleanup();
}

NTSTATUS
NTAPI
Usb4HrHostRouter::EvtChildListCreateDevice(
    _In_ WDFCHILDLIST ChildList,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER IdentificationDescription,
    _In_ PWDFDEVICE_INIT ChildInit)
{
    Usb4HrChildDescription* Child = CONTAINING_RECORD(IdentificationDescription, Usb4HrChildDescription, Header);
    Usb4HrHostRouter* HostRouter = FromDevice(WdfChildListGetDevice(ChildList));

    if (Child->Type != Usb4HrChildType::RootRouter)
    {
        DPRINT1("Child type %lu cannot be enumerated\n", (ULONG)Child->Type);
        return STATUS_INVALID_PARAMETER;
    }

    return Usb4HrRootRouter::CreateDevice(HostRouter, ChildInit);
}

BOOLEAN
NTAPI
Usb4HrHostRouter::EvtChildListDeviceReenumerated(
    _In_ WDFCHILDLIST ChildList,
    _In_ WDFDEVICE OldDevice,
    _In_ PWDF_CHILD_ADDRESS_DESCRIPTION_HEADER OldAddressDescription,
    _Out_ PWDF_CHILD_ADDRESS_DESCRIPTION_HEADER NewAddressDescription)
{
    Usb4HrHostRouter* HostRouter = FromDevice(WdfChildListGetDevice(ChildList));

    UNREFERENCED_PARAMETER(OldDevice);
    UNREFERENCED_PARAMETER(OldAddressDescription);
    UNREFERENCED_PARAMETER(NewAddressDescription);

    /* A version 2 domain is only rebuilt after a host router reset, so restart the FDO */
    if (HostRouter->m_Hardware.IsHostRouterResetRequired())
    {
        DPRINT1("Root router reenumerated on a version 2 domain, restarting\n");
        WdfDeviceSetFailed(HostRouter->m_Device, WdfDeviceFailedAttemptRestart);
    }

    return TRUE;
}

VOID
NTAPI
Usb4HrHostRouter::EvtInternalIoctl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    FromDevice(WdfIoQueueGetDevice(Queue))->DispatchInternalIoctl(Request, IoControlCode);
}

VOID
NTAPI
Usb4HrHostRouter::EvtResetIoctl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode != IOCTL_USB4HR_HOST_ROUTER_RESET)
    {
        DPRINT1("Reset queue got IOCTL 0x%lx\n", IoControlCode);
        WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
        return;
    }

    FromDevice(WdfIoQueueGetDevice(Queue))->ServiceResetRequest(Request);
}
