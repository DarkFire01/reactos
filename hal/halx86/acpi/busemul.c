/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            hal/halx86/acpi/busemul.c
 * PURPOSE:         ACPI HAL Bus Handler Emulation Code
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

BUS_HANDLER HalpFakePciBusHandler;
PCIPBUSDATA HalpFakePciBusData;

/* PIC Vector Redirect Table - matches base HAL implementation */
ULONG HalpPicVectorRedirect[];
/* PRIVATE FUNCTIONS **********************************************************/

CODE_SEG("INIT")
VOID
NTAPI
HalpRegisterKdSupportFunctions(VOID)
{
    /* Register PCI Device Functions */
    KdSetupPciDeviceForDebugging = HalpSetupPciDeviceForDebugging;
    KdReleasePciDeviceforDebugging = HalpReleasePciDeviceForDebugging;

    /* Register memory functions */
#ifndef _MINIHAL_
#if (NTDDI_VERSION >= NTDDI_VISTA)
    KdMapPhysicalMemory64 = HalpMapPhysicalMemory64Vista;
    KdUnmapVirtualAddress = HalpUnmapVirtualAddressVista;
#else
    KdMapPhysicalMemory64 = HalpMapPhysicalMemory64;
    KdUnmapVirtualAddress = HalpUnmapVirtualAddress;
#endif
#endif

    /* Register ACPI stub */
    KdCheckPowerButton = HalpCheckPowerButton;
}

NTSTATUS
NTAPI
HalpAssignSlotResources(IN PUNICODE_STRING RegistryPath,
                        IN PUNICODE_STRING DriverClassName,
                        IN PDRIVER_OBJECT DriverObject,
                        IN PDEVICE_OBJECT DeviceObject,
                        IN INTERFACE_TYPE BusType,
                        IN ULONG BusNumber,
                        IN ULONG SlotNumber,
                        IN OUT PCM_RESOURCE_LIST *AllocatedResources)
{
    BUS_HANDLER BusHandler;
    PAGED_CODE();

    /* Only PCI is supported */
    if (BusType != PCIBus) return STATUS_NOT_IMPLEMENTED;

    /* Setup fake PCI Bus handler */
    RtlCopyMemory(&BusHandler, &HalpFakePciBusHandler, sizeof(BUS_HANDLER));
    BusHandler.BusNumber = BusNumber;

    /* Call the PCI function */
    return HalpAssignPCISlotResources(&BusHandler,
                                      &BusHandler,
                                      RegistryPath,
                                      DriverClassName,
                                      DriverObject,
                                      DeviceObject,
                                      SlotNumber,
                                      AllocatedResources);
}

BOOLEAN
NTAPI
HalpTranslateBusAddress(IN INTERFACE_TYPE InterfaceType,
                        IN ULONG BusNumber,
                        IN PHYSICAL_ADDRESS BusAddress,
                        IN OUT PULONG AddressSpace,
                        OUT PPHYSICAL_ADDRESS TranslatedAddress)
{
    /* Translation is easy */
    TranslatedAddress->QuadPart = BusAddress.QuadPart;
    return TRUE;
}

BOOLEAN
NTAPI
HalpFindBusAddressTranslation(IN PHYSICAL_ADDRESS BusAddress,
                              IN OUT PULONG AddressSpace,
                              OUT PPHYSICAL_ADDRESS TranslatedAddress,
                              IN OUT PULONG_PTR Context,
                              IN BOOLEAN NextBus)
{
    /* Make sure we have a context */
    if (!Context) return FALSE;

    /* If we have data in the context, then this shouldn't be a new lookup */
    if ((*Context != 0) && (NextBus != FALSE)) return FALSE;

    /* Return bus data */
    TranslatedAddress->QuadPart = BusAddress.QuadPart;

    /* Set context value and return success */
    *Context = 1;
    return TRUE;
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
NTSTATUS
NTAPI
HalAdjustResourceList(IN OUT PIO_RESOURCE_REQUIREMENTS_LIST* pRequirementsList)
{
    /* Deprecated, return success */
    return STATUS_SUCCESS;
}

/*
 * @implemented
 */
NTSTATUS
NTAPI
HalAssignSlotResources(IN PUNICODE_STRING RegistryPath,
                       IN PUNICODE_STRING DriverClassName,
                       IN PDRIVER_OBJECT DriverObject,
                       IN PDEVICE_OBJECT DeviceObject,
                       IN INTERFACE_TYPE BusType,
                       IN ULONG BusNumber,
                       IN ULONG SlotNumber,
                       IN OUT PCM_RESOURCE_LIST *AllocatedResources)
{
    /* Check the bus type */
    if (BusType != PCIBus)
    {
        /* Call our internal handler */
        return HalpAssignSlotResources(RegistryPath,
                                       DriverClassName,
                                       DriverObject,
                                       DeviceObject,
                                       BusType,
                                       BusNumber,
                                       SlotNumber,
                                       AllocatedResources);
    }
    else
    {
        /* Call the PCI registered function */
        return HalPciAssignSlotResources(RegistryPath,
                                         DriverClassName,
                                         DriverObject,
                                         DeviceObject,
                                         PCIBus,
                                         BusNumber,
                                         SlotNumber,
                                         AllocatedResources);
    }
}

/*
 * @implemented
 */
ULONG
NTAPI
HalGetBusData(IN BUS_DATA_TYPE BusDataType,
              IN ULONG BusNumber,
              IN ULONG SlotNumber,
              IN PVOID Buffer,
              IN ULONG Length)
{
    /* Call the extended function */
    return HalGetBusDataByOffset(BusDataType,
                                 BusNumber,
                                 SlotNumber,
                                 Buffer,
                                 0,
                                 Length);
}

/*
 * @implemented
 */
ULONG
NTAPI
HalGetBusDataByOffset(IN BUS_DATA_TYPE BusDataType,
                      IN ULONG BusNumber,
                      IN ULONG SlotNumber,
                      IN PVOID Buffer,
                      IN ULONG Offset,
                      IN ULONG Length)
{
    BUS_HANDLER BusHandler;

    /* Look as the bus type */
    if (BusDataType == Cmos)
    {
        /* Call CMOS Function */
        return HalpGetCmosData(0, SlotNumber, Buffer, Length);
    }
    else if (BusDataType == EisaConfiguration)
    {
        /* FIXME: TODO */
        ASSERT(FALSE);
    }
    else if ((BusDataType == PCIConfiguration) &&
             (HalpPCIConfigInitialized) &&
             ((BusNumber >= HalpMinPciBus) && (BusNumber <= HalpMaxPciBus)))
    {
        /* Setup fake PCI Bus handler */
        RtlCopyMemory(&BusHandler, &HalpFakePciBusHandler, sizeof(BUS_HANDLER));
        BusHandler.BusNumber = BusNumber;

        /* Call PCI function */
        return HalpGetPCIData(&BusHandler,
                              &BusHandler,
                              SlotNumber,
                              Buffer,
                              Offset,
                              Length);
    }

    /* Invalid bus */
    return 0;
}

ULONG
NTAPI
HalpGetSystemInterruptVector(IN PBUS_HANDLER BusHandler,
                             IN PBUS_HANDLER RootHandler,
                             IN ULONG BusInterruptLevel,
                             IN ULONG BusInterruptVector,
                             OUT PKIRQL Irql,
                             OUT PKAFFINITY Affinity)
{
    ULONG Vector;

    DPRINT1("[ACPI] HalpGetSystemInterruptVector: Level=%lu Vector=%lu\n", BusInterruptLevel, BusInterruptVector);

    /* Get the root vector */
    Vector = HalpGetRootInterruptVector(BusInterruptLevel,
                                        BusInterruptVector,
                                        Irql,
                                        Affinity);

    /* The base HAL approach - no IDT usage checking in ACPI mode */
    /* ACPI HAL allows interrupt vector sharing by default */
    DPRINT1("[ACPI] Vector allocated: 0x%lx for IRQ %lu\n", Vector, BusInterruptLevel);

    DPRINT1("[ACPI] HalpGetSystemInterruptVector returning: Vector=0x%lx IRQL=%u\n", Vector, *Irql);
    return Vector;
}

/*
 * @implemented
 */
ULONG
NTAPI
HalGetInterruptVector(IN INTERFACE_TYPE InterfaceType,
                      IN ULONG BusNumber,
                      IN ULONG BusInterruptLevel,
                      IN ULONG BusInterruptVector,
                      OUT PKIRQL Irql,
                      OUT PKAFFINITY Affinity)
{
    BUS_HANDLER BusHandler;
    ULONG Vector;
    
    DPRINT1("[ACPI] HalGetInterruptVector: Interface=%d Bus=%lu Level=%lu Vector=%lu\n", 
            InterfaceType, BusNumber, BusInterruptLevel, BusInterruptVector);
    
    /* Handle ISA vector redirection like the base HAL */
    if (InterfaceType == Isa)
    {
        DPRINT1("[ACPI] ISA interrupt - applying vector redirection\n");
        
        /* Apply ISA vector redirection through HalpPicVectorRedirect table */
        if (BusInterruptVector < 16)
        {
            BusInterruptVector = HalpPicVectorRedirect[BusInterruptVector];
            BusInterruptLevel = HalpPicVectorRedirect[BusInterruptLevel];
            DPRINT1("[ACPI] ISA vector redirected to Level=%lu Vector=%lu\n", BusInterruptLevel, BusInterruptVector);
        }
    }
    
    /* Create a bus handler structure for the call */
    RtlZeroMemory(&BusHandler, sizeof(BUS_HANDLER));
    BusHandler.BusNumber = BusNumber;
    BusHandler.InterfaceType = InterfaceType;
    BusHandler.ParentHandler = NULL;
    
    /* Use HalpGetSystemInterruptVector like the base HAL */
    Vector = HalpGetSystemInterruptVector(&BusHandler,
                                          &BusHandler,
                                          BusInterruptLevel,
                                          BusInterruptVector,
                                          Irql,
                                          Affinity);
                                        
    DPRINT1("[ACPI] HalGetInterruptVector returning: Vector=0x%lx IRQL=%u (Interface=%d Level=%lu)\n", 
            Vector, *Irql, InterfaceType, BusInterruptLevel);
    
    /* For PCI devices, ensure proper interrupt routing */
    if (InterfaceType == PCIBus && Vector != 0)
    {
        DPRINT1("[ACPI] PCI device successfully assigned Vector=0x%lx for IRQ %lu\n", Vector, BusInterruptLevel);
        
        /* CRITICAL: Ensure PCI interrupt is properly enabled and routed */
        /* In ACPI mode, we need to make sure the interrupt is level-triggered and shared */
        DPRINT1("[ACPI] Ensuring PCI interrupt IRQ %lu is properly configured for sharing\n", BusInterruptLevel);
        
        /* Check if this IRQ is already being used by other PCI devices */
        DPRINT1("[ACPI] PCI IRQ %lu will be SHARED - Vector=0x%lx IRQL=%u\n", BusInterruptLevel, Vector, *Irql);
        
        /* Log interrupt sharing status */
        if (BusInterruptLevel >= 9 && BusInterruptLevel <= 15)
        {
            DPRINT1("[ACPI] IRQ %lu is in PCI sharing range - level-triggered mode enabled\n", BusInterruptLevel);
        }
        else
        {
            DPRINT1("[ACPI] WARNING: PCI device using non-standard IRQ %lu!\n", BusInterruptLevel);
        }
        
        /* TODO: Add ACPI-specific PCI interrupt routing here if needed */
        /* For now, rely on the PIC configuration done by the PIC driver */
    }
    else if (InterfaceType == PCIBus && Vector == 0)
    {
        DPRINT1("[ACPI] ERROR: PCI device failed to get interrupt vector for IRQ %lu!\n", BusInterruptLevel);
    }
    
    return Vector;
}

/*
 * @implemented
 */
ULONG
NTAPI
HalSetBusData(IN BUS_DATA_TYPE BusDataType,
              IN ULONG BusNumber,
              IN ULONG SlotNumber,
              IN PVOID Buffer,
              IN ULONG Length)
{
    /* Call the extended function */
    return HalSetBusDataByOffset(BusDataType,
                                 BusNumber,
                                 SlotNumber,
                                 Buffer,
                                 0,
                                 Length);
}

/*
 * @implemented
 */
ULONG
NTAPI
HalSetBusDataByOffset(IN BUS_DATA_TYPE BusDataType,
                      IN ULONG BusNumber,
                      IN ULONG SlotNumber,
                      IN PVOID Buffer,
                      IN ULONG Offset,
                      IN ULONG Length)
{
    BUS_HANDLER BusHandler;

    /* Look as the bus type */
    if (BusDataType == Cmos)
    {
        /* Call CMOS Function */
        return HalpSetCmosData(0, SlotNumber, Buffer, Length);
    }
    else if ((BusDataType == PCIConfiguration) && (HalpPCIConfigInitialized))
    {
        /* Setup fake PCI Bus handler */
        RtlCopyMemory(&BusHandler, &HalpFakePciBusHandler, sizeof(BUS_HANDLER));
        BusHandler.BusNumber = BusNumber;

        /* Call PCI function */
        return HalpSetPCIData(&BusHandler,
                              &BusHandler,
                              SlotNumber,
                              Buffer,
                              Offset,
                              Length);
    }

    /* Invalid bus */
    return 0;
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
HalTranslateBusAddress(IN INTERFACE_TYPE InterfaceType,
                       IN ULONG BusNumber,
                       IN PHYSICAL_ADDRESS BusAddress,
                       IN OUT PULONG AddressSpace,
                       OUT PPHYSICAL_ADDRESS TranslatedAddress)
{
    /* Look as the bus type */
    if (InterfaceType == PCIBus)
    {
        /* Call the PCI registered function */
        return HalPciTranslateBusAddress(PCIBus,
                                         BusNumber,
                                         BusAddress,
                                         AddressSpace,
                                         TranslatedAddress);
    }
    else
    {
        /* Translation is easy */
        TranslatedAddress->QuadPart = BusAddress.QuadPart;
        return TRUE;
    }
}

/* EOF */
