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
    
    // For now, this is a simplified allocation - just return a value in the preferred range
    // A full implementation would use the actual arbiter to allocate from available ranges
    
    if (InterruptType == PciInterruptMsi || InterruptType == PciInterruptMsiX)
    {
        // MSI vectors - allocate from high range
        *AllocatedInterrupt = PreferredBase + (DeviceExtension->Slot.u.AsULONG % 100);
        DPRINT("PciAllocateAcpiApicInterrupt: Allocated MSI interrupt %d\n", *AllocatedInterrupt);
    }
    else if (InterruptType == PciInterruptApic)
    {
        // APIC vectors - allocate from medium range
        *AllocatedInterrupt = PreferredBase + (DeviceExtension->Slot.u.AsULONG % 50);
        DPRINT("PciAllocateAcpiApicInterrupt: Allocated APIC interrupt %d\n", *AllocatedInterrupt);
    }
    else
    {
        // Legacy/ACPI PIC - use the device's current interrupt line
        *AllocatedInterrupt = DeviceExtension->RawInterruptLine;
        if (*AllocatedInterrupt == 0 || *AllocatedInterrupt > 15)
        {
            // Assign a default if invalid
            *AllocatedInterrupt = 11;  // Common IRQ for PCI devices
        }
        DPRINT("PciAllocateAcpiApicInterrupt: Using/assigned legacy interrupt %d\n", *AllocatedInterrupt);
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
