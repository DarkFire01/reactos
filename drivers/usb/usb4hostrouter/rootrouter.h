/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Root router PDO, its query interfaces and IOCTL forwarding
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#define USB4HR_ROOT_ROUTER_IDLE_MS      5000

/** The depth 0 router the device router driver loads on. Lives in its WDFDEVICE context. */
class Usb4HrRootRouter
{
public:
    /** EvtChildListCreateDevice for the child description of type Usb4HrChildType::RootRouter. */
    _IRQL_requires_(PASSIVE_LEVEL)
    static
    NTSTATUS
    CreateDevice(
        _In_ Usb4HrHostRouter* HostRouter,
        _In_ PWDFDEVICE_INIT DeviceInit);

    static Usb4HrRootRouter*
    FromDevice(
        _In_ WDFDEVICE Device);

    WDFDEVICE Device() const;

private:
    static
    NTSTATUS
    AssignIds(
        _In_ Usb4HrHostRouter* HostRouter,
        _In_ PWDFDEVICE_INIT DeviceInit);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Initialize(
        _In_ Usb4HrHostRouter* HostRouter,
        _In_ WDFDEVICE Device);

    NTSTATUS
    FillParentInterface(
        _Inout_ PUSB4HR_PARENT_INTERFACE Interface);

    NTSTATUS
    FillHardwareServices(
        _Inout_ PUSB4HR_HARDWARE_SERVICES Interface);

    static
    NTSTATUS
    NTAPI
    EvtD0Entry(
        _In_ WDFDEVICE Device,
        _In_ WDF_POWER_DEVICE_STATE PreviousState);

    static
    NTSTATUS
    NTAPI
    EvtEnableWakeAtBus(
        _In_ WDFDEVICE Device,
        _In_ SYSTEM_POWER_STATE PowerState);

    static
    VOID
    NTAPI
    EvtDisableWakeAtBus(
        _In_ WDFDEVICE Device);

    static
    VOID
    NTAPI
    EvtIoInternalDeviceControl(
        _In_ WDFQUEUE Queue,
        _In_ WDFREQUEST Request,
        _In_ size_t OutputBufferLength,
        _In_ size_t InputBufferLength,
        _In_ ULONG IoControlCode);

    static
    NTSTATUS
    NTAPI
    EvtQueryParentInterface(
        _In_ WDFDEVICE Device,
        _In_ LPGUID InterfaceType,
        _Inout_ PINTERFACE ExposedInterface,
        _Inout_opt_ PVOID ExposedInterfaceSpecificData);

    static
    NTSTATUS
    NTAPI
    EvtQueryHardwareServices(
        _In_ WDFDEVICE Device,
        _In_ LPGUID InterfaceType,
        _Inout_ PINTERFACE ExposedInterface,
        _Inout_opt_ PVOID ExposedInterfaceSpecificData);

    /* Parent interface functions; the root router has no parent link */
    static
    VOID
    NTAPI
    InterfaceNoop(
        _In_ PVOID Context);

    static
    BOOLEAN
    NTAPI
    IsLaneBondingSupported(
        _In_ PVOID Context);

    static
    BOOLEAN
    NTAPI
    IsAsymmetricSupported(
        _In_ PVOID Context);

    static
    BOOLEAN
    NTAPI
    IsDeviceGenerationCurrent(
        _In_ PVOID Context);

    static
    ULONG
    NTAPI
    GetMaxEnumerationTime(
        _In_ PVOID Context,
        _In_ ULONG Seconds);

    WDFDEVICE m_Device;
    Usb4HrHostRouter* m_HostRouter;

    /* Domain facts captured when the PDO is created, as the parent interface reports them */
    USB4HR_HANDLE m_RouterHandle;
    ULONG m_DomainId;
    UCHAR m_Cmid;
    UCHAR m_HostInterfaceVersion;
    BOOLEAN m_Usb4V2Enabled;
    GUID m_DomainUuid;
    USB4HR_TUNNEL_POLICY m_TunnelPolicy;
    ULONG64 m_DomainFlags;

    /** Set while PnP has armed the root router for wake at the bus. */
    BOOLEAN m_WakeAtBusArmed;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Usb4HrRootRouter, Usb4HrGetRootRouter);
