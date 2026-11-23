/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Intel LPSS I2C controller private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * An SpbCx client for the DesignWare I2C core inside Intel's LPSS block.
 * The first memory BAR holds the core, the LPSS private registers 0x200
 * bytes in.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <reshub.h>
#include <spbcxstub.h>

#define LI2C_POOL_TAG 'c2IL'

/* DesignWare core */
#define DW_CON              0x00
#define DW_TAR              0x04
#define DW_HS_MADDR         0x0C
#define DW_DATA_CMD         0x10
#define DW_SS_SCL_HCNT      0x14
#define DW_SS_SCL_LCNT      0x18
#define DW_FS_SCL_HCNT      0x1C
#define DW_FS_SCL_LCNT      0x20
#define DW_HS_SCL_HCNT      0x24
#define DW_HS_SCL_LCNT      0x28
#define DW_INTR_MASK        0x30
#define DW_RAW_INTR_STAT    0x34
#define DW_RX_TL            0x38
#define DW_TX_TL            0x3C
#define DW_CLR_INTR         0x40
#define DW_CLR_RX_OVER      0x48
#define DW_CLR_TX_ABRT      0x54
#define DW_CLR_STOP_DET     0x60
#define DW_ENABLE           0x6C
#define DW_TXFLR            0x74
#define DW_RXFLR            0x78
#define DW_SDA_HOLD         0x7C
#define DW_TX_ABRT_SOURCE   0x80
#define DW_ENABLE_STATUS    0x9C
#define DW_FS_SPKLEN        0xA0
#define DW_HS_SPKLEN        0xA4

#define DW_CON_STANDARD     0x63
#define DW_CON_FAST         0x65
#define DW_CON_HIGH         0x67
#define DW_TAR_10BIT        0x1000

#define DW_ENABLE_ON        0x0001
#define DW_ENABLE_ABORT     0x0002

#define DW_CMD_READ         0x0100
#define DW_CMD_STOP         0x0200
#define DW_CMD_RESTART      0x0400

#define DW_INTR_RX_OVER     0x0002
#define DW_INTR_RX_FULL     0x0004
#define DW_INTR_TX_EMPTY    0x0010
#define DW_INTR_TX_ABRT     0x0040
#define DW_INTR_STOP_DET    0x0200

/* Address NACK in 7 bit or either 10 bit phase, and data NACK */
#define DW_ABRT_ADDRESS_NACK 0x0007
#define DW_ABRT_DATA_NACK    0x0008

/* Both FIFOs are this deep on every LPSS part */
#define DW_FIFO_DEPTH       64

/* LPSS private registers, relative to the core */
#define LPSS_PRIVATE        0x200
#define LPSS_RESETS         0x04
#define LPSS_ACTIVE_LTR     0x10
#define LPSS_IDLE_LTR       0x14
#define LPSS_TX_COUNT       0x18
#define LPSS_INTR_CONTROL   0x20
#define LPSS_CLOCK_GATE     0x38
#define LPSS_REMAP_LOW      0x40
#define LPSS_REMAP_HIGH     0x44
#define LPSS_CAPABILITIES   0xFC

/* An LTR at or above this is not programmed */
#define LPSS_LTR_UNUSED     0x7FE1

/** Counts for one bus speed, in controller clocks. */
typedef struct _LI2C_TIMING
{
    ULONG HighCount;
    ULONG LowCount;
    ULONG TxHold;
    ULONG RxHold;
    ULONG Spike;
} LI2C_TIMING, *PLI2C_TIMING;

typedef enum _LI2C_SPEED
{
    Li2cStandard,
    Li2cFast,
    Li2cFastPlus,
    Li2cHigh,
    Li2cSpeedCount
} LI2C_SPEED;

/** The transfer list of the request in flight, walked one transfer at a time. */
typedef struct _LI2C_TRANSFER
{
    SPBREQUEST Request;
    ULONG Count;
    ULONG Index;

    PUCHAR Buffer;
    SIZE_T Length;
    SPB_TRANSFER_DIRECTION Direction;
    SIZE_T Queued;
    SIZE_T Received;

    /* Bytes of the transfers before Index */
    SIZE_T Done;
} LI2C_TRANSFER, *PLI2C_TRANSFER;

typedef struct _LI2C_DEVICE
{
    WDFDEVICE Device;
    WDFINTERRUPT Interrupt;
    WDFSPINLOCK Lock;
    WDFTIMER DelayTimer;

    PUCHAR Core;
    PUCHAR Private;
    SIZE_T CoreLength;
    PHYSICAL_ADDRESS CorePhysical;

    USHORT PciDeviceId;
    UCHAR Revision;
    BOOLEAN DynamicClockGating;
    LI2C_TIMING Timing[Li2cSpeedCount];
    ULONG ActiveLtr[Li2cSpeedCount];
    ULONG IdleLtr;

    /* Mask written to the core; the ISR only looks at these bits */
    ULONG InterruptMask;
    ULONG PendingStatus;
    ULONG AbortSource;

    LI2C_TRANSFER Transfer;
} LI2C_DEVICE, *PLI2C_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(LI2C_DEVICE, Li2cGetDevice)

/** Per target settings, from the ACPI I2cSerialBus descriptor. */
typedef struct _LI2C_TARGET
{
    USHORT Address;
    BOOLEAN TenBit;
    ULONG Speed;
} LI2C_TARGET, *PLI2C_TARGET;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(LI2C_TARGET, Li2cGetTarget)

FORCEINLINE
ULONG
Li2cRead(
    _In_ PLI2C_DEVICE Device,
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(Device->Core + Offset));
}

FORCEINLINE
VOID
Li2cWrite(
    _In_ PLI2C_DEVICE Device,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(Device->Core + Offset), Value);
}

FORCEINLINE
ULONG
Li2cReadPrivate(
    _In_ PLI2C_DEVICE Device,
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(Device->Private + Offset));
}

FORCEINLINE
VOID
Li2cWritePrivate(
    _In_ PLI2C_DEVICE Device,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(Device->Private + Offset), Value);
}

/* platform.c */
_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
Li2cLoadPlatformDefaults(
    _Inout_ PLI2C_DEVICE Device);

_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
Li2cLoadAcpiTiming(
    _Inout_ PLI2C_DEVICE Device);

/* controller.c */
VOID
NTAPI
Li2cHardwareInitialize(
    _In_ PLI2C_DEVICE Device);

VOID
NTAPI
Li2cHardwareQuiesce(
    _In_ PLI2C_DEVICE Device);

NTSTATUS
NTAPI
Li2cConfigureForTarget(
    _In_ PLI2C_DEVICE Device,
    _In_ PLI2C_TARGET Target);

VOID
NTAPI
Li2cSetInterruptMask(
    _In_ PLI2C_DEVICE Device,
    _In_ ULONG Mask);

VOID
NTAPI
Li2cAbortTransfer(
    _In_ PLI2C_DEVICE Device);

/* transfer.c */
EVT_WDF_INTERRUPT_ISR Li2cEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC Li2cEvtInterruptDpc;
EVT_WDF_TIMER Li2cEvtDelayTimer;
EVT_SPB_CONTROLLER_READ Li2cEvtIoRead;
EVT_SPB_CONTROLLER_WRITE Li2cEvtIoWrite;
EVT_SPB_CONTROLLER_SEQUENCE Li2cEvtIoSequence;
