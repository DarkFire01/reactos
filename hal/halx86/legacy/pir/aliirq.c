/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ALi PCI IRQ router miniports
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include "irqrouter.h"

#define NDEBUG
#include <debug.h>

/* M1523 and M1533 steer INT1 to INT8 through the nibbles at 0x48 to 0x4B */
#define ALI_INT_REGISTER        0x48
#define ALI_INT_LINKS           8

/* The M1533 also steers its USB controller, which the BIOS lists as link 0x59 */
#define ALI_USB_LINK            0x59

/* The register holds a code for the IRQ, and 0 for an IRQ that cannot be steered */
static const UCHAR HalpAliIrqCodes[16] =
{
    0x00, 0x00, 0x00, 0x02, 0x04, 0x05, 0x07, 0x06,
    0x00, 0x01, 0x03, 0x09, 0x0B, 0x00, 0x0D, 0x0F
};

static
UCHAR
NTAPI
HalpAliCodeToIrq(
    _In_ UCHAR Code)
{
    UCHAR Irq;

    if (Code == 0)
        return 0;

    for (Irq = 0; Irq < RTL_NUMBER_OF(HalpAliIrqCodes); Irq++)
    {
        if (HalpAliIrqCodes[Irq] == Code)
            return Irq;
    }

    return 0;
}

static
BOOLEAN
NTAPI
HalpAli1523IsLink(
    _In_ UCHAR Link)
{
    return (Link >= 1 && Link <= ALI_INT_LINKS);
}

static
BOOLEAN
NTAPI
HalpAli1533IsLink(
    _In_ UCHAR Link)
{
    return HalpAli1523IsLink(Link) || (Link == ALI_USB_LINK);
}

/* The pair math puts link 0x59 in the low nibble of 0x74, where the M1533 keeps it */
static
NTSTATUS
NTAPI
HalpAliGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    UCHAR Register = HalpPirPairRegister(ALI_INT_REGISTER, Link);
    UCHAR Code;

    Code = HalpPirGetNibble(HalpPirReadRouterByte(Register), HalpPirPairHighNibble(Link));

    *Irq = HalpAliCodeToIrq(Code);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpAliSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    UCHAR Register = HalpPirPairRegister(ALI_INT_REGISTER, Link);
    UCHAR Value;

    Value = HalpPirSetNibble(HalpPirReadRouterByte(Register),
                             HalpPirPairHighNibble(Link),
                             HalpAliIrqCodes[Irq & 0x0F]);

    HalpPirWriteRouterByte(Register, Value);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpAli1523ValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    return HalpPirAllLinks(Table, HalpAli1523IsLink) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpAli1523GetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    if (!HalpAli1523IsLink(Link))
        return STATUS_INVALID_PARAMETER;

    return HalpAliGetIrq(Link, Irq);
}

static
NTSTATUS
NTAPI
HalpAli1523SetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    if (!HalpAli1523IsLink(Link))
        return STATUS_INVALID_PARAMETER;

    return HalpAliSetIrq(Link, Irq);
}

static
NTSTATUS
NTAPI
HalpAli1533ValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    return HalpPirAllLinks(Table, HalpAli1533IsLink) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpAli1533GetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    if (!HalpAli1533IsLink(Link))
        return STATUS_INVALID_PARAMETER;

    return HalpAliGetIrq(Link, Irq);
}

static
NTSTATUS
NTAPI
HalpAli1533SetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    if (!HalpAli1533IsLink(Link))
        return STATUS_INVALID_PARAMETER;

    return HalpAliSetIrq(Link, Irq);
}

HALP_IRQ_ROUTER HalpAli1523Router =
{
    HalpAli1523ValidateTable,
    HalpAli1523GetIrq,
    HalpAli1523SetIrq,
    HalpElcrGetTrigger,
    HalpElcrSetTrigger
};

HALP_IRQ_ROUTER HalpAli1533Router =
{
    HalpAli1533ValidateTable,
    HalpAli1533GetIrq,
    HalpAli1533SetIrq,
    HalpElcrGetTrigger,
    HalpElcrSetTrigger
};
