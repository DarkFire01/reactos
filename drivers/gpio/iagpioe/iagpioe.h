/*
 * PROJECT:     ReactOS Intel Cherryview GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private declarations
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <acpiioct.h>
#include <oprghdlr.h>
#include <msgpioclxstub.h>

#define CHV_POOL_TAG 'oPhC'

/* Community window layout */
#define CHV_INTERRUPT_STATUS    0x300
#define CHV_INTERRUPT_MASK      0x380
#define CHV_FAMILY_BASE         0x4400
#define CHV_FAMILY_STRIDE       0x400
#define CHV_PAD_STRIDE          8
#define CHV_PADS_PER_FAMILY     15
#define CHV_MAX_FAMILIES        7

#define CHV_MAX_BANKS           7
#define CHV_MAX_PINS_PER_BANK   16

/* First pad control dword */
#define CHV_CTRL0_RX_STATE      0x00000001UL
#define CHV_CTRL0_TX_STATE      0x00000002UL
#define CHV_CTRL0_CFG_MASK      0x00000700UL
#define CHV_CTRL0_CFG_INOUT     0x00000000UL
#define CHV_CTRL0_CFG_OUTPUT    0x00000100UL
#define CHV_CTRL0_CFG_INPUT     0x00000200UL
#define CHV_CTRL0_GPIO_ENABLE   0x00008000UL
#define CHV_CTRL0_LINE_SHIFT    28

/* Pads with any of these set keep their firmware setup across a restore */
#define CHV_CTRL0_KEEP_ON_RESTORE 0x000F0004UL

/* Second pad control dword */
#define CHV_CTRL1_INT_MASK      0x00000007UL
#define CHV_CTRL1_INT_FALLING   0x00000001UL
#define CHV_CTRL1_INT_RISING    0x00000002UL
#define CHV_CTRL1_INT_BOTH      0x00000003UL
#define CHV_CTRL1_INT_LEVEL     0x00000004UL
#define CHV_CTRL1_INVERT_RX     0x00000040UL

/* Operation region spaces start here, offset by the community _UID */
#define CHV_REGION_SPACE_BASE   0x90

/** Pads one community has, family by family. */
typedef struct _CHV_COMMUNITY
{
    UCHAR Uid;
    UCHAR PadCount;
    UCHAR PinsPerBank;
    USHORT FamilyPads[CHV_MAX_FAMILIES];
} CHV_COMMUNITY, *PCHV_COMMUNITY;

/** Last values written to a pad, replayed on restore. */
typedef struct _CHV_PAD_SHADOW
{
    ULONG Ctrl0;
    ULONG Ctrl1;
} CHV_PAD_SHADOW, *PCHV_PAD_SHADOW;

/** Controller state, carved out by GpioClx as ControllerContextSize. */
typedef struct _CHV_CONTROLLER
{
    WDFDEVICE Device;
    PUCHAR Base;
    SIZE_T Length;
    ULONG Uid;
    const CHV_COMMUNITY *Community;
    USHORT BankCount;
    PVOID OpRegion;

    BOOLEAN BankSaved[CHV_MAX_BANKS];
    BOOLEAN MaskSaved;
    ULONG SavedMask;
    CHV_PAD_SHADOW Shadow[CHV_MAX_BANKS][CHV_MAX_PINS_PER_BANK];
} CHV_CONTROLLER, *PCHV_CONTROLLER;

/* pads.c */
VOID
NTAPI
ChvInitializeRegisterLock(VOID);

_IRQL_requires_max_(HIGH_LEVEL)
const CHV_COMMUNITY *
NTAPI
ChvFindCommunity(
    _In_ ULONG Uid);

_IRQL_requires_max_(HIGH_LEVEL)
BOOLEAN
NTAPI
ChvIsPinValid(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber);

_IRQL_requires_max_(HIGH_LEVEL)
ULONG
NTAPI
ChvReadRegister(
    _In_ PCHV_CONTROLLER Controller,
    _In_ ULONG Offset);

_IRQL_requires_max_(HIGH_LEVEL)
VOID
NTAPI
ChvWriteRegister(
    _In_ PCHV_CONTROLLER Controller,
    _In_ ULONG Offset,
    _In_ ULONG Value);

_IRQL_requires_max_(HIGH_LEVEL)
ULONG
NTAPI
ChvPadOffset(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber);

_IRQL_requires_max_(HIGH_LEVEL)
VOID
NTAPI
ChvSetPadDirection(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber,
    _In_ BOOLEAN Output);

_IRQL_requires_max_(HIGH_LEVEL)
VOID
NTAPI
ChvWritePadOutput(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber,
    _In_ BOOLEAN High);

_IRQL_requires_max_(HIGH_LEVEL)
VOID
NTAPI
ChvSetPadTrigger(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber,
    _In_ KINTERRUPT_MODE Mode,
    _In_ KINTERRUPT_POLARITY Polarity);

_IRQL_requires_max_(HIGH_LEVEL)
VOID
NTAPI
ChvClearPadTrigger(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber);

_IRQL_requires_max_(HIGH_LEVEL)
VOID
NTAPI
ChvSetLineMask(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber,
    _In_ BOOLEAN Enable);

_IRQL_requires_max_(HIGH_LEVEL)
BOOLEAN
NTAPI
ChvIsLinePending(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber);

_IRQL_requires_max_(HIGH_LEVEL)
VOID
NTAPI
ChvAckLine(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber);

/* controller.c */
GPIO_CLIENT_PREPARE_CONTROLLER ChvPrepareController;
GPIO_CLIENT_RELEASE_CONTROLLER ChvReleaseController;
GPIO_CLIENT_START_CONTROLLER ChvStartController;
GPIO_CLIENT_STOP_CONTROLLER ChvStopController;
GPIO_CLIENT_QUERY_CONTROLLER_BASIC_INFORMATION ChvQueryControllerBasicInformation;
GPIO_CLIENT_SAVE_BANK_HARDWARE_CONTEXT ChvSaveBankHardwareContext;
GPIO_CLIENT_RESTORE_BANK_HARDWARE_CONTEXT ChvRestoreBankHardwareContext;

/* pins.c */
GPIO_CLIENT_CONNECT_IO_PINS ChvConnectIoPins;
GPIO_CLIENT_DISCONNECT_IO_PINS ChvDisconnectIoPins;
GPIO_CLIENT_READ_PINS ChvReadGpioPins;
GPIO_CLIENT_WRITE_PINS ChvWriteGpioPins;
GPIO_CLIENT_ENABLE_INTERRUPT ChvEnableInterrupt;
GPIO_CLIENT_DISABLE_INTERRUPT ChvDisableInterrupt;
GPIO_CLIENT_UNMASK_INTERRUPT ChvUnmaskInterrupt;
GPIO_CLIENT_MASK_INTERRUPTS ChvMaskInterrupts;
GPIO_CLIENT_QUERY_ACTIVE_INTERRUPTS ChvQueryActiveInterrupts;
GPIO_CLIENT_CLEAR_ACTIVE_INTERRUPTS ChvClearActiveInterrupts;
GPIO_CLIENT_RECONFIGURE_INTERRUPT ChvReconfigureInterrupt;
