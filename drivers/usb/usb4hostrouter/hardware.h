/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Host interface registers, PCI and ACPI identity, names and host router reset
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/* Public WDK values the ReactOS headers do not carry yet */
DEFINE_GUID(GUID_USB4HR_BUS_TYPE_ACPI,
    0xd7b46895, 0x001a, 0x4942, 0x89, 0x1f, 0xa7, 0xd4, 0x66, 0x10, 0xa8, 0x43);

DEFINE_GUID(GUID_USB4HR_DEVICE_RESET_INTERFACE,
    0x649fdf26, 0x3bc0, 0x4813, 0xad, 0x24, 0x7e, 0x0c, 0x1e, 0xda, 0x3f, 0xa3);

/* pciprop.h: PCI serial number (pid 40) and USB DVSEC port attributes (pid 42) */
DEFINE_DEVPROPKEY(USB4HR_DEVPKEY_PCI_SERIAL_NUMBER,
    0x3ab22e31, 0x8264, 0x4b4e, 0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62, 40);
DEFINE_DEVPROPKEY(USB4HR_DEVPKEY_PCI_USB_DVSEC_ATTRIBUTES,
    0x3ab22e31, 0x8264, 0x4b4e, 0x9a, 0xf5, 0xa8, 0xd2, 0xd8, 0xe3, 0x3e, 0x62, 42);

/** Bus the host router was enumerated on. */
enum class Usb4HrParentBus : ULONG
{
    Unknown,
    Pci,
    Acpi
};

#define USB4HR_ACPI_ID_CHARS            9
#define USB4HR_NAME_CHARS               128

/** Who the host router is. PCI fields are zero for an ACPI host router and the reverse. */
struct Usb4HrIdentity
{
    Usb4HrParentBus Bus;

    USHORT VendorId;
    USHORT DeviceId;
    UCHAR RevisionId;
    USHORT SubsystemVendorId;
    USHORT SubsystemId;
    ULONG PciBus;
    ULONG PciDevice;
    ULONG PciFunction;

    /** From IOCTL_ACPI_GET_DEVICE_INFORMATION; "UKWN" and "FFFF" when the query fails. */
    CHAR AcpiVendor[USB4HR_ACPI_ID_CHARS];
    CHAR AcpiDevice[USB4HR_ACPI_ID_CHARS];
    USHORT AcpiRevision;
};

/** The host router hardware: MMIO, PCI configuration space, identity and domain naming. */
class Usb4HrHardware
{
public:
    /** Identity, shim flags, names, connection manager ID and domain ID. Called from EvtDriverDeviceAdd. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Initialize(
        _In_ Usb4HrHostRouter* HostRouter);

    /** Releases the connection manager ID whenever one was taken. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Cleanup();

    const Usb4HrIdentity* Identity() const;

    ULONG64 ShimFlags() const;

    BOOLEAN
    HasShimFlag(
        _In_ ULONG64 Flag) const;

    /** Domain ID, format 1 (PCI) or 2 (ACPI). */
    ULONG DomainId() const;

    /** Connection manager ID, 0 to 7. */
    UCHAR Cmid() const;

    /** Escaped ACPI namespace path ('/', '\' and '^' become '#'); empty when unknown. */
    PCWSTR AcpiName() const;

    /** "%I64x_%d" of the PCI serial number and DVSEC port attributes; empty when absent. */
    PCWSTR DvsecName() const;

    /** Maps the first memory BAR and checks it covers USB4HR_MMIO_MIN_LENGTH. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    PrepareHardware(
        _In_ WDFCMRESLIST Translated);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID ReleaseHardware();

    /** Mapped and not reading all ones. */
    _IRQL_requires_max_(HIGH_LEVEL)
    BOOLEAN MmioResponds() const;

    _IRQL_requires_max_(HIGH_LEVEL)
    ULONG
    Read32(
        _In_ ULONG Offset) const;

    _IRQL_requires_max_(HIGH_LEVEL)
    VOID
    Write32(
        _In_ ULONG Offset,
        _In_ ULONG Value);

    /** Interrupt status and mask access; two dword accesses with USB4HR_SHIM_32BIT_ACCESS. */
    _IRQL_requires_max_(HIGH_LEVEL)
    ULONG64
    Read64(
        _In_ ULONG Offset) const;

    _IRQL_requires_max_(HIGH_LEVEL)
    VOID
    Write64(
        _In_ ULONG Offset,
        _In_ ULONG64 Value);

    /** Total paths from the capabilities register, 1 to USB4HR_MAX_PATHS when valid. */
    _IRQL_requires_max_(HIGH_LEVEL)
    ULONG PathCount() const;

    /** Host interface version byte of the capabilities register. */
    _IRQL_requires_max_(HIGH_LEVEL)
    UCHAR HostInterfaceVersion() const;

    /** Interrupt status and mask registers are 64 bits wide (more than 10 paths). */
    _IRQL_requires_max_(HIGH_LEVEL)
    BOOLEAN WideInterruptRegisters() const;

    /** D0 entry: force power flow when flagged, MMIO and path count checks. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    ProgramHostInterface(
        _In_ WDF_POWER_DEVICE_STATE PreviousState);

    /** D0 exit: drops force power when flagged and no device is connected. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    QuiesceHostInterface(
        _In_ BOOLEAN DevicesConnected);

    /** USB4 version 2 enabled and a version 2 host interface: reset the host router at start. */
    _IRQL_requires_(PASSIVE_LEVEL)
    BOOLEAN IsHostRouterResetRequired() const;

    /** Sets the host router reset bit and waits up to 1 s for it to clear. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS ResetHostRouter();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    ReadPciConfig(
        _In_ ULONG Offset,
        _Out_writes_bytes_(Length) PVOID Buffer,
        _In_ ULONG Length);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    WritePciConfig(
        _In_ ULONG Offset,
        _In_reads_bytes_(Length) PVOID Buffer,
        _In_ ULONG Length);

    /** D3cold capability from the parent's D3cold interface; FALSE when unavailable. */
    _IRQL_requires_(PASSIVE_LEVEL)
    BOOLEAN QueryD3ColdSupport();

    /** The parent offers GUID_DEVICE_RESET_INTERFACE_STANDARD. */
    _IRQL_requires_(PASSIVE_LEVEL)
    BOOLEAN QueryResetSupport();

private:
    friend class Usb4HrHostRouter;

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS QueryBusType();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS QueryPciIdentity();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS QueryAcpiIdentity();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS QueryAcpiIdentityFromHardwareId();

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID QueryAcpiName();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS QueryNamesAndCmid();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS SetForcePower();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS ClearForcePower();

    _IRQL_requires_(PASSIVE_LEVEL)
    BOOLEAN
    WaitResetBitClear(
        _In_ ULONG TimeoutMs);

    Usb4HrHostRouter* m_HostRouter;
    WDFDEVICE m_Device;
    Usb4HrIdentity m_Identity;
    BOOLEAN m_CustomEnumerator;
    ULONG64 m_ShimFlags;
    ULONG m_DomainId;
    UCHAR m_Cmid;
    BOOLEAN m_CmidTaken;
    BOOLEAN m_Usb4V2Enabled;
    WCHAR m_AcpiName[USB4HR_NAME_CHARS];
    WCHAR m_DvsecName[USB4HR_NAME_CHARS];

    BUS_INTERFACE_STANDARD m_BusInterface;
    BOOLEAN m_HasBusInterface;

    PUCHAR m_Mmio;
    SIZE_T m_MmioLength;
    BOOLEAN m_WideInterruptRegisters;
};
