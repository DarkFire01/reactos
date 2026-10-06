/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Child router PDO: IDs, parent interface, hardware services and request forwarding
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/** A router plugged into one of our DFPs. Lives in its PDO's WDFDEVICE context. */
class Usb4DrChildPdo
{
public:
    /** EvtChildListCreateDevice of every DFP child list, for Usb4DrChildType::DeviceRouter. */
    _IRQL_requires_(PASSIVE_LEVEL)
    static
    NTSTATUS
    CreateDevice(
        _In_ Usb4DrFdo* Fdo,
        _In_ const Usb4DrChildDescription* Description,
        _In_ PWDFDEVICE_INIT DeviceInit);

    static Usb4DrChildPdo*
    FromDevice(
        _In_ WDFDEVICE Device);

    WDFDEVICE Device() const;
    const Usb4DrRouterIdentity* Identity() const;

    /** The DFP this router hangs off; NULL once the parent dropped the port. */
    Usb4DrPort* Port();

private:
    static
    NTSTATUS
    AssignIds(
        _In_ const Usb4DrRouterIdentity* Identity,
        _In_ PWDFDEVICE_INIT DeviceInit);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Initialize(
        _In_ Usb4DrFdo* Fdo,
        _In_ const Usb4DrChildDescription* Description,
        _In_ WDFDEVICE Device);

    static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
    static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
    static EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtIoInternalDeviceControl;

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

    /* Parent interface functions handed to the child router */
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

    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    FillParentInterface(
        _Inout_ PUSB4HR_PARENT_INTERFACE Interface);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    FillHardwareServices(
        _Inout_ PUSB4HR_HARDWARE_SERVICES Interface);

    /** Requests outside the router to router table go to the parent FDO's default queue. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    ForwardToParentDefaultQueue(
        _In_ WDFREQUEST Request,
        _In_ ULONG IoControlCode);

    WDFDEVICE m_Device;
    Usb4DrFdo* m_Fdo;
    Usb4DrRouterIdentity m_Identity;
    ULONG m_Generation;
    USB4HR_HANDLE m_RouterHandle;
    ULONG m_DomainId;
    USB4HR_TOPOLOGY_ID m_TopologyId;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Usb4DrChildPdo, Usb4DrGetChildPdo);
