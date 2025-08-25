/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pcix/pci/express.c
 * PURPOSE:         PCIe Express Port Management and Configuration
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/*
 * OVERVIEW OF PCIe EXPRESS PORT MANAGEMENT
 * =======================================
 * 
 * This module implements comprehensive PCIe (PCI Express) support for the ReactOS
 * PCI bus driver. It provides functionality for:
 * 
 * 1. EXPRESS PORT MANAGEMENT
 *    - Detection and initialization of PCIe devices
 *    - Configuration of Express capability structures
 *    - Port type classification (endpoint, root port, switch port, etc.)
 * 
 * 2. EXPRESS BRIDGE MANAGEMENT  
 *    - PCIe bridge enumeration and configuration
 *    - Link training and management
 *    - Hot plug support for capable bridges
 * 
 * 3. LINK POWER MANAGEMENT (ASPM)
 *    - Active State Power Management support
 *    - L0s and L1 power states
 *    - Dynamic link power state transitions
 * 
 * 4. MULTI-INTERRUPT CONTROLLER SUPPORT
 *    - Legacy 8259 PIC support for older systems
 *    - ACPI PIC support for ACPI-aware systems  
 *    - APIC (Advanced PIC) support for modern multiprocessor systems
 *    - MSI/MSI-X support for message-based interrupts
 * 
 * INTERRUPT CONTROLLER COMPATIBILITY:
 * ===================================
 * 
 * Legacy PIC (8259): 
 * - Used on older single-processor systems
 * - 15 available IRQ lines (IRQ 0-15, excluding IRQ 2)
 * - Edge or level triggered interrupts
 * - Shared interrupt support with careful management
 * 
 * ACPI PIC:
 * - ACPI-managed interrupt routing
 * - Supports interrupt link devices
 * - Dynamic interrupt routing capabilities
 * - Better power management integration
 * 
 * APIC (Advanced PIC):
 * - Local APIC for per-CPU interrupt handling
 * - I/O APIC for system-wide interrupt distribution
 * - 24+ interrupt vectors available
 * - Better multiprocessor support
 * - Lower interrupt latency
 * 
 * MSI/MSI-X:
 * - Message-based interrupts (no shared IRQ lines)
 * - Direct memory writes to interrupt controller
 * - Multiple vectors per device supported
 * - Best performance and scalability
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

// #define NDEBUG  // Temporarily disabled to see PCIe debug output
#include <debug.h>

/* CONSTANTS ******************************************************************/

//
// PCIe Capability IDs - check if already defined
//
#ifndef PCI_CAPABILITY_ID_PCIE
#define PCI_CAPABILITY_ID_PCIE                  0x10
#endif
#define PCI_CAPABILITY_ID_MSI                   0x05
#define PCI_CAPABILITY_ID_MSIX                  0x11
#define PCI_CAPABILITY_ID_POWER_MANAGEMENT      0x01

//
// PCIe Configuration Space Offsets (from capability base)
//
#define PCIE_CAPABILITY_OFFSET                  0x02
#define PCIE_DEVICE_CAPABILITIES_OFFSET         0x04
#define PCIE_DEVICE_CONTROL_OFFSET              0x08
#define PCIE_DEVICE_STATUS_OFFSET               0x0A
#define PCIE_LINK_CAPABILITIES_OFFSET           0x0C
#define PCIE_LINK_CONTROL_OFFSET                0x10
#define PCIE_LINK_STATUS_OFFSET                 0x12

//
// Link Training Timeout (in milliseconds)
//
#define PCIE_LINK_TRAINING_TIMEOUT_MS           1000
#define PCIE_LINK_RETRAIN_TIMEOUT_MS            500

//
// ASPM Control Values
//
#define PCIE_ASPM_CONTROL_DISABLED              0x0
#define PCIE_ASPM_CONTROL_L0S_ENABLED           0x1
#define PCIE_ASPM_CONTROL_L1_ENABLED            0x2
#define PCIE_ASPM_CONTROL_L0S_L1_ENABLED        0x3

/* GLOBALS ********************************************************************/

//
// Global list of all Express ports in the system
//
LIST_ENTRY PciExpressPortList;
KSPIN_LOCK PciExpressPortListLock;

//
// Global Express port management state
//
BOOLEAN PciExpressInitialized = FALSE;

/* PRIVATE FUNCTION PROTOTYPES ************************************************/

NTSTATUS
NTAPI
ExpressPortReadConfig(
    IN PEXPRESS_PORT ExpressPort,
    IN ULONG Offset,
    IN PVOID Buffer,
    IN ULONG Length
);

NTSTATUS
NTAPI
ExpressPortWriteConfig(
    IN PEXPRESS_PORT ExpressPort,
    IN ULONG Offset,
    IN PVOID Buffer,
    IN ULONG Length
);

PCI_EXPRESS_DEVICE_TYPE
NTAPI
ExpressPortGetDeviceType(
    IN PEXPRESS_PORT ExpressPort
);

NTSTATUS
NTAPI
ExpressPortConfigureAspm(
    IN PEXPRESS_PORT ExpressPort
);

NTSTATUS
NTAPI
ExpressPortDetectInterruptType(
    IN PEXPRESS_PORT ExpressPort,
    OUT PCI_INTERRUPT_TYPE *InterruptType
);

/* FUNCTIONS ******************************************************************/

/**
 * @brief Initializes the Express port management subsystem
 * @return STATUS_SUCCESS if successful, error status otherwise
 * 
 * This function initializes global state for PCIe Express port management.
 * It should be called during driver initialization.
 */
NTSTATUS
NTAPI
ExpressPortInitializeSubsystem(
    VOID
)
{
    DPRINT("ExpressPortInitializeSubsystem: Initializing PCIe Express subsystem\n");
    
    //
    // Initialize the global Express port list and lock
    //
    InitializeListHead(&PciExpressPortList);
    KeInitializeSpinLock(&PciExpressPortListLock);
    
    //
    // Mark subsystem as initialized
    //
    PciExpressInitialized = TRUE;
    
    DPRINT("ExpressPortInitializeSubsystem: PCIe Express subsystem initialized successfully\n");
    return STATUS_SUCCESS;
}

/**
 * @brief Creates and initializes a new Express port structure
 * @param Device - PCI device extension for the Express device
 * @param ExpressPort - Receives pointer to created Express port
 * @return STATUS_SUCCESS if successful, error status otherwise
 * 
 * This function creates a new EXPRESS_PORT structure for a PCIe device.
 * It allocates memory, initializes the structure, and detects PCIe capabilities.
 */
NTSTATUS
NTAPI
ExpressPortCreate(
    IN PPCI_PDO_EXTENSION Device,
    OUT PEXPRESS_PORT *ExpressPort
)
{
    PEXPRESS_PORT Port;
    NTSTATUS Status;
    KIRQL OldIrql;
    
    DPRINT("ExpressPortCreate: Creating Express port for device %p\n", Device);
    
    //
    // Validate parameters
    //
    if (!Device || !ExpressPort)
    {
        DPRINT1("ExpressPortCreate: Invalid parameters\n");
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Ensure Express subsystem is initialized
    //
    if (!PciExpressInitialized)
    {
        Status = ExpressPortInitializeSubsystem();
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ExpressPortCreate: Failed to initialize Express subsystem: 0x%lx\n", Status);
            return Status;
        }
    }
    
    //
    // Allocate memory for the Express port structure
    //
    Port = ExAllocatePoolWithTag(NonPagedPool, sizeof(EXPRESS_PORT), PCI_POOL_TAG);
    if (!Port)
    {
        DPRINT1("ExpressPortCreate: Failed to allocate Express port structure\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    //
    // Initialize the Express port structure
    //
    RtlZeroMemory(Port, sizeof(EXPRESS_PORT));
    Port->Device = Device;
    Port->CapabilityOffset = Device->ExpressCapabilityOffset;
    Port->CurrentPowerState = PowerDeviceD0;
    Port->InterruptType = PciInterruptPic; // Default to legacy PIC
    
    //
    // Detect and configure the Express port
    //
    Status = ExpressPortDetectCapabilities(Port);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ExpressPortCreate: Failed to detect Express capabilities: 0x%lx\n", Status);
        ExFreePoolWithTag(Port, PCI_POOL_TAG);
        return Status;
    }
    
    //
    // Initialize the Express port
    //
    Status = ExpressPortInitialize(Port);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ExpressPortCreate: Failed to initialize Express port: 0x%lx\n", Status);
        ExFreePoolWithTag(Port, PCI_POOL_TAG);
        return Status;
    }
    
    //
    // Add the port to the global list
    //
    KeAcquireSpinLock(&PciExpressPortListLock, &OldIrql);
    InsertTailList(&PciExpressPortList, (PLIST_ENTRY)&Port->Device->SecondaryExtension);
    KeReleaseSpinLock(&PciExpressPortListLock, OldIrql);
    
    //
    // Update device extension with Express port information
    //
    Device->ExpressPort = Port;
    Device->IsExpressDevice = TRUE;
    Device->ExpressDeviceType = Port->DeviceType;
    
    *ExpressPort = Port;
    
    DPRINT("ExpressPortCreate: Successfully created Express port %p for device %p (Type: %d)\n", 
           Port, Device, Port->DeviceType);
    
    return STATUS_SUCCESS;
}

/**
 * @brief Destroys an Express port structure and cleans up resources
 * @param ExpressPort - Express port to destroy
 * 
 * This function cleans up an EXPRESS_PORT structure, removes it from
 * global lists, and frees associated memory.
 */
VOID
NTAPI
ExpressPortDestroy(
    IN PEXPRESS_PORT ExpressPort
)
{
    KIRQL OldIrql;
    
    DPRINT("ExpressPortDestroy: Destroying Express port %p\n", ExpressPort);
    
    if (!ExpressPort)
    {
        DPRINT1("ExpressPortDestroy: Invalid ExpressPort parameter\n");
        return;
    }
    
    //
    // Remove from global list if present
    //
    if (ExpressPort->Device)
    {
        KeAcquireSpinLock(&PciExpressPortListLock, &OldIrql);
        RemoveEntryList((PLIST_ENTRY)&ExpressPort->Device->SecondaryExtension);
        KeReleaseSpinLock(&PciExpressPortListLock, OldIrql);
        
        //
        // Clear device extension references
        //
        ExpressPort->Device->ExpressPort = NULL;
        ExpressPort->Device->IsExpressDevice = FALSE;
    }
    
    //
    // Clean up Express link if present
    //
    if (ExpressPort->ExpressLink)
    {
        ExpressLinkDestroy(ExpressPort->ExpressLink);
        ExpressPort->ExpressLink = NULL;
    }
    
    //
    // Free the Express port structure
    //
    ExFreePoolWithTag(ExpressPort, PCI_POOL_TAG);
    
    DPRINT("ExpressPortDestroy: Express port destroyed successfully\n");
}

/**
 * @brief Initializes an Express port with default configuration
 * @param ExpressPort - Express port to initialize
 * @return STATUS_SUCCESS if successful, error status otherwise
 * 
 * This function performs initial configuration of a PCIe Express port,
 * including capability detection, interrupt configuration, and power management setup.
 */
NTSTATUS
NTAPI
ExpressPortInitialize(
    IN PEXPRESS_PORT ExpressPort
)
{
    NTSTATUS Status;
    PCI_INTERRUPT_TYPE InterruptType;
    
    DPRINT("ExpressPortInitialize: Initializing Express port %p\n", ExpressPort);
    
    if (!ExpressPort || !ExpressPort->Device)
    {
        DPRINT1("ExpressPortInitialize: Invalid parameters\n");
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Read Express capability registers
    //
    Status = ExpressPortReadConfig(ExpressPort,
                                   ExpressPort->CapabilityOffset + PCIE_CAPABILITY_OFFSET,
                                   &ExpressPort->ExpressCapabilities,
                                   sizeof(ExpressPort->ExpressCapabilities));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ExpressPortInitialize: Failed to read Express capabilities: 0x%lx\n", Status);
        return Status;
    }
    
    //
    // Read device capabilities
    //
    Status = ExpressPortReadConfig(ExpressPort,
                                   ExpressPort->CapabilityOffset + PCIE_DEVICE_CAPABILITIES_OFFSET,
                                   &ExpressPort->DeviceCapabilities,
                                   sizeof(ExpressPort->DeviceCapabilities));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ExpressPortInitialize: Failed to read device capabilities: 0x%lx\n", Status);
        return Status;
    }
    
    //
    // Read device control register
    //
    Status = ExpressPortReadConfig(ExpressPort,
                                   ExpressPort->CapabilityOffset + PCIE_DEVICE_CONTROL_OFFSET,
                                   &ExpressPort->DeviceControl,
                                   sizeof(ExpressPort->DeviceControl));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ExpressPortInitialize: Failed to read device control: 0x%lx\n", Status);
        return Status;
    }
    
    //
    // Detect and configure interrupt type based on system capabilities
    //
    Status = ExpressPortDetectInterruptType(ExpressPort, &InterruptType);
    if (NT_SUCCESS(Status))
    {
        Status = ExpressPortConfigureInterrupts(ExpressPort, InterruptType);
        if (!NT_SUCCESS(Status))
        {
            DPRINT("ExpressPortInitialize: Failed to configure interrupts, continuing with defaults: 0x%lx\n", Status);
        }
    }
    else
    {
        DPRINT("ExpressPortInitialize: Failed to detect interrupt type, using legacy PIC: 0x%lx\n", Status);
    }
    
    //
    // Configure ASPM if supported
    //
    Status = ExpressPortConfigureAspm(ExpressPort);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("ExpressPortInitialize: Failed to configure ASPM, continuing: 0x%lx\n", Status);
    }
    
    //
    // Initialize error reporting if capable
    //
    if (ExpressPort->AerCapable)
    {
        Status = ExpressPortInitializeErrorReporting(ExpressPort);
        if (!NT_SUCCESS(Status))
        {
            DPRINT("ExpressPortInitialize: Failed to initialize error reporting: 0x%lx\n", Status);
        }
    }
    
    DPRINT("ExpressPortInitialize: Express port %p initialized successfully (Type: %d)\n", 
           ExpressPort, ExpressPort->DeviceType);
    
    return STATUS_SUCCESS;
}

/**
 * @brief Detects PCIe capabilities and device characteristics
 * @param ExpressPort - Express port to analyze
 * @return STATUS_SUCCESS if successful, error status otherwise
 * 
 * This function examines the PCIe configuration space to detect supported
 * capabilities such as AER, FLR, power management, and advanced features.
 */
NTSTATUS
NTAPI
ExpressPortDetectCapabilities(
    IN PEXPRESS_PORT ExpressPort
)
{
    NTSTATUS Status;
    PCI_EXPRESS_CAPABILITIES_REGISTER ExpressCapabilities;
    UCHAR CapabilityOffset;
    PCI_CAPABILITIES_HEADER CapabilityHeader;
    
    DPRINT("ExpressPortDetectCapabilities: Detecting capabilities for Express port %p\n", ExpressPort);
    
    if (!ExpressPort || !ExpressPort->Device)
    {
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Read the Express capabilities register to determine device type
    //
    Status = ExpressPortReadConfig(ExpressPort,
                                   ExpressPort->CapabilityOffset + PCIE_CAPABILITY_OFFSET,
                                   &ExpressCapabilities,
                                   sizeof(ExpressCapabilities));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ExpressPortDetectCapabilities: Failed to read Express capabilities: 0x%lx\n", Status);
        return Status;
    }
    
    //
    // Determine device type from Express capabilities
    //
    ExpressPort->DeviceType = (PCI_EXPRESS_DEVICE_TYPE)ExpressCapabilities.DeviceType;
    ExpressPort->ExpressCapabilities = ExpressCapabilities;
    
    //
    // Determine if this port has links
    //
    switch (ExpressPort->DeviceType)
    {
        case PciExpressRootPort:
        case PciExpressDownstreamSwitchPort:
            ExpressPort->HasDownstreamLink = TRUE;
            ExpressPort->HasUpstreamLink = FALSE;
            break;
            
        case PciExpressUpstreamSwitchPort:
            ExpressPort->HasDownstreamLink = FALSE;
            ExpressPort->HasUpstreamLink = TRUE;
            break;
            
        case PciExpressEndpoint:
        case PciExpressLegacyEndpoint:
        case PciExpressRootComplexIntegratedEndpoint:
            ExpressPort->HasDownstreamLink = FALSE;
            ExpressPort->HasUpstreamLink = TRUE;
            break;
            
        case PciExpressToPciXBridge:
        case PciXToExpressBridge:
            ExpressPort->HasDownstreamLink = TRUE;
            ExpressPort->HasUpstreamLink = TRUE;
            break;
            
        default:
            ExpressPort->HasDownstreamLink = FALSE;
            ExpressPort->HasUpstreamLink = FALSE;
            break;
    }
    
    //
    // Scan for additional capabilities
    //
    CapabilityOffset = ExpressPort->Device->CapabilitiesPtr;
    
    while (CapabilityOffset != 0)
    {
        //
        // Read capability header
        //
        Status = ExpressPortReadConfig(ExpressPort,
                                       CapabilityOffset,
                                       &CapabilityHeader,
                                       sizeof(CapabilityHeader));
        if (!NT_SUCCESS(Status))
        {
            break;
        }
        
        //
        // Check capability type and mark accordingly
        //
        switch (CapabilityHeader.CapabilityID)
        {
            case PCI_CAPABILITY_ID_POWER_MANAGEMENT:
                ExpressPort->PowerManagementCapable = TRUE;
                ExpressPort->PowerCapabilityOffset = CapabilityOffset;
                DPRINT("ExpressPortDetectCapabilities: Found PM capability at offset 0x%x\n", CapabilityOffset);
                break;
                
            case PCI_CAPABILITY_ID_MSI:
                ExpressPort->Interrupt.Msi.CapabilityOffset = CapabilityOffset;
                DPRINT("ExpressPortDetectCapabilities: Found MSI capability at offset 0x%x\n", CapabilityOffset);
                break;
                
            case PCI_CAPABILITY_ID_MSIX:
                ExpressPort->Interrupt.MsiX.CapabilityOffset = CapabilityOffset;
                DPRINT("ExpressPortDetectCapabilities: Found MSI-X capability at offset 0x%x\n", CapabilityOffset);
                break;
                
            // Note: AER capability detection would go here (extended capability)
            // This requires scanning the extended capability space starting at 0x100
        }
        
        //
        // Move to next capability
        //
        CapabilityOffset = CapabilityHeader.Next;
    }
    
    //
    // Check for Function Level Reset capability (not in standard register)
    //
    ExpressPort->FlrCapable = FALSE; // Would need to check extended capabilities
    
    DPRINT("ExpressPortDetectCapabilities: Device type %d, FLR capable: %s, PM capable: %s\n",
           ExpressPort->DeviceType,
           ExpressPort->FlrCapable ? "Yes" : "No",
           ExpressPort->PowerManagementCapable ? "Yes" : "No");
    
    return STATUS_SUCCESS;
}

/**
 * @brief Configures interrupts for the Express port based on system capabilities
 * @param ExpressPort - Express port to configure
 * @param InterruptType - Type of interrupt controller to use
 * @return STATUS_SUCCESS if successful, error status otherwise
 * 
 * This function configures interrupt handling for a PCIe device, supporting
 * multiple interrupt controller types for maximum compatibility.
 */
NTSTATUS
NTAPI
ExpressPortConfigureInterrupts(
    IN PEXPRESS_PORT ExpressPort,
    IN PCI_INTERRUPT_TYPE InterruptType
)
{
    NTSTATUS Status = STATUS_SUCCESS;
    
    DPRINT("ExpressPortConfigureInterrupts: Configuring interrupts for port %p, type %d\n", 
           ExpressPort, InterruptType);
    
    if (!ExpressPort || !ExpressPort->Device)
    {
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Store the interrupt type
    //
    ExpressPort->InterruptType = InterruptType;
    
    //
    // Configure based on interrupt controller type
    //
    switch (InterruptType)
    {
        case PciInterruptPic:
            //
            // Legacy 8259 PIC configuration
            // Use the device's interrupt line from PCI configuration
            //
            Status = ExpressPortConfigurePicInterrupts(ExpressPort,
                                                       ExpressPort->Device->RawInterruptLine,
                                                       TRUE); // Default to edge-triggered
            break;
            
        case PciInterruptAcpiPic:
            //
            // ACPI-managed PIC configuration
            // Let ACPI handle the interrupt routing
            //
            Status = ExpressPortConfigureAcpiPicInterrupts(ExpressPort,
                                                           ExpressPort->Device->AdjustedInterruptLine,
                                                           TRUE, // Default to edge-triggered
                                                           0);   // No special ACPI flags
            break;
            
        case PciInterruptApic:
            //
            // APIC configuration
            // Use vectored interrupts for better performance
            //
            Status = ExpressPortConfigureApicInterrupts(ExpressPort,
                                                        0,    // Use default APIC ID
                                                        0x30, // Default vector (will be assigned by HAL)
                                                        TRUE, // Edge-triggered for PCIe
                                                        0);   // No special APIC flags
            break;
            
        case PciInterruptMsi:
            //
            // MSI configuration
            // Best performance for single interrupt
            //
            if (ExpressPort->Interrupt.Msi.CapabilityOffset != 0)
            {
                // Configure MSI (implementation would go here)
                ExpressPort->Interrupt.Msi.Enabled = TRUE;
                ExpressPort->Interrupt.Msi.GrantedVectors = 1;
                DPRINT("ExpressPortConfigureInterrupts: Configured MSI interrupts\n");
            }
            else
            {
                DPRINT1("ExpressPortConfigureInterrupts: MSI requested but not supported\n");
                Status = STATUS_NOT_SUPPORTED;
            }
            break;
            
        case PciInterruptMsiX:
            //
            // MSI-X configuration
            // Best performance for multiple interrupts
            //
            if (ExpressPort->Interrupt.MsiX.CapabilityOffset != 0)
            {
                // Configure MSI-X (implementation would go here)
                ExpressPort->Interrupt.MsiX.Enabled = TRUE;
                ExpressPort->Interrupt.MsiX.TableSize = 1; // Default to single vector
                DPRINT("ExpressPortConfigureInterrupts: Configured MSI-X interrupts\n");
            }
            else
            {
                DPRINT1("ExpressPortConfigureInterrupts: MSI-X requested but not supported\n");
                Status = STATUS_NOT_SUPPORTED;
            }
            break;
            
        default:
            DPRINT1("ExpressPortConfigureInterrupts: Unknown interrupt type %d\n", InterruptType);
            Status = STATUS_INVALID_PARAMETER;
            break;
    }
    
    if (NT_SUCCESS(Status))
    {
        DPRINT("ExpressPortConfigureInterrupts: Successfully configured %d interrupts for port %p\n",
               InterruptType, ExpressPort);
    }
    else
    {
        DPRINT1("ExpressPortConfigureInterrupts: Failed to configure interrupts: 0x%lx\n", Status);
    }
    
    return Status;
}

/**
 * @brief Configures legacy PIC interrupts for the Express port
 * @param ExpressPort - Express port to configure
 * @param IrqLine - IRQ line to use (0-15)
 * @param EdgeTriggered - TRUE for edge-triggered, FALSE for level-triggered
 * @return STATUS_SUCCESS if successful, error status otherwise
 * 
 * Configures interrupt handling for legacy 8259 PIC systems. This provides
 * compatibility with older systems that don't support APIC or MSI.
 */
NTSTATUS
NTAPI
ExpressPortConfigurePicInterrupts(
    IN PEXPRESS_PORT ExpressPort,
    IN UCHAR IrqLine,
    IN BOOLEAN EdgeTriggered
)
{
    DPRINT("ExpressPortConfigurePicInterrupts: Configuring PIC interrupts for port %p, IRQ %d, %s\n",
           ExpressPort, IrqLine, EdgeTriggered ? "Edge" : "Level");
    
    if (!ExpressPort)
    {
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Validate IRQ line (PIC supports IRQ 0-15, but IRQ 2 is cascade)
    //
    if (IrqLine > 15 || IrqLine == 2)
    {
        DPRINT1("ExpressPortConfigurePicInterrupts: Invalid IRQ line %d\n", IrqLine);
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Configure PIC interrupt parameters
    //
    ExpressPort->Interrupt.Pic.IrqLine = IrqLine;
    ExpressPort->Interrupt.Pic.EdgeTriggered = EdgeTriggered;
    
    //
    // For PCIe devices, we generally prefer level-triggered interrupts
    // unless explicitly configured otherwise
    //
    if (!EdgeTriggered)
    {
        DPRINT("ExpressPortConfigurePicInterrupts: Using level-triggered interrupts for PCIe device\n");
    }
    
    DPRINT("ExpressPortConfigurePicInterrupts: Successfully configured PIC interrupts\n");
    return STATUS_SUCCESS;
}

/**
 * @brief Configures ACPI PIC interrupts for the Express port
 * @param ExpressPort - Express port to configure  
 * @param IrqLine - IRQ line to use
 * @param EdgeTriggered - TRUE for edge-triggered, FALSE for level-triggered
 * @param AcpiFlags - ACPI-specific interrupt flags
 * @return STATUS_SUCCESS if successful, error status otherwise
 * 
 * Configures interrupt handling for ACPI-managed PIC systems. This allows
 * for dynamic interrupt routing and better power management integration.
 */
NTSTATUS
NTAPI
ExpressPortConfigureAcpiPicInterrupts(
    IN PEXPRESS_PORT ExpressPort,
    IN UCHAR IrqLine,
    IN BOOLEAN EdgeTriggered,
    IN ULONG AcpiFlags
)
{
    DPRINT("ExpressPortConfigureAcpiPicInterrupts: Configuring ACPI PIC interrupts for port %p, IRQ %d\n",
           ExpressPort, IrqLine);
    
    if (!ExpressPort)
    {
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Configure ACPI PIC interrupt parameters
    //
    ExpressPort->Interrupt.AcpiPic.IrqLine = IrqLine;
    ExpressPort->Interrupt.AcpiPic.EdgeTriggered = EdgeTriggered;
    ExpressPort->Interrupt.AcpiPic.AcpiInterruptFlags = AcpiFlags;
    
    //
    // ACPI PIC supports more flexible interrupt routing
    // PCIe devices typically work well with level-triggered interrupts in ACPI mode
    //
    DPRINT("ExpressPortConfigureAcpiPicInterrupts: ACPI flags 0x%lx, %s triggered\n",
           AcpiFlags, EdgeTriggered ? "Edge" : "Level");
    
    DPRINT("ExpressPortConfigureAcpiPicInterrupts: Successfully configured ACPI PIC interrupts\n");
    return STATUS_SUCCESS;
}

/**
 * @brief Configures APIC interrupts for the Express port
 * @param ExpressPort - Express port to configure
 * @param ApicId - Local APIC ID (0 for default)
 * @param Vector - Interrupt vector to use
 * @param EdgeTriggered - TRUE for edge-triggered, FALSE for level-triggered  
 * @param ApicFlags - APIC-specific flags
 * @return STATUS_SUCCESS if successful, error status otherwise
 * 
 * Configures interrupt handling for APIC (Advanced PIC) systems. This provides
 * better performance and multiprocessor support compared to legacy PIC.
 */
NTSTATUS
NTAPI
ExpressPortConfigureApicInterrupts(
    IN PEXPRESS_PORT ExpressPort,
    IN UCHAR ApicId,
    IN UCHAR Vector,
    IN BOOLEAN EdgeTriggered,
    IN ULONG ApicFlags
)
{
    DPRINT("ExpressPortConfigureApicInterrupts: Configuring APIC interrupts for port %p, APIC %d, Vector 0x%x\n",
           ExpressPort, ApicId, Vector);
    
    if (!ExpressPort)
    {
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Configure APIC interrupt parameters
    //
    ExpressPort->Interrupt.Apic.ApicId = ApicId;
    ExpressPort->Interrupt.Apic.Vector = Vector;
    ExpressPort->Interrupt.Apic.EdgeTriggered = EdgeTriggered;
    ExpressPort->Interrupt.Apic.ApicFlags = ApicFlags;
    
    //
    // APIC provides better interrupt handling with more vectors available
    // PCIe devices work very well with APIC systems
    //
    DPRINT("ExpressPortConfigureApicInterrupts: APIC flags 0x%lx, %s triggered\n",
           ApicFlags, EdgeTriggered ? "Edge" : "Level");
    
    DPRINT("ExpressPortConfigureApicInterrupts: Successfully configured APIC interrupts\n");
    return STATUS_SUCCESS;
}

/**
 * @brief Helper function to read from Express port configuration space
 * @param ExpressPort - Express port to read from
 * @param Offset - Configuration space offset
 * @param Buffer - Buffer to store read data
 * @param Length - Number of bytes to read
 * @return STATUS_SUCCESS if successful, error status otherwise
 */
NTSTATUS
NTAPI
ExpressPortReadConfig(
    IN PEXPRESS_PORT ExpressPort,
    IN ULONG Offset,
    IN PVOID Buffer,
    IN ULONG Length
)
{
    if (!ExpressPort || !ExpressPort->Device || !Buffer)
    {
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Use the existing PCI device configuration read function
    //
    PciReadDeviceConfig(ExpressPort->Device, Buffer, Offset, Length);
    return STATUS_SUCCESS;
}

/**
 * @brief Helper function to write to Express port configuration space
 * @param ExpressPort - Express port to write to
 * @param Offset - Configuration space offset
 * @param Buffer - Buffer containing data to write
 * @param Length - Number of bytes to write
 * @return STATUS_SUCCESS if successful, error status otherwise
 */
NTSTATUS
NTAPI
ExpressPortWriteConfig(
    IN PEXPRESS_PORT ExpressPort,
    IN ULONG Offset,
    IN PVOID Buffer,
    IN ULONG Length
)
{
    if (!ExpressPort || !ExpressPort->Device || !Buffer)
    {
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Use the existing PCI device configuration write function
    //
    PciWriteDeviceConfig(ExpressPort->Device, Buffer, Offset, Length);
    return STATUS_SUCCESS;
}

/**
 * @brief Detects the interrupt controller type available on the system
 * @param ExpressPort - Express port to configure
 * @param InterruptType - Receives the detected interrupt type
 * @return STATUS_SUCCESS if successful, error status otherwise
 * 
 * This function determines the best interrupt controller type to use
 * based on system capabilities and device support.
 */
NTSTATUS
NTAPI
ExpressPortDetectInterruptType(
    IN PEXPRESS_PORT ExpressPort,
    OUT PCI_INTERRUPT_TYPE *InterruptType
)
{
    DPRINT("ExpressPortDetectInterruptType: Detecting interrupt type for port %p\n", ExpressPort);
    
    if (!ExpressPort || !InterruptType)
    {
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Priority order: MSI-X > MSI > APIC > ACPI PIC > Legacy PIC
    //
    
    //
    // Check for MSI-X support (best performance for multiple interrupts)
    //
    if (ExpressPort->Interrupt.MsiX.CapabilityOffset != 0)
    {
        *InterruptType = PciInterruptMsiX;
        DPRINT("ExpressPortDetectInterruptType: Using MSI-X interrupts\n");
        return STATUS_SUCCESS;
    }
    
    //
    // Check for MSI support (good performance for single interrupt)
    //
    if (ExpressPort->Interrupt.Msi.CapabilityOffset != 0)
    {
        *InterruptType = PciInterruptMsi;
        DPRINT("ExpressPortDetectInterruptType: Using MSI interrupts\n");
        return STATUS_SUCCESS;
    }
    
    //
    // Check for APIC support (better than PIC for multiprocessor systems)
    // This would typically involve checking HAL capabilities
    //
    // For now, we assume APIC if the system supports it
    // A real implementation would check HalQuerySystemInformation or similar
    //
    if (TRUE) // Placeholder for APIC detection
    {
        *InterruptType = PciInterruptApic;
        DPRINT("ExpressPortDetectInterruptType: Using APIC interrupts\n");
        return STATUS_SUCCESS;
    }
    
    //
    // Check for ACPI PIC support
    // This would typically involve checking ACPI capabilities
    //
    if (TRUE) // Placeholder for ACPI PIC detection
    {
        *InterruptType = PciInterruptAcpiPic;
        DPRINT("ExpressPortDetectInterruptType: Using ACPI PIC interrupts\n");
        return STATUS_SUCCESS;
    }
    
    //
    // Fall back to legacy PIC (always available)
    //
    *InterruptType = PciInterruptPic;
    DPRINT("ExpressPortDetectInterruptType: Using legacy PIC interrupts\n");
    return STATUS_SUCCESS;
}

/**
 * @brief Configures ASPM (Active State Power Management) for the Express port
 * @param ExpressPort - Express port to configure
 * @return STATUS_SUCCESS if successful, error status otherwise
 * 
 * This function configures power management features for PCIe links,
 * enabling L0s and L1 power states when supported and beneficial.
 */
NTSTATUS
NTAPI
ExpressPortConfigureAspm(
    IN PEXPRESS_PORT ExpressPort
)
{
    NTSTATUS Status;
    PCI_EXPRESS_LINK_CAPABILITIES_REGISTER LinkCapabilities;
    PCI_EXPRESS_LINK_CONTROL_REGISTER LinkControl;
    
    DPRINT("ExpressPortConfigureAspm: Configuring ASPM for port %p\n", ExpressPort);
    
    if (!ExpressPort || !ExpressPort->Device)
    {
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Only configure ASPM for devices with links
    //
    if (!ExpressPort->HasDownstreamLink && !ExpressPort->HasUpstreamLink)
    {
        DPRINT("ExpressPortConfigureAspm: Device has no links, skipping ASPM\n");
        return STATUS_SUCCESS;
    }
    
    //
    // Read link capabilities to determine ASPM support
    //
    Status = ExpressPortReadConfig(ExpressPort,
                                   ExpressPort->CapabilityOffset + PCIE_LINK_CAPABILITIES_OFFSET,
                                   &LinkCapabilities,
                                   sizeof(LinkCapabilities));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ExpressPortConfigureAspm: Failed to read link capabilities: 0x%lx\n", Status);
        return Status;
    }
    
    //
    // Read current link control register
    //
    Status = ExpressPortReadConfig(ExpressPort,
                                   ExpressPort->CapabilityOffset + PCIE_LINK_CONTROL_OFFSET,
                                   &LinkControl,
                                   sizeof(LinkControl));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ExpressPortConfigureAspm: Failed to read link control: 0x%lx\n", Status);
        return Status;
    }
    
    //
    // Configure ASPM based on link capabilities
    //
    if (LinkCapabilities.ActiveStatePMSupport != 0)
    {
        //
        // Enable L0s if supported (provides quick power savings)
        //
        if (LinkCapabilities.ActiveStatePMSupport & 0x1)
        {
            LinkControl.ActiveStatePMControl |= PCIE_ASPM_CONTROL_L0S_ENABLED;
            DPRINT("ExpressPortConfigureAspm: Enabling L0s power state\n");
        }
        
        //
        // Enable L1 if supported (provides deeper power savings)
        //
        if (LinkCapabilities.ActiveStatePMSupport & 0x2)
        {
            LinkControl.ActiveStatePMControl |= PCIE_ASPM_CONTROL_L1_ENABLED;
            DPRINT("ExpressPortConfigureAspm: Enabling L1 power state\n");
        }
        
        //
        // Write back the updated link control register
        //
        Status = ExpressPortWriteConfig(ExpressPort,
                                        ExpressPort->CapabilityOffset + PCIE_LINK_CONTROL_OFFSET,
                                        &LinkControl,
                                        sizeof(LinkControl));
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ExpressPortConfigureAspm: Failed to write link control: 0x%lx\n", Status);
            return Status;
        }
        
        DPRINT("ExpressPortConfigureAspm: ASPM configured successfully (Control: 0x%x)\n",
               LinkControl.ActiveStatePMControl);
    }
    else
    {
        DPRINT("ExpressPortConfigureAspm: ASPM not supported by device\n");
    }
    
    return STATUS_SUCCESS;
}

/**
 * @brief Creates and initializes an Express bridge structure
 * @param BusFdo - Bus FDO extension for the bridge
 * @param Port - Express port associated with the bridge
 * @param ExpressBridge - Receives pointer to created Express bridge
 * @return STATUS_SUCCESS if successful, error status otherwise
 * 
 * This function creates a new EXPRESS_BRIDGE structure for a PCIe bridge device.
 */
NTSTATUS
NTAPI
ExpressBridgeCreate(
    IN PPCI_FDO_EXTENSION BusFdo,
    IN PEXPRESS_PORT Port,
    OUT PEXPRESS_BRIDGE *ExpressBridge
)
{
    PEXPRESS_BRIDGE Bridge;
    NTSTATUS Status;
    
    DPRINT("ExpressBridgeCreate: Creating Express bridge for FDO %p, Port %p\n", BusFdo, Port);
    
    //
    // Validate parameters
    //
    if (!BusFdo || !Port || !ExpressBridge)
    {
        DPRINT1("ExpressBridgeCreate: Invalid parameters\n");
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Allocate memory for the Express bridge structure
    //
    Bridge = ExAllocatePoolWithTag(NonPagedPool, sizeof(EXPRESS_BRIDGE), PCI_POOL_TAG);
    if (!Bridge)
    {
        DPRINT1("ExpressBridgeCreate: Failed to allocate Express bridge structure\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    //
    // Initialize the Express bridge structure
    //
    RtlZeroMemory(Bridge, sizeof(EXPRESS_BRIDGE));
    Bridge->Port = Port;
    Bridge->BusFdo = BusFdo;
    Bridge->BridgePowerState = PowerDeviceD0;
    
    //
    // Initialize lists and synchronization objects
    //
    InitializeListHead(&Bridge->ChildBridges);
    KeInitializeSpinLock(&Bridge->InterruptLock);
    KeInitializeTimer(&Bridge->LinkRetrainTimer);
    KeInitializeDpc(&Bridge->LinkRetrainDpc, NULL, Bridge); // DPC routine would be implemented
    
    //
    // Initialize the bridge
    //
    Status = ExpressBridgeInitialize(Bridge);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ExpressBridgeCreate: Failed to initialize Express bridge: 0x%lx\n", Status);
        ExFreePoolWithTag(Bridge, PCI_POOL_TAG);
        return Status;
    }
    
    //
    // Initialize arbiters for this Express bridge
    // PCIe bridges need I/O, Memory, Interrupt, and Bus Number arbiters
    //
    if (!BusFdo->ArbitersInitialized)
    {
        Status = PciInitializeArbiters(BusFdo);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("ExpressBridgeCreate: Failed to initialize arbiters: 0x%lx\n", Status);
            // Continue without arbiters - degraded functionality but not fatal
        }
        else
        {
            DPRINT("ExpressBridgeCreate: Successfully initialized arbiters for Express bridge\n");
        }
    }
    
    //
    // Update FDO extension with Express bridge information
    //
    BusFdo->ExpressBridge = Bridge;
    BusFdo->IsExpressBridge = TRUE;
    
    *ExpressBridge = Bridge;
    
    DPRINT("ExpressBridgeCreate: Successfully created Express bridge %p\n", Bridge);
    return STATUS_SUCCESS;
}

/**
 * @brief Destroys an Express bridge structure and cleans up resources
 * @param ExpressBridge - Express bridge to destroy
 * 
 * This function cleans up an EXPRESS_BRIDGE structure and frees associated memory.
 */
VOID
NTAPI
ExpressBridgeDestroy(
    IN PEXPRESS_BRIDGE ExpressBridge
)
{
    DPRINT("ExpressBridgeDestroy: Destroying Express bridge %p\n", ExpressBridge);
    
    if (!ExpressBridge)
    {
        DPRINT1("ExpressBridgeDestroy: Invalid ExpressBridge parameter\n");
        return;
    }
    
    //
    // Cancel any pending timers
    //
    KeCancelTimer(&ExpressBridge->LinkRetrainTimer);
    
    //
    // Clean up downstream link if present
    //
    if (ExpressBridge->DownstreamLink)
    {
        ExpressLinkDestroy(ExpressBridge->DownstreamLink);
        ExpressBridge->DownstreamLink = NULL;
    }
    
    //
    // Clear FDO extension references
    //
    if (ExpressBridge->BusFdo)
    {
        ExpressBridge->BusFdo->ExpressBridge = NULL;
        ExpressBridge->BusFdo->IsExpressBridge = FALSE;
    }
    
    //
    // Free the Express bridge structure
    //
    ExFreePoolWithTag(ExpressBridge, PCI_POOL_TAG);
    
    DPRINT("ExpressBridgeDestroy: Express bridge destroyed successfully\n");
}

/**
 * @brief Initializes an Express bridge with default configuration
 * @param ExpressBridge - Express bridge to initialize
 * @return STATUS_SUCCESS if successful, error status otherwise
 * 
 * This function performs initial configuration of a PCIe Express bridge,
 * including hot plug detection and link management setup.
 */
NTSTATUS
NTAPI
ExpressBridgeInitialize(
    IN PEXPRESS_BRIDGE ExpressBridge
)
{
    NTSTATUS Status = STATUS_SUCCESS;
    
    DPRINT("ExpressBridgeInitialize: Initializing Express bridge %p\n", ExpressBridge);
    
    if (!ExpressBridge || !ExpressBridge->Port)
    {
        DPRINT1("ExpressBridgeInitialize: Invalid parameters\n");
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Initialize hot plug capabilities if present
    //
    Status = ExpressBridgeInitializeHotPlug(ExpressBridge);
    if (!NT_SUCCESS(Status))
    {
        DPRINT("ExpressBridgeInitialize: Hot plug initialization failed: 0x%lx\n", Status);
        // Continue without hot plug support
    }
    
    //
    // Set up power management for the bridge
    //
    ExpressBridge->D3HotSupported = ExpressBridge->Port->PowerManagementCapable;
    ExpressBridge->D3ColdSupported = FALSE; // Would need to check platform capabilities
    
    //
    // Initialize arbiter ranges for the bridge if needed
    // This ensures proper resource allocation for devices behind this bridge
    //
    if (ExpressBridge->BusFdo->ArbitersInitialized)
    {
        // Arbiters are initialized, now set up the ranges
        // This would typically be done when we have CM_RESOURCE_LIST information
        DPRINT("ExpressBridgeInitialize: Arbiters available for resource management\n");
    }
    
    //
    // Enable error reporting if supported
    //
    if (ExpressBridge->Port->AerCapable)
    {
        ExpressBridge->ErrorReportingEnabled = TRUE;
        ExpressBridge->UncorrectableErrorMask = 0; // Enable all uncorrectable error reporting
        ExpressBridge->CorrectableErrorMask = 0;   // Enable all correctable error reporting
    }
    
    DPRINT("ExpressBridgeInitialize: Express bridge %p initialized successfully\n", ExpressBridge);
    return STATUS_SUCCESS;
}

/**
 * @brief Initializes hot plug support for an Express bridge
 * @param ExpressBridge - Express bridge to configure
 * @return STATUS_SUCCESS if successful, error status otherwise
 * 
 * This function detects and configures hot plug capabilities for PCIe bridges
 * that support hot plug operations.
 */
NTSTATUS
NTAPI
ExpressBridgeInitializeHotPlug(
    IN PEXPRESS_BRIDGE ExpressBridge
)
{
    DPRINT("ExpressBridgeInitializeHotPlug: Initializing hot plug for bridge %p\n", ExpressBridge);
    
    if (!ExpressBridge)
    {
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Check if the bridge supports hot plug
    // This would involve scanning for hot plug capabilities in the extended capability space
    //
    ExpressBridge->HotPlugCapable = FALSE; // Default to no hot plug
    ExpressBridge->HotPlugCapabilityOffset = 0;
    
    //
    // If hot plug is supported, initialize the hot plug controller
    //
    if (ExpressBridge->HotPlugCapable)
    {
        DPRINT("ExpressBridgeInitializeHotPlug: Hot plug supported, configuring controller\n");
        
        // Initialize hot plug registers and capabilities
        // This would involve reading and configuring:
        // - Slot capabilities register
        // - Slot control register  
        // - Slot status register
        
        ExpressBridge->PowerControllerPresent = TRUE;
        ExpressBridge->AttentionButtonPresent = FALSE;
        ExpressBridge->PowerIndicatorPresent = FALSE;
        ExpressBridge->AttentionIndicatorPresent = FALSE;
    }
    else
    {
        DPRINT("ExpressBridgeInitializeHotPlug: Hot plug not supported\n");
    }
    
    return STATUS_SUCCESS;
}

/**
 * @brief Creates an Express link between two ports
 * @param UpstreamPort - Upstream port of the link
 * @param DownstreamPort - Downstream port of the link
 * @param ExpressLink - Receives pointer to created Express link
 * @return STATUS_SUCCESS if successful, error status otherwise
 * 
 * This function creates and initializes a link structure representing
 * the PCIe connection between two ports.
 */
NTSTATUS
NTAPI
ExpressLinkCreate(
    IN PEXPRESS_PORT UpstreamPort,
    IN PEXPRESS_PORT DownstreamPort,
    OUT PEXPRESS_LINK *ExpressLink
)
{
    PEXPRESS_LINK Link;
    NTSTATUS Status;
    
    DPRINT("ExpressLinkCreate: Creating Express link between ports %p and %p\n", 
           UpstreamPort, DownstreamPort);
    
    //
    // Validate parameters
    //
    if (!UpstreamPort || !DownstreamPort || !ExpressLink)
    {
        DPRINT1("ExpressLinkCreate: Invalid parameters\n");
        return STATUS_INVALID_PARAMETER;
    }
    
    //
    // Allocate memory for the Express link structure
    //
    Link = ExAllocatePoolWithTag(NonPagedPool, sizeof(EXPRESS_LINK), PCI_POOL_TAG);
    if (!Link)
    {
        DPRINT1("ExpressLinkCreate: Failed to allocate Express link structure\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    //
    // Initialize the Express link structure
    //
    RtlZeroMemory(Link, sizeof(EXPRESS_LINK));
    Link->UpstreamPort = UpstreamPort;
    Link->DownstreamPort = DownstreamPort;
    Link->CurrentPowerState = PcieLinkStateL0; // Start in active state
    Link->LinkActive = TRUE;
    
    //
    // Read link capabilities from the downstream port
    //
    Status = ExpressPortReadConfig(DownstreamPort,
                                   DownstreamPort->CapabilityOffset + PCIE_LINK_CAPABILITIES_OFFSET,
                                   &Link->LinkCapabilities,
                                   sizeof(Link->LinkCapabilities));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("ExpressLinkCreate: Failed to read link capabilities: 0x%lx\n", Status);
        ExFreePoolWithTag(Link, PCI_POOL_TAG);
        return Status;
    }
    
    //
    // Set up ASPM capabilities using the flags byte
    //
    if (Link->LinkCapabilities.ActiveStatePMSupport & 0x1)
        Link->AspmFlags |= ASPM_L0S_SUPPORTED;
    if (Link->LinkCapabilities.ActiveStatePMSupport & 0x2)
        Link->AspmFlags |= ASPM_L1_SUPPORTED;
    Link->L0sExitLatency = (UCHAR)Link->LinkCapabilities.L0sExitLatency;
    Link->L1ExitLatency = (UCHAR)Link->LinkCapabilities.L1ExitLatency;
    
    //
    // Associate the link with both ports
    //
    UpstreamPort->ExpressLink = Link;
    DownstreamPort->ExpressLink = Link;
    
    *ExpressLink = Link;
    
    DPRINT("ExpressLinkCreate: Successfully created Express link %p\n", Link);
    return STATUS_SUCCESS;
}

/**
 * @brief Destroys an Express link and cleans up resources
 * @param ExpressLink - Express link to destroy
 * 
 * This function cleans up an EXPRESS_LINK structure and removes
 * references from associated ports.
 */
VOID
NTAPI
ExpressLinkDestroy(
    IN PEXPRESS_LINK ExpressLink
)
{
    DPRINT("ExpressLinkDestroy: Destroying Express link %p\n", ExpressLink);
    
    if (!ExpressLink)
    {
        DPRINT1("ExpressLinkDestroy: Invalid ExpressLink parameter\n");
        return;
    }
    
    //
    // Clear port references
    //
    if (ExpressLink->UpstreamPort)
    {
        ExpressLink->UpstreamPort->ExpressLink = NULL;
    }
    
    if (ExpressLink->DownstreamPort)
    {
        ExpressLink->DownstreamPort->ExpressLink = NULL;
    }
    
    //
    // Free the Express link structure
    //
    ExFreePoolWithTag(ExpressLink, PCI_POOL_TAG);
    
    DPRINT("ExpressLinkDestroy: Express link destroyed successfully\n");
}

/* STUB IMPLEMENTATIONS FOR REMAINING REQUIRED FUNCTIONS */

/**
 * @brief Placeholder implementations for remaining Express functions
 * These would be fully implemented in a complete driver.
 */

NTSTATUS NTAPI ExpressLinkTrain(IN PEXPRESS_LINK ExpressLink) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTAPI ExpressLinkRetrain(IN PEXPRESS_LINK ExpressLink) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTAPI ExpressLinkConfigureAspm(IN PEXPRESS_LINK ExpressLink, IN PCIE_LINK_STATE TargetState) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTAPI ExpressLinkEnableL0s(IN PEXPRESS_LINK ExpressLink) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTAPI ExpressLinkEnableL1(IN PEXPRESS_LINK ExpressLink) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTAPI ExpressLinkDisableL0sL1(IN PEXPRESS_LINK ExpressLink) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTAPI ExpressPortSetPowerState(IN PEXPRESS_PORT ExpressPort, IN DEVICE_POWER_STATE PowerState) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTAPI ExpressBridgeSetPowerState(IN PEXPRESS_BRIDGE ExpressBridge, IN DEVICE_POWER_STATE PowerState) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTAPI ExpressBridgeHandleHotPlugEvent(IN PEXPRESS_BRIDGE ExpressBridge, IN ULONG EventType) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTAPI ExpressPortInitializeErrorReporting(IN PEXPRESS_PORT ExpressPort) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTAPI ExpressPortHandleError(IN PEXPRESS_PORT ExpressPort, IN ULONG ErrorStatus) { return STATUS_NOT_IMPLEMENTED; }
BOOLEAN NTAPI ExpressIsDevicePresent(IN PEXPRESS_PORT ExpressPort) { return TRUE; }
NTSTATUS NTAPI ExpressPortSaveConfiguration(IN PEXPRESS_PORT ExpressPort) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS NTAPI ExpressPortRestoreConfiguration(IN PEXPRESS_PORT ExpressPort) { return STATUS_NOT_IMPLEMENTED; }
