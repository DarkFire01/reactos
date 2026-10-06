/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tunnel segments: HopID allocation, path credits, both halves of a tunnel request
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/* Thunderbolt 3 lane credits for a USB3 or PCIe path */
#define USB4DR_TBT3_LANE_CREDITS_BONDED     32
#define USB4DR_TBT3_LANE_CREDITS_SINGLE     16

/* Thunderbolt 3 protocol adapters get 7 credits on their own hop, USB4 ones keep their preset */
#define USB4DR_TBT3_PROTOCOL_CREDITS        7

/* Smallest PCIe share Windows plans with; a smaller baMaxPCIe is raised to it */
#define USB4DR_PCIE_MIN_CREDITS             6

/* baMinDPmain Windows uses instead of the router's value with USB4DR_FLAG_BUFFER_CALCULATION */
#define USB4DR_DP_MAIN_CREDITS_ADJUSTED     18

/* HopIDs are 7 bits wide in path and adapter configuration space */
#define USB4DR_MAX_HOPID                    127

/** Smallest free HopID allocator of one lane adapter; HopIDs 0 to 7 are never handed out. */
class Usb4DrHopIdAllocator
{
public:
    /** MaxHopId is the smaller of the adapter's maximum input and output HopIDs. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Initialize(
        _In_ USHORT MaxHopId);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Destroy();

    /** Reserves the smallest free HopID; STATUS_INSUFFICIENT_RESOURCES when none is left. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    Reserve(
        _Out_ PUSHORT HopId);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    Release(
        _In_ USHORT HopId);

private:
    KSPIN_LOCK m_Lock;
    RTL_BITMAP m_Bitmap;
    USHORT m_MaxHopId;          /**< 0 until Initialize */
    ULONG m_Bits[(USB4DR_MAX_HOPID + 1) / 32];
};

/** HopIDs and credits of this router's lane adapters, and the tunnels this router helped build. */
class Usb4DrSegments
{
public:
    /** One HopID allocator per lane adapter of the router. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Create(
        _In_ Usb4DrFdo* Fdo);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Destroy();

    /**
     * Child half of a USB3 or PCIe tunnel: Inbound[0] on the up adapter, Outbound[1] on
     * the UFP lane 0 adapter with a new HopID, rate and encapsulation fields.
     */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    FillChildHalf(
        _In_ Usb4DrProtocolAdapter* UpAdapter,
        _Out_ PUSB4HR_CREATE_TUNNEL_INPUT Input);

    /** Gives back the HopID FillChildHalf took on the UFP lane 0 adapter. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    ReleaseChildHalf(
        _In_ const USB4HR_CREATE_TUNNEL_INPUT* Input);

    /**
     * Parent half, on the DFP the child hangs off: Inbound[1] on the DFP lane 0 adapter with
     * a new HopID, Outbound[0] on the DFP's down adapter. *Reservation tracks what was taken.
     */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    FillParentHalf(
        _In_ Usb4DrPort* Dfp,
        _Inout_ PUSB4HR_CREATE_TUNNEL_INPUT Input,
        _Out_ PVOID* Reservation);

    /** CREATE_TUNNEL succeeded upstream: the reservation now belongs to TunnelHandle. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    CommitParentHalf(
        _In_ PVOID Reservation,
        _In_ USB4HR_HANDLE TunnelHandle);

    /** CREATE_TUNNEL failed upstream, or DESTROY_TUNNEL completed: the HopID is free again. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    ReleaseParentHalf(
        _In_ PVOID Reservation);

    /** The reservation committed for TunnelHandle, NULL when this router did not take part. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    PVOID
    FindByTunnel(
        _In_ USB4HR_HANDLE TunnelHandle);

    /** Credits for a USB3 or PCIe path on a lane adapter of Port. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    LaneCredits(
        _In_ Usb4DrPort* Port,
        _In_ ULONG TunnelType,
        _Out_ PUCHAR Credits);

private:
    friend class Usb4DrTunnels;

    struct Reservation;

    /** Sets up one allocator per lane adapter once the router read its adapters. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID BuildAllocators();

    /** TRUE while a parent half through the down adapter DownAdapter is reserved or committed. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    BOOLEAN
    HasTunnelThrough(
        _In_ UCHAR DownAdapter);

    /** Allocator of the lane adapter Adapter. */
    Usb4DrHopIdAllocator*
    Allocator(
        _In_ Usb4DrAdapter* Adapter);

    /** TRUE when the router has a USB3 adapter of either direction. */
    BOOLEAN HasUsb3Adapters();

    /** TRUE when the router has a PCIe adapter of either direction and the policy allows PCIe. */
    BOOLEAN HasPcieAdapters();

    /** Splits the buffers of Port's lane 0 adapter between the protocols, as Windows does. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    PlanLaneBuffers(
        _In_ Usb4DrPort* Port,
        _Out_ PUCHAR Usb3Credits,
        _Out_ PUCHAR PcieCredits);

    Usb4DrFdo* m_Fdo;
    KSPIN_LOCK m_Lock;
    LIST_ENTRY m_Reservations;
    Usb4DrHopIdAllocator m_HopIds[USB4DR_MAX_ADAPTERS];
};
