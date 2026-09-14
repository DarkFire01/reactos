/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     VIA PCI IRQ router miniport
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include "irqrouter.h"

#define NDEBUG
#include <debug.h>

/* VT82C586B, VT82C596B and VT82C686B steer PIRQA-D through nibbles at 0x55-0x57 */
#define VIA_PIRQA_REGISTER      0x55
#define VIA_PIRQBC_REGISTER     0x56
#define VIA_PIRQD_REGISTER      0x57

/* The BIOS numbers the links 1, 2, 3 and 5 */
static
BOOLEAN
HalpViaLocateLink(
    _In_ UCHAR Link,
    _Out_ PUCHAR Register,
    _Out_ PBOOLEAN High)
{
    switch (Link)
    {
        case 1:
            *Register = VIA_PIRQA_REGISTER;
            *High = TRUE;
            return TRUE;

        case 2:
            *Register = VIA_PIRQBC_REGISTER;
            *High = FALSE;
            return TRUE;

        case 3:
            *Register = VIA_PIRQBC_REGISTER;
            *High = TRUE;
            return TRUE;

        case 5:
            *Register = VIA_PIRQD_REGISTER;
            *High = TRUE;
            return TRUE;

        default:
            return FALSE;
    }
}

static
BOOLEAN
NTAPI
HalpViaIsLink(
    _In_ UCHAR Link)
{
    UCHAR Register;
    BOOLEAN High;

    return HalpViaLocateLink(Link, &Register, &High);
}

static
NTSTATUS
NTAPI
HalpViaValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    return HalpPirAllLinks(Table, HalpViaIsLink) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpViaGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    UCHAR Register;
    BOOLEAN High;

    if (!HalpViaLocateLink(Link, &Register, &High))
        return STATUS_INVALID_PARAMETER;

    *Irq = HalpPirGetNibble(HalpPirReadRouterByte(Register), High);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpViaSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    UCHAR Register;
    BOOLEAN High;

    if (!HalpViaLocateLink(Link, &Register, &High))
        return STATUS_INVALID_PARAMETER;

    HalpPirWriteRouterByte(Register, HalpPirSetNibble(HalpPirReadRouterByte(Register), High, Irq));
    return STATUS_SUCCESS;
}

HALP_IRQ_ROUTER HalpViaRouter =
{
    HalpViaValidateTable,
    HalpViaGetIrq,
    HalpViaSetIrq,
    HalpElcrGetTrigger,
    HalpElcrSetTrigger
};
