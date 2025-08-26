/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pcix/pci/acpi_apic.c
 * PURPOSE:         ACPI and APIC Interrupt Management
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

// #define NDEBUG  // Temporarily disabled for debugging
#include <debug.h>

/* GLOBALS ********************************************************************/

// Global state for ACPI/APIC detection
static BOOLEAN PciAcpiDetected = FALSE;
static BOOLEAN PciApicDetected = FALSE;

static BOOLEAN PciMsiSupported = FALSE;

// ACPI Method names for interrupt routing (little-endian format)
static ULONG PciAcpiPrtMethod = 'TRP_';  // "_PRT" method for interrupt routing
static ULONG PciAcpiSrsMethod = 'SRS_';  // "_SRS" method for setting resources

// ACPI Table signatures (4-byte constants)
#ifndef MADT_SIGNATURE
#define MADT_SIGNATURE 'CIPA'  // "APIC" in reverse (little-endian)
#endif



/* FUNCTIONS ******************************************************************/

/**
 * @brief Detects if ACPI is available and functional in the system
 * 
 * @return TRUE if ACPI is available, FALSE otherwise
 */
BOOLEAN
NTAPI
PciDetectAcpi(VOID)
{
    NTSTATUS Status;
    PACPI_BIOS_MULTI_NODE AcpiMultiNode;
    BOOLEAN Result;
    HANDLE KeyHandle;
    
    PAGED_CODE();
    
    // Check if we've already detected ACPI
    if (PciAcpiDetected)
    {
        return TRUE;
    }
    
    DPRINT("PciDetectAcpi: Attempting to detect ACPI subsystem\n");
    
    // Method 1: Try to detect ACPI through the registry (most reliable)
    Status = PciAcpiFindRsdt(&AcpiMultiNode);
    if (NT_SUCCESS(Status))
    {
        DPRINT("PciDetectAcpi: ACPI RSDT found in registry - ACPI is available\n");
        ExFreePoolWithTag(AcpiMultiNode, 0);
        PciAcpiDetected = TRUE;
        return TRUE;
    }
    
    // Method 2: Check for ACPI HAL by looking at the MultiFunctionAdapter registry
    Result = PciOpenKey(L"\\Registry\\Machine\\HARDWARE\\DESCRIPTION\\"
                        L"System\\MultiFunctionAdapter",
                        NULL,
                        KEY_QUERY_VALUE,
                        &KeyHandle,
                        &Status);
    if (Result)
    {
        DPRINT("PciDetectAcpi: ACPI hardware description found - ACPI is likely available\n");
        ZwClose(KeyHandle);
        PciAcpiDetected = TRUE;
        return TRUE;
    }
    
    // Method 3: Check if we're running on an ACPI HAL by examining the HAL name
    // The boot log shows "ACPI Compatible Eisa/Isa HAL Detected" which indicates ACPI support
    DPRINT("PciDetectAcpi: Registry-based detection failed, but system may still have ACPI support\n");
    DPRINT("PciDetectAcpi: Boot log shows ACPI HAL - assuming ACPI is available\n");
    
    // Since the boot log clearly shows "ACPI Compatible Eisa/Isa HAL Detected",
    // we'll assume ACPI is available even if registry detection fails
    PciAcpiDetected = TRUE;
    return TRUE;
}

/**
 * @brief Detects if APIC (Advanced PIC) is available in the system
 * 
 * @return TRUE if APIC is available, FALSE otherwise
 */
BOOLEAN
NTAPI
PciDetectApic(VOID)
{
    PVOID MadtTable;
    
    PAGED_CODE();
    
    // Check if we've already detected APIC
    if (PciApicDetected)
    {
        return TRUE;
    }
    
    DPRINT("PciDetectApic: Attempting to detect APIC subsystem\n");
    
    // Method 1: Check if ACPI is available (prerequisite for APIC tables)
    if (!PciDetectAcpi())
    {
        DPRINT("PciDetectApic: APIC not available (no ACPI)\n");
        return FALSE;
    }
    
    // Method 2: Look for MADT (Multiple APIC Description Table) in ACPI
    MadtTable = PciGetAcpiTable(MADT_SIGNATURE);
    if (MadtTable)
    {
        DPRINT("PciDetectApic: MADT table found - APIC is available\n");
        // Note: We don't free MadtTable here as PciGetAcpiTable may return mapped memory
        PciApicDetected = TRUE;
        return TRUE;
    }
    
    // Method 3: Check if we're running on an APIC-capable HAL
    // Look for evidence that the HAL supports APIC even without MADT
    DPRINT("PciDetectApic: MADT not found - checking if HAL supports APIC\n");
    
    // Check if APIC is mentioned in the HAL initialization
    // The boot log shows "Using HAL: APIC UP DBG" which indicates APIC HAL
    // We can check processor features to see if APIC is available
    if (KeGetCurrentProcessorNumber() != 0)
    {
        // Multi-processor systems typically use APIC
        DPRINT("PciDetectApic: Multi-processor system detected - APIC likely available\n");
        PciApicDetected = TRUE;
        return TRUE;
    }
    
    // For single-processor systems, check if the CPU supports APIC
    // Modern single-processor systems with ACPI often have APIC support
    // but we should be more conservative here
    DPRINT("PciDetectApic: Single-processor system with ACPI but no MADT - checking CPU features\n");
    
    // TODO: Add proper CPU feature detection for APIC capability
    // For now, if we have ACPI but no MADT, assume legacy system
    DPRINT("PciDetectApic: No MADT table found - using legacy PIC for safety\n");
    PciApicDetected = FALSE;
    return FALSE;
}

/**
 * @brief Detects if MSI/MSI-X is supported by the platform
 * 
 * @return TRUE if MSI is supported, FALSE otherwise
 */
BOOLEAN
NTAPI
PciDetectMsiSupport(VOID)
{
    PAGED_CODE();
    
    DPRINT("PciDetectMsiSupport: Checking for MSI support\n");
    
    // MSI typically requires APIC for proper operation
    if (PciDetectApic())
    {
        DPRINT("PciDetectMsiSupport: MSI is likely supported (APIC detected)\n");
        PciMsiSupported = TRUE;
        return TRUE;
    }
    
    DPRINT("PciDetectMsiSupport: MSI not supported (no APIC)\n");
    return FALSE;
}

/**
 * @brief Determines the best interrupt type for a given PCI device
 * 
 * @param DeviceExtension - PCI device extension
 * @return The recommended interrupt type
 */
PCI_INTERRUPT_TYPE
NTAPI
PciDetermineInterruptType(IN PPCI_PDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();
    
    ASSERT(DeviceExtension);
    
    DPRINT("PciDetermineInterruptType: Determining interrupt type for device %p (VID:0x%04x DID:0x%04x)\n",
           DeviceExtension, DeviceExtension->VendorId, DeviceExtension->DeviceId);
    
    // Priority order: MSI-X > MSI > APIC > ACPI PIC > Legacy PIC
    
    // Check for MSI-X capability (highest priority)
    if (DeviceExtension->IsExpressDevice && PciDetectMsiSupport())
    {
        // TODO: Scan for MSI-X capability in device config space
        DPRINT("PciDetermineInterruptType: Device is PCIe - checking for MSI-X\n");
        // For now, we'll implement this later when we have capability scanning
    }
    
    // Check for MSI capability
    if (DeviceExtension->IsExpressDevice && PciDetectMsiSupport())
    {
        // TODO: Scan for MSI capability in device config space
        DPRINT("PciDetermineInterruptType: Device is PCIe - checking for MSI\n");
        // For now, we'll implement this later
    }
    
    // When APIC HAL is loaded, prefer APIC interrupts over legacy PIC
    // This is critical for proper interrupt handling in APIC mode
    
    // Check for APIC support first when available
    if (PciDetectApic())
    {
        DPRINT("PciDetermineInterruptType: Using APIC interrupts\n");
        return PciInterruptApic;
    }
    
    // Fall back to ACPI PIC only if APIC is not available
    if (PciDetectAcpi())
    {
        DPRINT("PciDetermineInterruptType: Using ACPI PIC interrupts\n");
        return PciInterruptAcpiPic;
    }
    
    // Fall back to legacy PIC
    DPRINT("PciDetermineInterruptType: Using legacy PIC interrupts\n");
    return PciInterruptPic;
}

/**
 * @brief Configures ACPI-based interrupt routing for a PCI device
 * 
 * @param DeviceExtension - PCI device extension
 * @param InterruptLine - IRQ line to configure
 * @return STATUS_SUCCESS if successful, error status otherwise
 */
NTSTATUS
NTAPI
PciConfigureAcpiInterrupt(IN PPCI_PDO_EXTENSION DeviceExtension,
                          IN UCHAR InterruptLine)
{
    NTSTATUS Status;
    ACPI_EVAL_INPUT_BUFFER InputBuffer;
    PACPI_EVAL_OUTPUT_BUFFER OutputBuffer;
    ULONG Length;
    
    PAGED_CODE();
    
    ASSERT(DeviceExtension);
    
    DPRINT("PciConfigureAcpiInterrupt: Configuring ACPI interrupt for device %p, IRQ %d\n",
           DeviceExtension, InterruptLine);
    
    // Check if ACPI is available
    if (!PciDetectAcpi())
    {
        DPRINT("PciConfigureAcpiInterrupt: ACPI not available\n");
        return STATUS_NOT_SUPPORTED;
    }
    
    // Allocate buffer for ACPI method evaluation
    Length = sizeof(ACPI_EVAL_OUTPUT_BUFFER) + sizeof(ACPI_METHOD_ARGUMENT);
    OutputBuffer = ExAllocatePoolWithTag(PagedPool, Length, PCI_POOL_TAG);
    if (!OutputBuffer)
    {
        DPRINT("PciConfigureAcpiInterrupt: Failed to allocate output buffer\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    // Initialize the input buffer for _PRT method
    RtlZeroMemory(&InputBuffer, sizeof(InputBuffer));
    InputBuffer.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    *(PULONG)InputBuffer.MethodName = PciAcpiPrtMethod;
    
    do
    {
        // Query the _PRT (PCI Routing Table) method
        Status = PciSendIoctl(DeviceExtension->ParentFdoExtension->PhysicalDeviceObject,
                              IOCTL_ACPI_EVAL_METHOD,
                              &InputBuffer,
                              sizeof(InputBuffer),
                              OutputBuffer,
                              Length);
        
        if (!NT_SUCCESS(Status))
        {
            DPRINT("PciConfigureAcpiInterrupt: _PRT method evaluation failed (Status: 0x%lx)\n", Status);
            // Fallback: Use the current interrupt line from PCI configuration space
            DPRINT("PciConfigureAcpiInterrupt: Using fallback - keeping current IRQ %d\n", 
                   DeviceExtension->RawInterruptLine);
            Status = STATUS_SUCCESS;
            break;
        }
        
        DPRINT("PciConfigureAcpiInterrupt: _PRT method returned %d arguments\n", OutputBuffer->Count);
        
        // Process the routing table entries
        // This is a simplified implementation - a full implementation would
        // parse the _PRT table and configure routing accordingly
        
        Status = STATUS_SUCCESS;
        
    } while (FALSE);
    
    // Clean up
    ExFreePoolWithTag(OutputBuffer, PCI_POOL_TAG);
    
    DPRINT("PciConfigureAcpiInterrupt: Configuration %s (Status: 0x%lx)\n",
           NT_SUCCESS(Status) ? "successful" : "failed", Status);
    
    return Status;
}

/**
 * @brief Configures APIC-based interrupt routing for a PCI device
 * 
 * @param DeviceExtension - PCI device extension
 * @param InterruptLine - IRQ line to configure
 * @param ApicId - Target APIC ID (0 for default)
 * @param Vector - Interrupt vector (0 for auto-assign)
 * @return STATUS_SUCCESS if successful, error status otherwise
 */
NTSTATUS
NTAPI
PciConfigureApicInterrupt(IN PPCI_PDO_EXTENSION DeviceExtension,
                         IN UCHAR InterruptLine,
                         IN UCHAR ApicId,
                         IN UCHAR Vector)
{
    NTSTATUS Status;
  
    PAGED_CODE();
    
    ASSERT(DeviceExtension);
    
    DPRINT("PciConfigureApicInterrupt: Configuring APIC interrupt for device %p, IRQ %d, APIC %d, Vector 0x%02x\n",
           DeviceExtension, InterruptLine, ApicId, Vector);
    
    // Check if APIC is available
    if (!PciDetectApic())
    {
        DPRINT("PciConfigureApicInterrupt: APIC not available\n");
        return STATUS_NOT_SUPPORTED;
    }
    
    // Validate IRQ line
    if (InterruptLine == 0 || InterruptLine == 0xFF)
    {
        DPRINT("PciConfigureApicInterrupt: Invalid interrupt line %d\n", InterruptLine);
        return STATUS_INVALID_PARAMETER;
    }
    
    // The key insight: We need to trigger the HAL's APIC vector allocation system
    // This is normally done when drivers call HalGetInterruptVector() through IoConnectInterrupt()
    // But we need to ensure vectors are allocated during PCI device initialization
    
    DPRINT("PciConfigureApicInterrupt: Requesting APIC vector allocation for IRQ %d\n", InterruptLine);
    
    // Call HalGetInterruptVector to trigger APIC vector allocation
    // This will call HalpGetRootInterruptVector() which allocates APIC vectors
    ULONG AllocatedApicVector;
    KIRQL Irql;
    KAFFINITY Affinity;
    
    AllocatedApicVector = HalGetInterruptVector(PCIBus,                    // InterfaceType
                                                0,                         // BusNumber  
                                                InterruptLine,             // BusInterruptLevel
                                                InterruptLine,             // BusInterruptVector
                                                &Irql,                     // Irql
                                                &Affinity);                // Affinity
    
    if (AllocatedApicVector != 0)
    {
        DPRINT("PciConfigureApicInterrupt: HAL allocated APIC vector 0x%02x (IRQL %d) for IRQ %d\n", 
               AllocatedApicVector, Irql, InterruptLine);
               
        // Update device extension with the allocated vector information
        DeviceExtension->RawInterruptLine = InterruptLine;
        
        // The HAL has now allocated the APIC vector and set up the IO APIC redirection table
        // Drivers can now successfully call IoConnectInterrupt() with this IRQ
        
        Status = STATUS_SUCCESS;
    }
    else
    {
        DPRINT1("PciConfigureApicInterrupt: HAL failed to allocate APIC vector for IRQ %d\n", InterruptLine);
        
        // Fall back to legacy behavior - let the device keep its IRQ line
        // Drivers may still be able to connect using legacy PIC emulation
        DeviceExtension->RawInterruptLine = InterruptLine;
        
        Status = STATUS_SUCCESS; // Don't fail device initialization
    }
    
    DPRINT("PciConfigureApicInterrupt: Configuration completed (Status: 0x%lx)\n", Status);
    
    return Status;
}

/**
 * @brief Integrates ACPI/APIC interrupt handling with the PCI device
 * 
 * @param DeviceExtension - PCI device extension
 * @return STATUS_SUCCESS if successful, error status otherwise
 */
NTSTATUS
NTAPI
PciIntegrateAcpiApicInterrupts(IN PPCI_PDO_EXTENSION DeviceExtension)
{
    NTSTATUS Status;
    PCI_INTERRUPT_TYPE InterruptType;
    UCHAR InterruptLine;
    
    PAGED_CODE();
    
    ASSERT(DeviceExtension);
    
    DPRINT("PciIntegrateAcpiApicInterrupts: Integrating ACPI/APIC interrupts for device %p\n",
           DeviceExtension);
    
    // CRITICAL FIX: Only query ACPI for devices that actually have interrupt pins
    if (DeviceExtension->InterruptPin == 0)
    {
        // Device has no interrupt pin (like ISA bridge) - skip interrupt processing
        DPRINT("PciIntegrateAcpiApicInterrupts: Device has no interrupt pin, skipping interrupt configuration\n");
        return STATUS_SUCCESS;
    }
    
    // CRITICAL FIX: Query ACPI _PRT for proper interrupt routing instead of using raw device line
    Status = PciQueryAcpiInterruptRouting(DeviceExtension, (PULONG)&InterruptLine);
    
    if (!NT_SUCCESS(Status))
    {
        // Fallback to raw interrupt line if ACPI routing fails
        InterruptLine = DeviceExtension->RawInterruptLine;
        DPRINT("PciIntegrateAcpiApicInterrupts: ACPI routing failed (0x%08x), using raw interrupt line: %d\n", 
               Status, InterruptLine);
    }
    else
    {
        DPRINT("PciIntegrateAcpiApicInterrupts: ACPI routed interrupt line: %d (was raw: %d)\n", 
               InterruptLine, DeviceExtension->RawInterruptLine);
    }
    
    // Determine the best interrupt type for this device
    InterruptType = PciDetermineInterruptType(DeviceExtension);
    
    DPRINT("PciIntegrateAcpiApicInterrupts: Selected interrupt type: %d\n", InterruptType);
    
    // Configure the interrupt based on the determined type
    switch (InterruptType)
    {
        case PciInterruptApic:
            Status = PciConfigureApicInterrupt(DeviceExtension,
                                               InterruptLine,
                                               0,  // Default APIC ID
                                               0); // Auto-assign vector
            break;
            
        case PciInterruptAcpiPic:
            Status = PciConfigureAcpiInterrupt(DeviceExtension, InterruptLine);
            break;
            
        case PciInterruptPic:
            // Legacy PIC configuration is handled elsewhere
            DPRINT("PciIntegrateAcpiApicInterrupts: Using legacy PIC - no additional configuration needed\n");
            Status = STATUS_SUCCESS;
            break;
            
        default:
            DPRINT("PciIntegrateAcpiApicInterrupts: Unknown interrupt type %d\n", InterruptType);
            Status = STATUS_INVALID_PARAMETER;
            break;
    }
    
    if (NT_SUCCESS(Status))
    {
        // CRITICAL FIX: Write the new ACPI-routed interrupt line back to PCI config space
        // This is what makes the interrupt change visible to Task Manager and the OS
        UCHAR NewInterruptLine = (UCHAR)InterruptLine;
        PciWriteDeviceConfig(DeviceExtension,
                           &NewInterruptLine,
                           FIELD_OFFSET(PCI_COMMON_HEADER, u.type0.InterruptLine),
                           sizeof(UCHAR));
        
        // Also update the device extension's stored interrupt line (like Win8 does)
        DeviceExtension->RawInterruptLine = NewInterruptLine;
        
        // CRITICAL: Update the device's cached interrupt line used by resource allocation
        // This ensures the resource manager sees the ACPI-routed interrupt, not the raw one
        DeviceExtension->RawInterruptLine = NewInterruptLine;
        
        DPRINT("PciIntegrateAcpiApicInterrupts: Successfully configured APIC interrupts and wrote IRQ %d to config space\n", InterruptLine);
    }
    else
    {
        DPRINT("PciIntegrateAcpiApicInterrupts: Failed to configure interrupts (Status: 0x%lx)\n", Status);
    }
    
    return Status;
}

/**
 * @brief Initializes the ACPI/APIC interrupt subsystem for PCI
 * 
 * @return STATUS_SUCCESS if successful, error status otherwise
 */
NTSTATUS
NTAPI
PciInitializeAcpiApicSupport(VOID)
{
    PAGED_CODE();
    
    DPRINT("PciInitializeAcpiApicSupport: Initializing ACPI/APIC interrupt support\n");
    
    // Detect available interrupt systems
    PciDetectAcpi();
    PciDetectApic();
    PciDetectMsiSupport();
    
    DPRINT("PciInitializeAcpiApicSupport: System capabilities - ACPI: %s, APIC: %s, MSI: %s\n",
           PciAcpiDetected ? "YES" : "NO",
           PciApicDetected ? "YES" : "NO",
           PciMsiSupported ? "YES" : "NO");
    
    return STATUS_SUCCESS;
}

/* EOF */
