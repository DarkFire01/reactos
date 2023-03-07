/*
 * PROJECT:     Freeldr UEFI Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI stubs
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

#include <uefildr.h>

#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);
#define EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID \
  {0x2F707EBB,0x4A1A,0x11d4,\
    {0x9A,0x38,0x00,0x90,0x27,0x3F,0xC1,0x4D}}
extern EFI_SYSTEM_TABLE * GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;


BOOLEAN AcpiPresent = FALSE;

static PRSDP_DESCRIPTOR
FindAcpiBios(VOID)
{
   // EFI_GUID acpi1_guid = ACPI_TABLE_GUID;
    EFI_GUID acpi2_guid = EFI_ACPI_20_TABLE_GUID;
    RSDP_DESCRIPTOR* rsdp = NULL;

     for (unsigned int i = 0; i < GlobalSystemTable->NumberOfTableEntries; i++) {
        if (!memcmp(&GlobalSystemTable->ConfigurationTable[i].VendorGuid, &acpi2_guid, sizeof(EFI_GUID))) {
            rsdp = (RSDP_DESCRIPTOR*)GlobalSystemTable->ConfigurationTable[i].VendorTable;
            break;
        }
    }

    return rsdp;
}
extern  EFI_MEMORY_DESCRIPTOR* EfiMemoryMap;
extern  UINT32 FreeldrDescCount;

VOID
DetectAcpiBios(PCONFIGURATION_COMPONENT_DATA SystemKey, ULONG *BusNumber)
{
    PCONFIGURATION_COMPONENT_DATA BiosKey;
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptor;
    PRSDP_DESCRIPTOR Rsdp;
    PACPI_BIOS_DATA AcpiBiosData;
    ULONG TableSize;

    Rsdp = FindAcpiBios();

    if (Rsdp)
    {
        /* Set up the flag in the loader block */
        AcpiPresent = TRUE;

        /* Calculate the table size */
        TableSize = FreeldrDescCount * sizeof(BIOS_MEMORY_MAP) +
            sizeof(ACPI_BIOS_DATA) - sizeof(BIOS_MEMORY_MAP);

        /* Set 'Configuration Data' value */
        PartialResourceList =
            FrLdrHeapAlloc(sizeof(CM_PARTIAL_RESOURCE_LIST) + TableSize,
                           TAG_HW_RESOURCE_LIST);
        if (PartialResourceList == NULL)
        {
            ERR("Failed to allocate resource descriptor\n");
            return;
        }

        RtlZeroMemory(PartialResourceList, sizeof(CM_PARTIAL_RESOURCE_LIST) + TableSize);
        PartialResourceList->Version = 0;
        PartialResourceList->Revision = 0;
        PartialResourceList->Count = 1;

        PartialDescriptor = &PartialResourceList->PartialDescriptors[0];
        PartialDescriptor->Type = CmResourceTypeDeviceSpecific;
        PartialDescriptor->ShareDisposition = CmResourceShareUndetermined;
        PartialDescriptor->u.DeviceSpecificData.DataSize = TableSize;

        /* Fill the table */
        AcpiBiosData = (PACPI_BIOS_DATA)&PartialResourceList->PartialDescriptors[1];

        if (Rsdp->revision > 0)
        {
            TRACE("ACPI >1.0, using XSDT address\n");
            AcpiBiosData->RSDTAddress.QuadPart = Rsdp->xsdt_physical_address;
        }
        else
        {
            TRACE("ACPI 1.0, using RSDT address\n");
            AcpiBiosData->RSDTAddress.LowPart = Rsdp->rsdt_physical_address;
        }

        AcpiBiosData->Count = FreeldrDescCount;
        memcpy(AcpiBiosData->MemoryMap, EfiMemoryMap,
            FreeldrDescCount * sizeof(BIOS_MEMORY_MAP));

        TRACE("RSDT %p, data size %x\n", Rsdp->rsdt_physical_address,
            TableSize);

        /* Create new bus key */
        FldrCreateComponentKey(SystemKey,
                               AdapterClass,
                               MultiFunctionAdapter,
                               0x0,
                               0x0,
                               0xFFFFFFFF,
                               "ACPI BIOS",
                               PartialResourceList,
                               sizeof(CM_PARTIAL_RESOURCE_LIST) + TableSize,
                               &BiosKey);

        /* Increment bus number */
        (*BusNumber)++;
    }
}

BOOLEAN
PcFindPciBios(PPCI_REGISTRY_INFO BusData)
{
    EFI_STATUS Status;
    EFI_GUID guid = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
    EFI_HANDLE* handles = NULL;
     UINTN count;
         count = 0;
    Status = GlobalSystemTable->BootServices->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles);
    if (Status != EFI_SUCCESS)
    {
    }
//    Status = bs->LocateHandleBuffer(ByProtocol, &guid, NULL, &count, &handles);

      BusData->MajorRevision = 0x02;
      BusData->MinorRevision = 0x10;
      BusData->NoBuses = count;
      BusData->HardwareMechanism = 1;
        return TRUE;

}



static
PPCI_IRQ_ROUTING_TABLE
GetPciIrqRoutingTable(VOID)
{
    PPCI_IRQ_ROUTING_TABLE Table;
    PUCHAR Ptr;
    ULONG Sum;
    ULONG i;

    Table = (PPCI_IRQ_ROUTING_TABLE)0xF0000;
    while ((ULONG_PTR)Table < 0x100000)
    {
        if (Table->Signature == 'RIP$')
        {
            TRACE("Found signature\n");

            if (Table->TableSize < FIELD_OFFSET(PCI_IRQ_ROUTING_TABLE, Slot) ||
                Table->TableSize % 16 != 0)
            {
                ERR("Invalid routing table size (%u) at 0x%p. Continue searching...\n", Table->TableSize, Table);
                Table = (PPCI_IRQ_ROUTING_TABLE)((ULONG_PTR)Table + 0x10);
                continue;
            }

            Ptr = (PUCHAR)Table;
            Sum = 0;
            for (i = 0; i < Table->TableSize; i++)
            {
                Sum += Ptr[i];
            }

            if ((Sum & 0xFF) != 0)
            {
                ERR("Invalid routing table checksum (%#lx) at 0x%p. Continue searching...\n", Sum & 0xFF, Table);
            }
            else
            {
                TRACE("Valid checksum (%#lx): found routing table at 0x%p\n", Sum & 0xFF, Table);
                return Table;
            }
        }

        Table = (PPCI_IRQ_ROUTING_TABLE)((ULONG_PTR)Table + 0x10);
    }

    ERR("No valid routing table found!\n");

    return NULL;
}

static
VOID
DetectPciIrqRoutingTable(PCONFIGURATION_COMPONENT_DATA BusKey)
{
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptor;
    PPCI_IRQ_ROUTING_TABLE Table;
    PCONFIGURATION_COMPONENT_DATA TableKey;
    ULONG Size;

    Table = GetPciIrqRoutingTable();
    if (Table != NULL)
    {
        TRACE("Table size: %u\n", Table->TableSize);

        /* Set 'Configuration Data' value */
        Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors) +
               2 * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) + Table->TableSize;
        PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
        if (PartialResourceList == NULL)
        {
            ERR("Failed to allocate resource descriptor\n");
            return;
        }

        /* Initialize resource descriptor */
        RtlZeroMemory(PartialResourceList, Size);
        PartialResourceList->Version = 1;
        PartialResourceList->Revision = 1;
        PartialResourceList->Count = 2;

        PartialDescriptor = &PartialResourceList->PartialDescriptors[0];
        PartialDescriptor->Type = CmResourceTypeBusNumber;
        PartialDescriptor->ShareDisposition = CmResourceShareDeviceExclusive;
        PartialDescriptor->u.BusNumber.Start = 0;
        PartialDescriptor->u.BusNumber.Length = 1;

        PartialDescriptor = &PartialResourceList->PartialDescriptors[1];
        PartialDescriptor->Type = CmResourceTypeDeviceSpecific;
        PartialDescriptor->ShareDisposition = CmResourceShareUndetermined;
        PartialDescriptor->u.DeviceSpecificData.DataSize = Table->TableSize;

        memcpy(&PartialResourceList->PartialDescriptors[2],
               Table,
               Table->TableSize);

            FldrCreateComponentKey(BusKey,
                               PeripheralClass,
                               RealModeIrqRoutingTable,
                               0x0,
                               0x0,
                               0xFFFFFFFF,
                               "PCI Real-mode IRQ Routing Table",
                               PartialResourceList,
                               Size,
                               &TableKey);
    }
}


VOID
DetectPci(PCONFIGURATION_COMPONENT_DATA SystemKey, ULONG *BusNumber)
{
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptor;
    PCI_REGISTRY_INFO BusData;
    PCONFIGURATION_COMPONENT_DATA BiosKey;
    ULONG Size;
    PCONFIGURATION_COMPONENT_DATA BusKey;
    ULONG i;

PcFindPciBios(&BusData);
     /* Set 'Configuration Data' value */
        Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors);
        PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
        if (PartialResourceList == NULL)
        {
            ERR("Failed to allocate resource descriptor\n");
            return;
        }

        /* Initialize resource descriptor */
        RtlZeroMemory(PartialResourceList, Size);

        /* Create new bus key */
        FldrCreateComponentKey(SystemKey,
                               AdapterClass,
                               MultiFunctionAdapter,
                               0x0,
                               0x0,
                               0xFFFFFFFF,
                               "PCI BIOS",
                               PartialResourceList,
                               Size,
                               &BiosKey);

        /* Increment bus number */
        (*BusNumber)++;

        DetectPciIrqRoutingTable(BiosKey);

        /* Report PCI buses */
        for (i = 0; i < (ULONG)BusData.NoBuses; i++)
        {
            /* Check if this is the first bus */
            if (i == 0)
            {
                /* Set 'Configuration Data' value */
                Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST,
                                    PartialDescriptors) +
                       sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) +
                       sizeof(PCI_REGISTRY_INFO);
                PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
                if (!PartialResourceList)
                {
                    ERR("Failed to allocate resource descriptor! Ignoring remaining PCI buses. (i = %lu, NoBuses = %lu)\n",
                        i, (ULONG)BusData.NoBuses);
                    return;
                }

                /* Initialize resource descriptor */
                RtlZeroMemory(PartialResourceList, Size);
                PartialResourceList->Version = 1;
                PartialResourceList->Revision = 1;
                PartialResourceList->Count = 1;
                PartialDescriptor = &PartialResourceList->PartialDescriptors[0];
                PartialDescriptor->Type = CmResourceTypeDeviceSpecific;
                PartialDescriptor->ShareDisposition = CmResourceShareUndetermined;
                PartialDescriptor->u.DeviceSpecificData.DataSize = sizeof(PCI_REGISTRY_INFO);
                memcpy(&PartialResourceList->PartialDescriptors[1],
                       &BusData,
                       sizeof(PCI_REGISTRY_INFO));
            }
            else
            {
                /* Set 'Configuration Data' value */
                Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST,
                                    PartialDescriptors);
                PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
                if (!PartialResourceList)
                {
                    ERR("Failed to allocate resource descriptor! Ignoring remaining PCI buses. (i = %lu, NoBuses = %lu)\n",
                        i, (ULONG)BusData.NoBuses);
                    return;
                }

                /* Initialize resource descriptor */
                RtlZeroMemory(PartialResourceList, Size);
            }

            /* Create the bus key */
            FldrCreateComponentKey(SystemKey,
                                   AdapterClass,
                                   MultiFunctionAdapter,
                                   0x0,
                                   0x0,
                                   0xFFFFFFFF,
                                   "PCI",
                                   PartialResourceList,
                                   Size,
                                   &BusKey);

            /* Increment bus number */
            (*BusNumber)++;
        }

}

PCONFIGURATION_COMPONENT_DATA
UefiHwDetect(VOID)
{
   PCONFIGURATION_COMPONENT_DATA SystemKey;
    ULONG BusNumber = 0;

    TRACE("DetectHardware()\n");

    /* Create the 'System' key */
    FldrCreateSystemKey(&SystemKey);
    // TODO: Discover and set the other machine types
    FldrSetIdentifier(SystemKey, "AT/AT COMPATIBLE");


    /* Detect buses */
    DetectPci(SystemKey, &BusNumber);
   // DetectAcpiBios(SystemKey, &BusNumber);

    // TODO: Collect the ROM blocks from 0xC0000 to 0xF0000 and append their
    // CM_ROM_BLOCK data into the 'System' key's configuration data.

    TRACE("DetectHardware() Done\n");
    return SystemKey;
}
