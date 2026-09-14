/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     National Semiconductor PCI IRQ router miniport
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include "irqrouter.h"

#define NDEBUG
#include <debug.h>

/* The NS87560 steers INTA-INTD through nibbles at 0x6C and 0x6D */
#define NS87560_INT_REGISTER        0x6C
#define NS87560_INT_LINKS           4

/* Its own copy of the edge/level register, IRQs 0-7 at 0x67 and 8-15 at 0x68 */
#define NS87560_TRIGGER_LOW         0x67
#define NS87560_TRIGGER_HIGH        0x68

static
NTSTATUS
NTAPI
HalpNs87560ValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    UCHAR Lowest, Highest;

    HalpPirLinkRange(Table, &Lowest, &Highest);

    return (Highest <= NS87560_INT_LINKS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpNs87560GetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    UCHAR Register;

    if (Link == 0 || Link > NS87560_INT_LINKS)
        return STATUS_INVALID_PARAMETER;

    Register = HalpPirPairRegister(NS87560_INT_REGISTER, Link);
    *Irq = HalpPirGetNibble(HalpPirReadRouterByte(Register), HalpPirPairHighNibble(Link));
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpNs87560SetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    UCHAR Register;

    if (Link == 0 || Link > NS87560_INT_LINKS)
        return STATUS_INVALID_PARAMETER;

    Register = HalpPirPairRegister(NS87560_INT_REGISTER, Link);
    HalpPirWriteRouterByte(Register,
                           HalpPirSetNibble(HalpPirReadRouterByte(Register),
                                            HalpPirPairHighNibble(Link),
                                            Irq));
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpNs87560GetTrigger(
    _Out_ PUSHORT LevelIrqs)
{
    *LevelIrqs = HalpPirReadRouterByte(NS87560_TRIGGER_LOW) |
                 (USHORT)(HalpPirReadRouterByte(NS87560_TRIGGER_HIGH) << 8);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpNs87560SetTrigger(
    _In_ USHORT LevelIrqs)
{
    HalpPirWriteRouterByte(NS87560_TRIGGER_LOW, (UCHAR)LevelIrqs);
    HalpPirWriteRouterByte(NS87560_TRIGGER_HIGH, (UCHAR)(LevelIrqs >> 8));
    return STATUS_SUCCESS;
}

HALP_IRQ_ROUTER HalpNs87560Router =
{
    HalpNs87560ValidateTable,
    HalpNs87560GetIrq,
    HalpNs87560SetIrq,
    HalpNs87560GetTrigger,
    HalpNs87560SetTrigger
};
