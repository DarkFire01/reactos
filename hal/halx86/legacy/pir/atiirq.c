/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ATI and AMD south bridge PCI IRQ router miniport
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include "irqrouter.h"

#define NDEBUG
#include <debug.h>

/* The SB600 and the SB7x0 steer their lines through one index and data port */
#define ATI_ROUTE_INDEX_PORT    0x0C00
#define ATI_ROUTE_DATA_PORT     0x0C01

/* Indexes of the PCI lines, the ones between them route other sources */
#define ATI_INTA_INDEX          0x00
#define ATI_INTD_INDEX          0x03
#define ATI_INTE_INDEX          0x09
#define ATI_INTH_INDEX          0x0C

/* The data port carries the line itself, so anything above the PIC is unrouted */
#define ATI_HIGHEST_LINE        15

/* A link value is the route index plus one */
static
BOOLEAN
NTAPI
HalpAtiLocateLink(
    _In_ UCHAR Link,
    _Out_ PUCHAR Index)
{
    UCHAR Selected;

    if (Link == 0)
        return FALSE;

    Selected = (UCHAR)(Link - 1);
    if ((Selected > ATI_INTD_INDEX) &&
        ((Selected < ATI_INTE_INDEX) || (Selected > ATI_INTH_INDEX)))
    {
        return FALSE;
    }

    *Index = Selected;
    return TRUE;
}

static
BOOLEAN
NTAPI
HalpAtiIsLink(
    _In_ UCHAR Link)
{
    UCHAR Index;

    return HalpAtiLocateLink(Link, &Index);
}

static
NTSTATUS
NTAPI
HalpAtiValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    return HalpPirAllLinks(Table, HalpAtiIsLink) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpAtiGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    UCHAR Index, Line;

    if (!HalpAtiLocateLink(Link, &Index))
        return STATUS_INVALID_PARAMETER;

    __outbyte(ATI_ROUTE_INDEX_PORT, Index);
    Line = __inbyte(ATI_ROUTE_DATA_PORT);

    *Irq = (Line <= ATI_HIGHEST_LINE) ? Line : 0;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpAtiSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    UCHAR Index;

    if (!HalpAtiLocateLink(Link, &Index) || (Irq > ATI_HIGHEST_LINE))
        return STATUS_INVALID_PARAMETER;

    __outbyte(ATI_ROUTE_INDEX_PORT, Index);
    __outbyte(ATI_ROUTE_DATA_PORT, Irq);
    return STATUS_SUCCESS;
}

HALP_IRQ_ROUTER HalpAtiRouter =
{
    HalpAtiValidateTable,
    HalpAtiGetIrq,
    HalpAtiSetIrq,
    HalpElcrGetTrigger,
    HalpElcrSetTrigger
};
