/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DROM parser: header, vendor and model names, product descriptor
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#define USB4DR_DROM_NAME_CHARS          256

/** What the router keeps from its DROM. */
class Usb4DrDrom
{
public:
    /**
     * Parses a whole DROM. STATUS_BUFFER_TOO_SMALL for a short header,
     * STATUS_UNSUCCESSFUL for an entry that runs past the end. Entries found
     * before an error are kept.
     */
    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Parse(
        _In_reads_bytes_(Size) const UCHAR* Data,
        _In_ ULONG Size);

    /** Drops everything parsed. */
    VOID Reset();

    /** TRUE once a TBT3 or USB4 header was recognized. */
    BOOLEAN IsParsed() const;

    /** Header revision 1 or 2. */
    BOOLEAN HasTbt3Header() const;

    /** NUL terminated name from the ASCII vendor (1) and model (2) entries, or NULL. */
    PCSTR VendorName() const;
    PCSTR ModelName() const;

    /** TRUE when a two byte adapter entry marks adapter Number unused. */
    BOOLEAN
    IsAdapterUnused(
        _In_ UCHAR Number) const;

    /** Product descriptor (entry 9) values; FALSE when there is none. */
    BOOLEAN
    ProductDescriptor(
        _Out_opt_ PUSHORT VendorId,
        _Out_opt_ PUSHORT ProductId,
        _Out_opt_ PUCHAR HardwareRevision,
        _Out_opt_ PUSHORT FirmwareVersion) const;

private:
    BOOLEAN m_Parsed;
    BOOLEAN m_Tbt3Header;
    BOOLEAN m_HasProduct;
    UCHAR m_HardwareRevision;
    USHORT m_UnitVendorId;
    USHORT m_UnitProductId;
    USHORT m_FirmwareVersion;

    /** Bit n set once generic entry n was taken; a second one ends the parse. */
    ULONG m_SeenGeneric;
    BOOLEAN m_SeenPcieUpstream;
    ULONG64 m_UnusedAdapters;   /**< bit n: adapter n is marked unused */
    CHAR m_VendorName[USB4DR_DROM_NAME_CHARS];
    CHAR m_ModelName[USB4DR_DROM_NAME_CHARS];
};
