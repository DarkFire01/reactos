/*
 * PROJECT:     ReactOS HAL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PCI IRQ router miniports
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* pirroute.c */
VOID
NTAPI
HalpPirReadRouter(
    _In_ ULONG Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length);

VOID
NTAPI
HalpPirWriteRouter(
    _In_ ULONG Offset,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length);

VOID
NTAPI
HalpPirLinkRange(
    _In_ PPCI_IRQ_ROUTING_TABLE Table,
    _Out_ PUCHAR Lowest,
    _Out_ PUCHAR Highest);

BOOLEAN
NTAPI
HalpPirAllLinks(
    _In_ PPCI_IRQ_ROUTING_TABLE Table,
    _In_ BOOLEAN (NTAPI *IsValid)(_In_ UCHAR Link));

NTSTATUS
NTAPI
HalpElcrGetTrigger(
    _Out_ PUSHORT LevelIrqs);

NTSTATUS
NTAPI
HalpElcrSetTrigger(
    _In_ USHORT LevelIrqs);

static inline
UCHAR
HalpPirReadRouterByte(
    _In_ ULONG Offset)
{
    UCHAR Value = 0xFF;

    HalpPirReadRouter(Offset, &Value, sizeof(Value));
    return Value;
}

static inline
VOID
HalpPirWriteRouterByte(
    _In_ ULONG Offset,
    _In_ UCHAR Value)
{
    HalpPirWriteRouter(Offset, &Value, sizeof(Value));
}

static inline
USHORT
HalpPirReadRouterWord(
    _In_ ULONG Offset)
{
    USHORT Value = 0xFFFF;

    HalpPirReadRouter(Offset, &Value, sizeof(Value));
    return Value;
}

static inline
VOID
HalpPirWriteRouterWord(
    _In_ ULONG Offset,
    _In_ USHORT Value)
{
    HalpPirWriteRouter(Offset, &Value, sizeof(Value));
}

static inline
ULONG
HalpPirReadRouterLong(
    _In_ ULONG Offset)
{
    ULONG Value = MAXULONG;

    HalpPirReadRouter(Offset, &Value, sizeof(Value));
    return Value;
}

static inline
VOID
HalpPirWriteRouterLong(
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    HalpPirWriteRouter(Offset, &Value, sizeof(Value));
}

/* Many routers keep two links in each register, the odd link in the low nibble */
static inline
UCHAR
HalpPirPairRegister(
    _In_ UCHAR Base,
    _In_ UCHAR Link)
{
    return (UCHAR)(Base + ((Link - 1) >> 1));
}

static inline
BOOLEAN
HalpPirPairHighNibble(
    _In_ UCHAR Link)
{
    return !(Link & 1);
}

static inline
UCHAR
HalpPirGetNibble(
    _In_ UCHAR Value,
    _In_ BOOLEAN High)
{
    return High ? (UCHAR)(Value >> 4) : (UCHAR)(Value & 0x0F);
}

static inline
UCHAR
HalpPirSetNibble(
    _In_ UCHAR Value,
    _In_ BOOLEAN High,
    _In_ UCHAR Nibble)
{
    Nibble &= 0x0F;
    return High ? (UCHAR)((Value & 0x0F) | (Nibble << 4)) : (UCHAR)((Value & 0xF0) | Nibble);
}

/* aliirq.c */
extern HALP_IRQ_ROUTER HalpAli1523Router;
extern HALP_IRQ_ROUTER HalpAli1533Router;

/* compaqirq.c */
extern HALP_IRQ_ROUTER HalpCompaqCmc2Router;
extern HALP_IRQ_ROUTER HalpCompaqMisc3Router;
extern HALP_IRQ_ROUTER HalpCompaqOsbRouter;

/* cyrixirq.c */
extern HALP_IRQ_ROUTER HalpCx5520Router;

/* intelirq.c */
extern HALP_IRQ_ROUTER HalpEscRouter;
extern HALP_IRQ_ROUTER HalpPiixRouter;

/* nsirq.c */
extern HALP_IRQ_ROUTER HalpNs87560Router;

/* optiirq.c */
extern HALP_IRQ_ROUTER HalpOptiFireStarRouter;
extern HALP_IRQ_ROUTER HalpOptiViperRouter;

/* sisirq.c */
extern HALP_IRQ_ROUTER HalpSisRouter;

/* toshirq.c */
extern HALP_IRQ_ROUTER HalpToshibaRouter;

/* vesuvirq.c */
extern HALP_IRQ_ROUTER HalpVesuviusRouter;

/* viairq.c */
extern HALP_IRQ_ROUTER HalpViaRouter;

/* vlsiirq.c */
extern HALP_IRQ_ROUTER HalpVlsiEagleRouter;
extern HALP_IRQ_ROUTER HalpVlsiRouter;
