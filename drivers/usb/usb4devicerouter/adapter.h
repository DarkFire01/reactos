/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     One adapter of the router: basic configuration space and capability offsets
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/** One adapter. Embedded in Usb4DrRouter's adapter table, indexed by adapter number. */
class Usb4DrAdapter
{
public:
    /** Reads ADP_CS_0..8 and walks the adapter capability list. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Initialize(
        _In_ Usb4DrFdo* Fdo,
        _In_ UCHAR Number,
        _In_ USB4HR_HANDLE Handle);

    /** Rereads ADP_CS_0..8, for example after a resume. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS RefreshBasicConfig();

    /** Gives the handle back to the host router. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Destroy();

    BOOLEAN IsPresent() const;
    UCHAR Number() const;
    USB4HR_HANDLE Handle() const;

    /** ADP_CS_2 protocol, USB4DR_PROTOCOL_*. */
    UCHAR Protocol() const;

    /** ADP_CS_2 sub type, USB4DR_SUBTYPE_*. */
    UCHAR SubType() const;

    BOOLEAN IsLane() const;
    BOOLEAN IsUsb3() const;
    BOOLEAN IsPcie() const;
    BOOLEAN IsUp() const;

    /** DP IN adapter with a known DP protocol version, the kind that counts for buffer planning. */
    BOOLEAN IsDpIn() const;

    /** DP IN or DP OUT adapter the router uses. */
    BOOLEAN IsDp() const;

    /** The DROM marks the adapter unused: it is left alone like an adapter that does not exist. */
    BOOLEAN IsDromUnused() const;

    /** Called once the DROM was parsed. */
    VOID MarkDromUnused();

    /** One cached basic configuration dword, ADP_CS_0..8. */
    ULONG
    BasicDword(
        _In_ ULONG Index) const;

    /** Dword offset of the lane adapter, USB4 port, protocol or vendor capability; 0 when absent. */
    UCHAR LaneCapability() const;
    UCHAR PortCapability() const;
    UCHAR ProtocolCapability() const;
    UCHAR VendorCapability() const;

    /** ADP_CS_5 maximum input and output HopIDs. */
    USHORT MaxInputHopId() const;
    USHORT MaxOutputHopId() const;

    /** Reads one dword of the adapter space. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    ReadDword(
        _In_ ULONG DwordOffset,
        _Out_ PULONG Value);

    /** Writes one dword of the adapter space. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    WriteDword(
        _In_ ULONG DwordOffset,
        _In_ ULONG Value);

    /** Lane adapters: LANE_ADP_CS_1 current speed, negotiated width and adapter state. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    ReadLaneStatus(
        _Out_ PULONG LaneCs1);

    /** Lane adapters: credits of path HopID 0, the control buffers. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    ReadControlCredits(
        _Out_ PUCHAR Credits);

private:
    friend class Usb4DrRouter;

    /** Takes ownership of the handle before the adapter space is readable. */
    VOID
    Attach(
        _In_ Usb4DrFdo* Fdo,
        _In_ UCHAR Number,
        _In_ USB4HR_HANDLE Handle);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS WalkCapabilities();

    Usb4DrFdo* m_Fdo;
    USB4HR_HANDLE m_Handle;
    UCHAR m_Number;
    BOOLEAN m_Present;
    BOOLEAN m_DromUnused;
    UCHAR m_LaneCap;
    UCHAR m_PortCap;
    UCHAR m_ProtocolCap;
    UCHAR m_VendorCap;
    ULONG m_Basic[USB4DR_ADAPTER_BASIC_DWORDS];
};
