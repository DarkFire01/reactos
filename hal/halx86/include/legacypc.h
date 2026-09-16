/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Legacy PC Header
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Interrupt range attributes in the legacy PC arbiter */
#define HALP_IRQ_RANGE_LEVEL    0x04
#define HALP_IRQ_RANGE_LATCHED  0x08

/* One $PIR link. Arbiter ranges routed through it carry it as UserData. */
typedef struct _HALP_PCI_LINK
{
    struct _HALP_PCI_LINK *Next;
    UCHAR Link;
    USHORT IrqMask;
} HALP_PCI_LINK, *PHALP_PCI_LINK;

typedef struct _HALP_IRQ_ROUTER
{
    NTSTATUS (NTAPI *ValidateTable)(_Inout_ PPCI_IRQ_ROUTING_TABLE Table);
    NTSTATUS (NTAPI *GetIrq)(_In_ UCHAR Link, _Out_ PUCHAR Irq);
    NTSTATUS (NTAPI *SetIrq)(_In_ UCHAR Link, _In_ UCHAR Irq);
} HALP_IRQ_ROUTER, *PHALP_IRQ_ROUTER;

/* legacypcirqarb.c */
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyPCCreateArbiter(
    _In_ PDEVICE_OBJECT BusFdo);

CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyPCQueryArbInterface(
    _Out_writes_bytes_(Size) PVOID Interface,
    _In_ ULONG Size,
    _Out_ PULONG Length);

BOOLEAN
NTAPI
HalpLegacyPCArbitratesIrqs(VOID);

/* irqtrans.c */
CODE_SEG("PAGE")
NTSTATUS
NTAPI
HalpLegacyPCQueryIrqTranslator(
    _In_ PDEVICE_OBJECT BusFdo,
    _Out_writes_bytes_(Size) PVOID Interface,
    _In_ ULONG Size,
    _Out_ PULONG Length);

/* pirroute.c */
CODE_SEG("PAGE")
VOID
NTAPI
HalpLegacyPCInitIrqRouting(
    _In_ PDEVICE_OBJECT PciPdo);

NTSTATUS
NTAPI
HalpLegacyPCFindLink(
    _In_opt_ PDEVICE_OBJECT Device,
    _Out_ PHALP_PCI_LINK *Link);

PHALP_PCI_LINK
NTAPI
HalpLegacyPCFirstLink(VOID);

NTSTATUS
NTAPI
HalpLegacyPCGetLinkIrq(
    _In_ PHALP_PCI_LINK Link,
    _Out_ PUCHAR Irq);

NTSTATUS
NTAPI
HalpLegacyPCSetLinkIrq(
    _In_ PHALP_PCI_LINK Link,
    _In_ UCHAR Irq);

VOID
NTAPI
HalpLegacyPCUpdateInterruptLine(
    _In_ PDEVICE_OBJECT Device,
    _In_ UCHAR Irq);

BOOLEAN
NTAPI
HalpLegacyPCIrqRoutingActive(VOID);

/* intelirq.c */
CODE_SEG("PAGE")
PHALP_IRQ_ROUTER
NTAPI
HalpIntelGetRouter(
    _In_ ULONG Instance,
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot);
