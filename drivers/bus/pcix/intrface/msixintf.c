/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/intrface/msixintf.c
 * PURPOSE:         MSI-X Table Configuration Interface
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GUID for PCI_MSIX_TABLE_CONFIG_INTERFACE (define locally if not provided) */
#ifndef GUID_PCI_MSIX_TABLE_CONFIG_INTERFACE
#ifndef INITGUID
#define INITGUID
#endif
DEFINE_GUID(GUID_PCI_MSIX_TABLE_CONFIG_INTERFACE,
0x7feda7e1, 0x8c88, 0x4b77, 0x9e, 0x63, 0x6c, 0x2c, 0x25, 0xa9, 0xe0, 0xb9);
#endif

/* GLOBALS ********************************************************************/

static
BOOLEAN
PciMsixMapTable(IN PPCI_PDO_EXTENSION PdoExtension,
                OUT PVOID *TableVa,
                OUT SIZE_T *TableLength)
{
    PHYSICAL_ADDRESS BarBase;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Current;
    ULONG barIndex;
    SIZE_T length;

    *TableVa = NULL;
    *TableLength = 0;

    /* Validate that MSI-X capability is present */
    if (!PdoExtension->MsixCapabilityOffset) return FALSE;
    if (!PdoExtension->Resources) return FALSE;

    /* Determine BAR index and locate its current resource */
    barIndex = PdoExtension->MsixTableBir;
    if (barIndex >= PCI_TYPE0_ADDRESSES) return FALSE;

    Current = &PdoExtension->Resources->Current[barIndex];
    if (Current->Type != CmResourceTypeMemory) return FALSE;

    /* Map the BAR region and compute table VA */
    BarBase = Current->u.Memory.Start;
    length = Current->u.Memory.Length;
    if ((BarBase.QuadPart == 0) || (length == 0)) return FALSE;

    *TableVa = MmMapIoSpace(BarBase, length, MmNonCached);
    if (!*TableVa) return FALSE;

    /* Ensure the table fits within the mapped BAR */
    if (PdoExtension->MsixTableOffset >= length)
    {
        MmUnmapIoSpace(*TableVa, length);
        *TableVa = NULL;
        return FALSE;
    }

    *TableVa = (PVOID)((ULONG_PTR)*TableVa + PdoExtension->MsixTableOffset);
    *TableLength = length - PdoExtension->MsixTableOffset;
    return TRUE;
}

static
VOID
PciMsixUnmapTable(IN PVOID BaseVa,
                  IN SIZE_T TotalLength)
{
    if (BaseVa && TotalLength)
    {
        /* BaseVa passed to unmap must be the original BAR VA, not offset */
        MmUnmapIoSpace(BaseVa, TotalLength);
    }
}

static
BOOLEAN
PciMsixGetEntryPtr(IN PPCI_PDO_EXTENSION PdoExtension,
                   IN ULONG TableEntry,
                   OUT PVOID *EntryVa,
                   OUT PVOID *BarVa,
                   OUT SIZE_T *BarLength)
{
    PVOID tableBase, originalBarVa;
    SIZE_T tableLen, barLen;
    ULONG required;

    *EntryVa = NULL;
    *BarVa = NULL;
    *BarLength = 0;

    if (!PciMsixMapTable(PdoExtension, &tableBase, &tableLen)) return FALSE;

    /* Recover original BAR VA for unmapping */
    originalBarVa = (PVOID)((ULONG_PTR)tableBase - PdoExtension->MsixTableOffset);
    barLen = tableLen + PdoExtension->MsixTableOffset;

    required = (TableEntry + 1) * sizeof(PCI_MSIX_TABLE_ENTRY);
    if (required > tableLen)
    {
        PciMsixUnmapTable(originalBarVa, barLen);
        return FALSE;
    }

    *EntryVa = (PVOID)((ULONG_PTR)tableBase + (TableEntry * sizeof(PCI_MSIX_TABLE_ENTRY)));
    *BarVa = originalBarVa;
    *BarLength = barLen;
    return TRUE;
}

static
NTSTATUS
PciMsiXInterface_RefDeref(IN PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    return STATUS_SUCCESS;
}

static
NTSTATUS
PciMsiXInterface_GetTableSize(IN PVOID Context,
                              OUT PULONG TableSize)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Context;
    USHORT ctrl;
    ASSERT_PDO(PdoExtension);

    if (!PdoExtension->MsixCapabilityOffset) return STATUS_NOT_SUPPORTED;

    ctrl = PdoExtension->MsixControl;
    *TableSize = (ctrl & 0x07FF) + 1; /* TableSize is N-1 in spec */
    return STATUS_SUCCESS;
}

static
NTSTATUS
PciMsiXInterface_GetTableEntry(IN PVOID Context,
                               IN ULONG TableEntry,
                               OUT PULONG MessageNumber,
                               OUT PUCHAR Masked)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Context;
    PVOID entryVa, barVa;
    SIZE_T barLen;
    PCI_MSIX_TABLE_ENTRY entry;
    NTSTATUS Status = STATUS_SUCCESS;

    ASSERT_PDO(PdoExtension);
    if (!PciMsixGetEntryPtr(PdoExtension, TableEntry, &entryVa, &barVa, &barLen))
        return STATUS_INVALID_PARAMETER;

    RtlCopyMemory(&entry, entryVa, sizeof(entry));

    if (MessageNumber) *MessageNumber = entry.MessageData;
    if (Masked) *Masked = (UCHAR)(entry.VectorControl & 0x1);

    PciMsixUnmapTable(barVa, barLen);
    return Status;
}

static
NTSTATUS
PciMsiXInterface_SetTableEntry(IN PVOID Context,
                               IN ULONG TableEntry,
                               IN ULONG MessageNumber)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Context;
    PVOID entryVa, barVa;
    SIZE_T barLen;
    PCI_MSIX_TABLE_ENTRY *entry;

    ASSERT_PDO(PdoExtension);
    if (!PciMsixGetEntryPtr(PdoExtension, TableEntry, &entryVa, &barVa, &barLen))
        return STATUS_INVALID_PARAMETER;

    entry = (PCI_MSIX_TABLE_ENTRY*)entryVa;
    entry->MessageData = MessageNumber;

    PciMsixUnmapTable(barVa, barLen);
    return STATUS_SUCCESS;
}

static
NTSTATUS
PciMsiXInterface_MaskTableEntry(IN PVOID Context,
                                IN ULONG TableEntry)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Context;
    PVOID entryVa, barVa;
    SIZE_T barLen;
    PCI_MSIX_TABLE_ENTRY *entry;

    ASSERT_PDO(PdoExtension);
    if (!PciMsixGetEntryPtr(PdoExtension, TableEntry, &entryVa, &barVa, &barLen))
        return STATUS_INVALID_PARAMETER;

    entry = (PCI_MSIX_TABLE_ENTRY*)entryVa;
    entry->VectorControl |= 0x1; /* Mask */

    PciMsixUnmapTable(barVa, barLen);
    return STATUS_SUCCESS;
}

static
NTSTATUS
PciMsiXInterface_UnmaskTableEntry(IN PVOID Context,
                                  IN ULONG TableEntry)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)Context;
    PVOID entryVa, barVa;
    SIZE_T barLen;
    PCI_MSIX_TABLE_ENTRY *entry;

    ASSERT_PDO(PdoExtension);
    if (!PciMsixGetEntryPtr(PdoExtension, TableEntry, &entryVa, &barVa, &barLen))
        return STATUS_INVALID_PARAMETER;

    entry = (PCI_MSIX_TABLE_ENTRY*)entryVa;
    entry->VectorControl &= ~0x1; /* Unmask */

    PciMsixUnmapTable(barVa, barLen);
    return STATUS_SUCCESS;
}

PCI_INTERFACE PciMsiXTableConfigInterface =
{
    &GUID_PCI_MSIX_TABLE_CONFIG_INTERFACE,
    PCI_MSIX_TABLE_CONFIG_MINIMUM_SIZE,
    PCI_MSIX_TABLE_CONFIG_INTERFACE_VERSION,
    PCI_MSIX_TABLE_CONFIG_INTERFACE_VERSION,
    PCI_INTERFACE_PDO,
    0,
    (PCI_SIGNATURE)0, /* no secondary extension backing */
    (PCI_INTERFACE_CONSTRUCTOR)NULL,
    (PCI_INTERFACE_INITIALIZER)NULL
};

NTSTATUS
NTAPI
PciMsiXTableConfigInterfaceConstructor(IN PVOID DeviceExtension,
                                       IN PVOID Instance,
                                       IN PVOID InterfaceData,
                                       IN USHORT Version,
                                       IN USHORT Size,
                                       IN PINTERFACE Interface)
{
    PPCI_PDO_EXTENSION PdoExtension = (PPCI_PDO_EXTENSION)DeviceExtension;
    PPCI_MSIX_TABLE_CONFIG_INTERFACE MsixIf = (PPCI_MSIX_TABLE_CONFIG_INTERFACE)Interface;
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(InterfaceData);

    ASSERT_PDO(PdoExtension);
    if (Version != PCI_MSIX_TABLE_CONFIG_INTERFACE_VERSION) return STATUS_NOINTERFACE;
    if (!PdoExtension->MsixCapabilityOffset) return STATUS_NOT_SUPPORTED;
    if (Size < PCI_MSIX_TABLE_CONFIG_MINIMUM_SIZE) return STATUS_INFO_LENGTH_MISMATCH;

    MsixIf->Context = PdoExtension;
    MsixIf->InterfaceReference =    (PINTERFACE_REFERENCE)PciMsiXInterface_RefDeref;
    MsixIf->InterfaceDereference =  (PINTERFACE_DEREFERENCE)PciMsiXInterface_RefDeref;
    MsixIf->SetTableEntry =         (PPCI_MSIX_SET_ENTRY)PciMsiXInterface_SetTableEntry;
    MsixIf->MaskTableEntry =        (PPCI_MSIX_MASKUNMASK_ENTRY)PciMsiXInterface_MaskTableEntry;
    MsixIf->UnmaskTableEntry =      (PPCI_MSIX_MASKUNMASK_ENTRY)PciMsiXInterface_UnmaskTableEntry;
    MsixIf->GetTableEntry =         (PPCI_MSIX_GET_ENTRY)PciMsiXInterface_GetTableEntry;
    MsixIf->GetTableSize =          (PPCI_MSIX_GET_TABLE_SIZE)PciMsiXInterface_GetTableSize;
    return STATUS_SUCCESS;
}


