/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PicoPower Vesuvius PCI IRQ router miniport
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include "irqrouter.h"

#define NDEBUG
#include <debug.h>

/*
 * The route registers sit behind the 0x24/0x26 index and data pair. Links 1
 * to 4 are the nibbles of indexes 0x10 and 0x11, and bits 0 to 3 of index
 * 0x12 are cleared for the links whose IRQ is level triggered.
 */
#define VESUVIUS_INDEX_PORT     0x24
#define VESUVIUS_DATA_PORT      0x26
#define VESUVIUS_INT_INDEX      0x10
#define VESUVIUS_EDGE_INDEX     0x12
#define VESUVIUS_LINKS          4

static
UCHAR
HalpVesuviusRead(
    _In_ UCHAR Index)
{
    UCHAR Saved = __inbyte(VESUVIUS_INDEX_PORT);
    UCHAR Value;

    __outbyte(VESUVIUS_INDEX_PORT, Index);
    Value = __inbyte(VESUVIUS_DATA_PORT);
    __outbyte(VESUVIUS_INDEX_PORT, Saved);

    return Value;
}

static
VOID
HalpVesuviusWrite(
    _In_ UCHAR Index,
    _In_ UCHAR Value)
{
    UCHAR Saved = __inbyte(VESUVIUS_INDEX_PORT);

    __outbyte(VESUVIUS_INDEX_PORT, Index);
    __outbyte(VESUVIUS_DATA_PORT, Value);
    __outbyte(VESUVIUS_INDEX_PORT, Saved);
}

static
UCHAR
HalpVesuviusLinkIrq(
    _In_ UCHAR Link)
{
    UCHAR Index = HalpPirPairRegister(VESUVIUS_INT_INDEX, Link);

    return HalpPirGetNibble(HalpVesuviusRead(Index), HalpPirPairHighNibble(Link));
}

static
NTSTATUS
NTAPI
HalpVesuviusValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    UCHAR Lowest, Highest;

    HalpPirLinkRange(Table, &Lowest, &Highest);

    return (Highest <= VESUVIUS_LINKS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpVesuviusGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    if (Link == 0 || Link > VESUVIUS_LINKS)
        return STATUS_INVALID_PARAMETER;

    *Irq = HalpVesuviusLinkIrq(Link);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpVesuviusSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    UCHAR Index, Value;

    if (Link == 0 || Link > VESUVIUS_LINKS)
        return STATUS_INVALID_PARAMETER;

    Index = HalpPirPairRegister(VESUVIUS_INT_INDEX, Link);
    Value = HalpPirSetNibble(HalpVesuviusRead(Index), HalpPirPairHighNibble(Link), Irq);

    HalpVesuviusWrite(Index, Value);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpVesuviusGetTrigger(
    _Out_ PUSHORT LevelIrqs)
{
    UCHAR Edge = HalpVesuviusRead(VESUVIUS_EDGE_INDEX);
    UCHAR Link, Irq;

    *LevelIrqs = 0;
    for (Link = 1; Link <= VESUVIUS_LINKS; Link++)
    {
        if (Edge & (1 << (Link - 1)))
            continue;

        Irq = HalpVesuviusLinkIrq(Link);
        if (Irq != 0)
            *LevelIrqs |= 1 << Irq;
    }

    return STATUS_SUCCESS;
}

/* Only the links can be level triggered, so every level IRQ has to belong to one */
static
NTSTATUS
NTAPI
HalpVesuviusSetTrigger(
    _In_ USHORT LevelIrqs)
{
    UCHAR Edge = HalpVesuviusRead(VESUVIUS_EDGE_INDEX);
    USHORT Remaining = LevelIrqs;
    UCHAR Link, Irq;

    for (Link = 1; Link <= VESUVIUS_LINKS; Link++)
    {
        Irq = HalpVesuviusLinkIrq(Link);

        if (Irq != 0 && (LevelIrqs & (1 << Irq)))
        {
            Edge &= ~(1 << (Link - 1));
            Remaining &= ~(1 << Irq);
        }
        else
        {
            Edge |= 1 << (Link - 1);
        }
    }

    if (Remaining != 0)
        return STATUS_UNSUCCESSFUL;

    HalpVesuviusWrite(VESUVIUS_EDGE_INDEX, Edge);
    return STATUS_SUCCESS;
}

HALP_IRQ_ROUTER HalpVesuviusRouter =
{
    HalpVesuviusValidateTable,
    HalpVesuviusGetIrq,
    HalpVesuviusSetIrq,
    HalpVesuviusGetTrigger,
    HalpVesuviusSetTrigger
};
