/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Router operation mailbox (ROUTER_CS_9 to ROUTER_CS_26) and DROM read
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/** Runs router operations one at a time on a USB4 router. */
class Usb4DrRouterOps
{
public:
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Create(
        _In_ Usb4DrFdo* Fdo);

    /**
     * Runs one operation and waits for Operation Valid to clear.
     * STATUS_NOT_SUPPORTED for Operation Not Supported, STATUS_IO_TIMEOUT when it never clears,
     * STATUS_UNSUCCESSFUL with a non zero status field.
     */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Execute(
        _In_ USHORT Opcode,
        _In_opt_ const ULONG* MetadataIn,
        _In_reads_opt_(DataInDwords) const ULONG* DataIn,
        _In_ ULONG DataInDwords,
        _Out_opt_ PULONG MetadataOut,
        _Out_writes_opt_(DataOutDwords) PULONG DataOut,
        _In_ ULONG DataOutDwords);

    /** Reads the whole DROM with USB4DR_ROP_DROM_READ; the caller frees *Drom with USB4DR_TAG_DROM. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    ReadDrom(
        _Outptr_result_bytebuffer_(*Size) PUCHAR* Drom,
        _Out_ PULONG Size);

    /** USB4DR_ROP_BUFFER_ALLOCATION; Values[n] is parameter n, 0 when the router did not report it. Startup fails without it. */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    QueryBufferAllocation(
        _Out_writes_(USB4DR_BUFFER_MAX_USB3_GEN_T + 1) PUCHAR Values);

private:
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    WaitForOperationDone(
        _Out_ PULONG OpcodeRegister);

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    ReadDromChunk(
        _In_ ULONG DwordAddress,
        _In_ ULONG DwordCount,
        _Out_writes_(DwordCount) PULONG Buffer,
        _Out_ PULONG DwordsRead);

    Usb4DrFdo* m_Fdo;

    /** One operation at a time; an operation is several config accesses. */
    WDFWAITLOCK m_Lock;
};
