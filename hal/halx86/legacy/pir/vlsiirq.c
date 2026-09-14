/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     VLSI PCI IRQ router miniports
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include "irqrouter.h"

#define NDEBUG
#include <debug.h>

#define VLSI_NO_CODE            0xFF

/* VLSI ***********************************************************************/

/*
 * Register 0x74 holds a 3-bit IRQ code for each of links 1-4, and bits 16-23
 * enable the eight codes. The low byte of 0x5C marks the codes that are level
 * triggered, and is mirrored into 0x78.
 */
#define VLSI_ROUTE_REGISTER     0x74
#define VLSI_TRIGGER_REGISTER   0x5C
#define VLSI_TRIGGER_MIRROR     0x78
#define VLSI_LINKS              4
#define VLSI_CODES              8
#define VLSI_CODE_BITS          4
#define VLSI_CODE_MASK          0x07
#define VLSI_ENABLE_SHIFT       16

static const UCHAR HalpVlsiCodeIrqs[VLSI_CODES] = { 3, 5, 9, 10, 11, 12, 14, 15 };

static
UCHAR
HalpVlsiIrqToCode(
    _In_ UCHAR Irq)
{
    UCHAR Code;

    for (Code = 0; Code < VLSI_CODES; Code++)
    {
        if (HalpVlsiCodeIrqs[Code] == Irq)
            return Code;
    }

    return VLSI_NO_CODE;
}

static
NTSTATUS
NTAPI
HalpVlsiValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    UCHAR Lowest, Highest;

    HalpPirLinkRange(Table, &Lowest, &Highest);

    return (Highest <= VLSI_LINKS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpVlsiGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    ULONG Route;
    UCHAR Code;

    if (Link == 0 || Link > VLSI_LINKS)
        return STATUS_INVALID_PARAMETER;

    Route = HalpPirReadRouterLong(VLSI_ROUTE_REGISTER);
    Code = (Route >> ((Link - 1) * VLSI_CODE_BITS)) & VLSI_CODE_MASK;

    *Irq = (Route & (1 << (VLSI_ENABLE_SHIFT + Code))) ? HalpVlsiCodeIrqs[Code] : 0;
    return STATUS_SUCCESS;
}

/* Unrouted links get a code no routed link uses, and that code stays disabled */
static
NTSTATUS
NTAPI
HalpVlsiSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    UCHAR Codes[VLSI_LINKS];
    UCHAR Code, Spare;
    ULONG Route, Index;

    if (Link == 0 || Link > VLSI_LINKS)
        return STATUS_INVALID_PARAMETER;

    Code = VLSI_NO_CODE;
    if (Irq != 0)
    {
        Code = HalpVlsiIrqToCode(Irq);
        if (Code == VLSI_NO_CODE)
            return STATUS_INVALID_PARAMETER;
    }

    Route = HalpPirReadRouterLong(VLSI_ROUTE_REGISTER);
    for (Index = 0; Index < VLSI_LINKS; Index++)
    {
        Codes[Index] = (Route >> (Index * VLSI_CODE_BITS)) & VLSI_CODE_MASK;
        if (!(Route & (1 << (VLSI_ENABLE_SHIFT + Codes[Index]))))
            Codes[Index] = VLSI_NO_CODE;
    }
    Codes[Link - 1] = Code;

    for (Spare = 0; Spare < VLSI_CODES; Spare++)
    {
        for (Index = 0; Index < VLSI_LINKS; Index++)
        {
            if (Codes[Index] == Spare)
                break;
        }

        if (Index == VLSI_LINKS)
            break;
    }

    Route = 0;
    for (Index = 0; Index < VLSI_LINKS; Index++)
    {
        if (Codes[Index] == VLSI_NO_CODE)
        {
            Route |= (ULONG)Spare << (Index * VLSI_CODE_BITS);
        }
        else
        {
            Route |= (ULONG)Codes[Index] << (Index * VLSI_CODE_BITS);
            Route |= 1 << (VLSI_ENABLE_SHIFT + Codes[Index]);
        }
    }

    HalpPirWriteRouterLong(VLSI_ROUTE_REGISTER, Route);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpVlsiGetTrigger(
    _Out_ PUSHORT LevelIrqs)
{
    UCHAR Level = HalpPirReadRouterByte(VLSI_TRIGGER_REGISTER);
    UCHAR Code;

    *LevelIrqs = 0;
    for (Code = 0; Code < VLSI_CODES; Code++)
    {
        if (Level & (1 << Code))
            *LevelIrqs |= 1 << HalpVlsiCodeIrqs[Code];
    }

    return STATUS_SUCCESS;
}

/* Fails when a level IRQ has no code */
static
NTSTATUS
NTAPI
HalpVlsiSetTrigger(
    _In_ USHORT LevelIrqs)
{
    ULONG Level, Mirror;
    UCHAR Code;

    Level = HalpPirReadRouterLong(VLSI_TRIGGER_REGISTER) & ~0xFFUL;
    for (Code = 0; Code < VLSI_CODES; Code++)
    {
        if (LevelIrqs & (1 << HalpVlsiCodeIrqs[Code]))
        {
            Level |= 1 << Code;
            LevelIrqs &= ~(1 << HalpVlsiCodeIrqs[Code]);
        }
    }

    if (LevelIrqs != 0)
        return STATUS_INVALID_PARAMETER;

    HalpPirWriteRouterLong(VLSI_TRIGGER_REGISTER, Level);

    Mirror = HalpPirReadRouterLong(VLSI_TRIGGER_MIRROR) & ~0xFFUL;
    HalpPirWriteRouterLong(VLSI_TRIGGER_MIRROR, Mirror | (Level & 0xFF));
    return STATUS_SUCCESS;
}

/* VLSI EAGLE *****************************************************************/

/*
 * Register 0x74 holds the IRQ of links 1-8, one nibble each. Every IRQ in use
 * also has its steering bit set in the word registers 0x70 and 0x72, and its
 * trigger bit in the word register 0x88.
 */
#define EAGLE_ROUTE_REGISTER    0x74
#define EAGLE_STEER_FIRST       0x70
#define EAGLE_STEER_LAST        0x72
#define EAGLE_TRIGGER_REGISTER  0x88
#define EAGLE_LINKS             8
#define EAGLE_LINK_BITS         4

/* Steering and trigger bit of each IRQ, IRQs 0, 2 and 13 have none */
static const UCHAR HalpEagleIrqBits[16] =
{
    VLSI_NO_CODE, 8, VLSI_NO_CODE, 9, 10, 11, 12, 0,
    1, 2, 3, 4, 5, VLSI_NO_CODE, 6, 7
};

static
VOID
HalpEagleSteer(
    _In_ UCHAR Irq,
    _In_ BOOLEAN Enable)
{
    ULONG Register;
    USHORT Steer;

    if (Irq == 0 || Irq >= RTL_NUMBER_OF(HalpEagleIrqBits) || HalpEagleIrqBits[Irq] == VLSI_NO_CODE)
        return;

    for (Register = EAGLE_STEER_FIRST; Register <= EAGLE_STEER_LAST; Register += sizeof(USHORT))
    {
        Steer = HalpPirReadRouterWord(Register);

        if (Enable)
            Steer |= 1 << HalpEagleIrqBits[Irq];
        else
            Steer &= ~(1 << HalpEagleIrqBits[Irq]);

        HalpPirWriteRouterWord(Register, Steer);
    }
}

static
NTSTATUS
NTAPI
HalpEagleValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    UCHAR Lowest, Highest;

    HalpPirLinkRange(Table, &Lowest, &Highest);

    return (Highest <= EAGLE_LINKS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpEagleGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    if (Link == 0 || Link > EAGLE_LINKS)
        return STATUS_INVALID_PARAMETER;

    *Irq = (HalpPirReadRouterLong(EAGLE_ROUTE_REGISTER) >> ((Link - 1) * EAGLE_LINK_BITS)) & 0x0F;
    return STATUS_SUCCESS;
}

/* The old IRQ keeps its steering bit while another link still uses it */
static
NTSTATUS
NTAPI
HalpEagleSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    ULONG Route, Shift, Index;
    UCHAR Previous;

    if (Link == 0 || Link > EAGLE_LINKS)
        return STATUS_INVALID_PARAMETER;

    Shift = (Link - 1) * EAGLE_LINK_BITS;
    Route = HalpPirReadRouterLong(EAGLE_ROUTE_REGISTER);
    Previous = (Route >> Shift) & 0x0F;

    Route = (Route & ~(0x0FUL << Shift)) | ((ULONG)(Irq & 0x0F) << Shift);
    HalpPirWriteRouterLong(EAGLE_ROUTE_REGISTER, Route);

    for (Index = 0; Index < EAGLE_LINKS; Index++)
    {
        if (((Route >> (Index * EAGLE_LINK_BITS)) & 0x0F) == Previous)
            break;
    }

    if (Index == EAGLE_LINKS)
        HalpEagleSteer(Previous, FALSE);

    HalpEagleSteer(Irq, TRUE);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpEagleGetTrigger(
    _Out_ PUSHORT LevelIrqs)
{
    USHORT Level = HalpPirReadRouterWord(EAGLE_TRIGGER_REGISTER);
    UCHAR Irq;

    *LevelIrqs = 0;
    for (Irq = 0; Irq < RTL_NUMBER_OF(HalpEagleIrqBits); Irq++)
    {
        if (HalpEagleIrqBits[Irq] != VLSI_NO_CODE && (Level & (1 << HalpEagleIrqBits[Irq])))
            *LevelIrqs |= 1 << Irq;
    }

    return STATUS_SUCCESS;
}

/* Fails when a level IRQ has no trigger bit */
static
NTSTATUS
NTAPI
HalpEagleSetTrigger(
    _In_ USHORT LevelIrqs)
{
    USHORT Level = 0;
    UCHAR Irq;

    for (Irq = 0; Irq < RTL_NUMBER_OF(HalpEagleIrqBits); Irq++)
    {
        if (!(LevelIrqs & (1 << Irq)))
            continue;

        if (HalpEagleIrqBits[Irq] == VLSI_NO_CODE)
            return STATUS_INVALID_PARAMETER;

        Level |= 1 << HalpEagleIrqBits[Irq];
    }

    HalpPirWriteRouterWord(EAGLE_TRIGGER_REGISTER, Level);
    return STATUS_SUCCESS;
}

HALP_IRQ_ROUTER HalpVlsiRouter =
{
    HalpVlsiValidateTable,
    HalpVlsiGetIrq,
    HalpVlsiSetIrq,
    HalpVlsiGetTrigger,
    HalpVlsiSetTrigger
};

HALP_IRQ_ROUTER HalpVlsiEagleRouter =
{
    HalpEagleValidateTable,
    HalpEagleGetIrq,
    HalpEagleSetIrq,
    HalpEagleGetTrigger,
    HalpEagleSetTrigger
};
