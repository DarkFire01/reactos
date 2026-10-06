/*
 * PROJECT:     ReactOS USB4 Host Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver globals, registry policy, connection manager IDs and shim flags
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

struct Usb4HrIdentity;

/* Per host router quirk bits; Windows reads them from the shim engine */
#define USB4HR_SHIM_32BIT_ACCESS            0x00000001ULL   /**< split 64 bit register accesses */
#define USB4HR_SHIM_FORCE_POWER             0x00000002ULL   /**< vendor force power flow before configuration */
#define USB4HR_SHIM_MSIX_CHECK              0x00000004ULL   /**< verify the MSI-X table on D0 entry */
#define USB4HR_SHIM_WAIT_DESCRIPTOR_DONE    0x00000008ULL   /**< RX: wait for the done bit before reading */
#define USB4HR_SHIM_STATUS_CLEAR_ALT        0x00000010ULL   /**< interrupt status clear lives at 0x3780C */
#define USB4HR_SHIM_VALIDATED_HOST          0x00000020ULL   /**< the Windows load gate */
#define USB4HR_SHIM_NO_CLX                  0x00000080ULL   /**< disable CL states in the whole domain */
#define USB4HR_SHIM_HI_LINK_CREDITS         0x00000100ULL   /**< program link credits of the host interface adapter */
#define USB4HR_SHIM_CLEAR_PATH_CREDITS      0x00000200ULL   /**< clear path credits after teardown */
#define USB4HR_SHIM_POLL_PENDING_FIRST      0x00000400ULL   /**< poll pending packets before clearing path valid */
#define USB4HR_SHIM_TX_REQUEST_STATUS       0x00000800ULL   /**< set request status on every TX descriptor */
#define USB4HR_SHIM_XDOMAIN_FRAME_ALIGN     0x00001000ULL
#define USB4HR_SHIM_NO_DP_BW_MODE           0x00002000ULL
#define USB4HR_SHIM_LIMIT_USB3_20G          0x00004000ULL   /**< cap USB3 bandwidth on 20 Gbps links */
#define USB4HR_SHIM_PREALLOCATED_RINGS      0x00008000ULL
#define USB4HR_SHIM_NO_XDOMAIN_E2E          0x00010000ULL
#define USB4HR_SHIM_NO_DP_MIN_PREALLOC      0x00020000ULL
#define USB4HR_SHIM_P2P_RING1               0x00040000ULL
#define USB4HR_SHIM_LONG_DP_QUERY           0x00080000ULL

/** Driver policy, read once from the service Parameters key. */
struct Usb4HrPolicy
{
    BOOLEAN DriverDisableable;          /**< EnableDriverDisableable, test signing only */
    BOOLEAN Usb4V2Configured;           /**< EnableUSB4v2Support is present */
    BOOLEAN Usb4V2Enabled;              /**< its value when present */
    BOOLEAN DisableClxDomainWide;       /**< DisableCLxDomainWide, test signing only */
    BOOLEAN UserIdleControl;            /**< UserControlOfIdleSettings, test signing only */
    BOOLEAN PerHostRouterDomainUuid;    /**< UsePerHostRouterDomainUUID */
    BOOLEAN BugcheckOnDuplicateGfxRef;  /**< BugcheckOnDuplicateGfxRef */
};

/** Driver wide state. */
struct Usb4HrDriverData
{
    PDRIVER_OBJECT DriverObject;
    BOOLEAN TestSigning;

    /** KseQueryDeviceFlags when the kernel exports it, else NULL. */
    PVOID QueryDeviceFlags;

    Usb4HrPolicy Policy;

    /** Domain UUID shared by host routers that do not use their own. */
    GUID SystemDomainUuid;

    /** Bit n set: connection manager ID n is taken. */
    volatile LONG CmidBitmap;
};

extern Usb4HrDriverData Usb4HrDriver;

/** Takes the lowest free connection manager ID 0 to 7. */
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
NTAPI
Usb4HrAllocateCmid(
    _Out_ PUCHAR Cmid);

/** Returns an ID from Usb4HrAllocateCmid. */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
Usb4HrReleaseCmid(
    _In_ UCHAR Cmid);

/** Shim flags for a host router: shim engine, static table and registry override, ORed. */
_IRQL_requires_(PASSIVE_LEVEL)
ULONG64
NTAPI
Usb4HrQueryShimFlags(
    _In_ WDFDEVICE Device,
    _In_ const Usb4HrIdentity* Identity);
