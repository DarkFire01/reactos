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
 * Links 1 to 4 each have a 3-bit code in register 0x40 and a 2-bit code in
 * register 0x50, and at most one of the two is nonzero. The level triggered
 * codes are flagged in bits 17 to 23 of 0x40 and bits 10 down to 8 of 0x50.
 */
#define VIPER_WIDE_REGISTER     0x40
#define VIPER_NARROW_REGISTER   0x50
#define VIPER_LINKS             4
#define VIPER_WIDE_BITS         3
#define VIPER_NARROW_BITS       2
#define VIPER_WIDE_TRIGGER      0x00FE0000
#define VIPER_NARROW_TRIGGER    0x00000700

/* IRQ of each nonzero code, code 1 first */
static const UCHAR HalpViperWideIrqs[] = { 5, 9, 10, 11, 12, 14, 15 };
static const UCHAR HalpViperNarrowIrqs[] = { 3, 4, 7 };

static
UCHAR
NTAPI
HalpViperCodeOf(
    _In_reads_(Count) const UCHAR *Irqs,
    _In_ ULONG Count,
    _In_ UCHAR Irq)
{
    ULONG Index;

    for (Index = 0; Index < Count; Index++)
    {
        if (Irqs[Index] == Irq)
            return (UCHAR)(Index + 1);
    }

    return 0;
}

static
ULONG
NTAPI
HalpViperWideLevelBit(
    _In_ ULONG Code)
{
    return 1UL << (16 + Code);
}

static
ULONG
NTAPI
HalpViperNarrowLevelBit(
    _In_ ULONG Code)
{
    return 1UL << (11 - Code);
}

static
UCHAR
NTAPI
HalpViperReadCode(
    _In_ ULONG Register,
    _In_ ULONG Width,
    _In_ UCHAR Link)
{
    ULONG Shift = (Link - 1) * Width;

    return (UCHAR)((HalpPirReadRouterLong(Register) >> Shift) & ((1UL << Width) - 1));
}

static
VOID
NTAPI
HalpViperWriteCode(
    _In_ ULONG Register,
    _In_ ULONG Width,
    _In_ UCHAR Link,
    _In_ UCHAR Code)
{
    ULONG Shift = (Link - 1) * Width;
    ULONG Field = ((1UL << Width) - 1) << Shift;
    ULONG Value;

    Value = HalpPirReadRouterLong(Register) & ~Field;
    HalpPirWriteRouterLong(Register, Value | ((ULONG)Code << Shift));
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

    Code = HalpViperReadCode(VIPER_WIDE_REGISTER, VIPER_WIDE_BITS, Link);
    if (Code != 0)
    {
        *Irq = HalpViperWideIrqs[Code - 1];
        return STATUS_SUCCESS;
    }

    Code = HalpViperReadCode(VIPER_NARROW_REGISTER, VIPER_NARROW_BITS, Link);
    *Irq = (Code != 0) ? HalpViperNarrowIrqs[Code - 1] : 0;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpViperSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    UCHAR WideCode, NarrowCode;

    if (Link == 0 || Link > VIPER_LINKS)
        return STATUS_INVALID_PARAMETER;

    WideCode = HalpViperCodeOf(HalpViperWideIrqs, RTL_NUMBER_OF(HalpViperWideIrqs), Irq);
    NarrowCode = HalpViperCodeOf(HalpViperNarrowIrqs, RTL_NUMBER_OF(HalpViperNarrowIrqs), Irq);

    if (Irq != 0 && WideCode == 0 && NarrowCode == 0)
        return STATUS_INVALID_PARAMETER;

    HalpViperWriteCode(VIPER_WIDE_REGISTER, VIPER_WIDE_BITS, Link, WideCode);
    HalpViperWriteCode(VIPER_NARROW_REGISTER, VIPER_NARROW_BITS, Link, NarrowCode);
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
    USHORT Level = 0;
    ULONG Code;

    for (Code = 1; Code <= RTL_NUMBER_OF(HalpViperWideIrqs); Code++)
    {
        if (Wide & HalpViperWideLevelBit(Code))
            Level |= 1 << HalpViperWideIrqs[Code - 1];
    }

    for (Code = 1; Code <= RTL_NUMBER_OF(HalpViperNarrowIrqs); Code++)
    {
        if (Narrow & HalpViperNarrowLevelBit(Code))
            Level |= 1 << HalpViperNarrowIrqs[Code - 1];
    }

    *LevelIrqs = Level;
    return STATUS_SUCCESS;
}

/* Fails when a level IRQ has no code */
static
NTSTATUS
NTAPI
HalpViperSetTrigger(
    _In_ USHORT LevelIrqs)
{
    ULONG Wide = HalpPirReadRouterLong(VIPER_WIDE_REGISTER) & ~(ULONG)VIPER_WIDE_TRIGGER;
    ULONG Narrow = HalpPirReadRouterLong(VIPER_NARROW_REGISTER) & ~(ULONG)VIPER_NARROW_TRIGGER;
    USHORT Covered = 0;
    ULONG Code;

    for (Code = 1; Code <= RTL_NUMBER_OF(HalpViperWideIrqs); Code++)
    {
        Covered |= 1 << HalpViperWideIrqs[Code - 1];

        if (LevelIrqs & (1 << HalpViperWideIrqs[Code - 1]))
            Wide |= HalpViperWideLevelBit(Code);
    }

    for (Code = 1; Code <= RTL_NUMBER_OF(HalpViperNarrowIrqs); Code++)
    {
        Covered |= 1 << HalpViperNarrowIrqs[Code - 1];

        if (LevelIrqs & (1 << HalpViperNarrowIrqs[Code - 1]))
            Narrow |= HalpViperNarrowLevelBit(Code);
    }

    if (LevelIrqs & ~Covered)
        return STATUS_INVALID_PARAMETER;

    HalpPirWriteRouterLong(VIPER_WIDE_REGISTER, Wide);
    HalpPirWriteRouterLong(VIPER_NARROW_REGISTER, Narrow);
    return STATUS_SUCCESS;
}

/* FIRESTAR *******************************************************************/

/*
 * The low three bits of a link give its kind. Kind 1 links use register 0xB0
 * plus bits 4 to 6, with the IRQ in the low nibble and bit 4 set when the IRQ
 * is level triggered. Kind 2 and 3 links are PCI only lines in register 0xB8
 * plus bit 5, with bit 4 selecting the high nibble.
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
#define FIRESTAR_PCI_UNUSED     0x40
#define FIRESTAR_INDEX_BITS     0x70
#define FIRESTAR_IRQ_MASK       0x0F

static
_Success_(return != FALSE)
BOOLEAN
NTAPI
HalpFireStarLocateLink(
    _In_ UCHAR Link,
    _Out_ PUCHAR Register,
    _Out_ PBOOLEAN IsaLine,
    _Out_ PBOOLEAN High)
{
    UCHAR Kind = Link & FIRESTAR_KIND_MASK;

    if (Kind == FIRESTAR_KIND_ISA)
    {
        *Register = (UCHAR)(FIRESTAR_ISA_REGISTER + ((Link & FIRESTAR_INDEX_BITS) >> 4));
        *IsaLine = TRUE;
        *High = FALSE;
        return TRUE;
    }

    if (Kind >= FIRESTAR_KIND_PCI_FIRST && Kind <= FIRESTAR_KIND_PCI_LAST)
    {
        *Register = (UCHAR)(FIRESTAR_PCI_REGISTER + ((Link & FIRESTAR_PCI_SECOND) ? 1 : 0));
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
        return !(Link & FIRESTAR_PCI_UNUSED);

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

/* The PCI only lines are always level triggered */
static
NTSTATUS
NTAPI
HalpFireStarGetTrigger(
    _Out_ PUSHORT LevelIrqs)
{
    UCHAR Index, Value, Irq;

    *LevelIrqs = 0;

    for (Index = 0; Index < FIRESTAR_ISA_COUNT; Index++)
    {
        Value = HalpPirReadRouterByte(FIRESTAR_ISA_REGISTER + Index);
        Irq = Value & FIRESTAR_IRQ_MASK;

        if ((Value & FIRESTAR_ISA_LEVEL) && Irq != 0)
            *LevelIrqs |= 1 << Irq;
    }

    for (Index = 0; Index < FIRESTAR_PCI_COUNT; Index++)
    {
        Value = HalpPirReadRouterByte(FIRESTAR_PCI_REGISTER + Index);

        Irq = HalpPirGetNibble(Value, FALSE);
        if (Irq != 0)
            *LevelIrqs |= 1 << Irq;

        Irq = HalpPirGetNibble(Value, TRUE);
        if (Irq != 0)
            *LevelIrqs |= 1 << Irq;
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
        Irq = Value & FIRESTAR_IRQ_MASK;

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
