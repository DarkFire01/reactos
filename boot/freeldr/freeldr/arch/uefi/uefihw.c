/*
 * PROJECT:     FreeLoader UEFI Support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Hardware detection routines
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */

/* INCLUDES ******************************************************************/

#include <uefildr.h>
#include "../vidfb.h"

#include <debug.h>
DBG_DEFAULT_CHANNEL(HWDETECT);

/* GLOBALS *******************************************************************/

extern EFI_SYSTEM_TABLE * GlobalSystemTable;
extern EFI_HANDLE GlobalImageHandle;

/* From uefivid.c */
extern ULONG_PTR VramAddress;
extern ULONG VramSize;
extern PCM_FRAMEBUF_DEVICE_DATA FrameBufferData;

BOOLEAN AcpiPresent = FALSE;
static EFI_EVENT IdleTimerEvent = NULL;
static PVOID SmbiosEntryPoint = NULL;
static ULONG SmbiosEntryPointLength = 0;
static BOOLEAN SmbiosSearched = FALSE;

/* FUNCTIONS *****************************************************************/

VOID
StallExecutionProcessor(ULONG Microseconds)
{
    GlobalSystemTable->BootServices->Stall(Microseconds);
}

VOID
UefiHwIdle(VOID)
{
    UINTN Index;
    EFI_STATUS Status;
    EFI_BOOT_SERVICES *BootServices = GlobalSystemTable->BootServices;

    /* Keep one timer event around and arm it each idle tick */
    if (IdleTimerEvent == NULL)
    {
        Status = BootServices->CreateEvent(EVT_TIMER,
                                           TPL_APPLICATION,
                                           NULL,
                                           NULL,
                                           &IdleTimerEvent);
        if (EFI_ERROR(Status))
        {
            StallExecutionProcessor(10000); /* 10 ms fallback */
            return;
        }
    }

    /* Set a 10ms (100,000 * 100ns) relative timer */
    Status = BootServices->SetTimer(IdleTimerEvent, TimerRelative, 100000);
    if (!EFI_ERROR(Status))
        Status = BootServices->WaitForEvent(1, &IdleTimerEvent, &Index);
    if (EFI_ERROR(Status))
        StallExecutionProcessor(10000); /* 10 ms fallback */
}

BOOLEAN IsAcpiPresent(VOID)
{
    return AcpiPresent;
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
        if (!memcmp(&GlobalSystemTable->ConfigurationTable[i].VendorGuid,
                    &acpi2_guid, sizeof(acpi2_guid)))
        {
            rsdp = (RSDP_DESCRIPTOR*)GlobalSystemTable->ConfigurationTable[i].VendorTable;
            break;
        }
    }

    return rsdp;
}

PDESCRIPTION_HEADER
UefiFindAcpiTable(
    _In_ ULONG Signature)
{
    UINTN Index, Count;
    PRSDP_DESCRIPTOR Rsdp;

    Rsdp = FindAcpiBios();
    if (Rsdp == NULL)
        return NULL;

    if ((Rsdp->revision > 0) && (Rsdp->xsdt_physical_address != 0))
    {
        PXSDT Xsdt = (PXSDT)(ULONG_PTR)Rsdp->xsdt_physical_address;

        if ((Xsdt != NULL) && (Xsdt->Header.Length >= sizeof(Xsdt->Header)))
        {
            Count = (Xsdt->Header.Length - sizeof(Xsdt->Header)) / sizeof(Xsdt->Tables[0]);
            for (Index = 0; Index < Count; ++Index)
            {
                PDESCRIPTION_HEADER Header =
                    (PDESCRIPTION_HEADER)(ULONG_PTR)Xsdt->Tables[Index].QuadPart;

                if ((Header != NULL) && (Header->Signature == Signature))
                    return Header;
            }
        }
    }

    if (Rsdp->rsdt_physical_address != 0)
    {
        PRSDT Rsdt = (PRSDT)(ULONG_PTR)Rsdp->rsdt_physical_address;

        if ((Rsdt != NULL) && (Rsdt->Header.Length >= sizeof(Rsdt->Header)))
        {
            Count = (Rsdt->Header.Length - sizeof(Rsdt->Header)) / sizeof(Rsdt->Tables[0]);
            for (Index = 0; Index < Count; ++Index)
            {
                PDESCRIPTION_HEADER Header =
                    (PDESCRIPTION_HEADER)(ULONG_PTR)Rsdt->Tables[Index];

                if ((Header != NULL) && (Header->Signature == Signature))
                    return Header;
            }
        }
    }

    return NULL;
}

/**
 * @brief
 * Adds up the bytes of an SMBIOS anchor, which is built to sum to zero.
 *
 * @param[in] Data
 * The anchor to add up.
 *
 * @param[in] Length
 * Length of @p Data, in bytes.
 *
 * @return
 * The sum of the bytes.
 */
static
UCHAR
UefiSmbiosChecksum(
    _In_reads_bytes_(Length) const UCHAR *Data,
    _In_ ULONG Length)
{
    UCHAR Checksum = 0;
    ULONG Index;

    for (Index = 0; Index < Length; Index++)
    {
        Checksum += Data[Index];
    }

    return Checksum;
}

/**
 * @brief
 * Picks the SMBIOS entry point out of the firmware configuration table.
 *
 * @param[out] Length
 * Receives the length of the entry point, in bytes.
 *
 * @return
 * The entry point, or NULL when the firmware published none that holds up.
 *
 * @remarks
 * A firmware that publishes both entry points describes the same structures
 * through either of them, but only the 3.0 one can name a table above 4GB,
 * so that is the one to take. The two anchors keep their length in different
 * places, which is why the caller is handed it rather than reading it.
 */
PVOID
UefiGetSmbiosEntryPoint(
    _Out_opt_ PULONG Length)
{
    static const EFI_GUID Smbios3Guid = SMBIOS3_TABLE_GUID;
    static const EFI_GUID SmbiosGuid = SMBIOS_TABLE_GUID;
    PSMBIOS3_TABLE_HEADER Entry3 = NULL;
    PSMBIOS_TABLE_HEADER Entry = NULL;
    UINTN Index;

    if (SmbiosSearched)
    {
        if (Length != NULL)
            *Length = SmbiosEntryPointLength;

        return SmbiosEntryPoint;
    }

    SmbiosSearched = TRUE;
    if (Length != NULL)
        *Length = 0;

    for (Index = 0; Index < GlobalSystemTable->NumberOfTableEntries; Index++)
    {
        EFI_CONFIGURATION_TABLE *Table = &GlobalSystemTable->ConfigurationTable[Index];

        if (!memcmp(&Table->VendorGuid, &Smbios3Guid, sizeof(Smbios3Guid)))
            Entry3 = Table->VendorTable;
        else if (!memcmp(&Table->VendorGuid, &SmbiosGuid, sizeof(SmbiosGuid)))
            Entry = Table->VendorTable;
    }

    if ((Entry3 != NULL) &&
        !memcmp(Entry3->Signature, "_SM3_", 5) &&
        (Entry3->Length >= sizeof(*Entry3)) &&
        (Entry3->Length <= sizeof(SMBIOS_TABLE_HEADER)) &&
        (UefiSmbiosChecksum((const UCHAR *)Entry3, Entry3->Length) == 0))
    {
        TRACE("SMBIOS %u.%u entry point at %p\n",
              Entry3->MajorVersion, Entry3->MinorVersion, Entry3);
        SmbiosEntryPoint = Entry3;
        SmbiosEntryPointLength = Entry3->Length;

        if (Length != NULL)
            *Length = SmbiosEntryPointLength;

        return SmbiosEntryPoint;
    }

    if ((Entry != NULL) &&
        !memcmp(Entry->Signature, "_SM_", 4) &&
        (Entry->Length >= FIELD_OFFSET(SMBIOS_TABLE_HEADER, Revision)) &&
        (Entry->Length <= sizeof(SMBIOS_TABLE_HEADER)) &&
        (UefiSmbiosChecksum((const UCHAR *)Entry, Entry->Length) == 0))
    {
        TRACE("SMBIOS %u.%u entry point at %p\n",
              Entry->MajorVersion, Entry->MinorVersion, Entry);
        SmbiosEntryPoint = Entry;
        SmbiosEntryPointLength = Entry->Length;

        if (Length != NULL)
            *Length = SmbiosEntryPointLength;

        return SmbiosEntryPoint;
    }

    WARN("The firmware published no usable SMBIOS entry point\n");
    return NULL;
}

/**
 * @brief
 * Reads where an entry point says the structure table sits.
 *
 * @param[in] EntryPoint
 * The entry point to read, as handed back by UefiGetSmbiosEntryPoint.
 *
 * @param[out] TableAddress
 * Receives the physical address of the structure table.
 *
 * @param[out] TableLength
 * Receives the length of the structure table, in bytes.
 *
 * @return
 * TRUE when the entry point names a table, FALSE otherwise.
 */
BOOLEAN
UefiGetSmbiosTableRange(
    _In_ PVOID EntryPoint,
    _Out_ PULONGLONG TableAddress,
    _Out_ PULONG TableLength)
{
    PSMBIOS3_TABLE_HEADER Entry3 = EntryPoint;
    PSMBIOS_TABLE_HEADER Entry = EntryPoint;

    *TableAddress = 0;
    *TableLength = 0;

    if (!memcmp(Entry3->Signature, "_SM3_", 5))
    {
        *TableAddress = Entry3->StructureTableAddress;
        *TableLength = Entry3->StructureTableMaximumSize;
    }
    else
    {
        *TableAddress = Entry->StructureTableAddress;
        *TableLength = Entry->StructureTableLength;
    }

    return ((*TableAddress != 0) && (*TableLength != 0));
}

VOID
DetectAcpiBios(PCONFIGURATION_COMPONENT_DATA SystemKey, ULONG *BusNumber)
{
    PCONFIGURATION_COMPONENT_DATA BiosKey;
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptor;
    PRSDP_DESCRIPTOR Rsdp;
    PACPI_BIOS_DATA AcpiBiosData;
    ULONG TableSize, Size;

    Rsdp = FindAcpiBios();

    if (Rsdp)
    {
        /* Set up the flag in the loader block */
        AcpiPresent = TRUE;

        /* Calculate the table size */
        TableSize = sizeof(ACPI_BIOS_DATA);

        /* Set 'Configuration Data' value */
        Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors[1]) + TableSize;
        PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
        if (PartialResourceList == NULL)
        {
            ERR("Failed to allocate resource descriptor\n");
            return;
        }

        RtlZeroMemory(PartialResourceList, Size);
        PartialResourceList->Version = 0;
        PartialResourceList->Revision = 0;
        PartialResourceList->Count = 1;

        PartialDescriptor = &PartialResourceList->PartialDescriptors[0];
        PartialDescriptor->Type = CmResourceTypeDeviceSpecific;
        PartialDescriptor->ShareDisposition = CmResourceShareUndetermined;
        PartialDescriptor->u.DeviceSpecificData.DataSize = TableSize;

        /* Fill the table */
        AcpiBiosData = (PACPI_BIOS_DATA)(PartialDescriptor + 1);

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

        AcpiBiosData->Count = 0;

        TRACE("RSDT %p, data size %x\n", Rsdp->rsdt_physical_address, TableSize);

        /* Create new bus key */
        FldrCreateComponentKey(SystemKey,
                               AdapterClass,
                               MultiFunctionAdapter,
                               0x0,
                               0x0,
                               0xFFFFFFFF,
                               "ACPI BIOS",
                               PartialResourceList,
                               Size,
                               &BiosKey);

        /* Increment bus number */
        (*BusNumber)++;
    }
}

static VOID
DetectDisplayController(
    _In_ PCONFIGURATION_COMPONENT_DATA BusKey)
{
    PCONFIGURATION_COMPONENT_DATA ControllerKey;
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR PartialDescriptor;
    PCM_FRAMEBUF_DEVICE_DATA FramebufData;
    ULONG Size;

    if (!VramAddress || (VramSize == 0) || !FrameBufferData)
        return;

    Size = FIELD_OFFSET(CM_PARTIAL_RESOURCE_LIST, PartialDescriptors[2]) + sizeof(*FramebufData);
    PartialResourceList = FrLdrHeapAlloc(Size, TAG_HW_RESOURCE_LIST);
    if (PartialResourceList == NULL)
    {
        ERR("Failed to allocate resource descriptor\n");
        return;
    }

    /* Initialize resource descriptor */
    RtlZeroMemory(PartialResourceList, Size);
    PartialResourceList->Version  = 1;
    PartialResourceList->Revision = 2;
    PartialResourceList->Count = 2;

    /* Set Memory */
    PartialDescriptor = &PartialResourceList->PartialDescriptors[0];
    PartialDescriptor->Type = CmResourceTypeMemory;
    PartialDescriptor->ShareDisposition = CmResourceShareDeviceExclusive;
    PartialDescriptor->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
    PartialDescriptor->u.Memory.Start.QuadPart = VramAddress;
    PartialDescriptor->u.Memory.Length = VramSize;

    /* Set framebuffer-specific data */
    PartialDescriptor = &PartialResourceList->PartialDescriptors[1];
    PartialDescriptor->Type = CmResourceTypeDeviceSpecific;
    PartialDescriptor->ShareDisposition = CmResourceShareUndetermined;
    PartialDescriptor->Flags = 0;
    PartialDescriptor->u.DeviceSpecificData.DataSize = sizeof(*FramebufData);

    /* Get pointer to framebuffer-specific data */
    FramebufData = (PCM_FRAMEBUF_DEVICE_DATA)(PartialDescriptor + 1);
    RtlCopyMemory(FramebufData, FrameBufferData, sizeof(*FrameBufferData));
    FramebufData->Version  = 1;
    FramebufData->Revision = 3;
    FramebufData->VideoClock = 0; // FIXME: Use EDID

    FldrCreateComponentKey(BusKey,
                           ControllerClass,
                           DisplayController,
                           Output | ConsoleOut,
                           0,
                           0xFFFFFFFF,
                           "UEFI GOP Framebuffer",
                           PartialResourceList,
                           Size,
                           &ControllerKey);

    // NOTE: Don't add a MonitorPeripheral for now.
    // We should use EDID data for it.
}

static
VOID
DetectInternal(PCONFIGURATION_COMPONENT_DATA SystemKey, ULONG *BusNumber)
{
    PCM_PARTIAL_RESOURCE_LIST PartialResourceList;
    PCONFIGURATION_COMPONENT_DATA BusKey;
    ULONG Size;

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
    PartialResourceList->Version  = 1;
    PartialResourceList->Revision = 1;
    PartialResourceList->Count = 0;

    /* Create new bus key */
    FldrCreateComponentKey(SystemKey,
                           AdapterClass,
                           MultiFunctionAdapter,
                           0,
                           0,
                           0xFFFFFFFF,
                           "UEFI Internal",
                           PartialResourceList,
                           Size,
                           &BusKey);

    /* Increment bus number */
    (*BusNumber)++;

    /* Detect devices that do not belong to "standard" buses */
    DetectDisplayController(BusKey);

    /* FIXME: Detect more devices */
}

PCONFIGURATION_COMPONENT_DATA
UefiHwDetect(
    _In_opt_ PCSTR Options)
{
    PCONFIGURATION_COMPONENT_DATA SystemKey;
    ULONG BusNumber = 0;

    TRACE("DetectHardware()\n");

    /* Create the 'System' key */
#if defined(_M_IX86) || defined(_M_AMD64)
    FldrCreateSystemKey(&SystemKey, "AT/AT COMPATIBLE");
#elif defined(_M_IA64)
    FldrCreateSystemKey(&SystemKey, "Intel Itanium processor family");
#elif defined(_M_ARM) || defined(_M_ARM64)
    FldrCreateSystemKey(&SystemKey, "ARM processor family");
#else
    #error Please define a system key for your architecture
#endif

    /* Detect buses */
    DetectInternal(SystemKey, &BusNumber);
    // TODO: DetectPciBus
    DetectAcpiBios(SystemKey, &BusNumber);

    TRACE("DetectHardware() Done\n");
    return SystemKey;
}
