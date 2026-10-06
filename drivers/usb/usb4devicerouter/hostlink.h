/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client side of the host router: parent interfaces, root router target, config access
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/* Config access requests time out after 5 s, as in Windows */
#define USB4DR_CONFIG_TIMEOUT_MS        5000

/** Completion of an asynchronous config access; Usb4Status is USB4HR_STATUS_FAILURE on transport errors. */
typedef VOID
(NTAPI USB4DR_CONFIG_COMPLETION)(
    _In_ PVOID Context,
    _In_ USB4HR_HANDLE Handle,
    _In_ NTSTATUS Status,
    _In_ USB4HR_STATUS Usb4Status);
typedef USB4DR_CONFIG_COMPLETION *PUSB4DR_CONFIG_COMPLETION;

/**
 * Everything this router knows about the host router: the two interfaces queried from
 * its PDO, the remote target on the root router PDO and the reusable config request.
 */
class Usb4DrHostLink
{
public:
    /** Queries both interfaces from the PDO, opens the root router target, creates the config request. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Create(
        _In_ Usb4DrFdo* Fdo);

    /** Closes the target; the config request goes with the device. */
    _IRQL_requires_(PASSIVE_LEVEL)
    VOID Cleanup();

    /** Requeries the parent interface; the router handle in it changes after a reconnect. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS RefreshParentInterface();

    /** The parent interface copy taken at AddDevice or at the last refresh. */
    const USB4HR_PARENT_INTERFACE* Parent() const;

    /** The hardware services copy, passed unchanged to children. */
    const USB4HR_HARDWARE_SERVICES* Services() const;

    /** This router's handle (parent interface RouterHandle). */
    USB4HR_HANDLE RouterHandle() const;

    /** Depth of this router, 0 for the root router. */
    ULONG Depth() const;

    /** Remote I/O target opened on the root router PDO. */
    WDFIOTARGET RootTarget() const;

    /**
     * Synchronous config read. Usb4Status is what the host router reported; any USB4 status
     * other than USB4HR_STATUS_SUCCESS returns STATUS_UNSUCCESSFUL.
     */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    ReadConfig(
        _In_ USB4HR_HANDLE Handle,
        _In_ ULONG Space,
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _Out_writes_(DwordCount) PULONG Buffer,
        _Out_opt_ PUSB4HR_STATUS Usb4Status);

    /** Synchronous config write; same status rules as ReadConfig. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    WriteConfig(
        _In_ USB4HR_HANDLE Handle,
        _In_ ULONG Space,
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _In_reads_(DwordCount) const ULONG* Buffer,
        _Out_opt_ PUSB4HR_STATUS Usb4Status);

    /**
     * Asynchronous config read on the reusable request; Buffer must stay valid until Completion runs.
     * Completion runs only when this returns STATUS_SUCCESS.
     */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    ReadConfigAsync(
        _In_ USB4HR_HANDLE Handle,
        _In_ ULONG Space,
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _Out_writes_(DwordCount) PULONG Buffer,
        _In_ PUSB4DR_CONFIG_COMPLETION Completion,
        _In_opt_ PVOID Context);

    /** Asynchronous config write on the reusable request. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    WriteConfigAsync(
        _In_ USB4HR_HANDLE Handle,
        _In_ ULONG Space,
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _In_reads_(DwordCount) const ULONG* Buffer,
        _In_ PUSB4DR_CONFIG_COMPLETION Completion,
        _In_opt_ PVOID Context);

    /** Read, mask, write of one dword: bits in Mask take their value from Value. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    UpdateConfig(
        _In_ USB4HR_HANDLE Handle,
        _In_ ULONG Space,
        _In_ ULONG DwordOffset,
        _In_ ULONG Value,
        _In_ ULONG Mask);

    /** Sends an internal IOCTL to Target and waits; TimeoutMs 0 waits forever. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    SendIoctl(
        _In_ WDFIOTARGET Target,
        _In_ ULONG IoControlCode,
        _In_reads_bytes_opt_(InputLength) PVOID Input,
        _In_ ULONG InputLength,
        _Out_writes_bytes_opt_(OutputLength) PVOID Output,
        _In_ ULONG OutputLength,
        _In_ ULONG TimeoutMs,
        _Out_opt_ PULONG_PTR Information);

    /** IOCTL_USB4HR_HOST_ROUTER_RESET for the depth 0 router, 5 s timeout. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS ResetHostRouter();

private:
    static EVT_WDF_REQUEST_COMPLETION_ROUTINE EvtConfigCompleted;

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    QueryInterface(
        _In_ const GUID* InterfaceType,
        _Out_writes_bytes_(Size) PINTERFACE Interface,
        _In_ USHORT Size,
        _In_ USHORT Version);

    /** Claims the config request; FALSE when an access is already on the wire. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    BOOLEAN ClaimConfigRequest();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    VOID ReleaseConfigRequest();

    /** Reuses the config request and formats one config IOCTL on the root router target. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    FormatConfig(
        _In_ ULONG IoControlCode,
        _In_ USB4HR_HANDLE Handle,
        _In_ ULONG Space,
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _In_ PULONG Buffer);

    /** Sends the formatted config request and waits up to USB4DR_CONFIG_TIMEOUT_MS. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    SendConfigSync(
        _In_ ULONG IoControlCode,
        _In_ USB4HR_HANDLE Handle,
        _In_ ULONG Space,
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _In_ PULONG Buffer,
        _Out_opt_ PUSB4HR_STATUS Usb4Status);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS
    SendConfigAsync(
        _In_ ULONG IoControlCode,
        _In_ USB4HR_HANDLE Handle,
        _In_ ULONG Space,
        _In_ ULONG DwordOffset,
        _In_ ULONG DwordCount,
        _In_ PULONG Buffer,
        _In_ PUSB4DR_CONFIG_COMPLETION Completion,
        _In_opt_ PVOID Context);

    Usb4DrFdo* m_Fdo;
    USB4HR_PARENT_INTERFACE m_Parent;
    USB4HR_HARDWARE_SERVICES m_Services;
    WDFIOTARGET m_RootTarget;

    /** One config access on the wire per router; m_ConfigLock serializes the sync callers. */
    WDFREQUEST m_ConfigRequest;
    WDFWAITLOCK m_ConfigLock;

    /** Nonzero while m_ConfigRequest is formatted or in flight. */
    volatile LONG m_ConfigBusy;

    /** Buffers the config request carries; the host router writes m_ConfigOutput. */
    USB4HR_CONFIG_INPUT m_ConfigInput;
    USB4HR_CONFIG_EX_OUTPUT m_ConfigOutput;
    WDFMEMORY m_ConfigInputMemory;
    WDFMEMORY m_ConfigOutputMemory;

    /** Caller of the asynchronous access in flight. */
    PUSB4DR_CONFIG_COMPLETION m_AsyncCompletion;
    PVOID m_AsyncContext;
    USB4HR_HANDLE m_AsyncHandle;
};
