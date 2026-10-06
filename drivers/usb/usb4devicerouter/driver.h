/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver globals, registry policy and device flags
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/* Per router quirk bits; Windows reads them from the shim engine */
#define USB4DR_FLAG_LANE1_DISABLE           0x00000001ULL   /**< router requires lane 1 disabled */
#define USB4DR_FLAG_INTEL_HOST_FW_VERSION   0x00000002ULL   /**< read the Intel host firmware version */
#define USB4DR_FLAG_BUFFER_CALCULATION      0x00000004ULL   /**< apply buffer calculation adjustments */
#define USB4DR_FLAG_USB3_BACK_PRESSURE      0x00000008ULL   /**< USB3 Gen 3 back pressure workaround */
#define USB4DR_FLAG_SWLI_ON_DP_CLEANUP      0x00000010ULL   /**< toggle SWLI on DP tunnel cleanup */
#define USB4DR_FLAG_NO_CLX                  0x00000020ULL   /**< disable CL states */
#define USB4DR_FLAG_SKIP_DPR_RECONNECT      0x00000040ULL   /**< skip port reset on rapid reconnect with lane 1 disabled */
#define USB4DR_FLAG_DP_ALT_MODE_ACTIVE      0x00000080ULL   /**< keep the domain active in DP alt mode */
#define USB4DR_FLAG_DELAY_DPR_LANE1         0x00000100ULL   /**< delay port reset after enabling lane 1 */
#define USB4DR_FLAG_NO_LOW_RES_TMU          0x00000200ULL   /**< disable low resolution TMU */
#define USB4DR_FLAG_TIME_POSTING            0x00000400ULL   /**< time posting quirk */
#define USB4DR_FLAG_NO_DP_WAKE_STANDBY      0x00000800ULL   /**< disable DP wake in standby */
#define USB4DR_FLAG_LONG_ROUTER_RESET       0x00001000ULL   /**< high router reset time */
#define USB4DR_FLAG_TBT3_USB3_WAKE          0x00002000ULL   /**< USB3 wake in TBT3 mode */
#define USB4DR_FLAG_CLEAR_PM_SECONDARY      0x00004000ULL   /**< clear PM secondary when disabling lane 1 */
#define USB4DR_FLAG_NO_PORT_OPERATIONS      0x00008000ULL   /**< disable port operations */

/** Driver policy, read once from the service Parameters key. */
struct Usb4DrPolicy
{
    BOOLEAN ForceTbt3EnumOnPcieDisabled;    /**< ForceTbt3EnumOnPcieDisabled */
};

/** Driver wide state. */
struct Usb4DrDriverData
{
    PDRIVER_OBJECT DriverObject;
    BOOLEAN TestSigning;

    /** KseQueryDeviceFlags when the kernel exports it, else NULL. */
    PVOID QueryDeviceFlags;

    Usb4DrPolicy Policy;
};

extern Usb4DrDriverData Usb4DrDriver;

/** ORs the shim engine flags of one key (provider USB4DEVICEROUTER) into Flags. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
Usb4DrQueryDeviceFlagsKey(
    _In_ PCWSTR Key,
    _Inout_ PULONG64 Flags);
