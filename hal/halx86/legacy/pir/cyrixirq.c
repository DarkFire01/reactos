/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Cyrix PCI IRQ router miniport
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include "irqrouter.h"

#define NDEBUG
#include <debug.h>

/* The Cx5520 steers INTA-INTD through nibbles at 0x5C and 0x5D */
#define CX5520_INT_REGISTER     0x5C
#define CX5520_INT_LINKS        4

static
NTSTATUS
NTAPI
HalpCx5520ValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    UCHAR Lowest, Highest;

    HalpPirLinkRange(Table, &Lowest, &Highest);

    return (Highest <= CX5520_INT_LINKS) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpCx5520GetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    UCHAR Register;

    if (Link == 0 || Link > CX5520_INT_LINKS)
        return STATUS_INVALID_PARAMETER;

    Register = HalpPirPairRegister(CX5520_INT_REGISTER, Link);
    *Irq = HalpPirGetNibble(HalpPirReadRouterByte(Register), HalpPirPairHighNibble(Link));
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpCx5520SetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    UCHAR Register;

    if (Link == 0 || Link > CX5520_INT_LINKS)
        return STATUS_INVALID_PARAMETER;

    Register = HalpPirPairRegister(CX5520_INT_REGISTER, Link);
    HalpPirWriteRouterByte(Register,
                           HalpPirSetNibble(HalpPirReadRouterByte(Register),
                                            HalpPirPairHighNibble(Link),
                                            Irq));
    return STATUS_SUCCESS;
}

HALP_IRQ_ROUTER HalpCx5520Router =
{
    HalpCx5520ValidateTable,
    HalpCx5520GetIrq,
    HalpCx5520SetIrq,
    HalpElcrGetTrigger,
    HalpElcrSetTrigger
};
