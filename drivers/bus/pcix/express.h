/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pcix/express.h
 * PURPOSE:         PCIe Express Port Management Structures and Definitions
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

#pragma once

//
// Make sure we have all necessary types available
//
#ifndef _NTDDK_
#include <ntddk.h>
#endif

//
// PCIe Link States for ASPM (Active State Power Management)
//
typedef enum _PCIE_LINK_STATE
{
    PcieLinkStateL0 = 0,        // Active state
    PcieLinkStateL0s = 1,       // Standby state
    PcieLinkStateL1 = 2,        // Low power active state
    PcieLinkStateL2 = 3,        // Low power idle state
    PcieLinkStateL3 = 4         // Power off state
} PCIE_LINK_STATE;

//
// Interrupt Controller Types for PCIe support
//
typedef enum _PCI_INTERRUPT_TYPE
{
    PciInterruptPic = 0,        // Legacy 8259 PIC
    PciInterruptAcpiPic = 1,    // ACPI-managed PIC
    PciInterruptApic = 2,       // Advanced PIC (Local APIC + I/O APIC)
    PciInterruptMsi = 3,        // Message Signaled Interrupts
    PciInterruptMsiX = 4        // Extended Message Signaled Interrupts
} PCI_INTERRUPT_TYPE;

//
// Forward declarations to avoid circular dependencies
//
struct _EXPRESS_PORT;
struct _EXPRESS_BRIDGE;
struct _EXPRESS_LINK;
struct _PCI_PDO_EXTENSION;
struct _PCI_FDO_EXTENSION;

//
// PCIe Express Link Structure
// Represents a PCIe link between two devices/ports
//
typedef struct _EXPRESS_LINK
{
    //
    // Link identification and parent pointers
    //
    struct _EXPRESS_PORT *UpstreamPort;        // Upstream port of this link
    struct _EXPRESS_PORT *DownstreamPort;      // Downstream port of this link
    struct _EXPRESS_BRIDGE *ParentBridge;      // Parent bridge (if any)
    
    //
    // Link capabilities and current state
    //
    PCI_EXPRESS_LINK_CAPABILITIES_REGISTER LinkCapabilities;
    PCI_EXPRESS_LINK_CONTROL_REGISTER LinkControl;
    USHORT LinkStatus;                 // Current link status
    
    //
    // ASPM (Active State Power Management) information - pack into single UCHAR
    //
    UCHAR AspmFlags;                   // Bit flags for ASMP state
    // Bit 0: L0s state supported
    // Bit 1: L1 state supported  
    // Bit 2: L0s state enabled
    // Bit 3: L1 state enabled
    // Bit 4: ASPM overridden by policy
    // Bits 5-7: Reserved
    
    //
    // Link training and status
    //
    BOOLEAN LinkRetraining;            // Link is currently retraining
    BOOLEAN LinkActive;                // Data Link Layer is active
    UCHAR CurrentLinkSpeed;            // Current negotiated link speed
    UCHAR CurrentLinkWidth;            // Current negotiated link width
    
    //
    // Power management
    //
    PCIE_LINK_STATE CurrentPowerState; // Current link power state
    UCHAR L0sExitLatency;              // L0s exit latency
    UCHAR L1ExitLatency;               // L1 exit latency
    
} EXPRESS_LINK, *PEXPRESS_LINK;

//
// PCIe Express Port Structure
// Represents a single PCIe port (endpoint, root port, switch port, etc.)
//
typedef struct _EXPRESS_PORT
{
    //
    // Basic port information
    //
    struct _PCI_PDO_EXTENSION *Device; // Associated PCI device extension
    PCI_EXPRESS_DEVICE_TYPE DeviceType; // PCIe device type from capability
    UCHAR CapabilityOffset;            // Offset to PCIe capability in config space
    
    //
    // PCIe capability registers
    //
    PCI_EXPRESS_CAPABILITIES_REGISTER ExpressCapabilities;
    PCI_EXPRESS_DEVICE_CAPABILITIES_REGISTER DeviceCapabilities;
    PCI_EXPRESS_DEVICE_CONTROL_REGISTER DeviceControl;
    USHORT DeviceStatus;               // Device status register
    
    //
    // Link information (for ports that have links)
    //
    PEXPRESS_LINK ExpressLink;         // Link associated with this port
    BOOLEAN HasDownstreamLink;         // Port has a downstream link
    BOOLEAN HasUpstreamLink;           // Port has an upstream link
    
    //
    // Interrupt handling for different controller types
    //
    PCI_INTERRUPT_TYPE InterruptType;  // Type of interrupt controller
    union
    {
        struct  // Legacy PIC support
        {
            UCHAR IrqLine;             // IRQ line for legacy PIC
            BOOLEAN EdgeTriggered;     // Edge vs level triggered
        } Pic;
        
        struct  // ACPI PIC support  
        {
            UCHAR IrqLine;             // IRQ line for ACPI PIC
            BOOLEAN EdgeTriggered;     // Edge vs level triggered
            ULONG AcpiInterruptFlags;  // ACPI-specific interrupt flags
        } AcpiPic;
        
        struct  // APIC support
        {
            UCHAR ApicId;              // Local APIC ID
            UCHAR Vector;              // Interrupt vector
            BOOLEAN EdgeTriggered;     // Edge vs level triggered
            ULONG ApicFlags;           // APIC-specific flags
        } Apic;
        
        struct  // MSI support
        {
            UCHAR CapabilityOffset;    // MSI capability offset
            BOOLEAN Enabled;           // MSI enabled
            UCHAR GrantedVectors;      // Number of granted vectors
        } Msi;
        
        struct  // MSI-X support
        {
            UCHAR CapabilityOffset;    // MSI-X capability offset
            BOOLEAN Enabled;           // MSI-X enabled
            USHORT TableSize;          // MSI-X table size
        } MsiX;
    } Interrupt;
    
    //
    // Power management
    //
    BOOLEAN PowerManagementCapable;    // Device supports power management
    UCHAR PowerCapabilityOffset;       // PM capability offset
    DEVICE_POWER_STATE CurrentPowerState; // Current D-state
    
    //
    // Error handling and reporting
    //
    BOOLEAN AerCapable;                // Advanced Error Reporting capable
    UCHAR AerCapabilityOffset;         // AER capability offset
    ULONG ErrorStatus;                 // Current error status
    
    //
    // Advanced features
    //
    BOOLEAN FlrCapable;                // Function Level Reset capable
    BOOLEAN AtomicsSupported;          // Atomics operations supported
    BOOLEAN AtsCapable;                // Address Translation Services capable
    BOOLEAN PasidCapable;              // Process Address Space ID capable
    
} EXPRESS_PORT, *PEXPRESS_PORT;

//
// PCIe Express Bridge Structure  
// Represents a PCIe bridge (root port, switch port, etc.)
//
typedef struct _EXPRESS_BRIDGE
{
    //
    // Bridge hierarchy information
    //
    PEXPRESS_PORT Port;                // Associated Express port
    struct _PCI_FDO_EXTENSION *BusFdo; // Bus FDO extension for bridge
    struct _EXPRESS_BRIDGE *Parent;    // Parent bridge in hierarchy
    LIST_ENTRY ChildBridges;            // List of child bridges
    LIST_ENTRY ListEntry;               // Entry in parent's child list
    
    //
    // Bridge-specific capabilities
    //
    BOOLEAN HotPlugCapable;            // Hot plug capable
    BOOLEAN PowerControllerPresent;    // Power controller present
    BOOLEAN AttentionButtonPresent;    // Attention button present
    BOOLEAN PowerIndicatorPresent;     // Power indicator present
    BOOLEAN AttentionIndicatorPresent; // Attention indicator present
    
    //
    // Hot plug support
    //
    UCHAR HotPlugCapabilityOffset;     // Hot plug capability offset
    USHORT SlotCapabilities;           // Slot capabilities register
    USHORT SlotControl;                // Slot control register
    USHORT SlotStatus;                 // Slot status register
    
    //
    // Link management for bridge
    //
    PEXPRESS_LINK DownstreamLink;      // Downstream link from this bridge
    BOOLEAN LinkRetrainingInProgress;  // Link retrain in progress
    KTIMER LinkRetrainTimer;           // Timer for link retrain timeout
    KDPC LinkRetrainDpc;               // DPC for link retrain completion
    
    //
    // Power management for bridge
    //
    BOOLEAN D3HotSupported;            // D3hot power state supported
    BOOLEAN D3ColdSupported;           // D3cold power state supported
    DEVICE_POWER_STATE BridgePowerState; // Current bridge power state
    
    //
    // Interrupt handling specific to bridge
    //
    BOOLEAN InterruptsEnabled;         // Bridge interrupts enabled
    ULONG InterruptMask;               // Current interrupt mask
    KSPIN_LOCK InterruptLock;          // Spinlock for interrupt handling
    
    //
    // Error handling for bridge
    //
    BOOLEAN ErrorReportingEnabled;     // Error reporting enabled
    ULONG UncorrectableErrorMask;      // Uncorrectable error mask
    ULONG CorrectableErrorMask;        // Correctable error mask
    
} EXPRESS_BRIDGE, *PEXPRESS_BRIDGE;

//
// Helper macros for ASPM flags
//
#define ASPM_L0S_SUPPORTED      0x01
#define ASPM_L1_SUPPORTED       0x02
#define ASPM_L0S_ENABLED        0x04
#define ASPM_L1_ENABLED         0x08
#define ASPM_OVERRIDE           0x10
