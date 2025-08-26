/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pcix/arb/arb_irq_acpi_apic.c
 * PURPOSE:         ACPI/APIC Interrupt Arbiter Enhancement
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>
#include <arbiter.h>
#include <acpiioct.h>

// #define NDEBUG  // Temporarily disabled for ACPI/APIC debugging
#include <debug.h>

/* GLOBALS ********************************************************************/

// Enhanced interrupt range for ACPI/APIC systems
static ULONG PciAcpiApicInterruptBase = 16;    // Start after legacy PIC range
static ULONG PciAcpiApicInterruptLimit = 255;  // APIC can handle up to 255 vectors
static ULONG PciMsiInterruptBase = 256;        // MSI vectors start higher
static ULONG PciMsiInterruptLimit = 4095;     // MSI can handle many more vectors

/* FUNCTIONS ******************************************************************/

/**
 * @brief Parse ACPI _PRT table entry to extract interrupt routing
 * 
 * @param PrtPackage - ACPI _PRT package entry (4 elements: Address, Pin, Source, SourceIndex)
 * @param DeviceNumber - PCI device number to match
 * @param InterruptPin - PCI interrupt pin to match (INTA=1, INTB=2, etc.)
 * @param RoutedInterrupt - Output for routed interrupt number
 * @return STATUS_SUCCESS if matching entry found, error otherwise
 */
NTSTATUS
NTAPI
PciParsePrtPackageEntry(IN PACPI_METHOD_ARGUMENT PrtPackage,
                        IN ULONG DeviceNumber,
                        IN UCHAR InterruptPin,
                        OUT PULONG RoutedInterrupt)
{
    PACPI_METHOD_ARGUMENT AddressArg, PinArg, SourceArg, SourceIndexArg;
    ULONG PrtDeviceNumber, PrtFunction;
    UCHAR PrtPin;
    
    ASSERT(PrtPackage);
    ASSERT(RoutedInterrupt);
    
    *RoutedInterrupt = 0;
    
    // _PRT package must have exactly 4 elements
    if (PrtPackage->Type != ACPI_METHOD_ARGUMENT_PACKAGE || 
        PrtPackage->DataLength < (4 * sizeof(ACPI_METHOD_ARGUMENT)))
    {
        DPRINT1("PciParsePrtPackageEntry: Invalid _PRT package format\n");
        return STATUS_INVALID_PARAMETER;
    }
    
    // Extract the 4 _PRT elements: Address, Pin, Source, SourceIndex
    AddressArg = (PACPI_METHOD_ARGUMENT)PrtPackage->Data;
    PinArg = (PACPI_METHOD_ARGUMENT)((PUCHAR)AddressArg + FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) + AddressArg->DataLength);
    SourceArg = (PACPI_METHOD_ARGUMENT)((PUCHAR)PinArg + FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) + PinArg->DataLength);
    SourceIndexArg = (PACPI_METHOD_ARGUMENT)((PUCHAR)SourceArg + FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) + SourceArg->DataLength);
    
    // Validate argument types
    if (AddressArg->Type != ACPI_METHOD_ARGUMENT_INTEGER ||
        PinArg->Type != ACPI_METHOD_ARGUMENT_INTEGER)
    {
        DPRINT1("PciParsePrtPackageEntry: Invalid _PRT entry argument types\n");
        return STATUS_INVALID_PARAMETER;
    }
    
    // Extract device number and function from _PRT address field
    // Address format: (Device << 16) | Function
    PrtDeviceNumber = (AddressArg->Argument >> 16) & 0xFFFF;
    PrtFunction = AddressArg->Argument & 0xFFFF;
    PrtPin = (UCHAR)PinArg->Argument;
    
    DPRINT("PciParsePrtPackageEntry: _PRT entry - Device %d.%d Pin %d\n", 
           PrtDeviceNumber, PrtFunction, PrtPin);
    
    // Check if this entry matches our device and pin
    if (PrtDeviceNumber != DeviceNumber || PrtPin != (InterruptPin - 1))
    {
        // Not a match - this is normal, _PRT tables have many entries
        return STATUS_NO_MORE_ENTRIES;
    }
    
    DPRINT("PciParsePrtPackageEntry: Found matching _PRT entry for device %d.%d pin %d\n",
           DeviceNumber, PrtFunction, InterruptPin);
    
    // Check source type - can be either link device or direct interrupt
    if (SourceArg->Type == ACPI_METHOD_ARGUMENT_STRING)
    {
        // Link device reference - this requires ACPI link node resolution
        // For now, we'll implement a simplified fallback
        DPRINT("PciParsePrtPackageEntry: Link device routing not fully implemented, using fallback\n");
        
        // Use a reasonable default based on device position
        *RoutedInterrupt = 16 + ((DeviceNumber + InterruptPin - 1) % 4);
        
        DPRINT("PciParsePrtPackageEntry: Link device fallback -> IRQ %d\n", *RoutedInterrupt);
    }
    else if (SourceArg->Type == ACPI_METHOD_ARGUMENT_INTEGER && SourceArg->Argument == 0)
    {
        // Direct interrupt specification - SourceIndex contains the IRQ
        if (SourceIndexArg->Type == ACPI_METHOD_ARGUMENT_INTEGER)
        {
            *RoutedInterrupt = (ULONG)SourceIndexArg->Argument;
            DPRINT("PciParsePrtPackageEntry: Direct interrupt -> IRQ %d\n", *RoutedInterrupt);
        }
        else
        {
            DPRINT1("PciParsePrtPackageEntry: Invalid SourceIndex argument type\n");
            return STATUS_INVALID_PARAMETER;
        }
    }
    else
    {
        DPRINT1("PciParsePrtPackageEntry: Unknown source type or format\n");
        return STATUS_INVALID_PARAMETER;
    }
    
    return STATUS_SUCCESS;
}

/**
 * @brief Query ACPI _PRT method for interrupt routing
 * 
 * @param DeviceExtension - PCI device extension
 * @param RoutedInterrupt - Output for routed interrupt number
 * @return STATUS_SUCCESS if successful, error status otherwise
 */
NTSTATUS
NTAPI
PciQueryAcpiInterruptRouting(IN PPCI_PDO_EXTENSION DeviceExtension,
                             OUT PULONG RoutedInterrupt)
{
    NTSTATUS Status;
    PACPI_EVAL_INPUT_BUFFER InputBuffer = NULL;
    PACPI_EVAL_OUTPUT_BUFFER OutputBuffer = NULL;
    PACPI_METHOD_ARGUMENT PrtPackage;
    ULONG DeviceNumber, FunctionNumber;
    ULONG BufferSize, i;
    
    ASSERT(DeviceExtension);
    ASSERT(RoutedInterrupt);
    
    *RoutedInterrupt = 0;
    
    // Extract device and function numbers
    DeviceNumber = DeviceExtension->Slot.u.bits.DeviceNumber;
    FunctionNumber = DeviceExtension->Slot.u.bits.FunctionNumber;
    
    DPRINT("PciQueryAcpiInterruptRouting: Querying ACPI _PRT for device %d.%d pin %d\n", 
           DeviceNumber, FunctionNumber, DeviceExtension->InterruptPin);
    
    // CRITICAL FIX: Handle devices without interrupt pins (like ISA bridge)
    if (DeviceExtension->InterruptPin == 0)
    {
        DPRINT("PciQueryAcpiInterruptRouting: Device has no interrupt pin, skipping ACPI routing\n");
        return STATUS_NOT_SUPPORTED;
    }
    
    // Validate interrupt pin
    if (DeviceExtension->InterruptPin < 1 || DeviceExtension->InterruptPin > 4)
    {
        DPRINT1("PciQueryAcpiInterruptRouting: Invalid interrupt pin %d\n", DeviceExtension->InterruptPin);
        return STATUS_INVALID_PARAMETER;
    }
    
    // CRITICAL FIX: Allocate buffers to prevent stack corruption
    InputBuffer = ExAllocatePoolWithTag(PagedPool, sizeof(ACPI_EVAL_INPUT_BUFFER), PCI_POOL_TAG);
    if (!InputBuffer)
    {
        DPRINT1("PciQueryAcpiInterruptRouting: Failed to allocate input buffer\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    BufferSize = sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 4096; // Reserve space for _PRT table
    OutputBuffer = ExAllocatePoolWithTag(PagedPool, BufferSize, PCI_POOL_TAG);
    if (!OutputBuffer)
    {
        DPRINT1("PciQueryAcpiInterruptRouting: Failed to allocate output buffer\n");
        ExFreePoolWithTag(InputBuffer, PCI_POOL_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    // Zero the buffers to prevent garbage data
    RtlZeroMemory(InputBuffer, sizeof(ACPI_EVAL_INPUT_BUFFER));
    RtlZeroMemory(OutputBuffer, BufferSize);
    
    // Initialize input buffer for _PRT method evaluation
    InputBuffer->Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    
    // CRITICAL FIX: Use correct method name for _PRT
    *(PULONG)InputBuffer->MethodName = 'TRP_';  // "_PRT" in little-endian
    
    do
    {
        // Call _PRT method on the parent PCI bus
        Status = PciSendIoctl(DeviceExtension->ParentFdoExtension->PhysicalDeviceObject,
                              IOCTL_ACPI_EVAL_METHOD,
                              InputBuffer,
                              sizeof(ACPI_EVAL_INPUT_BUFFER),
                              OutputBuffer,
                              BufferSize);
        
        if (!NT_SUCCESS(Status))
        {
            DPRINT("PciQueryAcpiInterruptRouting: _PRT method evaluation failed (Status: 0x%lx)\n", Status);
            // Use fallback mapping
            *RoutedInterrupt = 16 + ((DeviceNumber + DeviceExtension->InterruptPin - 1) % 4);
            DPRINT("PciQueryAcpiInterruptRouting: Using fallback mapping -> IRQ %d\n", *RoutedInterrupt);
            Status = STATUS_SUCCESS;
            break;
        }
        
        DPRINT("PciQueryAcpiInterruptRouting: _PRT method returned %d packages\n", OutputBuffer->Count);
        
        // Parse _PRT table to find matching entry
        PrtPackage = OutputBuffer->Argument;
        Status = STATUS_NOT_FOUND;
        
        for (i = 0; i < OutputBuffer->Count; i++)
        {
            NTSTATUS ParseStatus = PciParsePrtPackageEntry(PrtPackage, 
                                                           DeviceNumber,
                                                           DeviceExtension->InterruptPin,
                                                           RoutedInterrupt);
            
            if (NT_SUCCESS(ParseStatus))
            {
                DPRINT("PciQueryAcpiInterruptRouting: Found routing: Device %d.%d Pin %d -> IRQ %d\n",
                       DeviceNumber, FunctionNumber, DeviceExtension->InterruptPin, *RoutedInterrupt);
                Status = STATUS_SUCCESS;
                break;
            }
            else if (ParseStatus != STATUS_NO_MORE_ENTRIES)
            {
                DPRINT1("PciQueryAcpiInterruptRouting: Error parsing _PRT entry %d (Status: 0x%lx)\n", i, ParseStatus);
            }
            
            // Move to next package in the _PRT table
            PrtPackage = (PACPI_METHOD_ARGUMENT)((PUCHAR)PrtPackage + 
                         FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) + PrtPackage->DataLength);
        }
        
        if (!NT_SUCCESS(Status))
        {
            DPRINT("PciQueryAcpiInterruptRouting: No matching _PRT entry found, using fallback\n");
            *RoutedInterrupt = 16 + ((DeviceNumber + DeviceExtension->InterruptPin - 1) % 4);
            Status = STATUS_SUCCESS;
        }
        
    } while (FALSE);
    
    // Clean up both buffers
    if (OutputBuffer)
        ExFreePoolWithTag(OutputBuffer, PCI_POOL_TAG);
    if (InputBuffer)
        ExFreePoolWithTag(InputBuffer, PCI_POOL_TAG);
    
    DPRINT("PciQueryAcpiInterruptRouting: Result - IRQ %d (Status: 0x%lx)\n", *RoutedInterrupt, Status);
    return Status;
}

/**
 * @brief Enhanced interrupt range initialization for ACPI/APIC systems
 * 
 * @param Arbiter - Pointer to the interrupt arbiter instance
 * @return STATUS_SUCCESS if successful, error status otherwise
 */
NTSTATUS
NTAPI
PciInitializeAcpiApicInterruptRanges(IN PARBITER_INSTANCE Arbiter)
{
    NTSTATUS Status;
    RTL_RANGE_LIST_ITERATOR Iterator;
    PRTL_RANGE Range;
    
    PAGED_CODE();
    
    ASSERT(Arbiter);
    
    DPRINT("PciInitializeAcpiApicInterruptRanges: Setting up enhanced interrupt ranges\n");
    
    // Check if APIC is available for extended interrupt handling
    if (PciDetectApic())
    {
        DPRINT("PciInitializeAcpiApicInterruptRanges: APIC detected - adding extended interrupt range %d-%d\n",
               PciAcpiApicInterruptBase, PciAcpiApicInterruptLimit);
        
        // Add the extended APIC interrupt range
        Status = RtlAddRange(Arbiter->Allocation,
                             PciAcpiApicInterruptBase,
                             PciAcpiApicInterruptLimit,
                             RTL_RANGE_LIST_ADD_SHARED,
                             RTL_RANGE_LIST_ADD_IF_CONFLICT,
                             NULL,
                             NULL);
        
        if (!NT_SUCCESS(Status))
        {
            DPRINT("PciInitializeAcpiApicInterruptRanges: Failed to add APIC range (Status: 0x%lx)\n", Status);
            return Status;
        }
    }
    
    // Check if MSI is supported for even higher interrupt vectors
    if (PciDetectMsiSupport())
    {
        DPRINT("PciInitializeAcpiApicInterruptRanges: MSI detected - adding MSI interrupt range %d-%d\n",
               PciMsiInterruptBase, PciMsiInterruptLimit);
        
        // Add the MSI interrupt range
        Status = RtlAddRange(Arbiter->Allocation,
                             PciMsiInterruptBase,
                             PciMsiInterruptLimit,
                             RTL_RANGE_LIST_ADD_SHARED,
                             RTL_RANGE_LIST_ADD_IF_CONFLICT,
                             NULL,
                             NULL);
        
        if (!NT_SUCCESS(Status))
        {
            DPRINT("PciInitializeAcpiApicInterruptRanges: Failed to add MSI range (Status: 0x%lx)\n", Status);
            // Non-fatal - continue without MSI
        }
    }
    
    // Debug: Print all available ranges
    RtlGetFirstRange(Arbiter->Allocation, &Iterator, &Range);
    while (Range)
    {
        DPRINT("PciInitializeAcpiApicInterruptRanges: Available range: %I64u-%I64u\n",
               Range->Start, Range->End);
        
        if (!RtlGetNextRange(&Iterator, &Range, TRUE))
        {
            break;
        }
    }
    
    DPRINT("PciInitializeAcpiApicInterruptRanges: Enhanced interrupt ranges initialized successfully\n");
    
    return STATUS_SUCCESS;
}

/**
 * @brief Enhanced interrupt allocation that considers ACPI/APIC capabilities
 * 
 * @param DeviceExtension - PCI device extension requesting the interrupt
 * @param InterruptType - Type of interrupt being requested
 * @param AllocatedInterrupt - Pointer to receive the allocated interrupt number
 * @return STATUS_SUCCESS if successful, error status otherwise
 */
NTSTATUS
NTAPI
PciAllocateAcpiApicInterrupt(IN PPCI_PDO_EXTENSION DeviceExtension,
                             IN PCI_INTERRUPT_TYPE InterruptType,
                             OUT PULONG AllocatedInterrupt)
{
    NTSTATUS Status;
    ULONG PreferredBase, PreferredLimit;
    
    PAGED_CODE();
    
    ASSERT(DeviceExtension);
    ASSERT(AllocatedInterrupt);
    
    DPRINT("PciAllocateAcpiApicInterrupt: Allocating %s interrupt for device %p (VID:0x%04x DID:0x%04x)\n",
           (InterruptType == PciInterruptApic) ? "APIC" :
           (InterruptType == PciInterruptAcpiPic) ? "ACPI PIC" : 
           (InterruptType == PciInterruptMsi) ? "MSI" :
           (InterruptType == PciInterruptMsiX) ? "MSI-X" : "Legacy PIC",
           DeviceExtension, DeviceExtension->VendorId, DeviceExtension->DeviceId);
    
    // Determine the preferred interrupt range based on type
    switch (InterruptType)
    {
        case PciInterruptMsiX:
        case PciInterruptMsi:
            PreferredBase = PciMsiInterruptBase;
            PreferredLimit = PciMsiInterruptLimit;
            DPRINT("PciAllocateAcpiApicInterrupt: Using MSI range %d-%d\n", PreferredBase, PreferredLimit);
            break;
            
        case PciInterruptApic:
            PreferredBase = PciAcpiApicInterruptBase;
            PreferredLimit = PciAcpiApicInterruptLimit;
            DPRINT("PciAllocateAcpiApicInterrupt: Using APIC range %d-%d\n", PreferredBase, PreferredLimit);
            break;
            
        case PciInterruptAcpiPic:
        case PciInterruptPic:
        default:
            PreferredBase = 0;
            PreferredLimit = 15;  // Legacy PIC range
            DPRINT("PciAllocateAcpiApicInterrupt: Using legacy PIC range %d-%d\n", PreferredBase, PreferredLimit);
            break;
    }
    
    // CRITICAL FIX: Proper interrupt allocation based on system configuration
    
    if (InterruptType == PciInterruptMsi || InterruptType == PciInterruptMsiX)
    {
        // MSI vectors - allocate from high range (0x100-0xFFF)
        *AllocatedInterrupt = PciMsiInterruptBase + (DeviceExtension->Slot.u.AsULONG % 100);
        DPRINT("PciAllocateAcpiApicInterrupt: Allocated MSI interrupt %d\n", *AllocatedInterrupt);
    }
    else if (InterruptType == PciInterruptApic)
    {
        // CRITICAL: Use HAL to allocate APIC interrupts properly
        KIRQL Irql;
        KAFFINITY Affinity;
        
        // CRITICAL FIX: Use ACPI-routed interrupt instead of raw device line
        ULONG AcpiRoutedInterrupt = 0;
        if (NT_SUCCESS(PciQueryAcpiInterruptRouting(DeviceExtension, &AcpiRoutedInterrupt)))
        {
            // Use ACPI-routed interrupt
            *AllocatedInterrupt = HalGetInterruptVector(
                PCIBus,
                0, // Bus number
                AcpiRoutedInterrupt, // Bus interrupt level (ACPI-routed)
                AcpiRoutedInterrupt, // Bus interrupt vector (ACPI-routed)
                &Irql,
                &Affinity
            );
            DPRINT("PciAllocateAcpiApicInterrupt: Using ACPI-routed IRQ %d\n", AcpiRoutedInterrupt);
        }
        else
        {
            // Fallback to raw interrupt line
            *AllocatedInterrupt = HalGetInterruptVector(
                PCIBus,
                0, // Bus number
                DeviceExtension->RawInterruptLine, // Bus interrupt level
                DeviceExtension->RawInterruptLine, // Bus interrupt vector  
                &Irql,
                &Affinity
            );
            DPRINT1("PciAllocateAcpiApicInterrupt: ACPI routing failed, using raw IRQ %d\n", DeviceExtension->RawInterruptLine);
        }
        
        if (*AllocatedInterrupt == 0)
        {
            // Fallback: allocate from APIC range manually
            *AllocatedInterrupt = PciAcpiApicInterruptBase + (DeviceExtension->Slot.u.AsULONG % 8);
            DPRINT1("PciAllocateAcpiApicInterrupt: HAL allocation failed, using fallback interrupt %d\n", *AllocatedInterrupt);
        }
        else
        {
            DPRINT("PciAllocateAcpiApicInterrupt: HAL allocated APIC interrupt %d (IRQL %d)\n", *AllocatedInterrupt, Irql);
        }
    }
    else
    {
        // Legacy/ACPI PIC - use the device's current interrupt line but validate it
        *AllocatedInterrupt = DeviceExtension->RawInterruptLine;
        
        // CRITICAL FIX: Validate legacy interrupt assignment
        if (*AllocatedInterrupt == 0 || *AllocatedInterrupt > 15)
        {
            // Use ACPI _PRT method to get proper interrupt routing
            ULONG AcpiRoutedInterrupt = 0;
            
            if (NT_SUCCESS(PciQueryAcpiInterruptRouting(DeviceExtension, &AcpiRoutedInterrupt)))
            {
                *AllocatedInterrupt = AcpiRoutedInterrupt;
                DPRINT("PciAllocateAcpiApicInterrupt: ACPI routed interrupt %d\n", *AllocatedInterrupt);
            }
            else
            {
                // Final fallback - assign a sensible default
                *AllocatedInterrupt = 11;  // Common IRQ for PCI devices
                DPRINT1("PciAllocateAcpiApicInterrupt: ACPI routing failed, using default interrupt %d\n", *AllocatedInterrupt);
            }
        }
        else
        {
            DPRINT("PciAllocateAcpiApicInterrupt: Using device interrupt line %d\n", *AllocatedInterrupt);
        }
    }
    
    Status = STATUS_SUCCESS;
    
    DPRINT("PciAllocateAcpiApicInterrupt: Successfully allocated interrupt %d (Status: 0x%lx)\n",
           *AllocatedInterrupt, Status);
    
    return Status;
}

/**
 * @brief Validates that an interrupt allocation is compatible with ACPI/APIC
 * 
 * @param DeviceExtension - PCI device extension
 * @param InterruptType - Type of interrupt
 * @param InterruptNumber - Interrupt number to validate
 * @return TRUE if valid, FALSE otherwise
 */
BOOLEAN
NTAPI
PciValidateAcpiApicInterrupt(IN PPCI_PDO_EXTENSION DeviceExtension,
                             IN PCI_INTERRUPT_TYPE InterruptType,
                             IN ULONG InterruptNumber)
{
    PAGED_CODE();
    
    ASSERT(DeviceExtension);
    
    DPRINT("PciValidateAcpiApicInterrupt: Validating %s interrupt %d for device %p\n",
           (InterruptType == PciInterruptApic) ? "APIC" :
           (InterruptType == PciInterruptAcpiPic) ? "ACPI PIC" : 
           (InterruptType == PciInterruptMsi) ? "MSI" :
           (InterruptType == PciInterruptMsiX) ? "MSI-X" : "Legacy PIC",
           InterruptNumber, DeviceExtension);
    
    // Validate interrupt number ranges
    switch (InterruptType)
    {
        case PciInterruptMsiX:
        case PciInterruptMsi:
            if (InterruptNumber >= PciMsiInterruptBase && InterruptNumber <= PciMsiInterruptLimit)
            {
                DPRINT("PciValidateAcpiApicInterrupt: MSI interrupt %d is valid\n", InterruptNumber);
                return TRUE;
            }
            break;
            
        case PciInterruptApic:
            if (InterruptNumber >= PciAcpiApicInterruptBase && InterruptNumber <= PciAcpiApicInterruptLimit)
            {
                DPRINT("PciValidateAcpiApicInterrupt: APIC interrupt %d is valid\n", InterruptNumber);
                return TRUE;
            }
            break;
            
        case PciInterruptAcpiPic:
        case PciInterruptPic:
            if (InterruptNumber <= 15)
            {
                DPRINT("PciValidateAcpiApicInterrupt: Legacy/ACPI PIC interrupt %d is valid\n", InterruptNumber);
                return TRUE;
            }
            break;
            
        default:
            DPRINT("PciValidateAcpiApicInterrupt: Unknown interrupt type %d\n", InterruptType);
            return FALSE;
    }
    
    DPRINT("PciValidateAcpiApicInterrupt: Interrupt %d is NOT valid for type %d\n",
           InterruptNumber, InterruptType);
    
    return FALSE;
}

/* EOF */
