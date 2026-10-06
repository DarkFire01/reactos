/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USB3 and PCIe adapters: tunnel creation from the up adapter, teardown and rebuild
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/** Tunnel state of one protocol adapter. */
enum class Usb4DrTunnelState : ULONG
{
    Idle = 0,
    Requested = 1,
    Tunneling = 2,
    PoweredDown = 3,
    Failed = 4
};

/** One USB3 or PCIe adapter. Only an up adapter (in a router below depth 0) asks for a tunnel. */
class Usb4DrProtocolAdapter
{
public:
    /** Walks to the protocol capability and caches its registers and clears a stale Path Enable on the root router. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Initialize(
        _In_ Usb4DrFdo* Fdo,
        _In_ Usb4DrAdapter* Adapter);

    Usb4DrAdapter* Adapter();

    /** USB4HR_TUNNEL_USB3 or USB4HR_TUNNEL_PCIE. */
    ULONG TunnelType() const;

    BOOLEAN IsUp() const;

    /** Protocol capability dword offset, also the segment CapabilityOffset. */
    UCHAR CapabilityOffset() const;

    /** USB3 CS4 max supported link rate field; USB4DR_USB3_MAX_RATE_GEN2X2 for 20 Gbps class. */
    ULONG MaxLinkRateField() const;

    /** HopIDs the adapter uses on its own path entries. */
    USHORT InputHopId() const;
    USHORT OutputHopId() const;

    Usb4DrTunnelState State() const;
    USB4HR_HANDLE TunnelHandle() const;

    /** PCIe waits for LTSSM Detect first; then fills the child half, sends CREATE_TUNNEL and waits for it. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS CreateTunnel();

    /** Up adapter: sends DESTROY_TUNNEL and frees its HopID; no register is written, the host router does that. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS DestroyTunnel();

    /** Resume: PCIe waits for Detect, then REBUILD_TUNNEL; on failure DESTROY_TUNNEL and the device is failed for a restart. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS RebuildTunnel();

    /** D0 exit to Dx: the host router keeps the tunnel on its powered down list. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID MarkPoweredDown();

    /** USB3 back pressure workaround through the vendor capability when USB4DR_FLAG_USB3_BACK_PRESSURE is set. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS ApplyBackPressureWorkaround();

private:
    friend class Usb4DrTunnels;
    friend class Usb4DrSegments;

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS ResetPathEnable();

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS DisconnectUsb3();

    /** TRUE for a PCIe adapter that must reach LTSSM Detect before it is used; TBT3 routers skip it. */
    BOOLEAN NeedsPcieDetect() const;

    /** Clears the LTSSM retry count before a new round of polls. */
    VOID ResetPcieDetect();

    /**
     * One LTSSM read: STATUS_SUCCESS at Detect, STATUS_PENDING while retries are left,
     * STATUS_IO_TIMEOUT after the last one, or the read failure.
     */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS PollPcieDetect();

    /** Polls every 100 ms until Detect; STATUS_CANCELLED when D0 exit stops the tunnels. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS WaitForPcieDetect();

    /** Down adapter: blocks a parent half fill until the USB3 disconnect or the PCIe Detect wait is done. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS WaitUntilReady();

    /** Reads or writes Count dwords of the protocol capability, from Index on. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    ReadCapability(
        _In_ ULONG Index,
        _In_ ULONG Count,
        _Out_writes_(Count) PULONG Buffer);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    WriteCapability(
        _In_ ULONG Index,
        _In_ ULONG Value);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID FailDevice();

    Usb4DrFdo* m_Fdo;
    Usb4DrAdapter* m_Adapter;
    ULONG m_TunnelType;
    UCHAR m_CapabilityOffset;
    BOOLEAN m_ExtendedEncapsulation;    /**< PCIe: what the host router enabled */
    ULONG m_Cs[5];                      /**< USB3 CS_0..4, or PCIe CS_0 */
    Usb4DrTunnelState m_State;
    USB4HR_HANDLE m_TunnelHandle;
    ULONG m_DetectRetries;              /**< PCIe LTSSM polls that did not see Detect */
    PKEVENT m_StopEvent;                /**< the tunnels' stop event, ends a Detect wait */
    KEVENT m_Ready;                     /**< signaled while a parent half may use the adapter */
    USB4HR_CREATE_TUNNEL_INPUT m_Request;
};

/** Every protocol adapter of the router and the worker that builds and rebuilds tunnels. */
class Usb4DrTunnels
{
public:
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Create(
        _In_ Usb4DrFdo* Fdo);

    /**
     * One Usb4DrProtocolAdapter per USB3 adapter and per PCIe adapter the tunnel policy
     * allows, after the router read its adapters and built its ports; then the adapter
     * counts are checked against the ports as Windows does.
     */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS Build();

    /** End of router start: queues the worker that creates (first start) or rebuilds tunnels. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    OnRouterStarted(
        _In_ BOOLEAN FirstStart);

    /** D0 exit: waits for the worker; tunnels are destroyed on D3Final, else marked powered down. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    Stop(
        _In_ WDF_POWER_DEVICE_STATE TargetState);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Destroy();

    Usb4DrSegments* Segments();

    /** Down adapter of TunnelType that pairs with DFP index DfpIndex, NULL when there is none. */
    Usb4DrProtocolAdapter*
    DownAdapterForDfp(
        _In_ ULONG TunnelType,
        _In_ ULONG DfpIndex);

private:
    static EVT_WDF_WORKITEM EvtTunnelWork;
    static EVT_WDF_WORKITEM EvtDownAdapterWork;

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID RunUpAdapters();

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    RunUpAdapter(
        _In_ Usb4DrProtocolAdapter* Adapter);

    _IRQL_requires_(PASSIVE_LEVEL)
    VOID RunDownAdapters();

    /** Disconnects every USB3 down adapter without a tunnel and opens it for parent halves. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID SettleUsb3DownAdapters();

    /** One LTSSM poll of every PCIe down adapter in Pending; returns the ones still polling. */
    _IRQL_requires_(PASSIVE_LEVEL)
    ULONG64
    PollPcieDownAdapters(
        _In_ ULONG64 Pending);

    /** Counts of up and down adapters must fit the ports. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    ValidateAdapters(
        _In_ ULONG DisabledPcieDown);

    /** Number of protocol adapters of TunnelType facing Up (TRUE) or down. */
    ULONG
    CountAdapters(
        _In_ ULONG TunnelType,
        _In_ BOOLEAN Up);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    CreateWorkItem(
        _In_ PFN_WDF_WORKITEM Callback,
        _Out_ WDFWORKITEM* WorkItem);

    /** Cancels tunnel requests in flight until Idle is signaled. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID
    WaitForWorker(
        _In_ PKEVENT Idle,
        _In_ PCSTR What);

    Usb4DrFdo* m_Fdo;
    WDFWORKITEM m_Work;
    WDFWORKITEM m_DownWork;
    BOOLEAN m_RebuildPending;
    BOOLEAN m_Resuming;
    BOOLEAN m_Stopping;
    KEVENT m_WorkIdle;
    KEVENT m_DownWorkIdle;
    KEVENT m_StopEvent;
    ULONG m_AdapterCount;
    Usb4DrSegments m_Segments;

    /** Pool array of m_AdapterCount adapters in ascending adapter number order. */
    Usb4DrProtocolAdapter* m_Adapters;
};
