/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Toshiba PCI IRQ router miniport
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <hal.h>
#include "irqrouter.h"

#define NDEBUG
#include <debug.h>

/* Toshiba notebooks route PCI interrupts through a BIOS SMI interface */
#define TOSHIBA_SMI_GET         0xFE44
#define TOSHIBA_SMI_SET         0xFF44
#define TOSHIBA_SMI_LINK_IRQ    0x0701
#define TOSHIBA_SMI_TRIGGER     0x0702
#define TOSHIBA_SMI_SUCCESS     0x00
#define TOSHIBA_NO_IRQ          0xFF

/* toshsmi.S */
UCHAR
NTAPI
HalpToshibaCallSmi(
    _In_ USHORT Port,
    _In_ USHORT Function,
    _In_ USHORT Selector,
    _Inout_ PUSHORT Data);

/* The $PIR table carries the SMI port in the upper word of its miniport data */
static USHORT HalpToshibaSmiPort;

static
NTSTATUS
NTAPI
HalpToshibaSmi(
    _In_ USHORT Function,
    _In_ USHORT Selector,
    _Inout_ PUSHORT Data)
{
    if (HalpToshibaSmiPort == 0)
        return STATUS_UNSUCCESSFUL;

    if (HalpToshibaCallSmi(HalpToshibaSmiPort, Function, Selector, Data) != TOSHIBA_SMI_SUCCESS)
        return STATUS_UNSUCCESSFUL;

    return STATUS_SUCCESS;
}

/* Without a port from the BIOS there is nothing to call */
static
NTSTATUS
NTAPI
HalpToshibaValidateTable(
    _Inout_ PPCI_IRQ_ROUTING_TABLE Table)
{
    HalpToshibaSmiPort = (USHORT)(Table->MiniportData >> 16);

    return (HalpToshibaSmiPort != 0) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static
NTSTATUS
NTAPI
HalpToshibaGetIrq(
    _In_ UCHAR Link,
    _Out_ PUCHAR Irq)
{
    USHORT Data = (USHORT)(Link << 8);
    NTSTATUS Status;

    Status = HalpToshibaSmi(TOSHIBA_SMI_GET, TOSHIBA_SMI_LINK_IRQ, &Data);
    if (!NT_SUCCESS(Status))
        return Status;

    *Irq = ((Data & 0xFF) == TOSHIBA_NO_IRQ) ? 0 : (UCHAR)Data;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HalpToshibaSetIrq(
    _In_ UCHAR Link,
    _In_ UCHAR Irq)
{
    USHORT Data = (USHORT)((Link << 8) | ((Irq != 0) ? Irq : TOSHIBA_NO_IRQ));

    return HalpToshibaSmi(TOSHIBA_SMI_SET, TOSHIBA_SMI_LINK_IRQ, &Data);
}

static
NTSTATUS
NTAPI
HalpToshibaGetTrigger(
    _Out_ PUSHORT LevelIrqs)
{
    *LevelIrqs = 0;

    return HalpToshibaSmi(TOSHIBA_SMI_GET, TOSHIBA_SMI_TRIGGER, LevelIrqs);
}

static
NTSTATUS
NTAPI
HalpToshibaSetTrigger(
    _In_ USHORT LevelIrqs)
{
    return HalpToshibaSmi(TOSHIBA_SMI_SET, TOSHIBA_SMI_TRIGGER, &LevelIrqs);
}

HALP_IRQ_ROUTER HalpToshibaRouter =
{
    HalpToshibaValidateTable,
    HalpToshibaGetIrq,
    HalpToshibaSetIrq,
    HalpToshibaGetTrigger,
    HalpToshibaSetTrigger
};
