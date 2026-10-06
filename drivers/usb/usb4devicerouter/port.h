/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USB4 ports: plug handling, child router detection, handles and child lists
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/* Windows reports at most 5 s of enumeration time below a Thunderbolt 3 router */
#define USB4DR_TBT3_MAX_ENUM_SECONDS    5

/** One USB4 port: a lane 0 and a lane 1 adapter, facing up (UFP) or down (DFP). */
class Usb4DrPort
{
public:
    /** Builds the port; a DFP also gets its child list and its PDO request queue. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Initialize(
        _In_ Usb4DrFdo* Fdo,
        _In_ Usb4DrAdapter* Lane0,
        _In_opt_ Usb4DrAdapter* Lane1,
        _In_ BOOLEAN IsDfp,
        _In_ ULONG DfpIndex);

    /** D0 entry: a DFP reads its plug state, arms the event pump and handles a router already there. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Start(
        _In_ BOOLEAN FirstStart);

    /** D0 exit: stops the pump and waits for plug work; on D3Final the child goes missing. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    Stop(
        _In_ WDF_POWER_DEVICE_STATE TargetState);

    /** Frees the event pump, the child handle and the work item. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Destroy();

    BOOLEAN IsDfp() const;

    /** Position among the DFPs; DFP n pairs with USB3 down adapter n. */
    ULONG DfpIndex() const;

    Usb4DrAdapter* Lane0();
    Usb4DrAdapter* Lane1();
    UCHAR Lane0Number() const;

    /** DFP: child list of the router behind this port. */
    WDFCHILDLIST ChildList() const;

    /** DFP: queue that receives the router to router requests of the child PDO. */
    WDFQUEUE PdoRequestQueue() const;

    /** Topology ID of the router behind this DFP. */
    VOID
    ChildTopologyId(
        _Out_ PUSB4HR_TOPOLOGY_ID TopologyId) const;

    /** Connect generation; bumped when a different router shows up or on a start while unplugged. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    ULONG Generation();

    /** TRUE when Generation still equals the current one. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    BOOLEAN
    IsGenerationCurrent(
        _In_ ULONG Generation);

    /** Child PDO PrepareHardware: a reference on the child router handle, only for the current generation. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    AcquireChildHandle(
        _In_ ULONG Generation,
        _Out_ PUSB4HR_HANDLE Handle);

    /** Drops the reference from AcquireChildHandle. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID ReleaseChildHandle();

    /** Parent interface callbacks of the child PDO. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    BOOLEAN IsLaneBondingSupported();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    BOOLEAN IsAsymmetricSupported();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    ULONG
    GetMaxEnumerationTime(
        _In_ ULONG Seconds);

    /** UFP: upstream link bandwidth in Gbps from the lane adapter speed and width; 0 when down. */
    _IRQL_requires_(PASSIVE_LEVEL)
    ULONG LinkGbps();

private:
    friend class Usb4DrPortSet;

    static USB4DR_EVENT_CALLBACK OnEvent;
    static EVT_WDF_WORKITEM EvtPlugWork;

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID HandlePlug();

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID HandleUnplug();

    /** Reads the basic router space of the child; TBT3 routers also report their upstream adapter. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    ReadChildRouter(
        _In_ USB4HR_HANDLE Handle,
        _Out_writes_(USB4DR_ROUTER_BASIC_DWORDS) PULONG Basic,
        _Out_ PUCHAR UpstreamAdapter);

    /** Hands Identity to the child list as the one router present behind this DFP. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    ReportChild(
        _In_ const Usb4DrRouterIdentity* Identity);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID ReportChildMissing();

    /** Waits for the child PDO to drop its handle reference, then gives the handle back. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID DeleteChildHandle();

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID ClearLane1Disable();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID BumpGeneration();

    Usb4DrFdo* m_Fdo;
    Usb4DrAdapter* m_Lane0;
    Usb4DrAdapter* m_Lane1;
    BOOLEAN m_IsDfp;
    ULONG m_DfpIndex;
    WDFCHILDLIST m_ChildList;
    WDFQUEUE m_PdoRequestQueue;
    WDFWORKITEM m_PlugWork;
    Usb4DrEventPump m_Pump;

    /** Guards the generation, the child handle and its reference count. */
    KSPIN_LOCK m_Lock;
    ULONG m_Generation;
    USB4HR_HANDLE m_ChildHandle;
    LONG m_ChildHandleRefs;
    KEVENT m_ChildHandleIdle;

    BOOLEAN m_ChildHandleClosing;

    /** Latest plug state from the events, and whether an unplug arrived since the last work run. */
    BOOLEAN m_Plugged;
    BOOLEAN m_UnplugSeen;

    /** One plug work run at a time; a request during a run makes it loop once more. */
    BOOLEAN m_PlugWorkPending;
    BOOLEAN m_PlugWorkBusy;

    /** Child list state, owned by the plug work item. */
    BOOLEAN m_ChildReported;
    USB4HR_ROUTER_TYPE m_ChildType;
    USB4HR_TOPOLOGY_ID m_ChildTopology;

    ULONG m_MaxEnumSeconds;
    ULONG m_InitialEnumSeconds;
    Usb4DrRouterIdentity m_ChildIdentity;
};

/** Every port of the router. */
class Usb4DrPortSet
{
public:
    /** Pairs the lane adapters into ports; below depth 0 the lowest pair is the UFP (TBT3: the upstream adapter). */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Build(
        _In_ Usb4DrFdo* Fdo);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Start(
        _In_ BOOLEAN FirstStart);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    Stop(
        _In_ WDF_POWER_DEVICE_STATE TargetState);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Destroy();

    /** NULL at depth 0. */
    Usb4DrPort* Ufp();

    ULONG DfpCount() const;

    Usb4DrPort*
    Dfp(
        _In_ ULONG Index);

    Usb4DrPort*
    DfpByLane0(
        _In_ UCHAR Lane0Number);

    /** Fatal router error: every child router is reported missing. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID ReportAllChildrenMissing();

private:
    VOID SilenceAdapters();

    Usb4DrFdo* m_Fdo;
    Usb4DrPort* m_Ufp;
    ULONG m_PortCount;
    ULONG m_DfpCount;
    Usb4DrPort m_Ports[USB4DR_MAX_PORTS];
};
