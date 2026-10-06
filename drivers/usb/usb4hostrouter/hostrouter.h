/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Host router FDO: PnP, power, queues, USB4 _OSC, properties and host router reset
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#define USB4HR_FDO_IDLE_MS              15000

/** Child kinds; only the root router is reported through the child list. */
enum class Usb4HrChildType : ULONG
{
    RootRouter = 0,
    PowerPdo = 1,
    Invalid = 2
};

/** Identification description of the dynamic child list, 8 bytes. */
struct Usb4HrChildDescription
{
    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Header;
    Usb4HrChildType Type;
};

C_ASSERT(sizeof(Usb4HrChildDescription) == 8);

/**
 * The host router. Lives in the FDO's WDFDEVICE context, built in place by
 * Usb4HrEvtDeviceAdd. Every module object is embedded here.
 */
class Usb4HrHostRouter
{
public:
    static Usb4HrHostRouter*
    FromDevice(
        _In_ WDFDEVICE Device);

    WDFDEVICE Device() const;

    Usb4HrHardware* Hardware();
    Usb4HrInterrupts* Interrupts();
    Usb4HrRingZero* Ring();
    Usb4HrConfigAccessor* ConfigAccessor();
    Usb4HrTopology* Topology();
    Usb4HrTunnelManager* Tunnels();
    Usb4HrPowerPdo* PowerPdo();

    /* Domain facts published through the parent interface */
    ULONG DomainId() const;
    const GUID* DomainUuid() const;
    USB4HR_TUNNEL_POLICY TunnelPolicy() const;

    /** USB4HR_DOMAIN_FLAG_* */
    ULONG64 DomainFlags() const;

    /** USB4 version 2 support: registry value when present, else the platform policy. */
    BOOLEAN IsUsb4V2Enabled() const;

    /** Parallel, power managed queue every internal IOCTL is dispatched from. */
    WDFQUEUE InternalIoctlQueue() const;

    /** Hands a request from the root router PDO to InternalIoctlQueue. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    ForwardFromRootRouter(
        _In_ WDFREQUEST Request);

    /** Root router PDO entered D0: idle timeout drops to USB4HR_ROOT_ROUTER_IDLE_MS. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID OnRootRouterD0Entry();

    /* Event handlers; the KMDF callbacks in hostrouter.cpp forward here */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Initialize(
        _In_ WDFDEVICE Device);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    PrepareHardware(
        _In_ WDFCMRESLIST Raw,
        _In_ WDFCMRESLIST Translated);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS ReleaseHardware();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    D0Entry(
        _In_ WDF_POWER_DEVICE_STATE PreviousState);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    PostInterruptsEnabled(
        _In_ WDF_POWER_DEVICE_STATE PreviousState);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    PreInterruptsDisabled(
        _In_ WDF_POWER_DEVICE_STATE TargetState);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    D0Exit(
        _In_ WDF_POWER_DEVICE_STATE TargetState);

    /** Context cleanup: modules, then the connection manager ID. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Cleanup();

    /** Routes one internal IOCTL by code to topology, tunnels or the reset queue. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    DispatchInternalIoctl(
        _In_ WDFREQUEST Request,
        _In_ ULONG IoControlCode);

    /** IOCTL_USB4HR_HOST_ROUTER_RESET, from the sequential reset queue. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    IssueHostRouterReset(
        _In_ WDFREQUEST Request);

    /** USB4 _OSC query then commit through the ACPI root; Reevaluate after hibernation. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    EvaluateUsb4Osc(
        _In_ BOOLEAN Reevaluate);

    /** D3cold and reset support properties on the host router PDO. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID PublishDeviceProperties();

private:
    friend
    NTSTATUS
    NTAPI
    Usb4HrEvtDeviceAdd(
        _In_ WDFDRIVER Driver,
        _Inout_ PWDFDEVICE_INIT DeviceInit);

    static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
    static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
    static EVT_WDF_DEVICE_D0_ENTRY EvtD0Entry;
    static EVT_WDF_DEVICE_D0_EXIT EvtD0Exit;
    static EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED EvtPostInterruptsEnabled;
    static EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED EvtPreInterruptsDisabled;
    static EVT_WDF_OBJECT_CONTEXT_CLEANUP EvtCleanup;
    static EVT_WDF_CHILD_LIST_CREATE_DEVICE EvtChildListCreateDevice;
    static EVT_WDF_CHILD_LIST_DEVICE_REENUMERATED EvtChildListDeviceReenumerated;
    static EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtInternalIoctl;
    static EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtResetIoctl;

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS CreateQueues();

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    UpdateIdleTimeout(
        _In_ ULONG TimeoutMs);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    ApplyOscGrant(
        _In_ ULONG Granted);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS ReprogramHostInterface();

    WDFDEVICE m_Device;
    Usb4HrPowerPdo* m_PowerPdo;
    WDFQUEUE m_InternalIoctlQueue;
    WDFQUEUE m_ResetQueue;
    GUID m_DomainUuid;
    BOOLEAN m_OwnDomainUuid;
    BOOLEAN m_RingCreated;
    BOOLEAN m_TopologyCreated;
    BOOLEAN m_TunnelsCreated;
    USB4HR_TUNNEL_POLICY m_TunnelPolicy;
    ULONG m_OscGranted;
    ULONG m_IdleTimeoutMs;

    Usb4HrHardware m_Hardware;
    Usb4HrInterrupts m_Interrupts;
    Usb4HrRingZero m_Ring;
    Usb4HrConfigAccessor m_ConfigAccessor;
    Usb4HrTopology m_Topology;
    Usb4HrTunnelManager m_Tunnels;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Usb4HrHostRouter, Usb4HrGetHostRouter);
