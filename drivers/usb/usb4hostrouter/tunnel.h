/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USB3 and PCIe tunnels: path programming, USB3 bandwidth, power down and rebuild
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#define USB4HR_DOMAIN_POWER_WAIT_MS     6500

/** Every tunnel of the domain and the queue that serializes tunnel requests. */
class Usb4HrTunnelManager
{
public:
    /** Sequential passive level queue for CREATE, DESTROY and REBUILD, tunnel list, power up event. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Create(
        _In_ Usb4HrHostRouter* HostRouter);

    /** Tears down and frees every tunnel. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Cleanup();

    /** Forwards a CREATE_TUNNEL, DESTROY_TUNNEL or REBUILD_TUNNEL request to the tunnel queue. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    QueueRequest(
        _In_ WDFREQUEST Request);

    /** D0 exit: every live tunnel moves to the powered down list until REBUILD_TUNNEL. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID HandleDomainSleep();

    /** Some tunnels wait for REBUILD_TUNNEL. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    BOOLEAN HasPoweredDownTunnels();

    /** Waits for the powered down list to empty; STATUS_UNSUCCESSFUL after TimeoutMs. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    WaitForPowerUp(
        _In_ ULONG TimeoutMs);

    /** A router is going away: tunnels with a segment at or below Route are torn down and freed. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    OnRouterRemoved(
        _In_ const Usb4HrRoute* Route);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    BOOLEAN
    TunnelHandleKnown(
        _In_ USB4HR_HANDLE Handle);

private:
    struct Hop;
    struct Tunnel;

    static EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtIoInternalDeviceControl;

    /* Requests, run one at a time from the tunnel queue */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    OnCreateTunnel(
        _In_ WDFREQUEST Request);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    OnDestroyTunnel(
        _In_ WDFREQUEST Request);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    OnRebuildTunnel(
        _In_ WDFREQUEST Request);

    /* Configuration space access through the topology */
    _IRQL_requires_(PASSIVE_LEVEL)
    USB4HR_STATUS
    ReadSpace(
        _In_ USB4HR_HANDLE Adapter,
        _In_ ULONG Space,
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _Out_writes_(DwordCount) PULONG Buffer);

    _IRQL_requires_(PASSIVE_LEVEL)
    USB4HR_STATUS
    WriteSpace(
        _In_ USB4HR_HANDLE Adapter,
        _In_ ULONG Space,
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _In_reads_(DwordCount) const ULONG* Buffer);

    _IRQL_requires_(PASSIVE_LEVEL)
    USB4HR_STATUS
    UpdateAdapterDword(
        _In_ USB4HR_HANDLE Adapter,
        _In_ ULONG DwordOffset,
        _In_ ULONG Value,
        _In_ ULONG Mask);

    /* Path entries */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    DescribeHop(
        _In_ const USB4HR_TUNNEL_SEGMENT* Segment,
        _In_ ULONG Type,
        _In_ BOOLEAN ProtocolSide,
        _Out_ Hop* Out);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    AdoptFixedPathFields(
        _Inout_ Hop* Entry);

    _IRQL_requires_(PASSIVE_LEVEL)
    USB4HR_STATUS
    ProgramHop(
        _In_ const Hop* Entry);

    _IRQL_requires_(PASSIVE_LEVEL)
    USB4HR_STATUS
    InvalidateHop(
        _In_ const Hop* Entry);

    _IRQL_requires_(PASSIVE_LEVEL)
    USB4HR_STATUS
    WaitHopDrained(
        _In_ const Hop* Entry);

    _IRQL_requires_(PASSIVE_LEVEL)
    USB4HR_STATUS
    TeardownHop(
        _In_ const Hop* Entry);

    _IRQL_requires_(PASSIVE_LEVEL)
    USB4HR_STATUS
    ReleaseHopCredits(
        _In_ const Hop* Entry);

    _IRQL_requires_(PASSIVE_LEVEL)
    USB4HR_STATUS
    ProgramPath(
        _In_reads_(USB4HR_PROTOCOL_PATH_HOPS) const Hop* Path);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    TeardownPath(
        _In_reads_(USB4HR_PROTOCOL_PATH_HOPS) const Hop* Path);

    /* Protocol adapters */
    _IRQL_requires_(PASSIVE_LEVEL)
    USB4HR_STATUS
    SetPathEnable(
        _In_ const Tunnel* Entry,
        _In_ BOOLEAN UpAdapter,
        _In_ BOOLEAN Enable);

    _IRQL_requires_(PASSIVE_LEVEL)
    USB4HR_STATUS
    WriteExtendedEncapsulation(
        _In_ const Tunnel* Entry,
        _In_ BOOLEAN UpAdapter,
        _In_ BOOLEAN Enable);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Configure(
        _In_ const Tunnel* Entry,
        _In_ BOOLEAN Rebuild,
        _Out_opt_ PUSB4HR_TUNNEL_BUILD_RESULT Output,
        _Out_ PUSB4HR_STATUS Usb4Status);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    Teardown(
        _In_ const Tunnel* Entry,
        _In_ BOOLEAN SkipHardware);

    /* USB3 bandwidth of the host router's USB3 down adapter */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    PlanUsb3Bandwidth(
        _Inout_ Tunnel* Entry,
        _In_ ULONG MaxLinkRate);

    _IRQL_requires_(PASSIVE_LEVEL)
    USHORT
    QueryLinkCapacity(
        _In_ USB4HR_HANDLE LaneAdapter);

    _IRQL_requires_(PASSIVE_LEVEL)
    USB4HR_STATUS
    ProgramUsb3Bandwidth(
        _In_ const Tunnel* Entry);

    /* Tunnel list */
    _Requires_lock_held_(m_ListLock)
    Tunnel*
    FindTunnelLocked(
        _In_ USB4HR_HANDLE Handle);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    BOOLEAN
    MarkPoweredUp(
        _Inout_ Tunnel* Entry);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID AcquireOperationLock();

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID ReleaseOperationLock();

    Usb4HrHostRouter* m_HostRouter;
    WDFQUEUE m_Queue;

    /** Guards m_Tunnels, the powered down flags and m_PoweredDownCount. */
    KSPIN_LOCK m_ListLock;
    LIST_ENTRY m_Tunnels;
    ULONG m_PoweredDownCount;

    /** Signaled while no tunnel waits for REBUILD_TUNNEL. */
    KEVENT m_PowerUpEvent;

    /** Serializes hardware work on tunnels between the queue and router removal. */
    KEVENT m_OperationLock;

    BOOLEAN m_Initialized;
};
