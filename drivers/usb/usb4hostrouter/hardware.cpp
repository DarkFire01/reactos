/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Host interface registers, PCI and ACPI identity, names and host router reset
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4hr.h"
#include <wdmguid.h>
#include <acpiioct.h>
#include <drivers/acpi/acpipath.h>

#define NDEBUG
#include <debug.h>

/* IOCTL_ACPI_GET_DEVICE_INFORMATION; ReactOS acpiioct.h does not define it yet */
#define USB4HR_IOCTL_ACPI_DEVICE_INFORMATION \
    CTL_CODE(FILE_DEVICE_ACPI, 10, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

/* The namespace path request starts at 32 characters and grows by 32 */
#define USB4HR_ACPI_PATH_STEP_CHARS     32
#define USB4HR_ACPI_PATH_MAX_CHARS      2048

/* DVSEC port attributes: all three low bits set is not a valid port */
#define USB4HR_DVSEC_PORT_MASK          0x7

/* Hardware ID of a host router exposed by a custom ACPI enumerator: USB4\VVVVDDDDRRRR */
#define USB4HR_CUSTOM_HWID_PREFIX       L"USB4\\"
#define USB4HR_CUSTOM_HWID_PREFIX_CHARS 5
#define USB4HR_CUSTOM_HWID_CHARS        17
#define USB4HR_CUSTOM_HWID_FIELD_CHARS  4

#define USB4HR_FORCE_POWER_POLL_MS      100
#define USB4HR_FORCE_POWER_TIMEOUT_MS   1000
#define USB4HR_FORCE_POWER_KEEP_MASK    0x00FFFFFD
#define USB4HR_FORCE_POWER_REQUEST      0x00000002

#define USB4HR_RESET_POLL_MS            50
#define USB4HR_RESET_TIMEOUT_MS         1000

#define USB4HR_INTEL_VENDOR_ID          0x8086

/* PCI configuration header fields read for the identity */
#define USB4HR_PCI_HEADER_BYTES         64

/** Layout of the device reset interface (public wdm.h), version 1 is what is asked for. */
typedef struct _USB4HR_DEVICE_RESET_INTERFACE
{
    INTERFACE Header;
    PVOID DeviceReset;
    ULONG SupportedResetTypes;
    PVOID Reserved;
    PVOID QueryBusSpecificResetInfo;
    PVOID DeviceBusSpecificReset;
    PVOID GetDeviceResetStatus;
} USB4HR_DEVICE_RESET_INTERFACE;

#ifdef _WIN64
C_ASSERT(sizeof(USB4HR_DEVICE_RESET_INTERFACE) == 0x50);
#endif

/** Copies an ASCII field of the ACPI device information into Destination. */
static
VOID
NTAPI
Usb4HrCopyAcpiId(
    _Out_writes_(USB4HR_ACPI_ID_CHARS) PCHAR Destination,
    _In_reads_(Length) const CHAR* Source,
    _In_ ULONG Length)
{
    ULONG Index;

    if (Length >= USB4HR_ACPI_ID_CHARS)
        Length = USB4HR_ACPI_ID_CHARS - 1;

    for (Index = 0; Index < Length && Source[Index] != ANSI_NULL; Index++)
        Destination[Index] = Source[Index];

    Destination[Index] = ANSI_NULL;
}

NTSTATUS
Usb4HrHardware::Initialize(
    _In_ Usb4HrHostRouter* HostRouter)
{
    const Usb4HrPolicy* Policy = &Usb4HrDriver.Policy;
    NTSTATUS Status;

    m_HostRouter = HostRouter;
    m_Device = HostRouter->Device();

    Status = QueryBusType();
    if (!NT_SUCCESS(Status))
        return Status;

    if (m_Identity.Bus == Usb4HrParentBus::Pci)
    {
        Status = WdfFdoQueryForInterface(m_Device,
                                         &GUID_BUS_INTERFACE_STANDARD,
                                         (PINTERFACE)&m_BusInterface,
                                         sizeof(m_BusInterface),
                                         1,
                                         NULL);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("PCI bus interface query failed 0x%lx\n", Status);
            return Status;
        }

        m_HasBusInterface = TRUE;
    }

    Status = QueryNamesAndCmid();
    if (!NT_SUCCESS(Status))
        return Status;

    if (m_Identity.Bus == Usb4HrParentBus::Pci)
    {
        Status = QueryPciIdentity();
        if (!NT_SUCCESS(Status))
            return Status;

        m_DomainId = USB4HR_DOMAIN_FORMAT_PCI |
                     ((m_Identity.PciBus & 0xFF) << USB4HR_DOMAIN_PCI_BUS_SHIFT) |
                     ((m_Identity.PciDevice & 0x1F) << USB4HR_DOMAIN_PCI_DEVICE_SHIFT) |
                     ((m_Identity.PciFunction & 0x7) << USB4HR_DOMAIN_PCI_FUNCTION_SHIFT);
    }
    else
    {
        m_DomainId = USB4HR_DOMAIN_FORMAT_ACPI | ((ULONG)(m_Cmid & 0x3F) << USB4HR_DOMAIN_CMID_SHIFT);

        if (m_CustomEnumerator)
            Status = QueryAcpiIdentityFromHardwareId();
        else
            Status = QueryAcpiIdentity();

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ACPI identity query failed 0x%lx\n", Status);
            return Status;
        }
    }

    m_ShimFlags = Usb4HrQueryShimFlags(m_Device, &m_Identity);

    /* Registry policy wins; otherwise Windows turns USB4 version 2 on for Intel host routers */
    if (Policy->Usb4V2Configured)
        m_Usb4V2Enabled = Policy->Usb4V2Enabled;
    else
        m_Usb4V2Enabled = (m_Identity.Bus == Usb4HrParentBus::Pci && m_Identity.VendorId == USB4HR_INTEL_VENDOR_ID);

    DPRINT("Host router domain 0x%lx CMID %u shim 0x%I64x v2 %u\n",
           m_DomainId, m_Cmid, m_ShimFlags, m_Usb4V2Enabled);
    return STATUS_SUCCESS;
}

NTSTATUS Usb4HrHardware::QueryBusType()
{
    GUID BusType;
    ULONG Length;
    NTSTATUS Status;

    Status = WdfDeviceQueryProperty(m_Device, DevicePropertyBusTypeGuid, sizeof(BusType), &BusType, &Length);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Bus type query failed 0x%lx\n", Status);
        return Status;
    }

    if (IsEqualGUID(BusType, GUID_BUS_TYPE_PCI))
    {
        m_Identity.Bus = Usb4HrParentBus::Pci;
        return STATUS_SUCCESS;
    }

    /* Anything that is not PCI is handled as ACPI; a foreign enumerator carries its IDs in the hardware ID */
    m_Identity.Bus = Usb4HrParentBus::Acpi;
    m_CustomEnumerator = !IsEqualGUID(BusType, GUID_USB4HR_BUS_TYPE_ACPI);
    if (m_CustomEnumerator)
        DPRINT1("Host router enumerated by a custom bus\n");

    return STATUS_SUCCESS;
}

NTSTATUS Usb4HrHardware::QueryPciIdentity()
{
    PCI_COMMON_HEADER Header;
    ULONG Address;
    ULONG BusNumber;
    ULONG Length;
    NTSTATUS Status;

    C_ASSERT(sizeof(Header) >= USB4HR_PCI_HEADER_BYTES);

    Status = ReadPciConfig(0, &Header, USB4HR_PCI_HEADER_BYTES);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PCI header read failed 0x%lx\n", Status);
        return Status;
    }

    m_Identity.VendorId = Header.VendorID;
    m_Identity.DeviceId = Header.DeviceID;
    m_Identity.RevisionId = Header.RevisionID;
    m_Identity.SubsystemVendorId = Header.u.type0.SubVendorID;
    m_Identity.SubsystemId = Header.u.type0.SubSystemID;

    Status = WdfDeviceQueryProperty(m_Device, DevicePropertyAddress, sizeof(Address), &Address, &Length);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PCI address query failed 0x%lx\n", Status);
        return Status;
    }

    Status = WdfDeviceQueryProperty(m_Device, DevicePropertyBusNumber, sizeof(BusNumber), &BusNumber, &Length);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PCI bus number query failed 0x%lx\n", Status);
        return Status;
    }

    m_Identity.PciBus = BusNumber;
    m_Identity.PciDevice = Address >> 16;
    m_Identity.PciFunction = Address & 0xFFFF;
    return STATUS_SUCCESS;
}

NTSTATUS Usb4HrHardware::QueryAcpiIdentity()
{
    ACPI_DEVICE_INFORMATION_OUTPUT_BUFFER Header;
    PACPI_DEVICE_INFORMATION_OUTPUT_BUFFER Info;
    WDF_MEMORY_DESCRIPTOR Output;
    WDFIOTARGET Target = WdfDeviceGetIoTarget(m_Device);
    const CHAR* Base;
    ULONG VendorLength;
    ULONG DeviceLength;
    ULONG Size;
    NTSTATUS Status;

    /* The header alone tells how large the whole answer is */
    RtlZeroMemory(&Header, sizeof(Header));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Output, &Header, sizeof(Header));
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, USB4HR_IOCTL_ACPI_DEVICE_INFORMATION,
                                               NULL, &Output, NULL, NULL);
    if (!NT_SUCCESS(Status) && Status != STATUS_BUFFER_OVERFLOW)
        return Status;

    Size = max((ULONG)Header.Size, (ULONG)sizeof(Header));
    Info = (PACPI_DEVICE_INFORMATION_OUTPUT_BUFFER)ExAllocatePoolWithTag(NonPagedPool, Size, USB4HR_TAG_HARDWARE);
    if (!Info)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Info, Size);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Output, Info, Size);
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, USB4HR_IOCTL_ACPI_DEVICE_INFORMATION,
                                               NULL, &Output, NULL, NULL);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Info, USB4HR_TAG_HARDWARE);
        return Status;
    }

    Base = (const CHAR*)Info;
    m_Identity.AcpiRevision = Info->HardwareRevision;

    if (Info->VendorIdStringOffset == 0 || Info->VendorIdStringOffset >= Size)
    {
        RtlStringCchCopyA(m_Identity.AcpiVendor, RTL_NUMBER_OF(m_Identity.AcpiVendor), "UKWN");
        RtlStringCchCopyA(m_Identity.AcpiDevice, RTL_NUMBER_OF(m_Identity.AcpiDevice), "FFFF");
        ExFreePoolWithTag(Info, USB4HR_TAG_HARDWARE);
        return STATUS_SUCCESS;
    }

    /* VendorStringLength spans the vendor and the device part */
    if (Info->DeviceIdStringOffset != 0)
        VendorLength = (ULONG)(Info->DeviceIdStringOffset - Info->VendorIdStringOffset);
    else
        VendorLength = Info->VendorStringLength;

    VendorLength = min(VendorLength, Size - Info->VendorIdStringOffset);
    if (VendorLength == 0 || VendorLength > 4)
        DPRINT1("Unexpected ACPI vendor ID length %lu\n", VendorLength);

    Usb4HrCopyAcpiId(m_Identity.AcpiVendor, Base + Info->VendorIdStringOffset, VendorLength);

    if (Info->DeviceIdStringOffset != 0 && Info->DeviceIdStringOffset < Size)
    {
        DeviceLength = (Info->VendorStringLength > VendorLength) ? Info->VendorStringLength - VendorLength : 0;
        DeviceLength = min(DeviceLength, Size - Info->DeviceIdStringOffset);
        if (DeviceLength == 0 || DeviceLength > 4)
            DPRINT1("Unexpected ACPI device ID length %lu\n", DeviceLength);

        Usb4HrCopyAcpiId(m_Identity.AcpiDevice, Base + Info->DeviceIdStringOffset, DeviceLength);
    }
    else
    {
        RtlStringCchCopyA(m_Identity.AcpiDevice, RTL_NUMBER_OF(m_Identity.AcpiDevice), "FFFF");
    }

    ExFreePoolWithTag(Info, USB4HR_TAG_HARDWARE);
    return STATUS_SUCCESS;
}

NTSTATUS Usb4HrHardware::QueryAcpiIdentityFromHardwareId()
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING Revision;
    WDFMEMORY Memory;
    PCWSTR Entry;
    PCWSTR End;
    size_t BufferSize;
    ULONG Value;
    ULONG Index;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = m_Device;
    Status = WdfDeviceAllocAndQueryProperty(m_Device, DevicePropertyHardwareID, NonPagedPool, &Attributes, &Memory);
    if (!NT_SUCCESS(Status))
        return Status;

    Entry = (PCWSTR)WdfMemoryGetBuffer(Memory, &BufferSize);
    End = Entry + BufferSize / sizeof(WCHAR);
    Status = STATUS_DEVICE_CONFIGURATION_ERROR;

    while (Entry < End && *Entry != UNICODE_NULL)
    {
        size_t Length = 0;

        while (Entry + Length < End && Entry[Length] != UNICODE_NULL)
            Length++;

        if (Length == USB4HR_CUSTOM_HWID_CHARS &&
            RtlCompareMemory(Entry, USB4HR_CUSTOM_HWID_PREFIX,
                             USB4HR_CUSTOM_HWID_PREFIX_CHARS * sizeof(WCHAR)) ==
                USB4HR_CUSTOM_HWID_PREFIX_CHARS * sizeof(WCHAR))
        {
            PCWSTR Fields = Entry + USB4HR_CUSTOM_HWID_PREFIX_CHARS;

            for (Index = 0; Index < USB4HR_CUSTOM_HWID_FIELD_CHARS; Index++)
            {
                m_Identity.AcpiVendor[Index] = (CHAR)Fields[Index];
                m_Identity.AcpiDevice[Index] = (CHAR)Fields[USB4HR_CUSTOM_HWID_FIELD_CHARS + Index];
            }
            m_Identity.AcpiVendor[USB4HR_CUSTOM_HWID_FIELD_CHARS] = ANSI_NULL;
            m_Identity.AcpiDevice[USB4HR_CUSTOM_HWID_FIELD_CHARS] = ANSI_NULL;

            Revision.Buffer = (PWSTR)(Fields + 2 * USB4HR_CUSTOM_HWID_FIELD_CHARS);
            Revision.Length = USB4HR_CUSTOM_HWID_FIELD_CHARS * sizeof(WCHAR);
            Revision.MaximumLength = Revision.Length;

            Value = 0;
            Status = RtlUnicodeStringToInteger(&Revision, 16, &Value);
            if (NT_SUCCESS(Status))
                m_Identity.AcpiRevision = (USHORT)Value;
            break;
        }

        Entry += Length + 1;
    }

    if (!NT_SUCCESS(Status))
        DPRINT1("No USB4 hardware ID on the custom enumerated host router 0x%lx\n", Status);

    WdfObjectDelete(Memory);
    return Status;
}

VOID Usb4HrHardware::QueryAcpiName()
{
    UACPINT_NAMESPACE_PATH_REQUEST Request;
    WDF_MEMORY_DESCRIPTOR Input;
    WDF_MEMORY_DESCRIPTOR Output;
    WDFIOTARGET Target = WdfDeviceGetIoTarget(m_Device);
    PWCHAR Path = NULL;
    ULONG Chars = 0;
    ULONG Index;
    NTSTATUS Status;

    m_AcpiName[0] = UNICODE_NULL;

    Request.Signature = UACPINT_NAMESPACE_PATH_SIGNATURE;
    Request.Flags = UACPINT_NAMESPACE_PATH_PADDED;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, &Request, sizeof(Request));

    /* Windows grows the buffer without a limit; stop at a sane size */
    do
    {
        if (Path)
            ExFreePoolWithTag(Path, USB4HR_TAG_HARDWARE);

        Chars += USB4HR_ACPI_PATH_STEP_CHARS;
        if (Chars > USB4HR_ACPI_PATH_MAX_CHARS)
        {
            DPRINT1("ACPI namespace path too long\n");
            return;
        }

        Path = (PWCHAR)ExAllocatePoolWithTag(NonPagedPool, Chars * sizeof(WCHAR), USB4HR_TAG_HARDWARE);
        if (!Path)
        {
            DPRINT1("No memory for the ACPI namespace path\n");
            return;
        }

        RtlZeroMemory(Path, Chars * sizeof(WCHAR));
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Output, Path, Chars * sizeof(WCHAR));
        Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_UACPINT_QUERY_NAMESPACE_PATH,
                                                   &Input, &Output, NULL, NULL);
    }
    while (Status == STATUS_BUFFER_TOO_SMALL);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ACPI namespace path query failed 0x%lx, no ACPI name\n", Status);
        ExFreePoolWithTag(Path, USB4HR_TAG_HARDWARE);
        return;
    }

    Path[Chars - 1] = UNICODE_NULL;
    Status = RtlStringCchCopyW(m_AcpiName, RTL_NUMBER_OF(m_AcpiName), Path);
    ExFreePoolWithTag(Path, USB4HR_TAG_HARDWARE);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ACPI namespace path does not fit the name buffer\n");
        m_AcpiName[0] = UNICODE_NULL;
        return;
    }

    /* USBHUB3 escapes its own ACPI paths the same way before it compares */
    for (Index = 0; m_AcpiName[Index] != UNICODE_NULL; Index++)
    {
        if (m_AcpiName[Index] == L'/' || m_AcpiName[Index] == L'\\' || m_AcpiName[Index] == L'^')
            m_AcpiName[Index] = L'#';
    }

    DPRINT("ACPI name %ws\n", m_AcpiName);
}

NTSTATUS Usb4HrHardware::QueryNamesAndCmid()
{
    WDF_DEVICE_PROPERTY_DATA Property;
    DEVPROPTYPE Type;
    ULONG64 SerialNumber = 0;
    ULONG Attributes = 0;
    ULONG Required;
    NTSTATUS Status;

    m_DvsecName[0] = UNICODE_NULL;
    QueryAcpiName();

    WDF_DEVICE_PROPERTY_DATA_INIT(&Property, &USB4HR_DEVPKEY_PCI_SERIAL_NUMBER);
    Status = WdfDeviceQueryPropertyEx(m_Device, &Property, sizeof(SerialNumber), &SerialNumber, &Required, &Type);
    if (NT_SUCCESS(Status))
    {
        WDF_DEVICE_PROPERTY_DATA_INIT(&Property, &USB4HR_DEVPKEY_PCI_USB_DVSEC_ATTRIBUTES);
        Status = WdfDeviceQueryPropertyEx(m_Device, &Property, sizeof(Attributes), &Attributes, &Required, &Type);
    }

    if (NT_SUCCESS(Status))
    {
        if ((Attributes & USB4HR_DVSEC_PORT_MASK) == USB4HR_DVSEC_PORT_MASK)
        {
            DPRINT1("Invalid DVSEC port attributes 0x%lx\n", Attributes);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        Status = RtlStringCchPrintfW(m_DvsecName, RTL_NUMBER_OF(m_DvsecName), L"%I64x_%d",
                                     SerialNumber, (INT)(Attributes & USB4HR_DVSEC_PORT_MASK));
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("DVSEC name format failed 0x%lx\n", Status);
            return Status;
        }
    }
    else if (Status != STATUS_OBJECT_NAME_NOT_FOUND)
    {
        DPRINT1("DVSEC property query failed 0x%lx\n", Status);
        return Status;
    }

    /* QUIRK: the 3 bit connection manager ID is shared by all host routers, a ninth one fails */
    Status = Usb4HrAllocateCmid(&m_Cmid);
    if (!NT_SUCCESS(Status))
        return Status;

    m_CmidTaken = TRUE;
    return STATUS_SUCCESS;
}

VOID Usb4HrHardware::Cleanup()
{
    /* Windows only releases the ID when the domain ID was built and leaks it on early failures */
    if (m_CmidTaken)
    {
        Usb4HrReleaseCmid(m_Cmid);
        m_CmidTaken = FALSE;
    }

    if (m_HasBusInterface)
    {
        if (m_BusInterface.InterfaceDereference)
            m_BusInterface.InterfaceDereference(m_BusInterface.Context);
        m_HasBusInterface = FALSE;
    }
}

const Usb4HrIdentity* Usb4HrHardware::Identity() const
{
    return &m_Identity;
}

ULONG64 Usb4HrHardware::ShimFlags() const
{
    return m_ShimFlags;
}

BOOLEAN
Usb4HrHardware::HasShimFlag(
    _In_ ULONG64 Flag) const
{
    return (ShimFlags() & Flag) != 0;
}

ULONG Usb4HrHardware::DomainId() const
{
    return m_DomainId;
}

UCHAR Usb4HrHardware::Cmid() const
{
    return m_Cmid;
}

PCWSTR Usb4HrHardware::AcpiName() const
{
    return m_AcpiName;
}

PCWSTR Usb4HrHardware::DvsecName() const
{
    return m_DvsecName;
}

NTSTATUS
Usb4HrHardware::PrepareHardware(
    _In_ WDFCMRESLIST Translated)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    ULONG Count = WdfCmResourceListGetCount(Translated);
    ULONG Index;

    if (HasShimFlag(USB4HR_SHIM_MSIX_CHECK))
        DPRINT1("MSI-X table check requested by the shim flags; not supported\n");

    for (Index = 0; Index < Count; Index++)
    {
        Descriptor = WdfCmResourceListGetDescriptor(Translated, Index);
        if (!Descriptor || Descriptor->Type != CmResourceTypeMemory)
            continue;

        if (Descriptor->u.Memory.Length < USB4HR_MMIO_MIN_LENGTH)
        {
            DPRINT1("Host interface BAR of 0x%lx bytes is too small\n", Descriptor->u.Memory.Length);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        m_MmioLength = Descriptor->u.Memory.Length;
        m_Mmio = (PUCHAR)MmMapIoSpace(Descriptor->u.Memory.Start, m_MmioLength, MmNonCached);
        if (!m_Mmio)
        {
            DPRINT1("Mapping the host interface failed\n");
            m_MmioLength = 0;
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        m_WideInterruptRegisters = (PathCount() > USB4HR_NARROW_INTERRUPT_PATHS);
        DPRINT("Host interface mapped, %lu paths, version 0x%x\n", PathCount(), HostInterfaceVersion());
        return STATUS_SUCCESS;
    }

    DPRINT1("No memory resource for the host interface\n");
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

VOID Usb4HrHardware::ReleaseHardware()
{
    if (m_Mmio)
    {
        MmUnmapIoSpace(m_Mmio, m_MmioLength);
        m_Mmio = NULL;
        m_MmioLength = 0;
    }
}

BOOLEAN Usb4HrHardware::IsMmioValid() const
{
    if (!m_Mmio)
        return FALSE;

    return Read32(USB4HR_INTERRUPT_VECTOR_ALLOCATION) != 0xFFFFFFFF;
}

ULONG
Usb4HrHardware::Read32(
    _In_ ULONG Offset) const
{
    if (!m_Mmio || Offset + sizeof(ULONG) > m_MmioLength)
        return 0xFFFFFFFF;

    return READ_REGISTER_ULONG((PULONG)(m_Mmio + Offset));
}

VOID
Usb4HrHardware::Write32(
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    if (!m_Mmio || Offset + sizeof(ULONG) > m_MmioLength)
        return;

    WRITE_REGISTER_ULONG((PULONG)(m_Mmio + Offset), Value);
}

ULONG64
Usb4HrHardware::Read64(
    _In_ ULONG Offset) const
{
    if (!m_Mmio || Offset + sizeof(ULONG64) > m_MmioLength)
        return 0xFFFFFFFFFFFFFFFFULL;

    if (HasShimFlag(USB4HR_SHIM_32BIT_ACCESS))
        return (ULONG64)Read32(Offset) | ((ULONG64)Read32(Offset + sizeof(ULONG)) << 32);

    return READ_REGISTER_ULONG64((PULONG64)(m_Mmio + Offset));
}

VOID
Usb4HrHardware::Write64(
    _In_ ULONG Offset,
    _In_ ULONG64 Value)
{
    if (!m_Mmio || Offset + sizeof(ULONG64) > m_MmioLength)
        return;

    if (HasShimFlag(USB4HR_SHIM_32BIT_ACCESS))
    {
        Write32(Offset, (ULONG)Value);
        Write32(Offset + sizeof(ULONG), (ULONG)(Value >> 32));
        return;
    }

    WRITE_REGISTER_ULONG64((PULONG64)(m_Mmio + Offset), Value);
}

ULONG Usb4HrHardware::PathCount() const
{
    if (!m_Mmio)
        return 0;

    return Read32(USB4HR_HOST_CAPABILITIES) & USB4HR_HOST_CAPS_PATHS_MASK;
}

UCHAR Usb4HrHardware::HostInterfaceVersion() const
{
    if (!m_Mmio)
        return 0;

    return (UCHAR)((Read32(USB4HR_HOST_CAPABILITIES) & USB4HR_HOST_CAPS_VERSION_MASK) >> USB4HR_HOST_CAPS_VERSION_SHIFT);
}

BOOLEAN Usb4HrHardware::WideInterruptRegisters() const
{
    return m_WideInterruptRegisters;
}

NTSTATUS Usb4HrHardware::SetForcePower()
{
    ULONG Value = 0;
    ULONG Elapsed;
    NTSTATUS Status;

    Status = ReadPciConfig(USB4HR_PCI_FORCE_POWER, &Value, sizeof(Value));
    if (!NT_SUCCESS(Status))
        return Status;

    Value = (Value & USB4HR_FORCE_POWER_KEEP_MASK) | USB4HR_PCI_FORCE_POWER_BITS;
    Status = WritePciConfig(USB4HR_PCI_FORCE_POWER, &Value, sizeof(Value));
    if (!NT_SUCCESS(Status))
        return Status;

    Status = ReadPciConfig(USB4HR_PCI_FORCE_POWER, &Value, sizeof(Value));
    if (!NT_SUCCESS(Status))
        return Status;

    for (Elapsed = 0; Elapsed < USB4HR_FORCE_POWER_TIMEOUT_MS; Elapsed += USB4HR_FORCE_POWER_POLL_MS)
    {
        Status = ReadPciConfig(USB4HR_PCI_FORCE_POWER_STATUS, &Value, sizeof(Value));
        if (!NT_SUCCESS(Status))
            return Status;

        if (Value & USB4HR_PCI_FORCE_POWER_DONE)
            return STATUS_SUCCESS;

        Usb4HrSleepMs(USB4HR_FORCE_POWER_POLL_MS);
    }

    DPRINT1("Force power did not complete\n");
    return STATUS_IO_TIMEOUT;
}

NTSTATUS Usb4HrHardware::ClearForcePower()
{
    ULONG Value = 0;
    NTSTATUS Status;

    Status = ReadPciConfig(USB4HR_PCI_FORCE_POWER, &Value, sizeof(Value));
    if (!NT_SUCCESS(Status))
        return Status;

    if (!(Value & USB4HR_FORCE_POWER_REQUEST))
        return STATUS_SUCCESS;

    Value &= ~USB4HR_FORCE_POWER_REQUEST;
    return WritePciConfig(USB4HR_PCI_FORCE_POWER, &Value, sizeof(Value));
}

NTSTATUS
Usb4HrHardware::ConfigHostInterface(
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    ULONG Paths;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(PreviousState);

    if (HasShimFlag(USB4HR_SHIM_FORCE_POWER))
    {
        /* Windows dereferences a missing PCI helper when the flag is set on a non Intel router */
        if (!m_HasBusInterface)
        {
            DPRINT1("Force power flagged without PCI configuration access\n");
        }
        else
        {
            Status = SetForcePower();
            if (!NT_SUCCESS(Status))
            {
                DPRINT1("Force power failed 0x%lx\n", Status);
                return Status;
            }
        }
    }

    if (!IsMmioValid())
    {
        DPRINT1("Host interface MMIO reads all ones\n");
        return STATUS_DEVICE_NOT_READY;
    }

    Paths = PathCount();
    if (Paths < 1 || Paths > USB4HR_MAX_PATHS)
    {
        DPRINT1("Host interface reports %lu paths\n", Paths);
        return STATUS_INVALID_PARAMETER;
    }

    m_WideInterruptRegisters = (Paths > USB4HR_NARROW_INTERRUPT_PATHS);
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrHardware::DeconfigHostInterface(
    _In_ BOOLEAN DevicesConnected)
{
    NTSTATUS Status;

    /* Force power is only dropped for an idle power down with nothing attached */
    if (HasShimFlag(USB4HR_SHIM_FORCE_POWER) &&
        m_HasBusInterface &&
        !DevicesConnected &&
        WdfDeviceGetSystemPowerAction(m_Device) == PowerActionNone)
    {
        Status = ClearForcePower();
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Clearing force power failed 0x%lx\n", Status);
            return Status;
        }
    }

    return STATUS_SUCCESS;
}

BOOLEAN Usb4HrHardware::IsHostRouterResetRequired() const
{
    return m_Usb4V2Enabled && HostInterfaceVersion() >= USB4HR_HOST_VERSION_2;
}

BOOLEAN
Usb4HrHardware::WaitResetBitClear(
    _In_ ULONG TimeoutMs)
{
    ULONG64 Start = KeQueryInterruptTime();

    for (;;)
    {
        Usb4HrSleepMs(USB4HR_RESET_POLL_MS);

        if (!(Read32(USB4HR_HOST_ROUTER_RESET) & USB4HR_HOST_ROUTER_RESET_BIT))
            return TRUE;

        if ((KeQueryInterruptTime() - Start) / 10000 >= TimeoutMs)
            return FALSE;
    }
}

NTSTATUS Usb4HrHardware::ResetHostRouter()
{
    BOOLEAN Cleared;

    if (Read32(USB4HR_HOST_ROUTER_RESET) & USB4HR_HOST_ROUTER_RESET_BIT)
    {
        /* A reset is already running; only wait for it */
        DPRINT1("Host router reset bit already set\n");
        Cleared = WaitResetBitClear(USB4HR_RESET_TIMEOUT_MS);
    }
    else
    {
        Write32(USB4HR_HOST_ROUTER_RESET, Read32(USB4HR_HOST_ROUTER_RESET) | USB4HR_HOST_ROUTER_RESET_BIT);
        Cleared = WaitResetBitClear(USB4HR_RESET_TIMEOUT_MS);
    }

    if (!Cleared)
    {
        DPRINT1("Host router reset did not complete\n");
        WdfDeviceSetFailed(m_Device, WdfDeviceFailedAttemptRestart);
        return STATUS_DEVICE_NOT_READY;
    }

    DPRINT("Host router reset done, domain 0x%lx\n", m_DomainId);
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrHardware::ReadPciConfig(
    _In_ ULONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    if (!m_HasBusInterface || !m_BusInterface.GetBusData)
    {
        RtlZeroMemory(Buffer, Length);
        DPRINT1("PCI configuration read without a bus interface\n");
        return STATUS_UNSUCCESSFUL;
    }

    if (m_BusInterface.GetBusData(m_BusInterface.Context, PCI_WHICHSPACE_CONFIG, Buffer, Offset, Length) != Length)
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}

NTSTATUS
Usb4HrHardware::WritePciConfig(
    _In_ ULONG Offset,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length)
{
    if (!m_HasBusInterface || !m_BusInterface.SetBusData)
    {
        DPRINT1("PCI configuration write without a bus interface\n");
        return STATUS_UNSUCCESSFUL;
    }

    if (m_BusInterface.SetBusData(m_BusInterface.Context, PCI_WHICHSPACE_CONFIG, Buffer, Offset, Length) != Length)
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}

BOOLEAN Usb4HrHardware::QueryD3ColdSupport()
{
    D3COLD_SUPPORT_INTERFACE Interface;
    BOOLEAN Supported = FALSE;
    NTSTATUS Status;

    RtlZeroMemory(&Interface, sizeof(Interface));
    Status = WdfFdoQueryForInterface(m_Device,
                                     &GUID_D3COLD_SUPPORT_INTERFACE,
                                     (PINTERFACE)&Interface,
                                     sizeof(Interface),
                                     D3COLD_SUPPORT_INTERFACE_VERSION,
                                     NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("No D3cold interface 0x%lx\n", Status);
        return FALSE;
    }

    if (Interface.GetD3ColdCapability)
    {
        Status = Interface.GetD3ColdCapability(Interface.Context, &Supported);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("D3cold capability query failed 0x%lx\n", Status);
            Supported = FALSE;
        }
    }

    /* Windows never releases this interface */
    if (Interface.InterfaceDereference)
        Interface.InterfaceDereference(Interface.Context);

    return Supported;
}

BOOLEAN Usb4HrHardware::QueryResetSupport()
{
    USB4HR_DEVICE_RESET_INTERFACE Interface;
    NTSTATUS Status;

    RtlZeroMemory(&Interface, sizeof(Interface));
    Status = WdfFdoQueryForInterface(m_Device,
                                     &GUID_USB4HR_DEVICE_RESET_INTERFACE,
                                     &Interface.Header,
                                     sizeof(Interface),
                                     1,
                                     NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("No device reset interface 0x%lx\n", Status);
        return FALSE;
    }

    if (Interface.Header.InterfaceDereference)
        Interface.Header.InterfaceDereference(Interface.Header.Context);

    return TRUE;
}
