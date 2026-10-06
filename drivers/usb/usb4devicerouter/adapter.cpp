/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     One adapter of the router: basic configuration space and capability offsets
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4dr.h"

#define NDEBUG
#include <debug.h>

/* Folds the NT result and the USB4 status of one config access into one NTSTATUS */
static
NTSTATUS
NTAPI
Usb4DrAdapterAccessResult(
    _In_ NTSTATUS Status,
    _In_ USB4HR_STATUS Usb4Status)
{
    if (Usb4Status == USB4HR_STATUS_ERR_ADDR)
        return STATUS_NO_SUCH_DEVICE;

    if (!NT_SUCCESS(Status))
        return Status;

    return (Usb4Status == USB4HR_STATUS_SUCCESS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

VOID
Usb4DrAdapter::Attach(
    _In_ Usb4DrFdo* Fdo,
    _In_ UCHAR Number,
    _In_ USB4HR_HANDLE Handle)
{
    m_Fdo = Fdo;
    m_Number = Number;
    m_Handle = Handle;
    m_Present = FALSE;
    m_DromUnused = FALSE;
}

NTSTATUS
Usb4DrAdapter::Initialize(
    _In_ Usb4DrFdo* Fdo,
    _In_ UCHAR Number,
    _In_ USB4HR_HANDLE Handle)
{
    NTSTATUS Status;

    m_Fdo = Fdo;
    m_Number = Number;
    m_Handle = Handle;
    m_Present = FALSE;
    m_DromUnused = FALSE;
    m_LaneCap = 0;
    m_PortCap = 0;
    m_ProtocolCap = 0;
    m_VendorCap = 0;
    RtlZeroMemory(m_Basic, sizeof(m_Basic));

    Status = RefreshBasicConfig();
    if (Status == STATUS_NO_SUCH_DEVICE)
    {
        DPRINT("Adapter %u is not implemented\n", Number);
        return Status;
    }
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Adapter %u basic config read failed 0x%lx\n", Number, Status);
        return Status;
    }

    Status = WalkCapabilities();
    if (!NT_SUCCESS(Status))
        return Status;

    m_Present = TRUE;
    DPRINT("Adapter %u protocol 0x%02x sub type %u lane %u port %u protocol cap %u\n",
           Number, Protocol(), SubType(), m_LaneCap, m_PortCap, m_ProtocolCap);
    return STATUS_SUCCESS;
}

NTSTATUS Usb4DrAdapter::RefreshBasicConfig()
{
    ULONG Basic[USB4DR_ADAPTER_BASIC_DWORDS];
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    NTSTATUS Status;

    Status = m_Fdo->HostLink()->ReadConfig(m_Handle,
                                           USB4HR_SPACE_ADAPTER,
                                           USB4DR_ADAPTER_CS_0,
                                           RTL_NUMBER_OF(Basic),
                                           Basic,
                                           &Usb4Status);
    Status = Usb4DrAdapterAccessResult(Status, Usb4Status);
    if (NT_SUCCESS(Status))
        RtlCopyMemory(m_Basic, Basic, sizeof(m_Basic));

    return Status;
}

VOID Usb4DrAdapter::Destroy()
{
    const USB4HR_HARDWARE_SERVICES* Services;
    NTSTATUS Status;

    m_Present = FALSE;
    if (m_Handle == NULL || m_Fdo == NULL)
        return;

    Services = m_Fdo->HostLink()->Services();
    Status = Services->DestroyAdapterHandle(Services->Header.Context, m_Handle);
    if (!NT_SUCCESS(Status))
        DPRINT1("Adapter %u handle destroy failed 0x%lx\n", m_Number, Status);

    m_Handle = NULL;
}

BOOLEAN
Usb4DrAdapter::IsPresent() const
{
    return m_Present;
}

UCHAR
Usb4DrAdapter::Number() const
{
    return m_Number;
}

USB4HR_HANDLE
Usb4DrAdapter::Handle() const
{
    return m_Handle;
}

UCHAR
Usb4DrAdapter::Protocol() const
{
    return (UCHAR)Usb4DrField(m_Basic[USB4DR_ADAPTER_CS_2], USB4DR_ADAPTER_CS2_PROTOCOL_MASK);
}

UCHAR
Usb4DrAdapter::SubType() const
{
    return (UCHAR)Usb4DrField(m_Basic[USB4DR_ADAPTER_CS_2], USB4DR_ADAPTER_CS2_SUBTYPE_MASK);
}

BOOLEAN
Usb4DrAdapter::IsLane() const
{
    return m_Present && !m_DromUnused && Protocol() == USB4DR_PROTOCOL_LANE && SubType() == USB4DR_SUBTYPE_LANE;
}

BOOLEAN
Usb4DrAdapter::IsUsb3() const
{
    /* Windows skips a USB3 adapter that reports protocol version 0 */
    return m_Present && !m_DromUnused && Protocol() == USB4DR_PROTOCOL_USB3 &&
           (SubType() == USB4DR_SUBTYPE_DOWN || SubType() == USB4DR_SUBTYPE_UP) &&
           Usb4DrField(m_Basic[USB4DR_ADAPTER_CS_2], USB4DR_ADAPTER_CS2_VERSION_MASK) != 0;
}

BOOLEAN
Usb4DrAdapter::IsPcie() const
{
    /* Windows skips a PCIe adapter that reports protocol version 0 */
    return m_Present && !m_DromUnused && Protocol() == USB4DR_PROTOCOL_PCIE &&
           (SubType() == USB4DR_SUBTYPE_DOWN || SubType() == USB4DR_SUBTYPE_UP) &&
           Usb4DrField(m_Basic[USB4DR_ADAPTER_CS_2], USB4DR_ADAPTER_CS2_VERSION_MASK) != 0;
}

BOOLEAN
Usb4DrAdapter::IsUp() const
{
    return m_Present && Protocol() != USB4DR_PROTOCOL_LANE && SubType() == USB4DR_SUBTYPE_UP;
}

BOOLEAN
Usb4DrAdapter::IsDpIn() const
{
    return IsDp() && SubType() == USB4DR_SUBTYPE_DOWN;
}

BOOLEAN
Usb4DrAdapter::IsDp() const
{
    /* Windows skips a DP adapter that reports protocol version 0 */
    return m_Present && !m_DromUnused &&
           Protocol() == USB4DR_PROTOCOL_DP &&
           (SubType() == USB4DR_SUBTYPE_DOWN || SubType() == USB4DR_SUBTYPE_UP) &&
           Usb4DrField(m_Basic[USB4DR_ADAPTER_CS_2], USB4DR_ADAPTER_CS2_VERSION_MASK) != 0;
}

BOOLEAN
Usb4DrAdapter::IsDromUnused() const
{
    return m_DromUnused;
}

VOID
Usb4DrAdapter::MarkDromUnused()
{
    m_DromUnused = TRUE;
}

ULONG
Usb4DrAdapter::BasicDword(
    _In_ ULONG Index) const
{
    if (Index >= USB4DR_ADAPTER_BASIC_DWORDS)
        return 0;

    return m_Basic[Index];
}

UCHAR
Usb4DrAdapter::LaneCapability() const
{
    return m_LaneCap;
}

UCHAR
Usb4DrAdapter::PortCapability() const
{
    return m_PortCap;
}

UCHAR
Usb4DrAdapter::ProtocolCapability() const
{
    return m_ProtocolCap;
}

UCHAR
Usb4DrAdapter::VendorCapability() const
{
    return m_VendorCap;
}

USHORT
Usb4DrAdapter::MaxInputHopId() const
{
    return (USHORT)Usb4DrField(m_Basic[USB4DR_ADAPTER_CS_5], USB4DR_ADAPTER_CS5_MAX_IN_HOPID);
}

USHORT
Usb4DrAdapter::MaxOutputHopId() const
{
    return (USHORT)Usb4DrField(m_Basic[USB4DR_ADAPTER_CS_5], USB4DR_ADAPTER_CS5_MAX_OUT_HOPID);
}

NTSTATUS
Usb4DrAdapter::ReadDword(
    _In_ ULONG DwordOffset,
    _Out_ PULONG Value)
{
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    NTSTATUS Status;

    *Value = 0;
    if (m_Handle == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    Status = m_Fdo->HostLink()->ReadConfig(m_Handle,
                                           USB4HR_SPACE_ADAPTER,
                                           DwordOffset,
                                           1,
                                           Value,
                                           &Usb4Status);
    return Usb4DrAdapterAccessResult(Status, Usb4Status);
}

NTSTATUS
Usb4DrAdapter::WriteDword(
    _In_ ULONG DwordOffset,
    _In_ ULONG Value)
{
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    NTSTATUS Status;

    if (m_Handle == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    Status = m_Fdo->HostLink()->WriteConfig(m_Handle,
                                            USB4HR_SPACE_ADAPTER,
                                            DwordOffset,
                                            1,
                                            &Value,
                                            &Usb4Status);
    return Usb4DrAdapterAccessResult(Status, Usb4Status);
}

NTSTATUS
Usb4DrAdapter::ReadLaneStatus(
    _Out_ PULONG LaneCs1)
{
    *LaneCs1 = 0;
    if (m_LaneCap == 0)
        return STATUS_NOT_SUPPORTED;

    return ReadDword(m_LaneCap + USB4DR_LANE_CS_1, LaneCs1);
}

NTSTATUS
Usb4DrAdapter::ReadControlCredits(
    _Out_ PUCHAR Credits)
{
    USB4HR_STATUS Usb4Status = USB4HR_STATUS_FAILURE;
    ULONG PathCs0 = 0;
    NTSTATUS Status;

    *Credits = 0;
    if (m_Handle == NULL)
        return STATUS_INVALID_DEVICE_STATE;

    /* HopID 0 is the first path entry; its credits are the buffers kept for control packets */
    Status = m_Fdo->HostLink()->ReadConfig(m_Handle,
                                           USB4HR_SPACE_PATH,
                                           0,
                                           1,
                                           &PathCs0,
                                           &Usb4Status);
    Status = Usb4DrAdapterAccessResult(Status, Usb4Status);
    if (NT_SUCCESS(Status))
        *Credits = (UCHAR)Usb4DrField(PathCs0, USB4DR_PATH_CS0_CREDITS_MASK);

    return Status;
}

NTSTATUS Usb4DrAdapter::WalkCapabilities()
{
    ULONG Offset;
    ULONG Header;
    ULONG VsecHeader;
    ULONG Count;
    UCHAR Id;
    UCHAR Cached;
    NTSTATUS Status;

    Offset = Usb4DrField(m_Basic[USB4DR_ADAPTER_CS_1], USB4DR_ADAPTER_CS1_NEXT_CAP_MASK);

    for (Count = 0; Offset != 0; Count++)
    {
        if (Count >= USB4DR_MAX_CAPABILITIES)
        {
            DPRINT1("Adapter %u capability list does not end\n", m_Number);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        Status = ReadDword(Offset, &Header);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("Adapter %u capability read at %lu failed 0x%lx\n", m_Number, Offset, Status);
            return Status;
        }

        Id = (UCHAR)Usb4DrField(Header, USB4DR_CAP_ID_MASK);

        /* A vendor capability with a zero length is the long form; its next pointer sits in the second dword */
        if (Id == USB4DR_ADAPTER_CAP_VENDOR && Usb4DrField(Header, USB4DR_CAP_VSC_LENGTH_MASK) == 0)
        {
            Status = ReadDword(Offset + 1, &VsecHeader);
            if (!NT_SUCCESS(Status))
                return Status;

            Offset = VsecHeader & 0xFFFF;
            continue;
        }

        /* Offsets past a long vendor capability can exceed a byte; those are not kept */
        Cached = (Offset <= MAXUCHAR) ? (UCHAR)Offset : 0;

        switch (Id)
        {
            case USB4DR_ADAPTER_CAP_LANE:
                if (m_LaneCap == 0)
                    m_LaneCap = Cached;
                break;

            case USB4DR_ADAPTER_CAP_PROTOCOL:
                if (m_ProtocolCap == 0)
                    m_ProtocolCap = Cached;
                break;

            case USB4DR_ADAPTER_CAP_VENDOR:
                if (m_VendorCap == 0 && Usb4DrField(Header, USB4DR_CAP_VSC_ID_MASK) == 0)
                    m_VendorCap = Cached;
                break;

            case USB4DR_ADAPTER_CAP_USB4_PORT:
                if (m_PortCap == 0)
                    m_PortCap = Cached;
                break;

            default:
                break;
        }

        Offset = Usb4DrField(Header, USB4DR_CAP_NEXT_MASK);
    }

    return STATUS_SUCCESS;
}
