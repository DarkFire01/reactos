/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Hardware detection routines
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES ******************************************************************/

#include <uefildr.h>
#include <drivers/bootvid/framebuf.h>
#include <debug.h>
DBG_DEFAULT_CHANNEL(WARNING);

/* GLOBALS *******************************************************************/

extern EFI_SYSTEM_TABLE * GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;
extern UCHAR PcBiosDiskCount;
extern EFI_MEMORY_DESCRIPTOR* EfiMemoryMap;
extern UINT32 FreeldrDescCount;

BOOLEAN AcpiPresent = FALSE;

/* FUNCTIONS *****************************************************************/
extern REACTOS_INTERNAL_BGCONTEXT framebufferDatatwo;

static VOID
DetectDisplayController(PCONFIGURATION_COMPONENT_DATA BusKey)
{
    PCONFIGURATION_COMPONENT_DATA ControllerKey;
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptor;
    PCM_FRAMEBUF_DEVICE_DATA FramebufferData;
    ULONG Size;


    Size = sizeof(CM_PARTIAL_RESOURCE_LIST) +
           2 * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR) +
           sizeof(CM_FRAMEBUF_DEVICE_DATA);
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
    PartialResourceList->Count = 3;

    /* Set Memory */
    PartialDescriptor = &PartialResourceList->PartialDescriptors[1];
    PartialDescriptor->Type = CmResourceTypeMemory;
    PartialDescriptor->ShareDisposition = CmResourceShareDeviceExclusive;
    PartialDescriptor->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
    PartialDescriptor->u.Memory.Start.QuadPart = ((ULONG_PTR)framebufferDatatwo.BaseAddress & 0x0FFFFFFF);
    PartialDescriptor->u.Memory.Length = framebufferDatatwo.BufferSize;

    /* Set framebuffer-specific data */
    PartialDescriptor = &PartialResourceList->PartialDescriptors[2];
    PartialDescriptor->Type = CmResourceTypeDeviceSpecific;
    PartialDescriptor->ShareDisposition = CmResourceShareUndetermined;
    PartialDescriptor->Flags = 0;
    PartialDescriptor->u.DeviceSpecificData.DataSize =
        sizeof(CM_FRAMEBUF_DEVICE_DATA);

    /* Get pointer to framebuffer-specific data */
    FramebufferData = (PVOID)(PartialDescriptor + 1);
    FramebufferData->Version  = 2; // ARC_VERSION;
    FramebufferData->Revision = 0; // ARC_REVISION;

    RtlZeroMemory(FramebufferData, sizeof(*FramebufferData));
    FramebufferData->VideoClock = 0; // FIXME

    /* NOTE: FrameBufferSize == PixelsPerScanLine x VerticalResolution x PixelElementSize */

    FramebufferData->ScreenWidth  = framebufferDatatwo.ScreenWidth;
    FramebufferData->ScreenHeight = framebufferDatatwo.ScreenHeight;

    /* Number of pixel elements per video memory line */
    // FramebufferData->PixelsPerScanLine;

    /* Physical format of the pixel */
    FramebufferData->BitsPerPixel /*PixelDepth*/ = (8 * 4);
    RtlZeroMemory(&FramebufferData->PixelInformation,
                  sizeof(FramebufferData->PixelInformation));

    FldrCreateComponentKey(BusKey,
                           ControllerClass,
                           DisplayController,
                           Output | ConsoleOut,
                           0,
                           0xFFFFFFFF,
                           "UEFI Framebuffer",
                           PartialResourceList,
                           Size,
                           &ControllerKey);
}


static
VOID
DetectIsaBios(PCONFIGURATION_COMPONENT_DATA SystemKey, ULONG *BusNumber)
{
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCONFIGURATION_COMPONENT_DATA BusKey;
    ULONG Size;

    /* Set 'Configuration Data' value */
    Size = sizeof(CM_PARTIAL_RESOURCE_LIST) -
           sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
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
    PartialResourceList->Count = 0;

    /* Create new bus key */
    FldrCreateComponentKey(SystemKey,
                           AdapterClass,
                           MultiFunctionAdapter,
                           0,
                           0,
                           0xFFFFFFFF,
                           "ISA",
                           PartialResourceList,
                           Size,
                           &BusKey);

    /* Increment bus number */
    (*BusNumber)++;
    DetectDisplayController(BusKey);
    /* Detect ISA/BIOS devices */
    //DetectBiosDisks(SystemKey, BusKey);
    //DetectSerialPorts(BusKey, XboxGetSerialPort, MAX_XBOX_COM_PORTS);
    // DetectDisplayController(BusKey);

    /* FIXME: Detect more ISA devices */
}


static
PRSDP_DESCRIPTOR
FindAcpiBios(VOID)
{
    UINTN i;
    RSDP_DESCRIPTOR* rsdp = NULL;
    EFI_GUID acpi2_guid = EFI_ACPI_20_TABLE_GUID;

    for (i = 0; i < GlobalSystemTable->NumberOfTableEntries; i++)
    {
        if (!memcmp(&GlobalSystemTable->ConfigurationTable[i].VendorGuid, &acpi2_guid, sizeof(acpi2_guid)))
        {
            rsdp = (RSDP_DESCRIPTOR*)GlobalSystemTable->ConfigurationTable[i].VendorTable;
            break;
        }
    }

    return rsdp;
}

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
        PartialResourceList = FrLdrHeapAlloc(sizeof(CM_PARTIAL_RESOURCE_LIST) +
                                             TableSize, TAG_HW_RESOURCE_LIST);
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

PCONFIGURATION_COMPONENT_DATA
UefiHwDetect(VOID)
{
    PCONFIGURATION_COMPONENT_DATA SystemKey;
    ULONG BusNumber = 0;

    TRACE("DetectHardware()\n");

    /* Create the 'System' key */
    FldrCreateSystemKey(&SystemKey, "AT/AT COMPATIBLE");
#if defined(_M_IX86) || defined(_M_AMD64) /* Taken from Windows 11 */
  //  FldrSetIdentifier(SystemKey, "AT/AT COMPATIBLE");
#elif defined(_M_IA64) /* Taken from Windows XP 64-bit*/
    FldrSetIdentifier(SystemKey, "Intel Itanium processor family");
#elif defined(_M_ARM) || defined(_M_ARM64) /* Taken from Windows RT/11 */
    FldrSetIdentifier(SystemKey, "ARM processor family");
#else
    #error Please define a system key for your architecture
#endif

    /* Detect ACPI */
    DetectAcpiBios(SystemKey, &BusNumber);
    DetectIsaBios(SystemKey, &BusNumber);

    TRACE("DetectHardware() Done\n");
    return SystemKey;
}
