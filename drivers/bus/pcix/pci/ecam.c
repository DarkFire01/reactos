/*
 * PROJECT:     ReactOS PCI Bus Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Basic ECAM Handling
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <pci.h>
#include <ndk/exfuncs.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

#define PCI_ECAM_BUS_COUNT  (PCI_MAX_BRIDGE_NUMBER + 1)
#define PCI_ECAM_BUS_SIZE   (PCI_MAX_DEVICES * PCI_MAX_FUNCTION * PCI_EXTENDED_CONFIG_LENGTH)
#define PCI_ECAM_MAX_WINDOWS    16

/* 'ACPI' provider and the 'MCFG' table, as GetSystemFirmwareTable spells them */
#define PCI_FIRMWARE_PROVIDER_ACPI  0x41435049
#define PCI_FIRMWARE_TABLE_MCFG     0x4746434D

/* MCFG: the common header, 8 reserved bytes, then one entry per window */
#define PCI_MCFG_ENTRIES_OFFSET     44

#include <pshpack1.h>
typedef struct _PCI_ECAM_MCFG_ENTRY
{
    ULONGLONG Base;     ///< address of bus 0 in this segment
    USHORT Segment;
    UCHAR StartBus;
    UCHAR EndBus;
    ULONG Reserved;
} PCI_ECAM_MCFG_ENTRY, *PPCI_ECAM_MCFG_ENTRY;
#include <poppack.h>

typedef struct _PCI_ECAM_WINDOW
{
    ULONGLONG Base;     ///< address of bus 0 in this segment
    USHORT Segment;
    UCHAR StartBus;
    UCHAR EndBus;
    PUCHAR BusView[PCI_ECAM_BUS_COUNT];
} PCI_ECAM_WINDOW, *PPCI_ECAM_WINDOW;

/* Segment 0 is compared against the legacy mechanism, no other segment has one */
BOOLEAN PciEcamVerified = FALSE;

static BOOLEAN PciEcamWindowQueried = FALSE;
static BOOLEAN PciEcamSkipK8Northbridge = FALSE;
static PCI_ECAM_WINDOW PciEcamWindows[PCI_ECAM_MAX_WINDOWS];
static ULONG PciEcamWindowCount;

/* FUNCTIONS ******************************************************************/

static
PPCI_ECAM_WINDOW
NTAPI
PciEcamFindWindow(
    _In_ USHORT Segment,
    _In_ ULONG Bus)
{
    ULONG Index;

    for (Index = 0; Index < PciEcamWindowCount; Index++)
    {
        if ((PciEcamWindows[Index].Segment == Segment) &&
            (Bus >= PciEcamWindows[Index].StartBus) &&
            (Bus <= PciEcamWindows[Index].EndBus))
        {
            return &PciEcamWindows[Index];
        }
    }

    return NULL;
}

/* Reads MCFG through the firmware table provider, the ACPI driver registers it */
static
BOOLEAN
NTAPI
PciEcamQueryMcfg(VOID)
{
    PSYSTEM_FIRMWARE_TABLE_INFORMATION Info;
    SYSTEM_FIRMWARE_TABLE_INFORMATION Probe;
    PPCI_ECAM_MCFG_ENTRY Entry;
    PDESCRIPTION_HEADER Header;
    ULONG Size = 0;
    ULONG Offset;
    NTSTATUS Status;
    PAGED_CODE();

    RtlZeroMemory(&Probe, sizeof(Probe));
    Probe.ProviderSignature = PCI_FIRMWARE_PROVIDER_ACPI;
    Probe.Action = SystemFirmwareTable_Get;
    Probe.TableID = PCI_FIRMWARE_TABLE_MCFG;

    Status = ZwQuerySystemInformation(SystemFirmwareTableInformation,
                                      &Probe,
                                      sizeof(Probe),
                                      &Size);
    if ((Status != STATUS_BUFFER_TOO_SMALL) ||
        (Size < FIELD_OFFSET(SYSTEM_FIRMWARE_TABLE_INFORMATION, TableBuffer) +
                PCI_MCFG_ENTRIES_OFFSET))
    {
        return FALSE;
    }

    Info = ExAllocatePoolWithTag(PagedPool, Size, PCI_POOL_TAG);
    if (!Info) return FALSE;

    RtlCopyMemory(Info, &Probe, FIELD_OFFSET(SYSTEM_FIRMWARE_TABLE_INFORMATION, TableBuffer));
    Info->TableBufferLength = Size - FIELD_OFFSET(SYSTEM_FIRMWARE_TABLE_INFORMATION, TableBuffer);

    Status = ZwQuerySystemInformation(SystemFirmwareTableInformation, Info, Size, &Size);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Info, PCI_POOL_TAG);
        return FALSE;
    }

    Header = (PDESCRIPTION_HEADER)Info->TableBuffer;
    if (Header->Length > Info->TableBufferLength)
    {
        ExFreePoolWithTag(Info, PCI_POOL_TAG);
        return FALSE;
    }

    for (Offset = PCI_MCFG_ENTRIES_OFFSET;
         (Offset + sizeof(*Entry) <= Header->Length) &&
         (PciEcamWindowCount < PCI_ECAM_MAX_WINDOWS);
         Offset += sizeof(*Entry))
    {
        Entry = (PPCI_ECAM_MCFG_ENTRY)((PUCHAR)Header + Offset);
        if (Entry->EndBus < Entry->StartBus) continue;

        PciEcamWindows[PciEcamWindowCount].Base = Entry->Base;
        PciEcamWindows[PciEcamWindowCount].Segment = Entry->Segment;
        PciEcamWindows[PciEcamWindowCount].StartBus = Entry->StartBus;
        PciEcamWindows[PciEcamWindowCount].EndBus = Entry->EndBus;
        PciEcamWindowCount++;
    }

    ExFreePoolWithTag(Info, PCI_POOL_TAG);
    return (PciEcamWindowCount != 0);
}

/* Without the provider only the first range is known, the HAL writes segment 0 first */
static
BOOLEAN
NTAPI
PciEcamParseWindow(
    _In_ PKEY_VALUE_PARTIAL_INFORMATION Value)
{
    PIO_RESOURCE_REQUIREMENTS_LIST Requirements;
    PIO_RESOURCE_DESCRIPTOR Window;
    ULONGLONG Start, End, Buses;
    PAGED_CODE();

    if ((Value->Type != REG_RESOURCE_REQUIREMENTS_LIST) ||
        (Value->DataLength < sizeof(IO_RESOURCE_REQUIREMENTS_LIST)))
    {
        return FALSE;
    }

    Requirements = (PIO_RESOURCE_REQUIREMENTS_LIST)Value->Data;
    if (!(Requirements->AlternativeLists) || !(Requirements->List[0].Count)) return FALSE;

    Window = &Requirements->List[0].Descriptors[0];
    if ((Window->Type != CmResourceTypeMemory) &&
        (Window->Type != CmResourceTypeMemoryLarge))
    {
        return FALSE;
    }

    Start = (ULONGLONG)Window->u.Memory.MinimumAddress.QuadPart;
    End = (ULONGLONG)Window->u.Memory.MaximumAddress.QuadPart;
    if (End < Start) return FALSE;

    Buses = (End - Start + 1) / PCI_ECAM_BUS_SIZE;
    if (!(Buses) || (Buses > PCI_ECAM_BUS_COUNT)) return FALSE;

    PciEcamWindows[0].Base = Start;
    PciEcamWindows[0].Segment = 0;
    PciEcamWindows[0].StartBus = 0;
    PciEcamWindows[0].EndBus = (UCHAR)(Buses - 1);
    PciEcamWindowCount = 1;
    return TRUE;
}

/* The HAL publishes the MCFG windows here so the arbiters keep them reserved */
static
NTSTATUS
NTAPI
PciEcamQueryWindow(VOID)
{
    PKEY_VALUE_PARTIAL_INFORMATION Value;
    UNICODE_STRING ValueName;
    HANDLE KeyHandle;
    NTSTATUS Status;
    ULONG Size;
    PAGED_CODE();

    /* MCFG names every segment, the published ranges only cover segment 0 */
    if (PciEcamQueryMcfg()) return STATUS_SUCCESS;

    if (!PciOpenKey(L"\\Registry\\Machine\\System\\CurrentControlSet"
                    L"\\Control\\Arbiters\\ReservedResources",
                    NULL,
                    KEY_QUERY_VALUE,
                    &KeyHandle,
                    &Status))
    {
        return Status;
    }

    RtlInitUnicodeString(&ValueName, L"MmConfigRange");
    Status = ZwQueryValueKey(KeyHandle,
                             &ValueName,
                             KeyValuePartialInformation,
                             NULL,
                             0,
                             &Size);
    if ((Status != STATUS_BUFFER_TOO_SMALL) && (Status != STATUS_BUFFER_OVERFLOW))
    {
        ZwClose(KeyHandle);
        return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;
    }

    Value = ExAllocatePoolWithTag(PagedPool, Size, PCI_POOL_TAG);
    if (!Value)
    {
        ZwClose(KeyHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = ZwQueryValueKey(KeyHandle,
                             &ValueName,
                             KeyValuePartialInformation,
                             Value,
                             Size,
                             &Size);
    ZwClose(KeyHandle);

    if ((NT_SUCCESS(Status)) && !(PciEcamParseWindow(Value)))
    {
        Status = STATUS_UNSUCCESSFUL;
    }

    ExFreePoolWithTag(Value, 0);
    return Status;
}

#if defined(_M_IX86) || defined(_M_AMD64)
/*
 * K8 northbridge functions on bus 0 are not decoded by the chipset ECAM window.
 * This really should be a hardware quirk so I'm marking this as TODO:
 * However I want to be able to boot until we got a solid system for this!
 */
static
BOOLEAN
NTAPI
PciEcamIsAmdK8(VOID)
{
    INT CpuInfo[4];

    /* "AuthenticAMD" */
    __cpuid(CpuInfo, 0);
    if ((CpuInfo[1] != 0x68747541) ||
        (CpuInfo[3] != 0x69746E65) ||
        (CpuInfo[2] != 0x444D4163))
    {
        return FALSE;
    }

    /* Family 0Fh with no extended family */
    __cpuid(CpuInfo, 1);
    return ((CpuInfo[0] & 0x0FF00F00) == 0x00000F00);
}
#endif

/* Mappings are kept for the life of the driver since config access can happen at any IRQL */
static
PUCHAR
NTAPI
PciEcamMapBus(
    _In_ USHORT Segment,
    _In_ ULONG Bus)
{
    PPCI_ECAM_WINDOW Window;
    PHYSICAL_ADDRESS Address;
    PAGED_CODE();

    Window = PciEcamFindWindow(Segment, Bus);
    if (!Window) return NULL;
    if (Window->BusView[Bus]) return Window->BusView[Bus];

    Address.QuadPart = (LONGLONG)(Window->Base + ((ULONGLONG)Bus * PCI_ECAM_BUS_SIZE));
    Window->BusView[Bus] = MmMapIoSpace(Address, PCI_ECAM_BUS_SIZE, MmNonCached);
    if (!Window->BusView[Bus])
    {
        DPRINT1("PCIX - Unable to map ECAM for segment 0x%x bus 0x%lx\n", Segment, Bus);
    }

    return Window->BusView[Bus];
}

static
VOID
NTAPI
PciEcamDiscardWindow(VOID)
{
    PPCI_ECAM_WINDOW Window;
    ULONG Index;
    ULONG Bus;
    PAGED_CODE();

    for (Index = 0; Index < PciEcamWindowCount; Index++)
    {
        Window = &PciEcamWindows[Index];
        for (Bus = 0; Bus < PCI_ECAM_BUS_COUNT; Bus++)
        {
            if (!Window->BusView[Bus]) continue;

            MmUnmapIoSpace(Window->BusView[Bus], PCI_ECAM_BUS_SIZE);
            Window->BusView[Bus] = NULL;
        }
    }

    PciEcamWindowCount = 0;
}

static
PUCHAR
NTAPI
PciEcamFunctionBase(
    _In_ USHORT Segment,
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot)
{
    PPCI_ECAM_WINDOW Window;
    ULONG Index;

    Window = PciEcamFindWindow(Segment, Bus);
    if (!Window || !(Window->BusView[Bus])) return NULL;

    if ((PciEcamSkipK8Northbridge) &&
        (Segment == 0) &&
        (Bus == 0) &&
        (Slot.u.bits.DeviceNumber >= 0x18))
    {
        return NULL;
    }

    Index = (Slot.u.bits.DeviceNumber * PCI_MAX_FUNCTION) + Slot.u.bits.FunctionNumber;
    return Window->BusView[Bus] + (Index * PCI_EXTENDED_CONFIG_LENGTH);
}

/* Picks the same access widths as the HAL does for the legacy mechanism */
static
VOID
NTAPI
PciEcamTransfer(
    _In_ PUCHAR Function,
    _Inout_updates_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ BOOLEAN Read)
{
    PUCHAR Register;
    ULONG Width;

    while (Length)
    {
        Register = Function + Offset;

        if ((Offset & 1) || ((Length & 3) == 1))
        {
            Width = sizeof(UCHAR);
            if (Read)
            {
                *Buffer = READ_REGISTER_UCHAR(Register);
            }
            else
            {
                WRITE_REGISTER_UCHAR(Register, *Buffer);
            }
        }
        else if (!(Offset & 3) && !(Length & 3))
        {
            Width = sizeof(ULONG);
            if (Read)
            {
                *(PULONG)Buffer = READ_REGISTER_ULONG((PULONG)Register);
            }
            else
            {
                WRITE_REGISTER_ULONG((PULONG)Register, *(PULONG)Buffer);
            }
        }
        else
        {
            Width = sizeof(USHORT);
            if (Read)
            {
                *(PUSHORT)Buffer = READ_REGISTER_USHORT((PUSHORT)Register);
            }
            else
            {
                WRITE_REGISTER_USHORT((PUSHORT)Register, *(PUSHORT)Buffer);
            }
        }

        Buffer += Width;
        Offset += Width;
        Length -= Width;
    }
}

/* The window is assumed to start at bus 0, so compare IDs through both mechanisms to prove it */
static
NTSTATUS
NTAPI
PciEcamVerifyBus(
    _In_ PPCI_FDO_EXTENSION FdoExtension)
{
    PCI_SLOT_NUMBER Slot;
    PUCHAR Function;
    ULONG Device, LegacyIds, EcamIds, Matched;
    PAGED_CODE();

    Slot.u.AsULONG = 0;
    Matched = 0;

    for (Device = 0; Device < PCI_MAX_DEVICES; Device++)
    {
        Slot.u.bits.DeviceNumber = Device;

        Function = PciEcamFunctionBase(0, FdoExtension->BaseBus, Slot);
        if (!Function) continue;

        LegacyIds = MAXULONG;
        PciReadSlotConfig(FdoExtension, Slot, &LegacyIds, 0, sizeof(ULONG));
        if (((USHORT)LegacyIds == PCI_INVALID_VENDORID) || !((USHORT)LegacyIds)) continue;

        EcamIds = READ_REGISTER_ULONG((PULONG)Function);
        if (EcamIds != LegacyIds)
        {
            DPRINT1("PCIX - ECAM mismatch at %02lx:%02lx.0, read %08lx expected %08lx\n",
                    FdoExtension->BaseBus,
                    Device,
                    EcamIds,
                    LegacyIds);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        Matched++;
    }

    return Matched ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

/**
 * @brief
 * Maps the bus an FDO decodes into the ECAM window and verifies the
 * window the first time a bus with devices on it is seen.
 *
 * @param[in] FdoExtension
 * The FDO extension of the bus being started.
 */
VOID
NTAPI
PciInitializeEcam(
    _In_ PPCI_FDO_EXTENSION FdoExtension)
{
    NTSTATUS Status;
    PAGED_CODE();

    if (!PciEcamWindowQueried)
    {
        PciEcamWindowQueried = TRUE;

        Status = PciEcamQueryWindow();
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("PCIX - No ECAM window available (0x%lx)\n", Status);
            return;
        }

#if defined(_M_IX86) || defined(_M_AMD64)
        PciEcamSkipK8Northbridge = PciEcamIsAmdK8();
#endif
        DPRINT("PCIX - %lu ECAM window(s), the first at 0x%I64x for segment 0x%x\n",
               PciEcamWindowCount,
               PciEcamWindows[0].Base,
               PciEcamWindows[0].Segment);
    }

    if (!PciEcamMapBus(FdoExtension->Segment, FdoExtension->BaseBus)) return;

    /* Other segments have no legacy path to be checked against */
    if ((FdoExtension->Segment != 0) || PciEcamVerified) return;

    /* An empty bus proves nothing, so leave it to the next one */
    Status = PciEcamVerifyBus(FdoExtension);
    if (Status == STATUS_DEVICE_CONFIGURATION_ERROR)
    {
        PciEcamDiscardWindow();
    }
    else if (NT_SUCCESS(Status))
    {
        PciEcamVerified = TRUE;
        DPRINT("PCIX - ECAM verified on bus 0x%lx\n", FdoExtension->BaseBus);
    }
}

/**
 * @brief Unmaps every bus mapped through the ECAM window.
 */
VOID
NTAPI
PciReleaseEcam(VOID)
{
    PAGED_CODE();

    /* Config access falls back to the legacy mechanism before the mappings go away */
    PciEcamVerified = FALSE;
    PciEcamDiscardWindow();
    PciEcamWindowQueried = FALSE;
}

/**
 * @brief
 * Performs a configuration space access through the ECAM window.
 *
 * @return
 * FALSE if the window cannot serve the access, in which case the
 * caller falls back to the legacy mechanism.
 */
BOOLEAN
NTAPI
PciEcamReadWriteConfig(
    _In_ USHORT Segment,
    _In_ ULONG Bus,
    _In_ PCI_SLOT_NUMBER Slot,
    _Inout_updates_bytes_(Length) PVOID Buffer,
    _In_ ULONG Offset,
    _In_ ULONG Length,
    _In_ BOOLEAN Read)
{
    PUCHAR Function;

    if ((Segment == 0) && !(PciEcamVerified)) return FALSE;

    if ((Offset >= PCI_EXTENDED_CONFIG_LENGTH) ||
        (Length > (PCI_EXTENDED_CONFIG_LENGTH - Offset)))
    {
        return FALSE;
    }

    Function = PciEcamFunctionBase(Segment, Bus, Slot);
    if (!Function) return FALSE;

    PciEcamTransfer(Function, Buffer, Offset, Length, Read);
    return TRUE;
}

/* EOF */
