/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     LPSS block and DesignWare core setup
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "ialpss2i_i2c.h"

#define NDEBUG
#include <debug.h>

/* Polls for IC_ENABLE_STATUS to follow IC_ENABLE, 25us apart */
#define LI2C_ENABLE_POLLS   10
#define LI2C_ENABLE_STALL   25

/**
 * @brief
 * Turns an LTR value into the encoded register form, with the requirement bit set.
 */
static
ULONG
NTAPI
Li2cEncodeLtr(
    _In_ ULONG Value)
{
    if (Value <= 0x3FF)
        return Value | 0x8800;

    return (Value | 0x118000) >> 5;
}

/**
 * @brief
 * Masks the core's interrupts and the LPSS block's own interrupt source.
 */
static
VOID
NTAPI
Li2cMaskEverything(
    _In_ PLI2C_DEVICE Device)
{
    Device->InterruptMask = 0;
    Li2cWrite(Device, DW_INTR_MASK, 0);
    Li2cWritePrivate(Device,
                     LPSS_INTR_CONTROL,
                     Li2cReadPrivate(Device, LPSS_INTR_CONTROL) | 0x2);
}

/**
 * @brief
 * Takes the LPSS block out of reset and sets up clocks, remap and idle LTR.
 *
 * The core reads back zero while the block holds it in reset.
 */
VOID
NTAPI
Li2cHardwareInitialize(
    _In_ PLI2C_DEVICE Device)
{
    ULONG Capabilities;
    ULONG Resets;

    Resets = Li2cReadPrivate(Device, LPSS_RESETS);
    Li2cWritePrivate(Device, LPSS_RESETS, Resets & ~0x3UL);
    Li2cWritePrivate(Device, LPSS_RESETS, Resets | 0x3UL);

    /* Zero lets the hardware gate the clocks, all ones forces them on */
    Li2cWritePrivate(Device, LPSS_CLOCK_GATE, Device->DynamicClockGating ? 0 : 0xF);

    Li2cMaskEverything(Device);

    /* The block decodes DMA addresses relative to where its BAR sits */
    Li2cWritePrivate(Device, LPSS_REMAP_LOW, Device->CorePhysical.LowPart);
    Li2cWritePrivate(Device, LPSS_REMAP_HIGH, (ULONG)Device->CorePhysical.HighPart);

    Capabilities = Li2cReadPrivate(Device, LPSS_CAPABILITIES);
    if ((Capabilities & 0xF0) != 0 || Capabilities == MAXULONG)
    {
        DPRINT1("iaLPSS2 I2C: LPSS capabilities read 0x%08lx\n", Capabilities);
        return;
    }

    if (Device->IdleLtr < LPSS_LTR_UNUSED)
        Li2cWritePrivate(Device, LPSS_IDLE_LTR, Li2cEncodeLtr(Device->IdleLtr));
}

/**
 * @brief
 * Masks everything and drops the active LTR before the block powers down.
 */
VOID
NTAPI
Li2cHardwareQuiesce(
    _In_ PLI2C_DEVICE Device)
{
    Li2cMaskEverything(Device);
    Li2cWritePrivate(Device, LPSS_ACTIVE_LTR, 0x800);

    if (Device->IdleLtr < LPSS_LTR_UNUSED)
    {
        Li2cWritePrivate(Device,
                         LPSS_IDLE_LTR,
                         Li2cReadPrivate(Device, LPSS_IDLE_LTR) & ~0x8000UL);
    }
}

/**
 * @brief
 * Writes the interrupt mask and remembers it for the ISR.
 */
VOID
NTAPI
Li2cSetInterruptMask(
    _In_ PLI2C_DEVICE Device,
    _In_ ULONG Mask)
{
    Device->InterruptMask = Mask;
    Li2cWrite(Device, DW_INTR_MASK, Mask & 0xFFFF);
}

/**
 * @brief
 * Waits for IC_ENABLE_STATUS bit 0 to read the wanted state.
 */
static
BOOLEAN
NTAPI
Li2cWaitEnableState(
    _In_ PLI2C_DEVICE Device,
    _In_ BOOLEAN Enabled)
{
    ULONG Poll;

    for (Poll = 0; ; Poll++)
    {
        if (((Li2cRead(Device, DW_ENABLE_STATUS) & 1) != 0) == Enabled)
            return TRUE;

        if (Poll >= LI2C_ENABLE_POLLS)
            return FALSE;

        KeStallExecutionProcessor(LI2C_ENABLE_STALL);
    }
}

/**
 * @brief
 * Loads one speed's counts into the standard or fast register pair.
 */
static
VOID
NTAPI
Li2cLoadCounts(
    _In_ PLI2C_DEVICE Device,
    _In_ const LI2C_TIMING *Timing,
    _In_ ULONG HighRegister)
{
    Li2cWrite(Device, HighRegister, Timing->HighCount);
    Li2cWrite(Device, HighRegister + 4, Timing->LowCount);
    Li2cWrite(Device, DW_SDA_HOLD, Timing->TxHold | (Timing->RxHold << 16));
}

/**
 * @brief
 * Points the core at a target, at the speed its descriptor asks for, and enables it.
 *
 * A speed the table has no entry for keeps whatever timing the core holds.
 */
NTSTATUS
NTAPI
Li2cConfigureForTarget(
    _In_ PLI2C_DEVICE Device,
    _In_ PLI2C_TARGET Target)
{
    const LI2C_TIMING *Timing;
    ULONG Ltr = LPSS_LTR_UNUSED;

    Li2cWrite(Device, DW_ENABLE, 0);
    if (!Li2cWaitEnableState(Device, FALSE))
    {
        DPRINT1("iaLPSS2 I2C: core would not disable\n");
        return STATUS_IO_TIMEOUT;
    }

    /* Nothing latched by an earlier request may complete this one */
    (VOID)Li2cRead(Device, DW_CLR_INTR);
    Li2cWrite(Device, DW_TX_TL, 0);
    Li2cWrite(Device, DW_TAR, Target->Address | (Target->TenBit ? DW_TAR_10BIT : 0));

    switch (Target->Speed)
    {
        case 100000:
            Timing = &Device->Timing[Li2cStandard];
            Li2cWrite(Device, DW_CON, DW_CON_STANDARD);
            Li2cLoadCounts(Device, Timing, DW_SS_SCL_HCNT);
            Li2cWrite(Device, DW_FS_SPKLEN, Timing->Spike);
            Ltr = Device->ActiveLtr[Li2cStandard];
            break;

        case 400000:
            Timing = &Device->Timing[Li2cFast];
            Li2cWrite(Device, DW_CON, DW_CON_FAST);
            Li2cLoadCounts(Device, Timing, DW_FS_SCL_HCNT);
            Li2cWrite(Device, DW_FS_SPKLEN, Timing->Spike);
            Ltr = Device->ActiveLtr[Li2cFast];
            break;

        case 1000000:
            Timing = &Device->Timing[Li2cFastPlus];
            Li2cWrite(Device, DW_CON, DW_CON_FAST);
            Li2cLoadCounts(Device, Timing, DW_FS_SCL_HCNT);
            Li2cWrite(Device, DW_FS_SPKLEN, Timing->Spike);
            Ltr = Device->ActiveLtr[Li2cFastPlus];
            break;

        case 1400000:
        case 3400000:
            /* The master code phase runs at fast speed */
            Timing = &Device->Timing[Li2cHigh];
            Li2cWrite(Device, DW_HS_MADDR, 1);
            Li2cWrite(Device, DW_CON, DW_CON_HIGH);
            Li2cWrite(Device, DW_FS_SCL_HCNT, Device->Timing[Li2cFast].HighCount);
            Li2cWrite(Device, DW_FS_SCL_LCNT, Device->Timing[Li2cFast].LowCount);
            Li2cLoadCounts(Device, Timing, DW_HS_SCL_HCNT);
            Li2cWrite(Device, DW_HS_SPKLEN, Timing->Spike);
            Ltr = Device->ActiveLtr[Li2cHigh];
            break;

        default:
            DPRINT1("iaLPSS2 I2C: no timing for %lu Hz\n", Target->Speed);
            break;
    }

    if (Ltr < LPSS_LTR_UNUSED)
        Li2cWritePrivate(Device, LPSS_ACTIVE_LTR, Li2cEncodeLtr(Ltr));

    Li2cWrite(Device, DW_ENABLE, DW_ENABLE_ON);
    if (!Li2cWaitEnableState(Device, TRUE))
    {
        DPRINT1("iaLPSS2 I2C: core would not enable\n");
        return STATUS_IO_TIMEOUT;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Stops whatever the core is doing: it flushes the FIFO and sends STOP.
 */
VOID
NTAPI
Li2cAbortTransfer(
    _In_ PLI2C_DEVICE Device)
{
    ULONG Poll;

    Li2cSetInterruptMask(Device, 0);

    if ((Li2cRead(Device, DW_ENABLE_STATUS) & 1) == 0)
        return;

    Li2cWrite(Device, DW_ENABLE, Li2cRead(Device, DW_ENABLE) | DW_ENABLE_ABORT);
    for (Poll = 0; Poll < LI2C_ENABLE_POLLS; Poll++)
    {
        if ((Li2cRead(Device, DW_ENABLE) & DW_ENABLE_ABORT) == 0)
            break;

        KeStallExecutionProcessor(LI2C_ENABLE_STALL);
    }

    (VOID)Li2cRead(Device, DW_CLR_INTR);
    Li2cWrite(Device, DW_ENABLE, 0);
    (VOID)Li2cWaitEnableState(Device, FALSE);
}
