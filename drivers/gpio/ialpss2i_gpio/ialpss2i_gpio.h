/*
 * PROJECT:     ReactOS Intel LPSS GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private declarations
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <msgpioclxstub.h>

#define IGPIO_POOL_TAG 'oPGI'

#define IGPIO_MAX_COMMUNITIES 5
#define IGPIO_MAX_GROUPS      19

/* Bits of the first pad configuration dword */
#define IGPIO_CFG0_TX_STATE        0x00000001UL
#define IGPIO_CFG0_RX_STATE        0x00000002UL
#define IGPIO_CFG0_TX_DISABLE      0x00000100UL
#define IGPIO_CFG0_RX_DISABLE      0x00000200UL
#define IGPIO_CFG0_MODE_MASK       0x00003C00UL
#define IGPIO_CFG0_ROUTE_IOAPIC    0x00100000UL
#define IGPIO_CFG0_RX_INVERT       0x00800000UL
#define IGPIO_CFG0_TRIGGER_MASK    0x06000000UL
#define IGPIO_CFG0_TRIGGER_EDGE    0x02000000UL
#define IGPIO_CFG0_TRIGGER_BOTH    0x06000000UL

/* Routing and raw override bits that must be off for a GPIO driver interrupt */
#define IGPIO_CFG0_ROUTE_MASK      0x10700000UL

/* Termination field of the second pad configuration dword */
#define IGPIO_CFG1_TERM_MASK       0x00003C00UL
#define IGPIO_CFG1_TERM_PULLUP     0x00003000UL
#define IGPIO_CFG1_TERM_PULLDOWN   0x00001000UL

/** Where the per group register arrays and the pads sit inside a community window. */
typedef struct _IGPIO_REGISTER_MAP
{
    USHORT HostOwnership;
    USHORT InterruptStatus;
    USHORT InterruptEnable;
    USHORT PadBase;
} IGPIO_REGISTER_MAP, *PIGPIO_REGISTER_MAP;

/** One pad group, which GpioClx sees as one bank. */
typedef struct _IGPIO_GROUP
{
    UCHAR Community;
    UCHAR Slot;
    USHORT PadOffset;
    UCHAR PadCount;
} IGPIO_GROUP, *PIGPIO_GROUP;

/** Pad geometry of one SoC. */
typedef struct _IGPIO_SOC
{
    PCWSTR HardwareId;
    const IGPIO_GROUP *Groups;
    UCHAR GroupCount;
    UCHAR CommunityCount;
    UCHAR PadStride;
    UCHAR PinsPerBank;
    IGPIO_REGISTER_MAP Map;
} IGPIO_SOC, *PIGPIO_SOC;

#define IGPIO_PAD_INTERRUPT 0x01
#define IGPIO_PAD_INPUT     0x02
#define IGPIO_PAD_OUTPUT    0x04

/** What the driver tracks for one pad. */
typedef struct _IGPIO_PAD
{
    UCHAR Use;
    UCHAR OutputValue;
    BOOLEAN Saved;
    ULONG SavedConfig0;
    ULONG SavedConfig1;
} IGPIO_PAD, *PIGPIO_PAD;

typedef struct _IGPIO_BANK
{
    PIGPIO_PAD Pads;
    BOOLEAN Saved;
    ULONG SavedEnable;
} IGPIO_BANK, *PIGPIO_BANK;

/** Controller state, carved out by GpioClx as ControllerContextSize. */
typedef struct _IGPIO_CONTROLLER
{
    WDFDEVICE Device;
    const IGPIO_SOC *Soc;

    PUCHAR Window[IGPIO_MAX_COMMUNITIES];
    SIZE_T WindowLength[IGPIO_MAX_COMMUNITIES];

    IGPIO_BANK Banks[IGPIO_MAX_GROUPS];
    USHORT TotalPins;

    /* Serializes read-modify-write of shared registers against the GpioClx ISR */
    KSPIN_LOCK RegisterLock;
} IGPIO_CONTROLLER, *PIGPIO_CONTROLLER;

FORCEINLINE
PUCHAR
IGpioCommunity(
    _In_ PIGPIO_CONTROLLER Controller,
    _In_ BANK_ID BankId)
{
    return Controller->Window[Controller->Soc->Groups[BankId].Community];
}

/** One of the per group dword arrays (ownership, status, enable) for a bank. */
FORCEINLINE
PULONG
IGpioGroupRegister(
    _In_ PIGPIO_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ USHORT ArrayOffset)
{
    return (PULONG)(IGpioCommunity(Controller, BankId) +
                    ArrayOffset +
                    Controller->Soc->Groups[BankId].Slot * sizeof(ULONG));
}

/** The first configuration dword of a pad; the second one follows it. */
FORCEINLINE
PULONG
IGpioPadRegister(
    _In_ PIGPIO_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    const IGPIO_SOC *Soc = Controller->Soc;

    return (PULONG)(IGpioCommunity(Controller, BankId) +
                    Soc->Map.PadBase +
                    Soc->Groups[BankId].PadOffset +
                    (ULONG)PinNumber * Soc->PadStride);
}

FORCEINLINE
BOOLEAN
IGpioIsBankValid(
    _In_ PIGPIO_CONTROLLER Controller,
    _In_ BANK_ID BankId)
{
    return Controller->Soc != NULL && BankId < Controller->Soc->GroupCount;
}

FORCEINLINE
BOOLEAN
IGpioIsPinValid(
    _In_ PIGPIO_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    return PinNumber < Controller->Soc->Groups[BankId].PadCount;
}

/**
 * @brief
 * Folds a GpioClx pull setting into the termination field of the second dword.
 */
FORCEINLINE
ULONG
IGpioApplyPull(
    _In_ ULONG Config1,
    _In_ UCHAR PullConfiguration)
{
    switch (PullConfiguration)
    {
        case GPIO_PIN_PULL_CONFIGURATION_PULLUP:
            return (Config1 & ~IGPIO_CFG1_TERM_MASK) | IGPIO_CFG1_TERM_PULLUP;

        case GPIO_PIN_PULL_CONFIGURATION_PULLDOWN:
            return (Config1 & ~IGPIO_CFG1_TERM_MASK) | IGPIO_CFG1_TERM_PULLDOWN;

        case GPIO_PIN_PULL_CONFIGURATION_NONE:
            return Config1 & ~IGPIO_CFG1_TERM_MASK;

        default:
            return Config1;
    }
}

/* layout.c */
_IRQL_requires_(PASSIVE_LEVEL)
const IGPIO_SOC *
NTAPI
IGpioFindSoc(
    _In_ WDFDEVICE Device);

/* controller.c */
GPIO_CLIENT_PREPARE_CONTROLLER IGpioPrepareController;
GPIO_CLIENT_RELEASE_CONTROLLER IGpioReleaseController;
GPIO_CLIENT_START_CONTROLLER IGpioStartController;
GPIO_CLIENT_STOP_CONTROLLER IGpioStopController;
GPIO_CLIENT_QUERY_CONTROLLER_BASIC_INFORMATION IGpioQueryControllerBasicInformation;
GPIO_CLIENT_SAVE_BANK_HARDWARE_CONTEXT IGpioSaveBankHardwareContext;
GPIO_CLIENT_RESTORE_BANK_HARDWARE_CONTEXT IGpioRestoreBankHardwareContext;

/* pins.c */
GPIO_CLIENT_CONNECT_IO_PINS IGpioConnectIoPins;
GPIO_CLIENT_DISCONNECT_IO_PINS IGpioDisconnectIoPins;
GPIO_CLIENT_READ_PINS IGpioReadGpioPins;
GPIO_CLIENT_WRITE_PINS IGpioWriteGpioPins;

/* interrupt.c */
GPIO_CLIENT_ENABLE_INTERRUPT IGpioEnableInterrupt;
GPIO_CLIENT_DISABLE_INTERRUPT IGpioDisableInterrupt;
GPIO_CLIENT_UNMASK_INTERRUPT IGpioUnmaskInterrupt;
GPIO_CLIENT_MASK_INTERRUPTS IGpioMaskInterrupts;
GPIO_CLIENT_QUERY_ACTIVE_INTERRUPTS IGpioQueryActiveInterrupts;
GPIO_CLIENT_QUERY_ENABLED_INTERRUPTS IGpioQueryEnabledInterrupts;
GPIO_CLIENT_CLEAR_ACTIVE_INTERRUPTS IGpioClearActiveInterrupts;
GPIO_CLIENT_RECONFIGURE_INTERRUPT IGpioReconfigureInterrupt;
