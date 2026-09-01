/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/pci/ecam.c
 * PURPOSE:         PCI Express Enhanced Configuration Access (ECAM)
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/*
 * The legacy 0xCF8/0xCFC mechanism can only reach the first 256 bytes of a
 * function's configuration space, which is where the PCI Express extended
 * capabilities stop being visible. Those live from 0x100 to 0xFFF and are only
 * reachable through the enhanced mechanism, where the firmware maps every
 * function's configuration space into one MMIO aperture: a megabyte per bus,
 * 32KB per device, 4KB per function.
 *
 * The aperture is described by the ACPI MCFG table, which this driver cannot
 * read directly. The HAL parses it at boot and republishes each window under
 * Arbiters\ReservedResources\MmConfigRange so the resource arbiters never hand
 * the aperture out to a device; that published window is what is read back
 * here. It records where a window starts and how many buses it covers, but not
 * which bus number the first megabyte belongs to. That is very nearly always
 * bus zero, so it is assumed to be - and then checked, by reading known
 * registers back through both mechanisms and comparing. A window that does not
 * agree is discarded rather than trusted, leaving the driver on the legacy
 * mechanism with no extended capabilities instead of quietly reading the wrong
 * function's configuration space.
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Set once the aperture has been found, mapped and verified */
BOOLEAN PciEcamEnabled;

/* The physical aperture, as published by the HAL */
static PHYSICAL_ADDRESS PciEcamPhysicalBase;
static ULONG PciEcamBusCount;

/* Whether the published window has been looked for yet */
static BOOLEAN PciEcamWindowSearched;

/* One mapped megabyte per bus, filled in as each bus is discovered */
static PVOID PciEcamBusMapping[PCI_MAX_BRIDGE_NUMBER + 1];

/* FUNCTIONS ******************************************************************/

/*
 * Read the aperture the HAL published out of the registry. Only the first
 * window is taken: this driver has no way to tell segments apart, so a machine
 * with more than one of them keeps the legacy mechanism.
 */
static
NTSTATUS
NTAPI
PciFindEcamWindow(VOID)
{
    UNICODE_STRING KeyName, ValueName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    PKEY_VALUE_PARTIAL_INFORMATION ValueInfo;
    PIO_RESOURCE_REQUIREMENTS_LIST List;
    PIO_RESOURCE_DESCRIPTOR Descriptor;
    HANDLE KeyHandle;
    NTSTATUS Status;
    ULONG Length, ResultLength;
    ULONGLONG WindowLength;
    PAGED_CODE();

    RtlInitUnicodeString(&KeyName,
                         L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet"
                         L"\\Control\\Arbiters\\ReservedResources");
    InitializeObjectAttributes(&ObjectAttributes,
                               &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    Status = ZwOpenKey(&KeyHandle, KEY_READ, &ObjectAttributes);
    if (!NT_SUCCESS(Status)) return Status;

    /* Ask for the size first, since the list grows with the window count */
    RtlInitUnicodeString(&ValueName, L"MmConfigRange");
    Status = ZwQueryValueKey(KeyHandle,
                             &ValueName,
                             KeyValuePartialInformation,
                             NULL,
                             0,
                             &ResultLength);
    if ((Status != STATUS_BUFFER_TOO_SMALL) && (Status != STATUS_BUFFER_OVERFLOW))
    {
        ZwClose(KeyHandle);
        return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;
    }

    Length = ResultLength;
    ValueInfo = ExAllocatePoolWithTag(PagedPool, Length, PCI_POOL_TAG);
    if (!ValueInfo)
    {
        ZwClose(KeyHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ZwQueryValueKey(KeyHandle,
                             &ValueName,
                             KeyValuePartialInformation,
                             ValueInfo,
                             Length,
                             &ResultLength);
    ZwClose(KeyHandle);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(ValueInfo, 0);
        return Status;
    }

    /* Make sure there is a whole list, with at least one window in it */
    Status = STATUS_UNSUCCESSFUL;
    if ((ValueInfo->Type == REG_RESOURCE_REQUIREMENTS_LIST) &&
        (ValueInfo->DataLength >= sizeof(IO_RESOURCE_REQUIREMENTS_LIST)))
    {
        List = (PIO_RESOURCE_REQUIREMENTS_LIST)ValueInfo->Data;
        if ((List->AlternativeLists >= 1) && (List->List[0].Count >= 1))
        {
            Descriptor = &List->List[0].Descriptors[0];
            if (Descriptor->Type == CmResourceTypeMemory)
            {
                WindowLength = Descriptor->u.Memory.MaximumAddress.QuadPart -
                               Descriptor->u.Memory.MinimumAddress.QuadPart + 1;

                /* One megabyte of configuration space per bus */
                PciEcamPhysicalBase = Descriptor->u.Memory.MinimumAddress;
                PciEcamBusCount = (ULONG)(WindowLength >> 20);
                if ((PciEcamBusCount) &&
                    (PciEcamBusCount <= (PCI_MAX_BRIDGE_NUMBER + 1)))
                {
                    DPRINT1("PCI - ECAM window at 0x%I64x covers %u bus(es)\n",
                            PciEcamPhysicalBase.QuadPart,
                            PciEcamBusCount);
                    Status = STATUS_SUCCESS;
                }
            }
        }
    }

    ExFreePoolWithTag(ValueInfo, 0);
    return Status;
}

/*
 * Map the megabyte of configuration space belonging to one bus. The mapping is
 * kept for the life of the driver, because a configuration access can arrive at
 * any IRQL and mapping is only legal at PASSIVE_LEVEL.
 */
static
PVOID
NTAPI
PciMapEcamBus(IN ULONG Bus)
{
    PHYSICAL_ADDRESS Address;
    PVOID Mapping;
    PAGED_CODE();

    /* Buses past the end of the published window are not covered by it */
    if (Bus >= PciEcamBusCount) return NULL;
    if (PciEcamBusMapping[Bus]) return PciEcamBusMapping[Bus];

    Address.QuadPart = PciEcamPhysicalBase.QuadPart + ((ULONGLONG)Bus << 20);
    Mapping = MmMapIoSpace(Address, 1 << 20, MmNonCached);
    if (!Mapping)
    {
        DPRINT1("PCI - failed to map ECAM for bus 0x%x\n", Bus);
        return NULL;
    }

    PciEcamBusMapping[Bus] = Mapping;
    return Mapping;
}

/*
 * Work out where one function's configuration space sits inside the mapped
 * aperture, or NULL when the enhanced mechanism cannot reach it.
 */
static
PUCHAR
NTAPI
PciEcamAddress(IN ULONG Bus,
               IN PCI_SLOT_NUMBER Slot,
               IN ULONG Offset)
{
    PUCHAR BusMapping;

    if (Bus > PCI_MAX_BRIDGE_NUMBER) return NULL;
    if (Offset >= PCI_EXTENDED_CONFIG_LENGTH) return NULL;

    BusMapping = PciEcamBusMapping[Bus];
    if (!BusMapping) return NULL;

    /* 32KB per device, 4KB per function */
    return BusMapping +
           (Slot.u.bits.DeviceNumber << 15) +
           (Slot.u.bits.FunctionNumber << 12) +
           Offset;
}

/*
 * Move a run of bytes to or from configuration space through the aperture.
 * Configuration space only answers naturally aligned accesses, so the run is
 * split into an unaligned head, a body of whole dwords, and a trailing tail.
 */
static
VOID
NTAPI
PciEcamTransfer(IN PUCHAR Address,
                IN PUCHAR Buffer,
                IN ULONG Length,
                IN BOOLEAN Read)
{
    while ((Length) && ((ULONG_PTR)Address & 3))
    {
        if (Read)
        {
            *Buffer = READ_REGISTER_UCHAR(Address);
        }
        else
        {
            WRITE_REGISTER_UCHAR(Address, *Buffer);
        }

        Address++;
        Buffer++;
        Length--;
    }

    while (Length >= sizeof(ULONG))
    {
        if (Read)
        {
            *(PULONG)Buffer = READ_REGISTER_ULONG((PULONG)Address);
        }
        else
        {
            WRITE_REGISTER_ULONG((PULONG)Address, *(PULONG)Buffer);
        }

        Address += sizeof(ULONG);
        Buffer += sizeof(ULONG);
        Length -= sizeof(ULONG);
    }

    while (Length)
    {
        if (Read)
        {
            *Buffer = READ_REGISTER_UCHAR(Address);
        }
        else
        {
            WRITE_REGISTER_UCHAR(Address, *Buffer);
        }

        Address++;
        Buffer++;
        Length--;
    }
}

/*
 * Compare what the aperture says about a bus against what the legacy mechanism
 * says about the same bus. They describe the same registers, so a window that
 * has been mapped at the right bus agrees on every function that answers.
 */
static
BOOLEAN
NTAPI
PciVerifyEcamBus(IN ULONG Bus)
{
    PCI_SLOT_NUMBER Slot;
    PUCHAR Address;
    ULONG Device, LegacyId, EcamId, Checked;
    PAGED_CODE();

    Slot.u.AsULONG = 0;
    Checked = 0;

    for (Device = 0; Device <= PCI_MAX_DEVICES - 1; Device++)
    {
        Slot.u.bits.DeviceNumber = Device;
        Slot.u.bits.FunctionNumber = 0;

        /* Ask the legacy mechanism what is in this slot */
        if (HalGetBusDataByOffset(PCIConfiguration,
                                  Bus,
                                  Slot.u.AsULONG,
                                  &LegacyId,
                                  0,
                                  sizeof(ULONG)) != sizeof(ULONG))
        {
            continue;
        }

        /* Empty slots read all ones and say nothing about the mapping */
        if ((LegacyId == 0xFFFFFFFF) || !(LegacyId)) continue;

        Address = PciEcamAddress(Bus, Slot, 0);
        if (!Address) return FALSE;

        EcamId = READ_REGISTER_ULONG((PULONG)Address);
        if (EcamId != LegacyId)
        {
            DPRINT1("PCI - ECAM mismatch on %02x:%02x.%x: %08lx vs %08lx\n",
                    Bus, Device, 0, EcamId, LegacyId);
            return FALSE;
        }

        Checked++;
    }

    /*
     * A bus with nothing on it proves nothing either way. Refusing it keeps the
     * driver on the legacy mechanism until a populated bus confirms the window.
     */
    if (!Checked)
    {
        DPRINT1("PCI - no devices on bus 0x%x to verify ECAM against\n", Bus);
        return FALSE;
    }

    DPRINT1("PCI - ECAM verified against %u device(s) on bus 0x%x\n", Checked, Bus);
    return TRUE;
}

VOID
NTAPI
PciInitializeEcam(IN PPCI_FDO_EXTENSION FdoExtension)
{
    ULONG Bus;
    PAGED_CODE();

    /* Find the published window once, on behalf of every bus */
    if (!PciEcamWindowSearched)
    {
        PciEcamWindowSearched = TRUE;
        if (!NT_SUCCESS(PciFindEcamWindow()))
        {
            DPRINT1("PCI - no ECAM window published, extended config unavailable\n");
            PciEcamBusCount = 0;
        }
    }

    if (!PciEcamBusCount) return;

    /* Map the space belonging to the bus this FDO is for */
    Bus = FdoExtension->BaseBus;
    if (!PciMapEcamBus(Bus)) return;

    /*
     * The window has to be proven right before anything is allowed to depend on
     * it, and that can only be done against a bus that has devices on it.
     */
    if (!PciEcamEnabled)
    {
        if (!PciVerifyEcamBus(Bus)) return;

        PciEcamEnabled = TRUE;
        DPRINT1("PCI - extended configuration space is available\n");
    }
}

BOOLEAN
NTAPI
PciEcamReadWriteConfig(IN ULONG Bus,
                       IN PCI_SLOT_NUMBER Slot,
                       IN PVOID Buffer,
                       IN ULONG Offset,
                       IN ULONG Length,
                       IN BOOLEAN Read)
{
    PUCHAR Address;

    /* Nothing can be served until a window has been found and proven */
    if (!PciEcamEnabled) return FALSE;

    /* The whole run has to land inside one function's configuration space */
    if ((Offset >= PCI_EXTENDED_CONFIG_LENGTH) ||
        (Length > (PCI_EXTENDED_CONFIG_LENGTH - Offset)))
    {
        return FALSE;
    }

    Address = PciEcamAddress(Bus, Slot, Offset);
    if (!Address) return FALSE;

    PciEcamTransfer(Address, Buffer, Length, Read);
    return TRUE;
}

/* EOF */
