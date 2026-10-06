/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Router FDO: PnP, power, device flags and device properties
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/* Windows waits 60 s per round for the router; we give up after three rounds */
#define USB4DR_POWER_WAIT_MS            60000
#define USB4DR_POWER_WAIT_ROUNDS        3

/**
 * One router. Lives in the FDO's WDFDEVICE context, built in place by
 * Usb4DrEvtDeviceAdd. Every module object is embedded here.
 */
class Usb4DrFdo
{
public:
    static Usb4DrFdo*
    FromDevice(
        _In_ WDFDEVICE Device);

    WDFDEVICE Device() const;

    Usb4DrHostLink* HostLink();
    Usb4DrRouter* Router();
    Usb4DrPortSet* Ports();
    Usb4DrTunnels* Tunnels();
    Usb4DrForwarder* Forwarder();
    Usb4DrParentLink* ParentLink();

    /** Depth of this router, 0 for the root router. */
    ULONG Depth() const;

    /** USB4DR_FLAG_* from the shim engine; zero on ReactOS. Valid after QueryDeviceFlags. */
    ULONG64 DeviceFlags() const;

    /** TRUE when the router was gone after a resume; D0 entry then fails with STATUS_DEVICE_NOT_CONNECTED. */
    BOOLEAN WasDisconnectedOnResume() const;

    /* Event handlers; the KMDF callbacks in fdo.cpp forward here */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Initialize(
        _In_ WDFDEVICE Device);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    D0Entry(
        _In_ WDF_POWER_DEVICE_STATE PreviousState);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    D0Exit(
        _In_ WDF_POWER_DEVICE_STATE TargetState);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS ReleaseHardware();

    /** Context cleanup: modules in reverse creation order. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Cleanup();

    /** Shim engine lookup over every key of the router; once per router. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID QueryDeviceFlags();

    /** Friendly name and pids 8, 11, 12, 16 to 19; once per router. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID PublishRouterProperties();

    /** Pids 9 and 10 from the parsed DROM. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID PublishDromProperties();

    /** Pid 2, the adapter support mask. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    PublishAdapterSupport(
        _In_ ULONG SupportMask);

    /**
     * Router start failed with Status: every child is reported missing and the FDO is failed,
     * for good after a capability error, else with a restart.
     */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    RaiseRouterFault(
        _In_ NTSTATUS Status);

    /** Waits up to USB4DR_POWER_WAIT_ROUNDS rounds; STATUS_IO_TIMEOUT after that. */
    _IRQL_requires_(PASSIVE_LEVEL)
    static
    NTSTATUS
    WaitBounded(
        _In_ PKEVENT Event,
        _In_ PCSTR What);

private:
    friend
    NTSTATUS
    NTAPI
    Usb4DrEvtDeviceAdd(
        _In_ WDFDRIVER Driver,
        _Inout_ PWDFDEVICE_INIT DeviceInit);

    static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
    static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
    static EVT_WDF_DEVICE_D0_ENTRY EvtD0Entry;
    static EVT_WDF_DEVICE_D0_EXIT EvtD0Exit;
    static EVT_WDF_OBJECT_CONTEXT_CLEANUP EvtCleanup;
    static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;
    static EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtIoInternalDeviceControl;

    /** Default queue: user IOCTLs and whatever a child PDO passes up outside the router to router table. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS CreateDefaultQueue();

    /** One property of GUID_USB4HR_PROPERTY_SET on the router PDO. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    SetProperty(
        _In_ ULONG Pid,
        _In_ DEVPROPTYPE Type,
        _In_ ULONG Size,
        _In_reads_bytes_opt_(Size) PVOID Data);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID PublishFriendlyName();

    WDFDEVICE m_Device;
    ULONG64 m_KseFlags;
    BOOLEAN m_KseFlagsLoaded;
    BOOLEAN m_PropertiesPublished;
    BOOLEAN m_DisconnectedOnResume;
    BOOLEAN m_Started;

    /** Which modules Initialize created, so Cleanup only tears those down. */
    BOOLEAN m_HostLinkCreated;
    BOOLEAN m_RouterCreated;
    BOOLEAN m_TunnelsCreated;
    BOOLEAN m_ParentLinkCreated;

    Usb4DrHostLink m_HostLink;
    Usb4DrRouter m_Router;
    Usb4DrPortSet m_Ports;
    Usb4DrTunnels m_Tunnels;
    Usb4DrForwarder m_Forwarder;
    Usb4DrParentLink m_ParentLink;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Usb4DrFdo, Usb4DrGetFdo);
