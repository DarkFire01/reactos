/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DROM parser: header, vendor and model names, product descriptor
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4dr.h"

#define NDEBUG
#include <debug.h>

/* Adapter entries are told apart by their length; this one is the TBT3 PCIe upstream adapter */
#define USB4DR_DROM_PCIE_UP_ENTRY_LENGTH    11

/* Two byte adapter entry: byte 1 bit 6 marks the adapter in bits 5:0 unused */
#define USB4DR_DROM_UNUSED_ENTRY_LENGTH     2
#define USB4DR_DROM_ADAPTER_UNUSED          0x40
#define USB4DR_DROM_ADAPTER_NUMBER_MASK     0x3F

/* Product descriptor fields, byte offsets inside the entry */
#define USB4DR_DROM_PRODUCT_VENDOR          4
#define USB4DR_DROM_PRODUCT_ID              6
#define USB4DR_DROM_PRODUCT_FIRMWARE        8
#define USB4DR_DROM_PRODUCT_HW_REVISION     14

/* Generic entries carry their data after the two byte entry header */
#define USB4DR_DROM_ENTRY_DATA              2

static
USHORT
NTAPI
Usb4DrReadLe16(
    _In_reads_bytes_(2) const UCHAR* Data)
{
    return (USHORT)(Data[0] | (Data[1] << 8));
}

/* Copies an ASCII name entry; the name must end inside the entry */
static
NTSTATUS
NTAPI
Usb4DrTakeName(
    _In_reads_bytes_(Length) const UCHAR* Entry,
    _In_ ULONG Length,
    _Out_writes_(USB4DR_DROM_NAME_CHARS) PCHAR Name)
{
    ULONG Chars = Length - USB4DR_DROM_ENTRY_DATA;

    if (Entry[Length - 1] != '\0')
        return STATUS_UNSUCCESSFUL;

    RtlCopyMemory(Name, Entry + USB4DR_DROM_ENTRY_DATA, min(Chars, (ULONG)USB4DR_DROM_NAME_CHARS));
    Name[USB4DR_DROM_NAME_CHARS - 1] = '\0';
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrDrom::Parse(
    _In_reads_bytes_(Size) const UCHAR* Data,
    _In_ ULONG Size)
{
    const UCHAR* Entry;
    ULONG Offset;
    ULONG Length;
    ULONG Type;
    UCHAR Revision;
    NTSTATUS Status = STATUS_SUCCESS;

    Reset();

    if (!Data || Size < USB4DR_DROM_HEADER_BYTES)
    {
        DPRINT1("DROM of %lu bytes has no room for a header\n", Size);
        return STATUS_BUFFER_TOO_SMALL;
    }

    Revision = Data[USB4DR_DROM_REVISION_OFFSET];
    if (Revision == 1 || Revision == 2)
    {
        if (Size < USB4DR_DROM_TBT3_HEADER_BYTES)
        {
            DPRINT1("DROM of %lu bytes has no room for a TBT3 header\n", Size);
            return STATUS_BUFFER_TOO_SMALL;
        }

        m_Tbt3Header = TRUE;
        m_Parsed = TRUE;
        Offset = USB4DR_DROM_TBT3_HEADER_BYTES;
    }
    else if (Revision >= 3)
    {
        m_Parsed = TRUE;
        Offset = USB4DR_DROM_HEADER_BYTES;
    }
    else
    {
        /* QUIRK: Windows parses a DROM with revision 0 as entries from the first byte */
        Offset = 0;
    }

    while (Offset < Size)
    {
        Entry = &Data[Offset];
        Length = Entry[0];

        if (Length <= 1)
        {
            DPRINT1("DROM entry at %lu has length %lu\n", Offset, Length);
            Status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }

        if (Length > Size - Offset)
        {
            DPRINT1("DROM entry at %lu runs past the end\n", Offset);
            Status = STATUS_UNSUCCESSFUL;
            break;
        }

        if (Entry[1] & USB4DR_DROM_ENTRY_ADAPTER)
        {
            /* Only unused adapter entries matter; a second PCIe upstream entry is an error */
            if (Length == USB4DR_DROM_UNUSED_ENTRY_LENGTH && (Entry[1] & USB4DR_DROM_ADAPTER_UNUSED))
            {
                m_UnusedAdapters |= 1ULL << (Entry[1] & USB4DR_DROM_ADAPTER_NUMBER_MASK);
            }
            else if (Length == USB4DR_DROM_PCIE_UP_ENTRY_LENGTH)
            {
                if (m_SeenPcieUpstream)
                {
                    Status = STATUS_NOT_SUPPORTED;
                    break;
                }
                m_SeenPcieUpstream = TRUE;
            }

            Offset += Length;
            continue;
        }

        Type = Entry[1] & USB4DR_DROM_ENTRY_TYPE_MASK;
        switch (Type)
        {
            case USB4DR_DROM_ENTRY_VENDOR_NAME:
            case USB4DR_DROM_ENTRY_MODEL_NAME:
            case USB4DR_DROM_ENTRY_TMU_MODE:
            case USB4DR_DROM_ENTRY_PRODUCT:
            case USB4DR_DROM_ENTRY_SERIAL:
            case USB4DR_DROM_ENTRY_USB_PORT_MAP:
            case USB4DR_DROM_ENTRY_VENDOR_NAME_UTF16:
            case USB4DR_DROM_ENTRY_MODEL_NAME_UTF16:
                if (m_SeenGeneric & (1UL << Type))
                {
                    DPRINT1("DROM has a second entry of type %lu\n", Type);
                    Status = STATUS_NOT_SUPPORTED;
                }
                break;

            default:
                break;
        }
        if (!NT_SUCCESS(Status))
            break;

        switch (Type)
        {
            case USB4DR_DROM_ENTRY_VENDOR_NAME:
                Status = Usb4DrTakeName(Entry, Length, m_VendorName);
                break;

            case USB4DR_DROM_ENTRY_MODEL_NAME:
                Status = Usb4DrTakeName(Entry, Length, m_ModelName);
                break;

            case USB4DR_DROM_ENTRY_PRODUCT:
                /* Windows reads a short product descriptor past its end; we ignore it */
                if (Length < USB4DR_DROM_PRODUCT_MIN_LENGTH)
                {
                    DPRINT1("DROM product descriptor of %lu bytes ignored\n", Length);
                    break;
                }

                m_UnitVendorId = Usb4DrReadLe16(Entry + USB4DR_DROM_PRODUCT_VENDOR);
                m_UnitProductId = Usb4DrReadLe16(Entry + USB4DR_DROM_PRODUCT_ID);
                m_FirmwareVersion = Usb4DrReadLe16(Entry + USB4DR_DROM_PRODUCT_FIRMWARE);
                m_HardwareRevision = Entry[USB4DR_DROM_PRODUCT_HW_REVISION];
                m_HasProduct = TRUE;
                break;

            default:
                break;
        }
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("DROM name entry of type %lu is not terminated\n", Type);
            break;
        }

        if (Type < 32)
            m_SeenGeneric |= (1UL << Type);

        Offset += Length;
    }

    DPRINT("DROM vendor '%s' model '%s' product %u\n",
           m_VendorName, m_ModelName, m_HasProduct);
    return Status;
}

VOID Usb4DrDrom::Reset()
{
    RtlZeroMemory(this, sizeof(*this));
}

BOOLEAN
Usb4DrDrom::IsParsed() const
{
    return m_Parsed;
}

BOOLEAN
Usb4DrDrom::HasTbt3Header() const
{
    return m_Tbt3Header;
}

PCSTR
Usb4DrDrom::VendorName() const
{
    return m_VendorName[0] ? m_VendorName : NULL;
}

PCSTR
Usb4DrDrom::ModelName() const
{
    return m_ModelName[0] ? m_ModelName : NULL;
}

BOOLEAN
Usb4DrDrom::IsAdapterUnused(
    _In_ UCHAR Number) const
{
    if (Number > USB4DR_DROM_ADAPTER_NUMBER_MASK)
        return FALSE;

    return (m_UnusedAdapters & (1ULL << Number)) != 0;
}

BOOLEAN
Usb4DrDrom::ProductDescriptor(
    _Out_opt_ PUSHORT VendorId,
    _Out_opt_ PUSHORT ProductId,
    _Out_opt_ PUCHAR HardwareRevision,
    _Out_opt_ PUSHORT FirmwareVersion) const
{
    if (VendorId)
        *VendorId = m_UnitVendorId;
    if (ProductId)
        *ProductId = m_UnitProductId;
    if (HardwareRevision)
        *HardwareRevision = m_HardwareRevision;
    if (FirmwareVersion)
        *FirmwareVersion = m_FirmwareVersion;

    return m_HasProduct;
}
