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

/* VLSI ***********************************************************************/

/*
 * Register 0x74 holds a 3-bit code for each of the links 1 to 4, one nibble
 * per link, and bits 16 to 23 enable the eight codes. The low byte of 0x5C
 * flags the codes that are level triggered, and 0x5C is copied into 0x78.
 */
#define VLSI_ROUTE_REGISTER     0x74
#define VLSI_TRIGGER_REGISTER   0x5C
#define VLSI_TRIGGER_COPY       0x78
#define VLSI_LINKS              4
#define VLSI_LINK_BITS          4
#define VLSI_CODE_MASK          0x07
#define VLSI_ENABLE_SHIFT       16
#define VLSI_LEVEL_CODES        0xFF

/* IRQ of each code */
static const UCHAR HalpVlsiCodeIrqs[] = { 3, 5, 9, 10, 11, 12, 14, 15 };

static
_Success_(return != FALSE)
BOOLEAN
NTAPI
HalpVlsiCodeOf(
    _In_ UCHAR Irq,
    _Out_ PUCHAR Code)
{
    UCHAR Index;

    for (Index = 0; Index < RTL_NUMBER_OF(HalpVlsiCodeIrqs); Index++)
    {
        if (HalpVlsiCodeIrqs[Index] == Irq)
        {
            *Code = Index;
            return TRUE;
        }
    }

    return FALSE;
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
    Code = (UCHAR)((Route >> ((Link - 1) * VLSI_LINK_BITS)) & VLSI_CODE_MASK);

    *Irq = (Route & (1UL << (VLSI_ENABLE_SHIFT + Code))) ? HalpVlsiCodeIrqs[Code] : 0;
    return STATUS_SUCCESS;
}

/*
 * The register is rebuilt from the routed links. Unrouted links are parked on
 * the lowest code no routed link uses, and that code stays disabled.
 */
static
NTSTATUS
NTAPI
HalpVlsiSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    UCHAR Codes[VLSI_LINKS] = { 0 };
    UCHAR Routed = 0, InUse = 0;
    ULONG Route, LinkIndex, Spare;

    if (Link == 0 || Link > VLSI_LINKS)
        return STATUS_INVALID_PARAMETER;

    if (Irq != 0 && !HalpVlsiCodeOf(Irq, &Codes[Link - 1]))
        return STATUS_INVALID_PARAMETER;

    Route = HalpPirReadRouterLong(VLSI_ROUTE_REGISTER);

    for (LinkIndex = 0; LinkIndex < VLSI_LINKS; LinkIndex++)
    {
        if (LinkIndex != (ULONG)(Link - 1))
        {
            Codes[LinkIndex] = (UCHAR)((Route >> (LinkIndex * VLSI_LINK_BITS)) & VLSI_CODE_MASK);
            if (!(Route & (1UL << (VLSI_ENABLE_SHIFT + Codes[LinkIndex]))))
                continue;
        }
        else if (Irq == 0)
        {
            continue;
        }

        Routed |= 1 << LinkIndex;
        InUse |= 1 << Codes[LinkIndex];
    }

    /* At most four of the eight codes are in use */
    BitScanForward(&Spare, (UCHAR)~InUse);

    Route = (ULONG)InUse << VLSI_ENABLE_SHIFT;
    for (LinkIndex = 0; LinkIndex < VLSI_LINKS; LinkIndex++)
    {
        if (Routed & (1 << LinkIndex))
            Route |= (ULONG)Codes[LinkIndex] << (LinkIndex * VLSI_LINK_BITS);
        else
            Route |= Spare << (LinkIndex * VLSI_LINK_BITS);
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
    UCHAR LevelCodes = HalpPirReadRouterByte(VLSI_TRIGGER_REGISTER);
    USHORT Level = 0;
    UCHAR Code;

    for (Code = 0; Code < RTL_NUMBER_OF(HalpVlsiCodeIrqs); Code++)
    {
        if (LevelCodes & (1 << Code))
            Level |= 1 << HalpVlsiCodeIrqs[Code];
    }

    *LevelIrqs = Level;
    return STATUS_SUCCESS;
}

/* Fails when a level IRQ has no code */
static
NTSTATUS
NTAPI
HalpVlsiSetTrigger(
    _In_ USHORT LevelIrqs)
{
    USHORT Covered = 0;
    ULONG LevelCodes = 0, Value;
    UCHAR Code;

    for (Code = 0; Code < RTL_NUMBER_OF(HalpVlsiCodeIrqs); Code++)
    {
        Covered |= 1 << HalpVlsiCodeIrqs[Code];

        if (LevelIrqs & (1 << HalpVlsiCodeIrqs[Code]))
            LevelCodes |= 1UL << Code;
    }

    if (LevelIrqs & ~Covered)
        return STATUS_INVALID_PARAMETER;

    Value = (HalpPirReadRouterLong(VLSI_TRIGGER_REGISTER) & ~(ULONG)VLSI_LEVEL_CODES) | LevelCodes;

    HalpPirWriteRouterLong(VLSI_TRIGGER_REGISTER, Value);
    HalpPirWriteRouterLong(VLSI_TRIGGER_COPY, Value);
    return STATUS_SUCCESS;
}

/* VLSI EAGLE *****************************************************************/

/*
 * Register 0x74 holds the IRQ of the links 1 to 8, one nibble each. Every IRQ
 * in use also has its steering bit set in the word registers 0x70 and 0x72,
 * and the level triggered IRQs have the same bit set in the word register 0x88.
 */
#define EAGLE_ROUTE_REGISTER    0x74
#define EAGLE_STEER_FIRST       0x70
#define EAGLE_STEER_LAST        0x72
#define EAGLE_TRIGGER_REGISTER  0x88
#define EAGLE_LINKS             8
#define EAGLE_LINK_BITS         4
#define EAGLE_IRQ_MASK          0x0F
#define EAGLE_HIGHEST_IRQ       15

/* IRQ of each steering and trigger bit, IRQs 0, 2 and 13 have none */
static const UCHAR HalpEagleBitIrqs[] = { 7, 8, 9, 10, 11, 12, 14, 15, 1, 3, 4, 5, 6 };

static
USHORT
NTAPI
HalpEagleIrqBit(
    _In_ UCHAR Irq)
{
    ULONG Bit;

    for (Bit = 0; Bit < RTL_NUMBER_OF(HalpEagleBitIrqs); Bit++)
    {
        if (HalpEagleBitIrqs[Bit] == Irq)
            return (USHORT)(1 << Bit);
    }

    return 0;
}

static
VOID
NTAPI
HalpEagleSteer(
    _In_ UCHAR Irq,
    _In_ BOOLEAN Enable)
{
    USHORT Bit = HalpEagleIrqBit(Irq);
    USHORT Steer;
    ULONG Register;

    if (Bit == 0)
        return;

    for (Register = EAGLE_STEER_FIRST; Register <= EAGLE_STEER_LAST; Register += sizeof(Steer))
    {
        Steer = HalpPirReadRouterWord(Register);
        Steer = Enable ? (Steer | Bit) : (Steer & ~Bit);
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

    *Irq = (UCHAR)((HalpPirReadRouterLong(EAGLE_ROUTE_REGISTER) >>
                    ((Link - 1) * EAGLE_LINK_BITS)) & EAGLE_IRQ_MASK);
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
    ULONG Route, Shift, LinkIndex;
    UCHAR Previous;
    BOOLEAN Shared = FALSE;

    if (Link == 0 || Link > EAGLE_LINKS || Irq > EAGLE_HIGHEST_IRQ)
        return STATUS_INVALID_PARAMETER;

    Shift = (Link - 1) * EAGLE_LINK_BITS;
    Route = HalpPirReadRouterLong(EAGLE_ROUTE_REGISTER);
    Previous = (UCHAR)((Route >> Shift) & EAGLE_IRQ_MASK);

    Route = (Route & ~((ULONG)EAGLE_IRQ_MASK << Shift)) | ((ULONG)Irq << Shift);
    HalpPirWriteRouterLong(EAGLE_ROUTE_REGISTER, Route);

    for (LinkIndex = 0; LinkIndex < EAGLE_LINKS; LinkIndex++)
    {
        if (((Route >> (LinkIndex * EAGLE_LINK_BITS)) & EAGLE_IRQ_MASK) == Previous)
            Shared = TRUE;
    }

    if (!Shared)
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
    USHORT LevelBits = HalpPirReadRouterWord(EAGLE_TRIGGER_REGISTER);
    USHORT Level = 0;
    ULONG Bit;

    for (Bit = 0; Bit < RTL_NUMBER_OF(HalpEagleBitIrqs); Bit++)
    {
        if (LevelBits & (1 << Bit))
            Level |= 1 << HalpEagleBitIrqs[Bit];
    }

    *LevelIrqs = Level;
    return STATUS_SUCCESS;
}

/* Fails when a level IRQ has no trigger bit */
static
NTSTATUS
NTAPI
HalpEagleSetTrigger(
    _In_ USHORT LevelIrqs)
{
    USHORT LevelBits = 0, Covered = 0;
    ULONG Bit;

    for (Bit = 0; Bit < RTL_NUMBER_OF(HalpEagleBitIrqs); Bit++)
    {
        Covered |= 1 << HalpEagleBitIrqs[Bit];

        if (LevelIrqs & (1 << HalpEagleBitIrqs[Bit]))
            LevelBits |= 1 << Bit;
    }

    if (LevelIrqs & ~Covered)
        return STATUS_INVALID_PARAMETER;

    HalpPirWriteRouterWord(EAGLE_TRIGGER_REGISTER, LevelBits);
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
