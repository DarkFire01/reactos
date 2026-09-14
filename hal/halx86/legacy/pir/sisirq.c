/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SiS PCI IRQ router miniport
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include "irqrouter.h"

#define NDEBUG
#include <debug.h>

/* The SiS 5503 links are its route registers, starting at 0x41 */
#define SIS_LOWEST_ROUTE_REGISTER   0x40
#define SIS_ROUTE_DISABLED          0x80
#define SIS_ROUTE_KEEP              0x70
#define SIS_ROUTE_IRQ               0x0F

static
NTSTATUS
NTAPI
HalpSisValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    UCHAR Lowest, Highest;

    HalpPirLinkRange(Table, &Lowest, &Highest);

    return (Lowest >= SIS_LOWEST_ROUTE_REGISTER) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpSisGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    UCHAR Route;

    if (Link < SIS_LOWEST_ROUTE_REGISTER)
        return STATUS_INVALID_PARAMETER;

    Route = HalpPirReadRouterByte(Link);

    *Irq = (Route & SIS_ROUTE_DISABLED) ? 0 : (UCHAR)(Route & SIS_ROUTE_IRQ);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpSisSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    UCHAR Route;

    if (Link < SIS_LOWEST_ROUTE_REGISTER)
        return STATUS_INVALID_PARAMETER;

    Route = HalpPirReadRouterByte(Link) & SIS_ROUTE_KEEP;
    Route |= (Irq != 0) ? (Irq & SIS_ROUTE_IRQ) : SIS_ROUTE_DISABLED;

    HalpPirWriteRouterByte(Link, Route);
    return STATUS_SUCCESS;
}

HALP_IRQ_ROUTER HalpSisRouter =
{
    HalpSisValidateTable,
    HalpSisGetIrq,
    HalpSisSetIrq,
    HalpElcrGetTrigger,
    HalpElcrSetTrigger
};
