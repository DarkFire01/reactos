/*
 * PROJECT:     ReactOS Intel LPSS GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private declarations
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/**
 * @file
 * @brief
 * The Intel LPSS GPIO controller, as a GpioClx client.
 *
 * All four SoC variants Intel
 * ships (CNL, GLK, BXT_P and the generic one) are the same engine: every one is
 * a single `_GPIO_CONTROLLER_CHASSIS3` C++ class, and what differs between them
 * is the community and group table, not the logic.
 *
 * The hardware is a pin controller, not a GPIO block. Pads are grouped into
 * communities, each community has its own register window inside the one BAR,
 * and every pad has a pair of 32-bit configuration registers. A GpioClx bank
 * maps onto one Intel group.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <gpioclx.h>

/*
 * Register offsets within a community. These are the documented Intel PCH
 * layout: HOSTSW_OWN at 0xD0, the interrupt status array at 0x100 and the pad
 * configuration block at 0x400.
 */
/*
 * Where HOSTSW_OWN sits inside a community window. This one moved with almost
 * every PCH generation, so a layout always names the value it needs and these
 * are only the spellings the table uses. The names are the earliest part that
 * introduced each offset; later parts that kept the offset reuse the name.
 */
#define GPIO_COMMUNITY_HOSTSW_OWN_SKL   0x0D0
#define GPIO_COMMUNITY_HOSTSW_OWN_CNL   0x0B0
#define GPIO_COMMUNITY_HOSTSW_OWN_CNL_H 0x0C0
#define GPIO_COMMUNITY_HOSTSW_OWN_MTL_M 0x110
#define GPIO_COMMUNITY_HOSTSW_OWN_MTL_P 0x140
#define GPIO_COMMUNITY_HOSTSW_OWN_ADL_S 0x150
#define GPIO_COMMUNITY_GPI_STATUS   0x100
#define GPIO_COMMUNITY_GPI_ENABLE   0x120
#define GPIO_COMMUNITY_PAD_BASE     0x400

/*
 * Each pad has two configuration registers, so the stride is eight bytes
 * (:7095 writes 8 into the stride field).
 */
/*
 * How far apart a community's pad configuration registers are. Skylake and
 * Kaby Lake give a pad two registers and so eight bytes; Cannon Lake onwards
 * gives it four and so sixteen. The reference carries it as a field the
 * per-part layout writes (:7095 stores 8 for Skylake-LP), which is why it is
 * one here too rather than a constant - and the pad offsets in each table
 * prove it: a Cannon Lake group of 25 pads is followed by one at 0x190,
 * which is 25 * 16, where a Skylake group of 24 is followed by one at 0xC0.
 */
#define GPIO_PAD_STRIDE_2REG        8
#define GPIO_PAD_STRIDE_4REG        16
#define GPIO_PAD_CONFIG0            0
#define GPIO_PAD_CONFIG1            4

/* PADCFG0 fields */
#define PADCFG0_PMODE_MASK          (0x7 << 10)   /* 0 selects GPIO, not a native function */
#define PADCFG0_GPIOTXDIS           (1 << 8)      /* output driver disabled */
#define PADCFG0_GPIORXDIS           (1 << 9)      /* input buffer disabled */
#define PADCFG0_GPIOTXSTATE         (1 << 0)      /* what to drive */
#define PADCFG0_GPIORXSTATE         (1 << 1)      /* what is on the pad */
#define PADCFG0_RXEVCFG_MASK        (0x3 << 25)
#define PADCFG0_RXEVCFG_LEVEL       (0x0 << 25)
#define PADCFG0_RXEVCFG_EDGE        (0x1 << 25)
#define PADCFG0_RXEVCFG_DISABLED    (0x2 << 25)
#define PADCFG0_RXEVCFG_EDGE_BOTH   (0x3 << 25)
#define PADCFG0_RXINV               (1 << 23)     /* invert, for an active-low source */
#define PADCFG0_GPIROUTIOXAPIC      (1 << 20)

/*
 * A bank is one Intel group. The GpioClx bank id indexes this table, so the
 * order here is the order GpioClx numbers them in.
 */
/*
 * One SoC's pad geometry.
 *
 * The reference keeps a C++ class per part - _GPIO_CONTROLLER_SKL_LP,
 * _GPIO_CONTROLLER_CNL_LP and the rest - and picks between them on the _HID
 * the device enumerated with: a table of id/index pairs
 * feeds a switch that constructs the
 * matching class (:2002-2050). The classes differ only in the numbers, so one
 * table per part and a lookup replaces the vtable here.
 */
typedef struct _GPIO_LAYOUT
{
    /* The hardware id this layout serves, as PnP spells it */
    PCWSTR HardwareId;

    /* A name for the traces */
    PCSTR Name;

    /* Its groups, in the order GpioClx numbers them */
    const struct _GPIO_BANK_DESCRIPTOR *Banks;
    ULONG BankCount;

    /* How many community windows the firmware must hand over */
    ULONG CommunityCount;

    /* Where HOSTSW_OWN sits in a community window on this part */
    USHORT HostSwOwnOffset;

    /* Bytes between one pad's configuration registers and the next one's */
    USHORT PadStride;
} GPIO_LAYOUT, *PGPIO_LAYOUT;

typedef struct _GPIO_BANK_DESCRIPTOR
{
    /* Which community's window the group lives in */
    UCHAR Community;

    /* Which HOSTSW_OWN dword covers it */
    UCHAR HostSwOwnIndex;

    /* Where its pads start, relative to the community's pad block */
    USHORT PadOffset;

    /* How many pads it has; the last group is usually short */
    USHORT PinCount;
} GPIO_BANK_DESCRIPTOR, *PGPIO_BANK_DESCRIPTOR;

/*
 * The controller reports its geometry to GpioClx once. A bank is 24 pins on
 * this silicon (:7093), which is a group's size rather than a register width.
 */
#define GPIO_PINS_PER_BANK  24

/* Skylake-LP has three; no Intel part in this family has more than eight */
#define GPIO_MAX_COMMUNITIES 8

/* Skylake-LP has seven groups, and no variant of this part has sixteen */
#define GPIO_MAX_BANKS 16

/* "GPIo", for the per-group pad state tables */
#define GPIO_POOL_TAG 'oIPG'

/**
 * @brief
 * What this driver remembers about one pad across a power transition.
 *
 * The reference keeps a table of these per group, allocated in
 * PrepareController at 36 bytes a pad and read
 * back by the save and restore paths at :5651 and :5560.
 */
typedef struct _GPIO_PIN_STATE
{
    /* PADCFG0 and PADCFG1 as they were when the group last went down */
    ULONG SavedConfig0;
    ULONG SavedConfig1;

    /* Set once the pad has been connected or armed, and never cleared */
    BOOLEAN InUse;

    /* Set while the two saved words hold something worth putting back */
    BOOLEAN ContextSaved;
} GPIO_PIN_STATE, *PGPIO_PIN_STATE;

/**
 * @brief
 * What this driver remembers about one group.
 *
 * The armed mask lives here rather than in the pad table because one GPI_IE
 * word covers the whole group; the reference keeps it in the bank entry at +28
 * with its flag at +24 (:5719).
 */
typedef struct _GPIO_BANK_STATE
{
    /* PinCount entries, allocated when the controller is prepared */
    PGPIO_PIN_STATE Pins;

    /* The group's GPI_IE word, as it was when the group went down */
    ULONG SavedInterruptEnable;

    /* Set between a save and the restore that consumes it */
    BOOLEAN ContextSaved;
} GPIO_BANK_STATE, *PGPIO_BANK_STATE;

/**
 * @brief
 * Per-controller state.
 *
 * GpioClx allocates this and hands it back through every callback. Its size is
 * declared in the registration packet as ControllerContextSize; the reference
 * asks for 56 bytes.
 */
typedef struct _GPIO_CONTROLLER
{
    /* The device this controller hangs off */
    WDFDEVICE Device;

    /*
     * Each community is its own memory resource, not an offset into a shared
     * window: the reference walks the translated list and calls MmMapIoSpaceEx
     * once per descriptor, stopping at the
     * community count.
     */
    PUCHAR CommunityBase[GPIO_MAX_COMMUNITIES];
    ULONG CommunityLength[GPIO_MAX_COMMUNITIES];
    ULONG CommunityCount;

    /* The group table this SoC uses, and how long it is */
    const GPIO_BANK_DESCRIPTOR *Banks;
    ULONG BankCount;

    /* The layout those came from, for its per-part register offsets */
    const GPIO_LAYOUT *Layout;

    /* What survives a power transition, one entry per group */
    GPIO_BANK_STATE BankState[GPIO_MAX_BANKS];

    /* Geometry, reported to GpioClx */
    USHORT TotalPins;
    UCHAR PinsPerBank;
} GPIO_CONTROLLER, *PGPIO_CONTROLLER;

/**
 * @brief
 * The address of one pad's configuration registers.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The group the pad is in.
 *
 * @param[in] PinNumber
 * The pad's index within that group.
 *
 * @return
 * Where its PADCFG0 lives, or NULL if the pad does not exist.
 */
FORCEINLINE PULONG
GpioPadAddress(
    _In_ PGPIO_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    const GPIO_BANK_DESCRIPTOR *Bank;

    if (BankId >= Controller->BankCount)
        return NULL;

    Bank = &Controller->Banks[BankId];
    if (PinNumber >= Bank->PinCount)
        return NULL;

    if (Bank->Community >= Controller->CommunityCount ||
        Controller->CommunityBase[Bank->Community] == NULL)
        return NULL;

    return (PULONG)(Controller->CommunityBase[Bank->Community] +
                    GPIO_COMMUNITY_PAD_BASE +
                    Bank->PadOffset +
                    (ULONG)PinNumber * Controller->Layout->PadStride);
}

/**
 * @brief
 * The address of one of a community's per-group dword arrays.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The group whose dword is wanted.
 *
 * @param[in] BlockOffset
 * Which array, as a community-relative offset.
 *
 * @return
 * The dword covering that group, or NULL if the group does not exist.
 */
FORCEINLINE PULONG
GpioGroupRegister(
    _In_ PGPIO_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ ULONG BlockOffset)
{
    const GPIO_BANK_DESCRIPTOR *Bank;

    if (BankId >= Controller->BankCount)
        return NULL;

    Bank = &Controller->Banks[BankId];

    if (Bank->Community >= Controller->CommunityCount ||
        Controller->CommunityBase[Bank->Community] == NULL)
        return NULL;

    return (PULONG)(Controller->CommunityBase[Bank->Community] +
                    BlockOffset +
                    (ULONG)Bank->HostSwOwnIndex * sizeof(ULONG));
}

/**
 * @brief
 * The saved state of one pad.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The group the pad is in.
 *
 * @param[in] PinNumber
 * The pad's index within that group.
 *
 * @return
 * Its entry in the group's pad table, or NULL if the pad does not exist or the
 * table was never allocated.
 */
FORCEINLINE PGPIO_PIN_STATE
GpioPinState(
    _In_ PGPIO_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    if (BankId >= Controller->BankCount ||
        PinNumber >= Controller->Banks[BankId].PinCount ||
        Controller->BankState[BankId].Pins == NULL)
        return NULL;

    return &Controller->BankState[BankId].Pins[PinNumber];
}

EVT_WDF_DRIVER_DEVICE_ADD GpioEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP GpioEvtDriverUnload;

GPIO_CLIENT_PREPARE_CONTROLLER GpioPrepareController;
GPIO_CLIENT_RELEASE_CONTROLLER GpioReleaseController;
GPIO_CLIENT_START_CONTROLLER GpioStartController;
GPIO_CLIENT_STOP_CONTROLLER GpioStopController;
GPIO_CLIENT_QUERY_CONTROLLER_BASIC_INFORMATION GpioQueryControllerBasicInformation;

GPIO_CLIENT_CONNECT_IO_PINS GpioConnectIoPins;
GPIO_CLIENT_DISCONNECT_IO_PINS GpioDisconnectIoPins;
GPIO_CLIENT_READ_PINS GpioReadGpioPins;
GPIO_CLIENT_WRITE_PINS GpioWriteGpioPins;

GPIO_CLIENT_ENABLE_INTERRUPT GpioEnableInterrupt;
GPIO_CLIENT_DISABLE_INTERRUPT GpioDisableInterrupt;
GPIO_CLIENT_UNMASK_INTERRUPT GpioUnmaskInterrupt;
GPIO_CLIENT_MASK_INTERRUPTS GpioMaskInterrupts;
GPIO_CLIENT_QUERY_ACTIVE_INTERRUPTS GpioQueryActiveInterrupts;
GPIO_CLIENT_CLEAR_ACTIVE_INTERRUPTS GpioClearActiveInterrupts;

GPIO_CLIENT_RECONFIGURE_INTERRUPT GpioReconfigureInterrupt;
GPIO_CLIENT_SAVE_BANK_HARDWARE_CONTEXT GpioSaveBankHardwareContext;
GPIO_CLIENT_RESTORE_BANK_HARDWARE_CONTEXT GpioRestoreBankHardwareContext;
