/*
 * PROJECT:     ReactOS Intel Cherryview GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pad geometry and register access
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "iagpioe.h"

#define NDEBUG
#include <debug.h>

/* The communities share one lock, every access of every instance takes it */
static KSPIN_LOCK ChvRegisterLock;

/* Bit n of a family mask means pad n of that family is wired */
static const CHV_COMMUNITY ChvCommunities[] =
{
    /* Southwest */
    { 1, 98, 16, { 0x00FF, 0x00FF, 0x00FF, 0x03F3, 0x00FF, 0x00FF, 0x00FF } },
    /* North */
    { 2, 73, 16, { 0x01FF, 0x1FFF, 0x0FDF, 0x0FFF, 0x1FFF } },
    /* East */
    { 3, 27, 16, { 0x0FFF, 0x0FFF } },
    /* Southeast */
    { 4, 86, 16, { 0x00FF, 0x0FFF, 0x003F, 0x00FF, 0x03FF, 0x07FF } },
    /* Fifth community, a single family of eight banked by eight */
    { 5, 8, 8, { 0x00FF } }
};

VOID
NTAPI
ChvInitializeRegisterLock(VOID)
{
    KeInitializeSpinLock(&ChvRegisterLock);
}

static
KIRQL
ChvLock(VOID)
{
    KIRQL OldIrql;

    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    KeAcquireSpinLockAtDpcLevel(&ChvRegisterLock);
    return OldIrql;
}

static
VOID
ChvUnlock(
    _In_ KIRQL OldIrql)
{
    KeReleaseSpinLockFromDpcLevel(&ChvRegisterLock);
    KeLowerIrql(OldIrql);
}

static
ULONG
ChvRawRead(
    _In_ PCHV_CONTROLLER Controller,
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(Controller->Base + Offset));
}

static
VOID
ChvRawWrite(
    _In_ PCHV_CONTROLLER Controller,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(Controller->Base + Offset), Value);
    KeMemoryBarrier();
}

const CHV_COMMUNITY *
NTAPI
ChvFindCommunity(
    _In_ ULONG Uid)
{
    ULONG Index;

    for (Index = 0; Index < RTL_NUMBER_OF(ChvCommunities); Index++)
    {
        if (ChvCommunities[Index].Uid == Uid)
            return &ChvCommunities[Index];
    }

    return NULL;
}

BOOLEAN
NTAPI
ChvIsPinValid(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    const CHV_COMMUNITY *Community = Controller->Community;
    ULONG Index;

    if (!Community || (PinNumber >= Community->PinsPerBank))
        return FALSE;

    Index = (ULONG)BankId * Community->PinsPerBank + PinNumber;
    if (Index >= Community->PadCount)
        return FALSE;

    return (Community->FamilyPads[Index / CHV_PADS_PER_FAMILY] >> (Index % CHV_PADS_PER_FAMILY)) & 1;
}

ULONG
NTAPI
ChvPadOffset(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    ULONG Index = (ULONG)BankId * Controller->Community->PinsPerBank + PinNumber;

    return CHV_FAMILY_BASE +
           (Index / CHV_PADS_PER_FAMILY) * CHV_FAMILY_STRIDE +
           (Index % CHV_PADS_PER_FAMILY) * CHV_PAD_STRIDE;
}

ULONG
NTAPI
ChvReadRegister(
    _In_ PCHV_CONTROLLER Controller,
    _In_ ULONG Offset)
{
    KIRQL OldIrql;
    ULONG Value;

    OldIrql = ChvLock();
    Value = ChvRawRead(Controller, Offset);
    ChvUnlock(OldIrql);

    return Value;
}

VOID
NTAPI
ChvWriteRegister(
    _In_ PCHV_CONTROLLER Controller,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    KIRQL OldIrql;

    OldIrql = ChvLock();
    ChvRawWrite(Controller, Offset, Value);
    ChvUnlock(OldIrql);
}

/**
 * @brief
 * Read-modify-write of one pad control dword, remembered for the next restore.
 *
 * @param[in] Second
 * FALSE for the first control dword, TRUE for the second one.
 *
 * @param[in] Always
 * Writes the register back even when nothing changed.
 */
static
VOID
ChvUpdatePad(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber,
    _In_ BOOLEAN Second,
    _In_ ULONG Clear,
    _In_ ULONG Set,
    _In_ BOOLEAN Always)
{
    PCHV_PAD_SHADOW Shadow = &Controller->Shadow[BankId][PinNumber];
    ULONG Offset;
    ULONG Old, New;
    KIRQL OldIrql;

    Offset = ChvPadOffset(Controller, BankId, PinNumber) + (Second ? sizeof(ULONG) : 0);

    OldIrql = ChvLock();
    Old = ChvRawRead(Controller, Offset);
    New = (Old & ~Clear) | Set;
    if (Always || (New != Old))
    {
        ChvRawWrite(Controller, Offset, New);
        if (Second)
            Shadow->Ctrl1 = New;
        else
            Shadow->Ctrl0 = New;
    }
    ChvUnlock(OldIrql);
}

VOID
NTAPI
ChvSetPadDirection(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber,
    _In_ BOOLEAN Output)
{
    ULONG Config;

    if (!ChvIsPinValid(Controller, BankId, PinNumber))
        return;

    if (Output)
        Config = CHV_CTRL0_CFG_OUTPUT;
    else if ((Controller->Uid == 5) && (BankId == 0) && (PinNumber >= 5))
        Config = CHV_CTRL0_CFG_INOUT; /* The top three virtual pads stay bidirectional */
    else
        Config = CHV_CTRL0_CFG_INPUT;

    ChvUpdatePad(Controller, BankId, PinNumber, FALSE, CHV_CTRL0_CFG_MASK, Config, FALSE);
}

VOID
NTAPI
ChvWritePadOutput(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber,
    _In_ BOOLEAN High)
{
    if (!ChvIsPinValid(Controller, BankId, PinNumber))
        return;

    ChvUpdatePad(Controller,
                 BankId,
                 PinNumber,
                 FALSE,
                 CHV_CTRL0_TX_STATE,
                 High ? CHV_CTRL0_TX_STATE : 0,
                 TRUE);
}

VOID
NTAPI
ChvSetPadTrigger(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber,
    _In_ KINTERRUPT_MODE Mode,
    _In_ KINTERRUPT_POLARITY Polarity)
{
    ULONG Clear = 0;
    ULONG Set = 0;

    if (!ChvIsPinValid(Controller, BankId, PinNumber))
        return;

    if (Mode == LevelSensitive)
    {
        Clear = CHV_CTRL1_INT_MASK;
        Set = CHV_CTRL1_INT_LEVEL;

        /* Active low levels are seen through the receive inverter */
        if (Polarity == InterruptActiveHigh)
            Clear |= CHV_CTRL1_INVERT_RX;
        else if (Polarity == InterruptActiveLow)
            Set |= CHV_CTRL1_INVERT_RX;
    }
    else if (Mode == Latched)
    {
        Clear = CHV_CTRL1_INT_MASK;
        if (Polarity == InterruptRisingEdge)
            Set = CHV_CTRL1_INT_RISING;
        else if (Polarity == InterruptFallingEdge)
            Set = CHV_CTRL1_INT_FALLING;
        else if (Polarity == InterruptActiveBoth)
            Set = CHV_CTRL1_INT_BOTH;
    }

    ChvUpdatePad(Controller, BankId, PinNumber, TRUE, Clear, Set, TRUE);
    ChvSetLineMask(Controller, BankId, PinNumber, TRUE);
}

VOID
NTAPI
ChvClearPadTrigger(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    ULONG Ctrl0;

    if (!ChvIsPinValid(Controller, BankId, PinNumber))
        return;

    /* Only pads in GPIO mode own their trigger, native pads are left alone */
    Ctrl0 = ChvReadRegister(Controller, ChvPadOffset(Controller, BankId, PinNumber));
    if (Ctrl0 & CHV_CTRL0_GPIO_ENABLE)
        ChvUpdatePad(Controller, BankId, PinNumber, TRUE, CHV_CTRL1_INT_MASK, 0, TRUE);
}

/** The interrupt line firmware routed the pad to; pads may share one. */
static
ULONG
ChvPadLineBit(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    ULONG Ctrl0 = ChvReadRegister(Controller, ChvPadOffset(Controller, BankId, PinNumber));

    return 1UL << (Ctrl0 >> CHV_CTRL0_LINE_SHIFT);
}

VOID
NTAPI
ChvSetLineMask(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber,
    _In_ BOOLEAN Enable)
{
    ULONG LineBit;
    ULONG Mask;
    KIRQL OldIrql;

    if (!ChvIsPinValid(Controller, BankId, PinNumber))
        return;

    LineBit = ChvPadLineBit(Controller, BankId, PinNumber);

    OldIrql = ChvLock();
    Mask = ChvRawRead(Controller, CHV_INTERRUPT_MASK);
    Mask = Enable ? (Mask | LineBit) : (Mask & ~LineBit);
    ChvRawWrite(Controller, CHV_INTERRUPT_MASK, Mask);
    ChvUnlock(OldIrql);

    Controller->SavedMask = Mask;
    Controller->MaskSaved = TRUE;
}

BOOLEAN
NTAPI
ChvIsLinePending(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    ULONG LineBit = ChvPadLineBit(Controller, BankId, PinNumber);

    return (ChvReadRegister(Controller, CHV_INTERRUPT_STATUS) & LineBit) != 0;
}

VOID
NTAPI
ChvAckLine(
    _In_ PCHV_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    ULONG LineBit;

    if (!ChvIsPinValid(Controller, BankId, PinNumber))
        return;

    /* Status bits are write one to clear */
    LineBit = ChvPadLineBit(Controller, BankId, PinNumber);
    if (ChvReadRegister(Controller, CHV_INTERRUPT_STATUS) & LineBit)
        ChvWriteRegister(Controller, CHV_INTERRUPT_STATUS, LineBit);
}
