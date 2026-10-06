/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Intel LPSS I2C controller - private definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * An SpbCx client: SpbCx owns the targets, the queue and the transfer lists;
 * this owns the hardware. The engine is Synopsys DesignWare I2C, which Intel
 * wraps in an LPSS shell. The same core AMD and several others ship, so the
 * register map below is not Intel-specific even though the PnP ids are.
 *
 * The reference keeps two register blocks in its device context, at offsets 48
 * and 56. Offset 48 is the DesignWare core: every offset it touches (0x04 TAR,
 * 0x10 DATA_CMD, 0x2C INTR_STAT, 0x6C ENABLE, 0x70 STATUS, 0x74/0x78 the FIFO
 * levels, 0xF8/0xFC the component id) is an exact match for the DW map, which is
 * what identifies it. Offset 56 is Intel's private shell at BAR0 + 0x200, of
 * which only the reset register's meaning is documented publicly.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>
#include <spb.h>
#include <spbcx.h>
#include <debug.h>

#define I2C_POOL_TAG 'c2iL'   /* "Li2c" */

/* DesignWare I2C core, from the controller's first memory resource */

#define DW_IC_CON               0x00
#define DW_IC_TAR               0x04
#define DW_IC_SAR               0x08
#define DW_IC_HS_MADDR          0x0C
#define DW_IC_DATA_CMD          0x10
#define DW_IC_SS_SCL_HCNT       0x14
#define DW_IC_SS_SCL_LCNT       0x18
#define DW_IC_FS_SCL_HCNT       0x1C
#define DW_IC_FS_SCL_LCNT       0x20
#define DW_IC_HS_SCL_HCNT       0x24
#define DW_IC_HS_SCL_LCNT       0x28
#define DW_IC_INTR_STAT         0x2C
#define DW_IC_INTR_MASK         0x30
#define DW_IC_RAW_INTR_STAT     0x34
#define DW_IC_RX_TL             0x38
#define DW_IC_TX_TL             0x3C
#define DW_IC_CLR_INTR          0x40
#define DW_IC_CLR_RX_UNDER      0x44
#define DW_IC_CLR_RX_OVER       0x48
#define DW_IC_CLR_TX_OVER       0x4C
#define DW_IC_CLR_RD_REQ        0x50
#define DW_IC_CLR_TX_ABRT       0x54
#define DW_IC_CLR_RX_DONE       0x58
#define DW_IC_CLR_ACTIVITY      0x5C
#define DW_IC_CLR_STOP_DET      0x60
#define DW_IC_CLR_START_DET     0x64
#define DW_IC_CLR_GEN_CALL      0x68
#define DW_IC_ENABLE            0x6C
#define DW_IC_STATUS            0x70
#define DW_IC_TXFLR             0x74
#define DW_IC_RXFLR             0x78
#define DW_IC_SDA_HOLD          0x7C
#define DW_IC_TX_ABRT_SOURCE    0x80
#define DW_IC_DMA_CR            0x88
#define DW_IC_DMA_TDLR          0x8C
#define DW_IC_DMA_RDLR          0x90
#define DW_IC_ENABLE_STATUS     0x9C
#define DW_IC_FS_SPKLEN         0xA0
#define DW_IC_HS_SPKLEN         0xA4
#define DW_IC_CLR_RESTART_DET   0xA8
#define DW_IC_COMP_VERSION      0xF8
#define DW_IC_COMP_TYPE         0xFC

/* IC_CON */
#define DW_IC_CON_MASTER            0x0001
#define DW_IC_CON_SPEED_STD         0x0002
#define DW_IC_CON_SPEED_FAST        0x0004
#define DW_IC_CON_SPEED_HIGH        0x0006
#define DW_IC_CON_SPEED_MASK        0x0006
#define DW_IC_CON_10BITADDR_MASTER  0x0010
#define DW_IC_CON_RESTART_EN        0x0020
#define DW_IC_CON_SLAVE_DISABLE     0x0040

/* IC_DATA_CMD: the command bits ride above the byte */
#define DW_IC_DATA_CMD_READ         0x0100
#define DW_IC_DATA_CMD_STOP         0x0200
#define DW_IC_DATA_CMD_RESTART      0x0400

/* IC_INTR_STAT / IC_INTR_MASK / IC_RAW_INTR_STAT */
#define DW_IC_INTR_RX_UNDER         0x0001
#define DW_IC_INTR_RX_OVER          0x0002
#define DW_IC_INTR_RX_FULL          0x0004
#define DW_IC_INTR_TX_OVER          0x0008
#define DW_IC_INTR_TX_EMPTY         0x0010
#define DW_IC_INTR_RD_REQ           0x0020
#define DW_IC_INTR_TX_ABRT          0x0040
#define DW_IC_INTR_RX_DONE          0x0080
#define DW_IC_INTR_ACTIVITY         0x0100
#define DW_IC_INTR_STOP_DET         0x0200
#define DW_IC_INTR_START_DET        0x0400
#define DW_IC_INTR_GEN_CALL         0x0800
#define DW_IC_INTR_RESTART_DET      0x1000

#define DW_IC_INTR_DEFAULT_MASK \
    (DW_IC_INTR_RX_FULL | DW_IC_INTR_TX_ABRT | DW_IC_INTR_STOP_DET)

/* IC_STATUS */
#define DW_IC_STATUS_ACTIVITY       0x0001
#define DW_IC_STATUS_TFNF           0x0002   /* transmit FIFO not full */
#define DW_IC_STATUS_TFE            0x0004   /* transmit FIFO empty */
#define DW_IC_STATUS_RFNE           0x0008   /* receive FIFO not empty */
#define DW_IC_STATUS_RFF            0x0010
#define DW_IC_STATUS_MST_ACTIVITY   0x0020

/*
 * Intel's LPSS shell, from the controller's second memory resource. Only the
 * reset register is publicly documented; the reference touches several more
 * whose meaning is not recoverable from the decomp alone, and none of those are
 * needed to run the bus.
 */
#define LPSS_PRIV_RESETS            0x04
#define LPSS_PRIV_RESETS_FUNC       0x03   /* both bits clear = held in reset */
#define LPSS_PRIV_CAPS              0xFC   /* bits 4-7 clear on a healthy read */

/*
 * Per-speed bus timing, in controller clock cycles.
 *
 * They are consistent with a ~133 MHz LPSS clock: 528 + 640 cycles is 8.8us,
 * which is 100 kHz once bus rise time and spike suppression are counted.
 */
typedef struct _I2C_SPEED_TIMING
{
    ULONG SclHighCount;
    ULONG SclLowCount;
    ULONG SdaWriteHoldCount;
    ULONG SdaReadHoldCount;
    ULONG SpikeLength;
} I2C_SPEED_TIMING, *PI2C_SPEED_TIMING;

/* One controller */
typedef struct _I2C_DEVICE
{
    WDFDEVICE Device;

    PUCHAR CoreBase;          /* DesignWare core */
    ULONG CoreLength;
    PUCHAR PrivBase;          /* Intel LPSS shell, may be absent */
    ULONG PrivLength;

    WDFINTERRUPT Interrupt;

    ULONG Capabilities;       /* LPSS shell capability register, diagnostics only */
    USHORT PciDeviceId;       /* traced only; the timing is not keyed off it */
    I2C_SPEED_TIMING StandardSpeed;
    I2C_SPEED_TIMING FastSpeed;
    I2C_SPEED_TIMING FastSpeedPlus;
    I2C_SPEED_TIMING HighSpeed;

    ULONG TxFifoDepth;
    ULONG RxFifoDepth;

    /* The transfer the interrupt is currently servicing */
    SPBTARGET CurrentTarget;
    SPBREQUEST CurrentRequest;
    PMDL CurrentMdl;
    PUCHAR CurrentBuffer;
    ULONG CurrentLength;
    ULONG BytesWritten;       /* commands pushed into the TX FIFO */
    ULONG BytesRead;
    SPB_TRANSFER_DIRECTION CurrentDirection;
    ULONG TransferIndex;
    ULONG TransferCount;
    NTSTATUS TransferStatus;

    /* Latched by the ISR for the DPC */
    ULONG InterruptStatus;
    ULONG AbortSource;
} I2C_DEVICE, *PI2C_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(I2C_DEVICE, I2cGetDeviceContext)

/* Per-target, from the ACPI I2cSerialBus descriptor */
typedef struct _I2C_TARGET
{
    USHORT SlaveAddress;
    ULONG ConnectionSpeed;
    BOOLEAN TenBitAddress;
} I2C_TARGET, *PI2C_TARGET;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(I2C_TARGET, I2cGetTargetContext)

FORCEINLINE ULONG
I2cRead(_In_ PI2C_DEVICE Device, _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(Device->CoreBase + Offset));
}

FORCEINLINE VOID
I2cWrite(_In_ PI2C_DEVICE Device, _In_ ULONG Offset, _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(Device->CoreBase + Offset), Value);
}

/* controller.c */

NTSTATUS I2cControllerInitialize(_In_ PI2C_DEVICE Device);
VOID I2cControllerUninitialize(_In_ PI2C_DEVICE Device);
NTSTATUS I2cControllerConfigureForTransfer(_In_ PI2C_DEVICE Device,
                                           _In_ PI2C_TARGET Target);
VOID I2cControllerDisableInterrupts(_In_ PI2C_DEVICE Device);
VOID I2cControllerEnableInterrupts(_In_ PI2C_DEVICE Device, _In_ ULONG Mask);
VOID I2cControllerLoadDefaultTiming(_In_ PI2C_DEVICE Device);

/* transfer.c */

EVT_WDF_INTERRUPT_ISR I2cEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC I2cEvtInterruptDpc;
EVT_SPB_CONTROLLER_READ I2cEvtIoRead;
EVT_SPB_CONTROLLER_WRITE I2cEvtIoWrite;
EVT_SPB_CONTROLLER_SEQUENCE I2cEvtIoSequence;
