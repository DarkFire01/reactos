/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Router and adapter handles, event queues, hot plug and notification packets
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#define USB4HR_MAX_ADAPTERS             64
#define USB4HR_EVENT_BACKLOG            16
#define USB4HR_ROOT_POWER_WAIT_MS       6000

/* A router or adapter as the handle table keeps it; defined in topology.cpp */
struct Usb4HrNode;

/** What a handle stands for. */
struct Usb4HrHandleInfo
{
    Usb4HrRoute Route;
    UCHAR Adapter;      /**< 0 for a router handle */
    BOOLEAN IsRouter;
    USB4HR_ROUTER_FAMILY RouterType;
    ULONG Usb4Version;
};

/** The routers and adapters of the domain as the device router drivers named them. */
class Usb4HrTopology
{
public:
    /** Handle table, root router handle, plug work item; registers for notification and hot plug packets. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Create(
        _In_ Usb4HrHostRouter* HostRouter);

    /** Completes every parked request and frees every handle. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Cleanup();

    /** Handle of the host router itself (route 0), allocated by Create. */
    USB4HR_HANDLE RootRouterHandle() const;

    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    AllocateRouterHandle(
        _In_ const USB4HR_TOPOLOGY_ID* TopologyId,
        _Out_ PUSB4HR_HANDLE Handle);

    /** Tears down tunnels through the router, purges its queues and frees the handle. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    DestroyRouterHandle(
        _In_ USB4HR_HANDLE Handle);

    /** Handles[n] is adapter n + 1; all or nothing. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    IssueAdapterHandles(
        _In_ USB4HR_HANDLE RouterHandle,
        _In_ UCHAR Count,
        _Out_writes_(Count) PUSB4HR_HANDLE Handles);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    RevokeAdapterHandle(
        _In_ USB4HR_HANDLE Handle);

    /** Records type and version on every node with the router's route. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    ReportRouterFamily(
        _In_ USB4HR_HANDLE RouterHandle,
        _In_ USB4HR_ROUTER_FAMILY Type,
        _In_ ULONG Usb4Version);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    PurgeEventQueues(
        _In_ USB4HR_HANDLE Handle,
        _In_ BOOLEAN Notifications,
        _In_ BOOLEAN InterDomain);

    /** The depth 0 device router finished D0 entry. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    OnRootRouterStarted(
        _In_ USB4HR_HANDLE RouterHandle,
        _In_ NTSTATUS D0Status);

    /** Sets the root router powered on event without a device router, for a failed D0 entry. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID SignalRootRouterStarted();

    /** Clears the root router powered on event at D0 exit. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID ResetRootRouterStarted();

    /** STATUS_UNSUCCESSFUL after TimeoutMs. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    WaitForRootRouterStarted(
        _In_ ULONG TimeoutMs);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    BOOLEAN
    IsHandleValid(
        _In_ USB4HR_HANDLE Handle);

    /** STATUS_INVALID_PARAMETER for an unknown handle. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    QueryHandle(
        _In_ USB4HR_HANDLE Handle,
        _Out_ Usb4HrHandleInfo* Info);

    /** Any router below the host router has a handle. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    BOOLEAN AreDevicesConnected();

    /** Synchronous read through the node's route, adapter and sequence numbers. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    ReadConfig(
        _In_ USB4HR_HANDLE Handle,
        _In_ ULONG Space,
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _Out_writes_(DwordCount) PULONG Buffer,
        _Out_ PUSB4HR_STATUS Status);

    /** Synchronous write through the node's route, adapter and sequence numbers. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    WriteConfig(
        _In_ USB4HR_HANDLE Handle,
        _In_ ULONG Space,
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _In_reads_(DwordCount) const ULONG* Buffer,
        _Out_ PUSB4HR_STATUS Status);

    /** IOCTL_USB4HR_READ_CONFIG and IOCTL_USB4HR_READ_CONFIG_EX. Completes or hands off the request. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    OnReadConfig(
        _In_ WDFREQUEST Request,
        _In_ BOOLEAN Extended);

    /** IOCTL_USB4HR_WRITE_CONFIG. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    OnWriteConfig(
        _In_ WDFREQUEST Request);

    /** IOCTL_USB4HR_WAIT_ROUTER_EVENT: completes with a stored event or parks the request. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    OnWaitRouterEvent(
        _In_ WDFREQUEST Request);

    /** IOCTL_USB4HR_WAIT_ADAPTER_EVENT. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    OnWaitAdapterEvent(
        _In_ WDFREQUEST Request);

private:
    /** New node in the handle table; STATUS_UNSUCCESSFUL when route and adapter already have one. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    AllocateNode(
        _In_ const Usb4HrRoute* Route,
        _In_ UCHAR Adapter,
        _Out_ Usb4HrNode** Node);

    /** Takes the node out of the table, stops its queues and drops the table reference. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    DestroyNode(
        _In_ USB4HR_HANDLE Handle);

    _Requires_lock_held_(m_Lock)
    Usb4HrNode*
    FindNodeLocked(
        _In_ USB4HR_HANDLE Handle);

    _Requires_lock_held_(m_Lock)
    Usb4HrNode*
    FindNodeByRouteLocked(
        _In_ const Usb4HrRoute* Route,
        _In_ UCHAR Adapter);

    /** Node behind Handle with a reference the caller drops with Usb4HrReleaseNode, or NULL. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    Usb4HrNode*
    ReferenceNode(
        _In_ USB4HR_HANDLE Handle);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    Usb4HrNode*
    ReferenceNodeByRoute(
        _In_ const Usb4HrRoute* Route,
        _In_ UCHAR Adapter);

    _Requires_lock_held_(m_Lock)
    VOID
    SetRouteTypeLocked(
        _In_ const Usb4HrRoute* Route,
        _In_ USB4HR_ROUTER_FAMILY Type,
        _In_ ULONG Usb4Version);

    /** Route, adapter and next sequence number of the node, after the space check. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    PrepareConfigTarget(
        _In_ USB4HR_HANDLE Handle,
        _In_ ULONG Space,
        _Out_ Usb4HrConfigTarget* Target);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    HandleConfigIoctl(
        _In_ WDFREQUEST Request,
        _In_ Usb4HrConfigKind Kind);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID
    HandleWaitIoctl(
        _In_ WDFREQUEST Request,
        _In_ BOOLEAN RouterEvent);

    /** Acknowledges one hot plug packet and queues it on its node. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    ProcessPlugPacket(
        _In_reads_(USB4HR_PACKET_HEADER_DWORDS) const ULONG* Dwords);

    static USB4HR_RX_HANDLER OnNotificationPacket;
    static USB4HR_RX_HANDLER OnHotPlugPacket;
    static EVT_WDF_WORKITEM EvtPlugWorkItem;

    Usb4HrHostRouter* m_HostRouter;

    /** Protects the node list, the router count and the per node type fields. */
    KSPIN_LOCK m_Lock;
    LIST_ENTRY m_Nodes;
    ULONG m_RouterCount;
    Usb4HrNode* m_RootNode;

    KEVENT m_RootPoweredOn;

    /** Hot plug packets copied at DISPATCH_LEVEL, handled by the work item. */
    KSPIN_LOCK m_PlugLock;
    LIST_ENTRY m_PlugPackets;
    WDFWORKITEM m_PlugWorkItem;
};

/* Hardware services interface slots; Context is the Usb4HrTopology */
USB4HR_ALLOCATE_ROUTER_HANDLE Usb4HrServiceAllocateRouterHandle;
USB4HR_DESTROY_ROUTER_HANDLE Usb4HrServiceDestroyRouterHandle;
USB4HR_ISSUE_ADAPTER_HANDLES Usb4HrServiceIssueAdapterHandles;
USB4HR_REVOKE_ADAPTER_HANDLE Usb4HrServiceRevokeAdapterHandle;
USB4HR_REPORT_ROUTER_FAMILY Usb4HrServiceReportRouterFamily;
USB4HR_PURGE_EVENT_QUEUES Usb4HrServicePurgeEventQueues;
USB4HR_ROOT_ROUTER_STARTED Usb4HrServiceRootRouterStarted;
USB4HR_REPORT_UPSTREAM_LINK_BANDWIDTH Usb4HrServiceReportUpstreamLinkBandwidth;
USB4HR_READ_DP_BANDWIDTH_GRANT Usb4HrServiceReadDpBandwidthGrant;
USB4HR_PIN_DOMAIN_FOR_DP_ALT_MODE Usb4HrServicePinDomainForDpAltMode;
USB4HR_UNPIN_DOMAIN_FOR_DP_ALT_MODE Usb4HrServiceUnpinDomainForDpAltMode;
