/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            hal/halx86/legacy/halpnpdd.c
 * PURPOSE:         HAL Plug and Play Device Driver
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include <initguid.h>
#include <wdmguid.h>
#define NDEBUG
#include <debug.h>

typedef enum _EXTENSION_TYPE
{
    PdoExtensionType = 0xC0,
    FdoExtensionType
} EXTENSION_TYPE;

typedef enum _PDO_TYPE
{
    AcpiPdo = 0x80,
    WdPdo
} PDO_TYPE;

typedef struct _FDO_EXTENSION
{
    EXTENSION_TYPE ExtensionType;
    struct _PDO_EXTENSION* ChildPdoList;
    PDEVICE_OBJECT PhysicalDeviceObject;
    PDEVICE_OBJECT FunctionalDeviceObject;
    PDEVICE_OBJECT AttachedDeviceObject;
} FDO_EXTENSION, *PFDO_EXTENSION;

typedef struct _PDO_EXTENSION
{
    EXTENSION_TYPE ExtensionType;
    struct _PDO_EXTENSION* Next;
    PDEVICE_OBJECT PhysicalDeviceObject;
    PFDO_EXTENSION ParentFdoExtension;
    PDO_TYPE PdoType;
    PDESCRIPTION_HEADER WdTable;
    LONG InterfaceReferenceCount;
} PDO_EXTENSION, *PPDO_EXTENSION;

/* GLOBALS ********************************************************************/

PDRIVER_OBJECT HalpDriverObject;
/* Simple stubs matching BUS_INTERFACE_STANDARD get/set callbacks */
ULONG
NTAPI
HalpSetBusData(
    PVOID Context,
    ULONG DataType,
    PVOID Buffer,
    ULONG Offset,
    ULONG Length)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(DataType);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Offset);
    DPRINT("HalpSetBusData: DataType=%lu, Offset=%lu, Length=%lu (legacy stub)\n",
           DataType, Offset, Length);
    return 0;
}

ULONG
NTAPI
HalpGetBusData(
    PVOID Context,
    ULONG DataType,
    PVOID Buffer,
    ULONG Offset,
    ULONG Length)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(DataType);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(Offset);
    DPRINT("HalpGetBusData: DataType=%lu, Offset=%lu, Length=%lu (legacy stub)\n",
           DataType, Offset, Length);
    return 0;
}

/* Wrapper for BUS_INTERFACE_STANDARD TranslateBusAddress */
static
BOOLEAN
NTAPI
HalpBusIfTranslate(
    _Inout_opt_ PVOID Context,
    _In_ PHYSICAL_ADDRESS BusAddress,
    _In_ ULONG Length,
    _Out_ PULONG AddressSpace,
    _Out_ PPHYSICAL_ADDRESS TranslatedAddress)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Length);

    /* Root PCI bus translation via HAL */
    return HaliTranslateBusAddress(PCIBus,
                                   0,
                                   BusAddress,
                                   AddressSpace,
                                   TranslatedAddress);
}

/* Return a simple location string for root bus */
static
NTSTATUS
NTAPI
HalpGetLocationString(
    _In_ PVOID Context,
    _Outptr_ PWCHAR *LocationStrings)
{
    static const WCHAR LocStr[] = L"ACPI Root Bus";
    PWCHAR Buf;
    UNREFERENCED_PARAMETER(Context);
    Buf = ExAllocatePoolWithTag(PagedPool, sizeof(LocStr), TAG_HAL);
    if (!Buf) return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(Buf, LocStr, sizeof(LocStr));
    *LocationStrings = Buf;
    return STATUS_SUCCESS;
}

/* No-op interface ref/deref for interfaces we return */
static
VOID
NTAPI
HalpInterfaceReference(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

static
VOID
NTAPI
HalpInterfaceDereference(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}


/* PRIVATE FUNCTIONS **********************************************************/

NTSTATUS
NTAPI
HalpAddDevice(IN PDRIVER_OBJECT DriverObject,
              IN PDEVICE_OBJECT TargetDevice)
{
    NTSTATUS Status;
    PFDO_EXTENSION FdoExtension;
    PPDO_EXTENSION PdoExtension;
    PDEVICE_OBJECT DeviceObject, AttachedDevice;
    PDEVICE_OBJECT PdoDeviceObject;
//    PDESCRIPTION_HEADER Wdrt;

    DPRINT("HAL: PnP Driver ADD!\n");

    /* Create the FDO */
    Status = IoCreateDevice(DriverObject,
                            sizeof(FDO_EXTENSION),
                            NULL,
                            FILE_DEVICE_BUS_EXTENDER,
                            0,
                            FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
    {
        /* Should not happen */
        DbgBreakPoint();
        return Status;
    }

    /* Setup the FDO extension */
    FdoExtension = DeviceObject->DeviceExtension;
    FdoExtension->ExtensionType = FdoExtensionType;
    FdoExtension->PhysicalDeviceObject = TargetDevice;
    FdoExtension->FunctionalDeviceObject = DeviceObject;
    FdoExtension->ChildPdoList = NULL;

    /* FDO is done initializing */
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    /* Attach to the physical device object (the bus) */
    AttachedDevice = IoAttachDeviceToDeviceStack(DeviceObject, TargetDevice);
    if (!AttachedDevice)
    {
        /* Failed, undo everything */
        IoDeleteDevice(DeviceObject);
        return STATUS_NO_SUCH_DEVICE;
    }

    /* Save the attachment */
    FdoExtension->AttachedDeviceObject = AttachedDevice;

    /* Create the PDO */
    Status = IoCreateDevice(DriverObject,
                            sizeof(PDO_EXTENSION),
                            NULL,
                            FILE_DEVICE_BUS_EXTENDER,
                            FILE_AUTOGENERATED_DEVICE_NAME,
                            FALSE,
                            &PdoDeviceObject);
    if (!NT_SUCCESS(Status))
    {
        /* Fail */
        DPRINT1("HAL: Could not create ACPI device object status=0x%08x\n", Status);
        return Status;
    }

    /* Setup the PDO device extension */
    PdoExtension = PdoDeviceObject->DeviceExtension;
    PdoExtension->ExtensionType = PdoExtensionType;
    PdoExtension->PhysicalDeviceObject = PdoDeviceObject;
    PdoExtension->ParentFdoExtension = FdoExtension;
    PdoExtension->PdoType = AcpiPdo;

    /* Add the PDO to the head of the list */
    PdoExtension->Next = FdoExtension->ChildPdoList;
    FdoExtension->ChildPdoList = PdoExtension;

    /* Initialization is finished */
    PdoDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    /* Return status */
    DPRINT("Device added %lx\n", Status);
    return Status;
}

NTSTATUS
NTAPI
HalpQueryInterface(IN PDEVICE_OBJECT DeviceObject,
                   IN CONST GUID* InterfaceType,
                   IN USHORT Version,
                   IN PVOID InterfaceSpecificData,
                   IN ULONG InterfaceBufferSize,
                   IN PINTERFACE Interface,
                   OUT PULONG Length)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    /* Default: nothing returned */
    if (Length) *Length = 0;

    /* IRQ translator interface */
    if (IsEqualIID(InterfaceType, &GUID_TRANSLATOR_INTERFACE_STANDARD))
    {
        CM_RESOURCE_TYPE ResourceType = (CM_RESOURCE_TYPE)(ULONG_PTR)InterfaceSpecificData;
        ULONG BridgeBusNumber = 0;

        /* Only interrupt translation is supported */
        if (ResourceType != CmResourceTypeInterrupt)
        {
            return STATUS_NOT_SUPPORTED;
        }

        if (InterfaceBufferSize < sizeof(TRANSLATOR_INTERFACE))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        /* Provide the system IRQ translator (Internal -> PCI) */
        return HalGetInterruptTranslator(Internal,
                                         0,
                                         PCIBus,
                                         (USHORT)InterfaceBufferSize,
                                         Version,
                                         (PTRANSLATOR_INTERFACE)Interface,
                                         &BridgeBusNumber);
    }

    /* Bus interface (basic address translation/DMA access) */
    if (IsEqualIID(InterfaceType, &GUID_BUS_INTERFACE_STANDARD))
    {
        PBUS_INTERFACE_STANDARD BusInterface;

        if (InterfaceBufferSize < sizeof(BUS_INTERFACE_STANDARD))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        BusInterface = (PBUS_INTERFACE_STANDARD)Interface;
        BusInterface->Size = sizeof(BUS_INTERFACE_STANDARD);
        BusInterface->Version = 1;
        BusInterface->Context = DeviceObject;
        BusInterface->InterfaceReference = HalpInterfaceReference;
        BusInterface->InterfaceDereference = HalpInterfaceDereference;
        BusInterface->TranslateBusAddress = HalpBusIfTranslate;
        BusInterface->GetDmaAdapter = HalpGetDmaAdapter;
        /* Legacy HAL stubs (per-device bus data not implemented here) */
        BusInterface->SetBusData = HalpSetBusData;
        BusInterface->GetBusData = HalpGetBusData;

        if (Length) *Length = sizeof(BUS_INTERFACE_STANDARD);
        return STATUS_SUCCESS;
    }

    if (IsEqualIID(InterfaceType, &GUID_PCI_BUS_INTERFACE_STANDARD))
    {
        PPCI_BUS_INTERFACE_STANDARD PciIf;

        if (InterfaceBufferSize < sizeof(PCI_BUS_INTERFACE_STANDARD))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        PciIf = (PPCI_BUS_INTERFACE_STANDARD)Interface;
        RtlZeroMemory(PciIf, sizeof(*PciIf));
        PciIf->Size = sizeof(*PciIf);
        PciIf->Version = PCI_BUS_INTERFACE_STANDARD_VERSION;
        PciIf->Context = DeviceObject;
        PciIf->InterfaceReference = HalpInterfaceReference;
        PciIf->InterfaceDereference = HalpInterfaceDereference;
        /* Forward to PCI driver via IRP if needed; for legacy HAL, provide NULLs */
        PciIf->ReadConfig = NULL;
        PciIf->WriteConfig = NULL;
        PciIf->PinToLine = NULL;
        PciIf->LineToPin = NULL;
        PciIf->RootBusCapability = NULL;
        PciIf->ExpressWakeControl = NULL;
        if (Length) *Length = sizeof(*PciIf);
        return STATUS_SUCCESS;
    }

    /* PNP Location Interface */
    if (IsEqualIID(InterfaceType, &GUID_PNP_LOCATION_INTERFACE))
    {
        PPNP_LOCATION_INTERFACE Loc;

        if (InterfaceBufferSize < sizeof(*Loc))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        Loc = (PPNP_LOCATION_INTERFACE)Interface;
        RtlZeroMemory(Loc, sizeof(*Loc));
        Loc->Size = sizeof(*Loc);
        Loc->Version = PNP_LOCATION_INTERFACE_VERSION;
        Loc->Context = DeviceObject;
        Loc->InterfaceReference = HalpInterfaceReference;
        Loc->InterfaceDereference = HalpInterfaceDereference;
        /* Provide a stable location string */
        Loc->GetLocationString = (PVOID)HalpGetLocationString;
        if (Length) *Length = sizeof(*Loc);
        return STATUS_SUCCESS;
    }

    DPRINT1("HalpQueryInterface({%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}) unsupported\n",
            InterfaceType->Data1, InterfaceType->Data2, InterfaceType->Data3,
            InterfaceType->Data4[0], InterfaceType->Data4[1],
            InterfaceType->Data4[2], InterfaceType->Data4[3],
            InterfaceType->Data4[4], InterfaceType->Data4[5],
            InterfaceType->Data4[6], InterfaceType->Data4[7]);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
HalpQueryDeviceRelations(IN PDEVICE_OBJECT DeviceObject,
                         IN DEVICE_RELATION_TYPE RelationType,
                         OUT PDEVICE_RELATIONS* DeviceRelations)
{
    EXTENSION_TYPE ExtensionType;
    PPDO_EXTENSION PdoExtension;
    PFDO_EXTENSION FdoExtension;
    PDEVICE_RELATIONS PdoRelations, FdoRelations;
    PDEVICE_OBJECT* ObjectEntry;
    ULONG i = 0, PdoCount = 0;

    /* Get FDO device extension and PDO count */
    FdoExtension = DeviceObject->DeviceExtension;
    ExtensionType = FdoExtension->ExtensionType;

    /* What do they want? */
    if (RelationType == BusRelations)
    {
        /* This better be an FDO */
        if (ExtensionType == FdoExtensionType)
        {
            /* Count how many PDOs we have */
            PdoExtension = FdoExtension->ChildPdoList;
            while (PdoExtension)
            {
                /* Next one */
                PdoExtension = PdoExtension->Next;
                PdoCount++;
            }

            /* Add the PDOs that already exist in the device relations */
            if (*DeviceRelations)
            {
                PdoCount += (*DeviceRelations)->Count;
            }

            /* Allocate our structure */
            FdoRelations = ExAllocatePoolWithTag(PagedPool,
                                                 FIELD_OFFSET(DEVICE_RELATIONS,
                                                              Objects) +
                                                 sizeof(PDEVICE_OBJECT) * PdoCount,
                                                 TAG_HAL);
            if (!FdoRelations) return STATUS_INSUFFICIENT_RESOURCES;

            /* Save our count */
            FdoRelations->Count = PdoCount;

            /* Query existing relations */
            ObjectEntry = FdoRelations->Objects;
            if (*DeviceRelations)
            {
                /* Check if there were any */
                if ((*DeviceRelations)->Count)
                {
                    /* Loop them all */
                    do
                    {
                        /* Copy into our structure */
                        *ObjectEntry++ = (*DeviceRelations)->Objects[i];
                    }
                    while (++i < (*DeviceRelations)->Count);
                }

                /* Free existing structure */
                ExFreePool(*DeviceRelations);
            }

            /* Now check if we have a PDO list */
            PdoExtension = FdoExtension->ChildPdoList;
            if (PdoExtension)
            {
                /* Loop the PDOs */
                do
                {
                    /* Save our own PDO and reference it */
                    *ObjectEntry++ = PdoExtension->PhysicalDeviceObject;
                    ObReferenceObject(PdoExtension->PhysicalDeviceObject);

                    /* Go to our next PDO */
                    PdoExtension = PdoExtension->Next;
                }
                while (PdoExtension);
            }

            /* Return the new structure */
            *DeviceRelations = FdoRelations;
            return STATUS_SUCCESS;
        }
    }
    else
    {
        /* The only other thing we support is a target relation for the PDO */
        if ((RelationType == TargetDeviceRelation) &&
            (ExtensionType == PdoExtensionType))
        {
            /* Only one entry */
            PdoRelations = ExAllocatePoolWithTag(PagedPool,
                                                 sizeof(DEVICE_RELATIONS),
                                                 TAG_HAL);
            if (!PdoRelations) return STATUS_INSUFFICIENT_RESOURCES;

            /* Fill it out and reference us */
            PdoRelations->Count = 1;
            PdoRelations->Objects[0] = DeviceObject;
            ObReferenceObject(DeviceObject);

            /* Return it */
            *DeviceRelations = PdoRelations;
            return STATUS_SUCCESS;
        }
    }

    /* We don't support anything else */
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
HalpQueryCapabilities(IN PDEVICE_OBJECT DeviceObject,
                      OUT PDEVICE_CAPABILITIES Capabilities)
{
    //PPDO_EXTENSION PdoExtension;
    NTSTATUS Status;
    PAGED_CODE();

    /* Get the extension and check for valid version */
    //PdoExtension = DeviceObject->DeviceExtension;
    ASSERT(Capabilities->Version == 1);
    if (Capabilities->Version == 1)
    {
        /* Can't lock or eject us */
        Capabilities->LockSupported = FALSE;
        Capabilities->EjectSupported = FALSE;

        /* Can't remove or dock us */
        Capabilities->Removable = FALSE;
        Capabilities->DockDevice = FALSE;

        /* Can't access us raw */
        Capabilities->RawDeviceOK = FALSE;

        /* We have a unique ID, and don't bother the user */
        Capabilities->UniqueID = TRUE;
        Capabilities->SilentInstall = TRUE;

        /* Fill out the address */
        Capabilities->Address = InterfaceTypeUndefined;
        Capabilities->UINumber = InterfaceTypeUndefined;

        /* Fill out latencies */
        Capabilities->D1Latency = 0;
        Capabilities->D2Latency = 0;
        Capabilities->D3Latency = 0;

        /* Fill out supported device states */
        Capabilities->DeviceState[PowerSystemWorking] = PowerDeviceD0;
        Capabilities->DeviceState[PowerSystemHibernate] = PowerDeviceD3;
        Capabilities->DeviceState[PowerSystemShutdown] = PowerDeviceD3;
        Capabilities->DeviceState[PowerSystemSleeping3] = PowerDeviceD3;

        /* Done */
        Status = STATUS_SUCCESS;
    }
    else
    {
        /* Fail */
        Status = STATUS_NOT_SUPPORTED;
    }

    /* Return status */
    return Status;
}

NTSTATUS
NTAPI
HalpQueryResources(IN PDEVICE_OBJECT DeviceObject,
                   OUT PCM_RESOURCE_LIST *Resources)
{
    PPDO_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    NTSTATUS Status;
    PCM_RESOURCE_LIST ResourceList;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR PartialDesc;
    PAGED_CODE();

    /* Only the ACPI PDO has requirements */
    if (DeviceExtension->PdoType == AcpiPdo)
    {
        SIZE_T Size;

        /* Only expose BusNumber here; IO/MEM windows are advertised via requirements */
        Size = FIELD_OFFSET(CM_RESOURCE_LIST,
                            List[0].PartialResourceList.PartialDescriptors) +
               (1 * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));

        /* Allocate the resource list */
        ResourceList = ExAllocatePoolWithTag(PagedPool, Size, TAG_HAL);
        if (!ResourceList)
        {
            /* Fail, no memory */
            Status = STATUS_INSUFFICIENT_RESOURCES;
            return Status;
        }

        /* Initialize it */
        RtlZeroMemory(ResourceList, Size);
        ResourceList->Count = 1;

        /* Setup the list fields */
        ResourceList->List[0].BusNumber = 0;
        ResourceList->List[0].InterfaceType = PCIBus;
        ResourceList->List[0].PartialResourceList.Version = 1;
        ResourceList->List[0].PartialResourceList.Revision = 1;
        ResourceList->List[0].PartialResourceList.Count = 1;

        PartialDesc = ResourceList->List[0].PartialResourceList.PartialDescriptors;

        /* PCI root bus number (bus 0) */
        PartialDesc->Type = CmResourceTypeBusNumber;
        PartialDesc->ShareDisposition = CmResourceShareDeviceExclusive;
        PartialDesc->Flags = 0;
        PartialDesc->u.BusNumber.Start = 0;
        PartialDesc->u.BusNumber.Length = 1;
        PartialDesc->u.BusNumber.Reserved = 0;

        /* Return resources and success */
        *Resources = ResourceList;
        return STATUS_SUCCESS;
    }
    else if (DeviceExtension->PdoType == WdPdo)
    {
        /* Watchdog doesn't */
        return STATUS_NOT_SUPPORTED;
    }
    else
    {
        /* This shouldn't happen */
        return STATUS_UNSUCCESSFUL;
    }
}

NTSTATUS
NTAPI
HalpQueryResourceRequirements(IN PDEVICE_OBJECT DeviceObject,
                              OUT PIO_RESOURCE_REQUIREMENTS_LIST *Requirements)
{
    PPDO_EXTENSION DeviceExtension = DeviceObject->DeviceExtension;
    PAGED_CODE();

    /* Only the ACPI PDO has requirements */
    if (DeviceExtension->PdoType == AcpiPdo)
    {
        PIO_RESOURCE_REQUIREMENTS_LIST ReqList;
        PIO_RESOURCE_DESCRIPTOR Desc;
        ULONG Count;
        SIZE_T Size;

        /* Advertise IO/MEM windows via requirements, in two IO windows and one MEM window */
        Count = 4; /* BusNumber + 2x IO + 1x MEM */
        Size = FIELD_OFFSET(IO_RESOURCE_REQUIREMENTS_LIST, List[0].Descriptors) +
               (Count * sizeof(IO_RESOURCE_DESCRIPTOR));

        /* Allocate requirements */
        ReqList = ExAllocatePoolWithTag(PagedPool, Size, TAG_HAL);
        if (!ReqList)
        {
            *Requirements = NULL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(ReqList, Size);
        ReqList->ListSize = (ULONG)Size;
        ReqList->InterfaceType = PCIBus;
        ReqList->BusNumber = 0;
        ReqList->SlotNumber = 0;
        ReqList->AlternativeLists = 1;
        ReqList->List[0].Version = 1;
        ReqList->List[0].Revision = 1;
        ReqList->List[0].Count = Count;

        Desc = &ReqList->List[0].Descriptors[0];

        /* 0) BusNumber 0 */
        Desc->Option = 0; /* Required */
        Desc->Type = CmResourceTypeBusNumber;
        Desc->ShareDisposition = CmResourceShareDeviceExclusive;
        Desc->Flags = 0;
        Desc->u.BusNumber.MinBusNumber = 0;
        Desc->u.BusNumber.MaxBusNumber = 0;
        Desc->u.BusNumber.Length = 1;
        Desc->u.BusNumber.Reserved = 0;
        Desc++;

        /* 1) IO: 0x0000 - 0x0CF7 */
        Desc->Option = 0;
        Desc->Type = CmResourceTypePort;
        Desc->ShareDisposition = CmResourceShareDeviceExclusive;
        Desc->Flags = 0;
        Desc->u.Port.MinimumAddress.QuadPart = 0x0000;
        Desc->u.Port.MaximumAddress.QuadPart = 0x0CF7;
        Desc->u.Port.Alignment = 1;
        Desc->u.Port.Length = 0x0CF8;
        Desc++;

        /* 2) IO: 0x0D00 - 0xFFFF */
        Desc->Option = 0;
        Desc->Type = CmResourceTypePort;
        Desc->ShareDisposition = CmResourceShareDeviceExclusive;
        Desc->Flags = 0;
        Desc->u.Port.MinimumAddress.QuadPart = 0x0D00;
        Desc->u.Port.MaximumAddress.QuadPart = 0xFFFF;
        Desc->u.Port.Alignment = 1;
        Desc->u.Port.Length = 0xF300;
        Desc++;

        /* 3) Memory: 0x00100000 - 0xFEBFFFFF */
        Desc->Option = 0;
        Desc->Type = CmResourceTypeMemory;
        Desc->ShareDisposition = CmResourceShareDeviceExclusive;
        Desc->Flags = 0; /* non-prefetchable */
        Desc->u.Memory.MinimumAddress.QuadPart = 0x00100000ULL;
        Desc->u.Memory.MaximumAddress.QuadPart = 0xFEBFFFFFULL;
        Desc->u.Memory.Alignment = 0x1000; /* 4K */
        Desc->u.Memory.Length = (ULONG)(0xFEBFFFFFULL - 0x00100000ULL + 1ULL);

        *Requirements = ReqList;
        return STATUS_SUCCESS;
    }
    else if (DeviceExtension->PdoType == WdPdo)
    {
        /* Watchdog doesn't */
        return STATUS_NOT_SUPPORTED;
    }
    else
    {
        /* This shouldn't happen */
        return STATUS_UNSUCCESSFUL;
    }
}

NTSTATUS
NTAPI
HalpQueryIdPdo(IN PDEVICE_OBJECT DeviceObject,
               IN BUS_QUERY_ID_TYPE IdType,
               OUT PUSHORT *BusQueryId)
{
    PPDO_EXTENSION PdoExtension;
    PDO_TYPE PdoType;
    PWCHAR CurrentId;
    WCHAR Id[100];
    NTSTATUS Status;
    ULONG Length = 0;
    PWCHAR Buffer;

    /* Get the PDO type */
    PdoExtension = DeviceObject->DeviceExtension;
    PdoType = PdoExtension->PdoType;

    /* What kind of ID is being requested? */
    DPRINT("ID: %d\n", IdType);
    switch (IdType)
    {
        case BusQueryDeviceID:
        case BusQueryHardwareIDs:

            /* What kind of PDO is this? */
            if (PdoType == AcpiPdo)
            {
                /* ACPI ID */
                CurrentId = L"PCI_HAL\\PNP0A03";
                RtlCopyMemory(Id, CurrentId, (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL));
                Length += (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL);

                CurrentId = L"*PNP0A03";
                RtlCopyMemory(&Id[wcslen(Id) + 1], CurrentId, (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL));
                Length += (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL);
            }
#if 0
            else if (PdoType == WdPdo)
            {
                /* WatchDog ID */
                CurrentId = L"ACPI_HAL\\PNP0C18";
                RtlCopyMemory(Id, CurrentId, (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL));
                Length += (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL);

                CurrentId = L"*PNP0C18";
                RtlCopyMemory(&Id[wcslen(Id) + 1], CurrentId, (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL));
                Length += (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL);
            }
#endif
            else
            {
                /* Unknown */
                return STATUS_NOT_SUPPORTED;
            }
            break;

        case BusQueryInstanceID:

            /* Instance ID */
            CurrentId = L"0";
            RtlCopyMemory(Id, CurrentId, (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL));
            Length += (wcslen(CurrentId) * sizeof(WCHAR)) + sizeof(UNICODE_NULL);
            break;

        case BusQueryCompatibleIDs:
        default:

            /* We don't support anything else */
            return STATUS_NOT_SUPPORTED;
    }

    /* Allocate the buffer */
    Buffer = ExAllocatePoolWithTag(PagedPool,
                                   Length + sizeof(UNICODE_NULL),
                                   TAG_HAL);
    if (Buffer)
    {
        /* Copy the string and null-terminate it */
        RtlCopyMemory(Buffer, Id, Length);
        Buffer[Length / sizeof(WCHAR)] = UNICODE_NULL;

        /* Return string */
        *BusQueryId = Buffer;
        Status = STATUS_SUCCESS;
        DPRINT("Returning: %S\n", *BusQueryId);
    }
    else
    {
        /* Fail */
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Return status */
    return Status;
}

NTSTATUS
NTAPI
HalpQueryIdFdo(IN PDEVICE_OBJECT DeviceObject,
               IN BUS_QUERY_ID_TYPE IdType,
               OUT PUSHORT *BusQueryId)
{
    NTSTATUS Status;
    ULONG Length;
    PWCHAR Id;
    PWCHAR Buffer;

    /* What kind of ID is being requested? */
    DPRINT("ID: %d\n", IdType);
    switch (IdType)
    {
        case BusQueryDeviceID:
        case BusQueryHardwareIDs:

            /* This is our hardware ID */
            Id = HalHardwareIdString;
            break;

        case BusQueryInstanceID:

            /* And our instance ID */
            Id = L"0";
            break;

        default:

            /* We don't support anything else */
            return STATUS_NOT_SUPPORTED;
    }

    /* Calculate the length */
    Length = (wcslen(Id) * sizeof(WCHAR)) + sizeof(UNICODE_NULL);

    /* Allocate the buffer */
    Buffer = ExAllocatePoolWithTag(PagedPool,
                                   Length + sizeof(UNICODE_NULL),
                                   TAG_HAL);
    if (Buffer)
    {
        /* Copy the string and null-terminate it */
        RtlCopyMemory(Buffer, Id, Length);
        Buffer[Length / sizeof(WCHAR)] = UNICODE_NULL;

        /* Return string */
        *BusQueryId = Buffer;
        Status = STATUS_SUCCESS;
        DPRINT("Returning: %S\n", *BusQueryId);
    }
    else
    {
        /* Fail */
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Return status */
    return Status;
}

NTSTATUS
NTAPI
HalpDispatchPnp(IN PDEVICE_OBJECT DeviceObject,
                IN PIRP Irp)
{
    PIO_STACK_LOCATION IoStackLocation;
    //PPDO_EXTENSION PdoExtension;
    PFDO_EXTENSION FdoExtension;
    NTSTATUS Status;
    UCHAR Minor;

    /* Get the device extension and stack location */
    FdoExtension = DeviceObject->DeviceExtension;
    IoStackLocation = IoGetCurrentIrpStackLocation(Irp);
    Minor = IoStackLocation->MinorFunction;

    /* FDO? */
    if (FdoExtension->ExtensionType == FdoExtensionType)
    {
        /* Query the IRP type */
        switch (Minor)
        {
            case IRP_MN_QUERY_DEVICE_RELATIONS:

                /* Call the worker */
                DPRINT("Querying device relations for FDO\n");
                Status = HalpQueryDeviceRelations(DeviceObject,
                                                  IoStackLocation->Parameters.QueryDeviceRelations.Type,
                                                  (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_INTERFACE:

                /* Call the worker */
                DPRINT("Querying interface for FDO\n");
                Status = HalpQueryInterface(DeviceObject,
                                            IoStackLocation->Parameters.QueryInterface.InterfaceType,
                                            IoStackLocation->Parameters.QueryInterface.Version,
                                            IoStackLocation->Parameters.QueryInterface.InterfaceSpecificData,
                                            IoStackLocation->Parameters.QueryInterface.Size,
                                            IoStackLocation->Parameters.QueryInterface.Interface,
                                            (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_ID:

                /* Call the worker */
                DPRINT("Querying ID for FDO\n");
                Status = HalpQueryIdFdo(DeviceObject,
                                        IoStackLocation->Parameters.QueryId.IdType,
                                        (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_CAPABILITIES:

                /* Call the worker */
                DPRINT("Querying the capabilities for the FDO\n");
                Status = HalpQueryCapabilities(DeviceObject,
                                               IoStackLocation->Parameters.DeviceCapabilities.Capabilities);
                break;

            default:

                DPRINT("Other IRP: %lx\n", Minor);
                Status = STATUS_NOT_SUPPORTED;
                break;
        }

        /* What happpened? */
        if ((NT_SUCCESS(Status)) || (Status == STATUS_NOT_SUPPORTED))
        {
            /* Set the IRP status, unless this isn't understood */
            if (Status != STATUS_NOT_SUPPORTED)
            {
                Irp->IoStatus.Status = Status;
            }

            /* Pass it on */
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(FdoExtension->AttachedDeviceObject, Irp);
        }

        /* Otherwise, we failed, so set the status and complete the request */
        DPRINT1("IRP failed with status: %lx\n", Status);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }
    else
    {
        /* This is a PDO instead */
        ASSERT(FdoExtension->ExtensionType == PdoExtensionType);
        //PdoExtension = (PPDO_EXTENSION)FdoExtension;

        /* Query the IRP type */
        Status = STATUS_SUCCESS;
        switch (Minor)
        {
            case IRP_MN_START_DEVICE:

                /* We only care about a PCI PDO */
                DPRINT("Start device received\n");
                /* Complete the IRP normally */
                break;

            case IRP_MN_REMOVE_DEVICE:

                /* Check if this is a PCI device */
                DPRINT("Remove device received\n");

                /* We're done */
                Status = STATUS_SUCCESS;
                break;

            case IRP_MN_SURPRISE_REMOVAL:

                /* Inherit whatever status we had */
                DPRINT("Surprise removal IRP\n");
                Status = Irp->IoStatus.Status;
                break;

            case IRP_MN_QUERY_DEVICE_RELATIONS:

                /* Query the device relations */
                DPRINT("Querying PDO relations\n");
                Status = HalpQueryDeviceRelations(DeviceObject,
                                                  IoStackLocation->Parameters.QueryDeviceRelations.Type,
                                                  (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_INTERFACE:

                /* Call the worker */
                DPRINT("Querying interface for PDO\n");
                Status = HalpQueryInterface(DeviceObject,
                                            IoStackLocation->Parameters.QueryInterface.InterfaceType,
                                            IoStackLocation->Parameters.QueryInterface.Version,
                                            IoStackLocation->Parameters.QueryInterface.InterfaceSpecificData,
                                            IoStackLocation->Parameters.QueryInterface.Size,
                                            IoStackLocation->Parameters.QueryInterface.Interface,
                                            (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_CAPABILITIES:

                /* Call the worker */
                DPRINT("Querying the capabilities for the PDO\n");
                Status = HalpQueryCapabilities(DeviceObject,
                                               IoStackLocation->Parameters.DeviceCapabilities.Capabilities);
                break;

            case IRP_MN_QUERY_RESOURCES:

                /* Call the worker */
                DPRINT("Querying the resources for the PDO\n");
                Status = HalpQueryResources(DeviceObject, (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:

                /* Call the worker */
                DPRINT("Querying the resource requirements for the PDO\n");
                Status = HalpQueryResourceRequirements(DeviceObject,
                                                       (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_ID:

                /* Call the worker */
                DPRINT("Query the ID for the PDO\n");
                Status = HalpQueryIdPdo(DeviceObject,
                                        IoStackLocation->Parameters.QueryId.IdType,
                                        (PVOID)&Irp->IoStatus.Information);
                break;

            case IRP_MN_QUERY_DEVICE_TEXT:

                /* Inherit whatever status we had */
                DPRINT("Query text for the PDO\n");
                Status = Irp->IoStatus.Status;
                break;

            case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:

                /* Inherit whatever status we had */
                DPRINT("Filter resource requirements for the PDO\n");
                Status = Irp->IoStatus.Status;
                break;

            case IRP_MN_QUERY_PNP_DEVICE_STATE:

                /* Inherit whatever status we had */
                DPRINT("Query device state for the PDO\n");
                Status = Irp->IoStatus.Status;
                break;

            case IRP_MN_QUERY_BUS_INFORMATION:

                /* Inherit whatever status we had */
                DPRINT("Query bus information for the PDO\n");
                Status = Irp->IoStatus.Status;
                break;

            default:

                /* We don't handle anything else, so inherit the old state */
                DPRINT1("Illegal IRP: %lx\n", Minor);
                Status = Irp->IoStatus.Status;
                break;
        }

        /* If it's not supported, inherit the old status */
        if (Status == STATUS_NOT_SUPPORTED) Status = Irp->IoStatus.Status;

        /* Complete the IRP */
        DPRINT("IRP completed with status: %lx\n", Status);
        Irp->IoStatus.Status = Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return Status;
    }
}

NTSTATUS
NTAPI
HalpDispatchWmi(IN PDEVICE_OBJECT DeviceObject,
                IN PIRP Irp)
{
    UNIMPLEMENTED_DBGBREAK("HAL: PnP Driver WMI!\n");
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
HalpDispatchPower(IN PDEVICE_OBJECT DeviceObject,
                  IN PIRP Irp)
{
    PFDO_EXTENSION FdoExtension;

    DPRINT1("HAL: PnP Driver Power!\n");
    FdoExtension = DeviceObject->DeviceExtension;
    if (FdoExtension->ExtensionType == FdoExtensionType)
    {
        PoStartNextPowerIrp(Irp);
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(FdoExtension->AttachedDeviceObject, Irp);
    }
    else
    {
        PoStartNextPowerIrp(Irp);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }
}

NTSTATUS
NTAPI
HalpDriverEntry(IN PDRIVER_OBJECT DriverObject,
                IN PUNICODE_STRING RegistryPath)
{
    NTSTATUS Status;
    PDEVICE_OBJECT TargetDevice = NULL;

    DPRINT("HAL: PnP Driver ENTRY!\n");

    /* This is us */
    HalpDriverObject = DriverObject;

    /* Set up add device */
    DriverObject->DriverExtension->AddDevice = HalpAddDevice;

    /* Set up the callouts */
    DriverObject->MajorFunction[IRP_MJ_PNP] = HalpDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = HalpDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = HalpDispatchWmi;

    /* Create the PDO and tell the PnP manager about us*/
    Status = IoReportDetectedDevice(DriverObject,
                                    InterfaceTypeUndefined,
                                    -1,
                                    -1,
                                    NULL,
                                    NULL,
                                    FALSE,
                                    &TargetDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    TargetDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    /* Set up the device stack */
    Status = HalpAddDevice(DriverObject, TargetDevice);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(TargetDevice);
        return Status;
    }

    /* Return to kernel */
    return Status;
}

NTSTATUS
NTAPI
HaliInitPnpDriver(VOID)
{
    NTSTATUS Status;
    UNICODE_STRING DriverString;
    PAGED_CODE();

    /* Create the driver */
    RtlInitUnicodeString(&DriverString, L"\\Driver\\PCI_HAL");
    Status = IoCreateDriver(&DriverString, HalpDriverEntry);

    /* Return status */
    return Status;
}

/* EOF */
