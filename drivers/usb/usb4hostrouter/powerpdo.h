/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Virtual power coordination PDO
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#define USB4HR_REFERENCE_PREFIX         L"Usb4-Host-Interface-"
#define USB4HR_MAX_GFX_CLAIMS           10

/* Prefix, both names, separators and "CMID<n>" with room to spare */
#define USB4HR_REFERENCE_CHARS          (2 * USB4HR_NAME_CHARS + 48)

/* Adapter numbers a GFXhh open can name, 0x00 to 0x3F */
#define USB4HR_DP_ADAPTER_SLOTS         64

/** What an open of the power PDO named after the reference string. */
enum class Usb4HrPowerOpenType : ULONG
{
    None,
    Usb,
    Pci,
    Graphics,
    GraphicsAdapter
};

/**
 * Raw static child of the host router FDO. Drivers whose traffic runs through
 * tunnels open its interface so their power up waits for the domain.
 * Lives in its WDFDEVICE context.
 */
class Usb4HrPowerPdo
{
public:
    /** Builds the PDO (IDs, text, raw class, file callbacks, S0 idle) and adds it as a static child. */
    _IRQL_requires_(PASSIVE_LEVEL)
    static
    NTSTATUS
    Create(
        _In_ Usb4HrHostRouter* HostRouter,
        _Out_ Usb4HrPowerPdo** PowerPdo);

    static Usb4HrPowerPdo*
    FromDevice(
        _In_ WDFDEVICE Device);

    WDFDEVICE Device() const;

    /** The reference string the interface was registered with. */
    PCUNICODE_STRING ReferenceString() const;

private:
    /** One host router DP IN adapter as graphics claims and DP tunnels see it. */
    struct DpInAdapter
    {
        BOOLEAN Present;
        BOOLEAN TunnelActive;
        BOOLEAN ReferenceHeld;
        BOOLEAN InAltMode;
        UCHAR Claims;
    };

    /** A name some handle was opened with; the characters follow the entry. */
    struct OpenName
    {
        LIST_ENTRY Link;
        UNICODE_STRING Name;
    };

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

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS BuildReferenceString();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    D0Entry(
        _In_ WDF_POWER_DEVICE_STATE PreviousState);

    /** Domain power up: tunnel rebuild when some were powered down, else the root router. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS WaitForDomain();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    FileCreate(
        _In_ WDFFILEOBJECT FileObject);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    FileClose(
        _In_ WDFFILEOBJECT FileObject);

    NTSTATUS
    ParseOpenName(
        _In_ PCUNICODE_STRING Name,
        _Out_ Usb4HrPowerOpenType* Type,
        _Out_ PUCHAR Adapter) const;

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    RecordOpen(
        _In_ PCUNICODE_STRING Name);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    ForgetOpen(
        _In_ PCUNICODE_STRING Name);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    ClaimGraphics(
        _In_ UCHAR Adapter);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    RelinquishGraphics(
        _In_ UCHAR Adapter);

    /** A DP tunnel (or the alt mode workaround) now uses a host router DP IN adapter. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    DpTunnelConfigured(
        _In_ UCHAR Adapter,
        _In_ BOOLEAN AltModeWorkaround);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    DpTunnelTornDown(
        _In_ UCHAR Adapter,
        _In_ BOOLEAN AltModeWorkaround);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Cleanup();

    static
    VOID
    NTAPI
    EvtCleanup(
        _In_ WDFOBJECT Object);

    static
    NTSTATUS
    NTAPI
    EvtD0Entry(
        _In_ WDFDEVICE Device,
        _In_ WDF_POWER_DEVICE_STATE PreviousState);

    static
    VOID
    NTAPI
    EvtFileCreate(
        _In_ WDFDEVICE Device,
        _In_ WDFREQUEST Request,
        _In_ WDFFILEOBJECT FileObject);

    static
    VOID
    NTAPI
    EvtFileClose(
        _In_ WDFFILEOBJECT FileObject);

    static
    VOID
    NTAPI
    EvtInterfaceEnable(
        _In_ WDFWORKITEM WorkItem);

    WDFDEVICE m_Device;
    Usb4HrHostRouter* m_HostRouter;

    UNICODE_STRING m_ReferenceString;
    WCHAR m_ReferenceBuffer[USB4HR_REFERENCE_CHARS];

    /** Names of the open handles, guarded by m_OpenLock. */
    WDFWAITLOCK m_OpenLock;
    LIST_ENTRY m_OpenNames;

    /** DP IN adapters indexed by adapter number, guarded by m_DpLock. */
    KSPIN_LOCK m_DpLock;
    DpInAdapter m_DpAdapters[USB4HR_DP_ADAPTER_SLOTS];

    /** System power action of the last D0 entry, for traces. */
    POWER_ACTION m_PowerAction;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(Usb4HrPowerPdo, Usb4HrGetPowerPdo);
