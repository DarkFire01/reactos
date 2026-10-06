/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DesignWare I2C engine
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */


#define NDEBUG
#include "i2cpriv.h"

/*
 * Almost every DesignWare register is only writable while the core is disabled,
 * so configuration is bracketed by this. IC_ENABLE_STATUS, not IC_ENABLE, says
 * when the core has actually stopped. A transfer in flight keeps it busy for a
 * while after the write.
 */
static
NTSTATUS
I2cSetCoreEnabled(
    _In_ PI2C_DEVICE Device,
    _In_ BOOLEAN Enable)
{
    ULONG Attempt;

    for (Attempt = 0; Attempt < 100; Attempt++)
    {
        I2cWrite(Device, DW_IC_ENABLE, Enable ? 1 : 0);

        if (((I2cRead(Device, DW_IC_ENABLE_STATUS) & 1) != 0) == (Enable != FALSE))
        {
            return STATUS_SUCCESS;
        }

        /* The datasheet asks for 10 bus cycles; at 100kHz that is 100us */
        KeStallExecutionProcessor(25);
    }

    DPRINT1("i2c: core would not %s\n", Enable ? "enable" : "disable");
    return STATUS_IO_TIMEOUT;
}

/*
 * The bus timing every LPSS I2C controller starts with.
 *
 * Intel used to ship a driver binary per SoC family, each with its own
 * GetDefaultSettings; the newest one, iaLPSS2_I2C_WCL.sys, carries a single set
 * for all of them and keys nothing off the PCI device id. These are its values
 * (sub_14000F648), and they are the same numbers the Cannon Lake driver
 * installed, so nothing changes for the parts that already worked.
 *
 * The reference lets the registry override each count by name and then lets
 * firmware override them again through the SSCN, FMCN, FPCN and HSCN control
 * methods on the controller's ACPI companion. Neither override is built here
 * yet, so a board that asks for something other than these counts still runs at
 * the default rate.
 */
static const I2C_SPEED_TIMING I2cStandardSpeedDefault = { 0x210, 0x280, 0x1C, 0x1C, 1 };
static const I2C_SPEED_TIMING I2cFastSpeedDefault     = { 0x080, 0x0A0, 0x1C, 0x1C, 6 };
static const I2C_SPEED_TIMING I2cFastSpeedPlusDefault = { 0x01E, 0x050, 0x28, 0x28, 6 };
static const I2C_SPEED_TIMING I2cHighSpeedDefault     = { 0x00F, 0x028, 0x14, 0x14, 2 };

/**
 * @brief
 * Installs the bus timing a controller runs at until something overrides it.
 *
 * @param[in,out] Device
 * The controller.
 */
VOID
I2cControllerLoadDefaultTiming(
    _In_ PI2C_DEVICE Device)
{
    Device->StandardSpeed = I2cStandardSpeedDefault;
    Device->FastSpeed = I2cFastSpeedDefault;
    Device->FastSpeedPlus = I2cFastSpeedPlusDefault;
    Device->HighSpeed = I2cHighSpeedDefault;
}

/*
 * Program one speed's counts. DesignWare has no separate fast-mode-plus register
 * set (fast-plus is the fast registers loaded with shorter counts), so the
 * caller picks the timing and this writes it into whichever pair applies.
 *
 * IC_SDA_HOLD carries both directions in one register: transmit hold in the low
 * sixteen bits, receive hold in the next eight.
 */
static
VOID
I2cApplyTiming(
    _In_ PI2C_DEVICE Device,
    _In_ PI2C_SPEED_TIMING Timing,
    _In_ BOOLEAN FastRegisters)
{
    if (FastRegisters)
    {
        I2cWrite(Device, DW_IC_FS_SCL_HCNT, Timing->SclHighCount);
        I2cWrite(Device, DW_IC_FS_SCL_LCNT, Timing->SclLowCount);
        I2cWrite(Device, DW_IC_FS_SPKLEN, Timing->SpikeLength);
    }
    else
    {
        I2cWrite(Device, DW_IC_SS_SCL_HCNT, Timing->SclHighCount);
        I2cWrite(Device, DW_IC_SS_SCL_LCNT, Timing->SclLowCount);
        I2cWrite(Device, DW_IC_FS_SPKLEN, Timing->SpikeLength);
    }

    I2cWrite(Device, DW_IC_SDA_HOLD,
             (Timing->SdaWriteHoldCount & 0xFFFF) |
             ((Timing->SdaReadHoldCount & 0xFF) << 16));
}

VOID
I2cControllerDisableInterrupts(
    _In_ PI2C_DEVICE Device)
{
    I2cWrite(Device, DW_IC_INTR_MASK, 0);
}

VOID
I2cControllerEnableInterrupts(
    _In_ PI2C_DEVICE Device,
    _In_ ULONG Mask)
{
    I2cWrite(Device, DW_IC_INTR_MASK, Mask);
}

/*
 * The reference takes the LPSS shell out of reset first, then reads IC_COMP_TYPE
 * to confirm what it is talking to. Both matter: the core reads back all-zero
 * while held in reset, so the identity check would fail for the wrong reason.
 */
NTSTATUS
I2cControllerInitialize(
    _In_ PI2C_DEVICE Device)
{
    ULONG Value;
    NTSTATUS Status;

    /*
     * Bits [1:0] of the LPSS reset register hold the function and its DMA in
     * reset. The reference clears them and sets them again, a pulse and not just
     * a release, so a controller left running by firmware starts from a known
     * state.
     */
    if (Device->PrivBase != NULL)
    {
        Value = READ_REGISTER_ULONG((PULONG)(Device->PrivBase + LPSS_PRIV_RESETS));

        WRITE_REGISTER_ULONG((PULONG)(Device->PrivBase + LPSS_PRIV_RESETS),
                             Value & ~(ULONG)LPSS_PRIV_RESETS_FUNC);
        KeStallExecutionProcessor(10);
        WRITE_REGISTER_ULONG((PULONG)(Device->PrivBase + LPSS_PRIV_RESETS),
                             Value | LPSS_PRIV_RESETS_FUNC);
        KeStallExecutionProcessor(10);
    }

    /*
     * The capability register in Intel's shell. The reference reads it and
     * checks only that bits 4-7 are clear and that it is not all ones, the
     * latter being the usual "the read never reached hardware" test, since an
     * absent or unpowered controller floats the bus high. It traces on a bad
     * value rather than refusing to start, so this does too.
     *
     * Note this is NOT the DesignWare IC_COMP_TYPE: that lives at the same 0xFC
     * offset but in the core, and none of the Intel drivers look at it. Checking
     * the core's identity here would be an invention, and one that would fail a
     * working controller whose core reports a different revision.
     */
    if (Device->PrivBase != NULL)
    {
        Device->Capabilities =
            READ_REGISTER_ULONG((PULONG)(Device->PrivBase + LPSS_PRIV_CAPS));

        if ((Device->Capabilities & 0xF0) != 0 ||
            Device->Capabilities == MAXULONG)
        {
            DPRINT1("i2c: LPSS capabilities read back 0x%08lX\n",
                    Device->Capabilities);
        }
    }

    Status = I2cSetCoreEnabled(Device, FALSE);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    I2cControllerDisableInterrupts(Device);

    /*
     * FIFO depth is not in a register of its own: the transmit threshold
     * saturates at depth-1, so writing all ones and reading back gives it. The
     * core has to stay disabled across this or the write is dropped.
     */
    I2cWrite(Device, DW_IC_TX_TL, 0xFF);
    Device->TxFifoDepth = (I2cRead(Device, DW_IC_TX_TL) & 0xFF) + 1;
    I2cWrite(Device, DW_IC_RX_TL, 0xFF);
    Device->RxFifoDepth = (I2cRead(Device, DW_IC_RX_TL) & 0xFF) + 1;

    /* One byte at a time in, empty out: simple and correct for PIO */
    I2cWrite(Device, DW_IC_RX_TL, 0);
    I2cWrite(Device, DW_IC_TX_TL, 0);

    /* Read-to-clear; discards anything latched before we owned the core */
    (VOID)I2cRead(Device, DW_IC_CLR_INTR);

    DPRINT("i2c: DesignWare core, TX FIFO %u, RX FIFO %u\n",
           Device->TxFifoDepth, Device->RxFifoDepth);

    return STATUS_SUCCESS;
}

VOID
I2cControllerUninitialize(
    _In_ PI2C_DEVICE Device)
{
    I2cControllerDisableInterrupts(Device);
    (VOID)I2cRead(Device, DW_IC_CLR_INTR);
    (VOID)I2cSetCoreEnabled(Device, FALSE);
}

/*
 * The target address and bus speed live in IC_CON and IC_TAR, both write-only
 * while disabled, so every target switch stops the core. That is the reference's
 * behavior too and is why SpbCx serializes the controller queue.
 */
NTSTATUS
I2cControllerConfigureForTransfer(
    _In_ PI2C_DEVICE Device,
    _In_ PI2C_TARGET Target)
{
    ULONG Control;
    NTSTATUS Status;

    Status = I2cSetCoreEnabled(Device, FALSE);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Control = DW_IC_CON_MASTER | DW_IC_CON_SLAVE_DISABLE | DW_IC_CON_RESTART_EN;

    /*
     * A sequence is a set of transfers with repeated starts between them, which
     * needs RESTART_EN above. The speed is whatever the peripheral's firmware
     * descriptor asked for, and it decides both the IC_CON mode bits and which
     * set of counts gets loaded. Fast-mode-plus rides in the fast registers,
     * because DesignWare has no separate pair for it.
     */
    if (Target->ConnectionSpeed > 400000)
    {
        Control |= DW_IC_CON_SPEED_FAST;
        I2cApplyTiming(Device, &Device->FastSpeedPlus, TRUE);
    }
    else if (Target->ConnectionSpeed > 100000)
    {
        Control |= DW_IC_CON_SPEED_FAST;
        I2cApplyTiming(Device, &Device->FastSpeed, TRUE);
    }
    else
    {
        Control |= DW_IC_CON_SPEED_STD;
        I2cApplyTiming(Device, &Device->StandardSpeed, FALSE);
    }

    if (Target->TenBitAddress)
    {
        Control |= DW_IC_CON_10BITADDR_MASTER;
    }

    I2cWrite(Device, DW_IC_CON, Control);
    I2cWrite(Device, DW_IC_TAR, Target->SlaveAddress);

    I2cControllerDisableInterrupts(Device);
    (VOID)I2cRead(Device, DW_IC_CLR_INTR);

    return I2cSetCoreEnabled(Device, TRUE);
}
