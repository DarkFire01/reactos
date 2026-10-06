/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Router startup: basic configuration, capabilities, adapters, Configuration Valid
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/** USB4 version byte of version 2 routers and of version 2 connection managers */
#define USB4DR_USB4_VERSION_2           0x40
#define USB4DR_CM_VERSION_2             0x20

/** The router this FDO manages. */
class Usb4DrRouter
{
public:
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Create(
        _In_ Usb4DrFdo* Fdo);

    /**
     * Startup sequence from D0 entry: handle, header read, full read, enumeration gate,
     * adapter handles, capabilities, ROUTER_CS_1..4, Router Ready, adapters, buffer
     * allocation, DROM, flags, ports; below depth 0 also the tunneling bits and
     * Configuration Valid and Ready. FirstStart is FALSE on resume.
     */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Start(
        _In_ BOOLEAN FirstStart);

    /** D0 exit; on D3Final the adapter handles go back to the host router. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    Stop(
        _In_ WDF_POWER_DEVICE_STATE TargetState);

    /** Releases adapters, handles and the DROM. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Destroy();

    USB4HR_HANDLE Handle() const;

    /** USB4 version byte below 0x20: a Thunderbolt 3 router. */
    BOOLEAN IsTbt3() const;

    /** Version 2 features: router byte >= 0x40 and connection manager >= 0x20. */
    BOOLEAN IsUsb4V2() const;

    /** An earlier connection manager configured this router, so state it left must be cleared. */
    BOOLEAN NeedsHostCleanup() const;

    UCHAR Usb4Version() const;
    USHORT VendorId() const;
    USHORT ProductId() const;
    UCHAR Revision() const;

    /** ROUTER_CS_1 upstream lane 0 adapter number. */
    UCHAR UpstreamAdapter() const;

    /** ROUTER_CS_1 max adapter number; adapters 1 to MaxAdapter exist. */
    UCHAR MaxAdapter() const;

    /** Cached basic router dword ROUTER_CS_0..26. */
    ULONG
    BasicDword(
        _In_ ULONG Index) const;

    /** Adapter by number 1 to MaxAdapter, NULL when out of range or not present. */
    Usb4DrAdapter*
    Adapter(
        _In_ UCHAR Number);

    /** Buffer allocation parameter USB4DR_BUFFER_*, 0 when unknown. */
    UCHAR
    BufferAllocation(
        _In_ ULONG Parameter) const;

    /** USB4HR_ADAPTER_SUPPORT_* of every adapter found. */
    ULONG AdapterSupport() const;

    /** DP IN adapters of this router, capped at 255. */
    UCHAR DpInAdapterCount() const;

    /** The parent's tunnel policy (USB4 _OSC) turned PCIe tunneling off. */
    BOOLEAN IsPcieTunnelingDisabled() const;

    const Usb4DrDrom* Drom() const;
    Usb4DrRouterOps* Ops();

    /** Read and write helpers on the router space. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    ReadDwords(
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _Out_writes_(DwordCount) PULONG Buffer);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    WriteDwords(
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _In_reads_(DwordCount) const ULONG* Buffer);

private:
    _IRQL_requires_(PASSIVE_LEVEL)
    BOOLEAN CanBeEnumerated();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS WalkCapabilities();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS AllocateAdapters();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS Configure();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    WaitForReadyBit(
        _In_ ULONG Bit);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID ReadDrom();

    /** Applies the DROM unused adapter entries, then the adapter support bits and DP IN count. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID ClassifyAdapters();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS EnableTunneling();

    _IRQL_requires_(PASSIVE_LEVEL)
    BOOLEAN IsUfpInTbt3Mode();

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID ReleaseAdapters();

    Usb4DrFdo* m_Fdo;
    USB4HR_HANDLE m_Handle;
    ULONG m_Basic[USB4DR_ROUTER_BASIC_DWORDS];
    USHORT m_TmuCap;
    USHORT m_Vsc1Cap;
    USHORT m_Vsec6Cap;
    BOOLEAN m_IsTbt3;

    /** ROUTER_CS_3 Topology ID Valid was already set when the root router was configured. */
    BOOLEAN m_TopologyValidAtPowerUp;

    /** Number of adapter handles taken from the host router. */
    UCHAR m_HandleCount;
    ULONG m_AdapterSupport;
    UCHAR m_DpInCount;
    UCHAR m_BufferAllocation[USB4DR_BUFFER_MAX_USB3_GEN_T + 1];
    Usb4DrRouterOps m_Ops;
    Usb4DrDrom m_Drom;
    Usb4DrAdapter m_Adapters[USB4DR_MAX_ADAPTERS];
};
