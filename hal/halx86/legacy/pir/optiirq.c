/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     OPTi PCI IRQ router miniports
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include "irqrouter.h"

#define NDEBUG
#include <debug.h>

/* VIPER **********************************************************************/

/*
 * Links 1-4 each have a 3-bit field in register 0x40 for IRQs 5, 9-12, 14 and
 * 15, and a 2-bit field in register 0x50 for IRQs 3, 4 and 7. Only one of the
 * two is nonzero. Bits 17-23 of 0x40 and bits 8-10 of 0x50 mark the codes
 * that are level triggered.
 */
#define VIPER_WIDE_REGISTER     0x40
#define VIPER_NARROW_REGISTER   0x50
#define VIPER_LINKS             4
#define VIPER_WIDE_BITS         3
#define VIPER_NARROW_BITS       2
#define VIPER_WIDE_TRIGGER      0x00FE0000
#define VIPER_NARROW_TRIGGER    0x00000700

static const UCHAR HalpViperWideCodes[16] =
{
    0, 0, 0, 0, 0, 1, 0, 0, 0, 2, 3, 4, 5, 0, 6, 7
};

static const UCHAR HalpViperNarrowCodes[16] =
{
    0, 0, 0, 1, 2, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0
};

static
UCHAR
HalpViperCodeToIrq(
    _In_ const UCHAR *Codes,
    _In_ UCHAR Code)
{
    UCHAR Irq;

    for (Irq = 0; Code != 0 && Irq < 16; Irq++)
    {
        if (Codes[Irq] == Code)
            return Irq;
    }

    return 0;
}

static
ULONG
HalpViperWideTriggerBit(
    _In_ UCHAR Code)
{
    return 1UL << (16 + Code);
}

static
ULONG
HalpViperNarrowTriggerBit(
    _In_ UCHAR Code)
{
    return 1UL << (11 - Code);
}

static
NTSTATUS
NTAPI
HalpViperValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    UCHAR Lowest, Highest;

    HalpPirLinkRange(Table, &Lowest, &Highest);

    return (Highest <= VIPER_LINKS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpViperGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    UCHAR Code;

    if (Link == 0 || Link > VIPER_LINKS)
        return STATUS_INVALID_PARAMETER;

    Code = (HalpPirReadRouterLong(VIPER_WIDE_REGISTER) >> ((Link - 1) * VIPER_WIDE_BITS)) & 0x07;
    *Irq = HalpViperCodeToIrq(HalpViperWideCodes, Code);

    if (*Irq == 0)
    {
        Code = (HalpPirReadRouterLong(VIPER_NARROW_REGISTER) >> ((Link - 1) * VIPER_NARROW_BITS)) & 0x03;
        *Irq = HalpViperCodeToIrq(HalpViperNarrowCodes, Code);
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpViperSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    ULONG Value, Shift;

    Irq &= 0x0F;
    if (Irq != 0 && HalpViperWideCodes[Irq] == 0 && HalpViperNarrowCodes[Irq] == 0)
        return STATUS_INVALID_PARAMETER;

    if (Link == 0 || Link > VIPER_LINKS)
        return STATUS_INVALID_PARAMETER;

    Shift = (Link - 1) * VIPER_WIDE_BITS;
    Value = HalpPirReadRouterLong(VIPER_WIDE_REGISTER) & ~(0x07UL << Shift);
    HalpPirWriteRouterLong(VIPER_WIDE_REGISTER, Value | ((ULONG)HalpViperWideCodes[Irq] << Shift));

    Shift = (Link - 1) * VIPER_NARROW_BITS;
    Value = HalpPirReadRouterLong(VIPER_NARROW_REGISTER) & ~(0x03UL << Shift);
    HalpPirWriteRouterLong(VIPER_NARROW_REGISTER, Value | ((ULONG)HalpViperNarrowCodes[Irq] << Shift));

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpViperGetTrigger(
    _Out_ PUSHORT LevelIrqs)
{
    ULONG Wide = HalpPirReadRouterLong(VIPER_WIDE_REGISTER);
    ULONG Narrow = HalpPirReadRouterLong(VIPER_NARROW_REGISTER);
    UCHAR Irq;

    *LevelIrqs = 0;
    for (Irq = 0; Irq < 16; Irq++)
    {
        if ((HalpViperWideCodes[Irq] && (Wide & HalpViperWideTriggerBit(HalpViperWideCodes[Irq]))) ||
            (HalpViperNarrowCodes[Irq] && (Narrow & HalpViperNarrowTriggerBit(HalpViperNarrowCodes[Irq]))))
        {
            *LevelIrqs |= 1 << Irq;
        }
    }

    return STATUS_SUCCESS;
}

/* Fails when a level IRQ has no code */
static
NTSTATUS
NTAPI
HalpViperSetTrigger(
    _In_ USHORT LevelIrqs)
{
    ULONG Wide = HalpPirReadRouterLong(VIPER_WIDE_REGISTER) & ~VIPER_WIDE_TRIGGER;
    ULONG Narrow = HalpPirReadRouterLong(VIPER_NARROW_REGISTER) & ~VIPER_NARROW_TRIGGER;
    UCHAR Irq;

    for (Irq = 0; Irq < 16; Irq++)
    {
        if (!(LevelIrqs & (1 << Irq)))
            continue;

        if (HalpViperWideCodes[Irq])
            Wide |= HalpViperWideTriggerBit(HalpViperWideCodes[Irq]);
        else if (HalpViperNarrowCodes[Irq])
            Narrow |= HalpViperNarrowTriggerBit(HalpViperNarrowCodes[Irq]);
        else
            return STATUS_INVALID_PARAMETER;
    }

    HalpPirWriteRouterLong(VIPER_WIDE_REGISTER, Wide);
    HalpPirWriteRouterLong(VIPER_NARROW_REGISTER, Narrow);
    return STATUS_SUCCESS;
}

/* FIRESTAR *******************************************************************/

/*
 * The low three bits of a link give its kind. Kind 1 links use register
 * 0xB0 plus bits 4-6, with the IRQ in the low nibble and bit 4 set when the
 * IRQ is level triggered. Kind 2 and 3 links are PCI-only lines in register
 * 0xB8 plus bit 5, with bit 4 selecting the high nibble.
 */
#define FIRESTAR_KIND_MASK      0x07
#define FIRESTAR_KIND_ISA       1
#define FIRESTAR_KIND_PCI_FIRST 2
#define FIRESTAR_KIND_PCI_LAST  3
#define FIRESTAR_ISA_REGISTER   0xB0
#define FIRESTAR_ISA_COUNT      8
#define FIRESTAR_ISA_LEVEL      0x10
#define FIRESTAR_PCI_REGISTER   0xB8
#define FIRESTAR_PCI_COUNT      2
#define FIRESTAR_PCI_HIGH       0x10
#define FIRESTAR_PCI_SECOND     0x20
#define FIRESTAR_INDEX_BITS     0x70

static
BOOLEAN
HalpFireStarLocateLink(
    _In_ UCHAR Link,
    _Out_ PUCHAR Register,
    _Out_ PBOOLEAN IsaLine,
    _Out_ PBOOLEAN High)
{
    UCHAR Kind = Link & FIRESTAR_KIND_MASK;

    if (Kind == FIRESTAR_KIND_ISA)
    {
        *Register = FIRESTAR_ISA_REGISTER + ((Link & FIRESTAR_INDEX_BITS) >> 4);
        *IsaLine = TRUE;
        *High = FALSE;
        return TRUE;
    }

    if (Kind >= FIRESTAR_KIND_PCI_FIRST && Kind <= FIRESTAR_KIND_PCI_LAST)
    {
        *Register = FIRESTAR_PCI_REGISTER + ((Link & FIRESTAR_PCI_SECOND) ? 1 : 0);
        *IsaLine = FALSE;
        *High = !!(Link & FIRESTAR_PCI_HIGH);
        return TRUE;
    }

    return FALSE;
}

static
BOOLEAN
NTAPI
HalpFireStarIsTableLink(
    _In_ UCHAR Link)
{
    UCHAR Kind = Link & FIRESTAR_KIND_MASK;

    if (Kind == 0)
        return !(Link & FIRESTAR_INDEX_BITS);

    if (Kind == FIRESTAR_KIND_ISA)
        return TRUE;

    if (Kind <= FIRESTAR_KIND_PCI_LAST)
        return !(Link & 0x40);

    return FALSE;
}

static
NTSTATUS
NTAPI
HalpFireStarValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    return HalpPirAllLinks(Table, HalpFireStarIsTableLink) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpFireStarGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    BOOLEAN IsaLine, High;
    UCHAR Register;

    if (!HalpFireStarLocateLink(Link, &Register, &IsaLine, &High))
        return STATUS_INVALID_PARAMETER;

    *Irq = HalpPirGetNibble(HalpPirReadRouterByte(Register), High);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpFireStarSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    BOOLEAN IsaLine, High;
    UCHAR Register, Value;

    if (!HalpFireStarLocateLink(Link, &Register, &IsaLine, &High))
        return STATUS_INVALID_PARAMETER;

    Value = HalpPirSetNibble(HalpPirReadRouterByte(Register), High, Irq);

    /* Routing an IRQ to the line marks it level triggered */
    if (IsaLine && Irq != 0)
        Value |= FIRESTAR_ISA_LEVEL;

    HalpPirWriteRouterByte(Register, Value);
    return STATUS_SUCCESS;
}

/* PCI-only lines are always level triggered */
static
NTSTATUS
NTAPI
HalpFireStarGetTrigger(
    _Out_ PUSHORT LevelIrqs)
{
    UCHAR Index, Value;

    *LevelIrqs = 0;

    for (Index = 0; Index < FIRESTAR_ISA_COUNT; Index++)
    {
        Value = HalpPirReadRouterByte(FIRESTAR_ISA_REGISTER + Index);
        if ((Value & FIRESTAR_ISA_LEVEL) && (Value & 0x0F))
            *LevelIrqs |= 1 << (Value & 0x0F);
    }

    for (Index = 0; Index < FIRESTAR_PCI_COUNT; Index++)
    {
        Value = HalpPirReadRouterByte(FIRESTAR_PCI_REGISTER + Index);

        if (HalpPirGetNibble(Value, FALSE))
            *LevelIrqs |= 1 << HalpPirGetNibble(Value, FALSE);

        if (HalpPirGetNibble(Value, TRUE))
            *LevelIrqs |= 1 << HalpPirGetNibble(Value, TRUE);
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpFireStarSetTrigger(
    _In_ USHORT LevelIrqs)
{
    UCHAR Index, Value, Irq;

    for (Index = 0; Index < FIRESTAR_ISA_COUNT; Index++)
    {
        Value = HalpPirReadRouterByte(FIRESTAR_ISA_REGISTER + Index);
        Irq = Value & 0x0F;

        if (Irq != 0 && (LevelIrqs & (1 << Irq)))
            Value |= FIRESTAR_ISA_LEVEL;
        else
            Value &= ~FIRESTAR_ISA_LEVEL;

        HalpPirWriteRouterByte(FIRESTAR_ISA_REGISTER + Index, Value);
    }

    return STATUS_SUCCESS;
}

HALP_IRQ_ROUTER HalpOptiViperRouter =
{
    HalpViperValidateTable,
    HalpViperGetIrq,
    HalpViperSetIrq,
    HalpViperGetTrigger,
    HalpViperSetTrigger
};

HALP_IRQ_ROUTER HalpOptiFireStarRouter =
{
    HalpFireStarValidateTable,
    HalpFireStarGetIrq,
    HalpFireStarSetIrq,
    HalpFireStarGetTrigger,
    HalpFireStarSetTrigger
};
