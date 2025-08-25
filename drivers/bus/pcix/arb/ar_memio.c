/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/arb/ar_memiono.c
 * PURPOSE:         Memory and I/O Port Resource Arbitration
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

//#define NDEBUG
#include <debug.h>

/* External arbiter library helpers missing from the public header */
extern NTSTATUS NTAPI ArbRetestAllocation(_In_ PARBITER_INSTANCE Arbiter,
                                          _In_ PLIST_ENTRY ArbitrationList);
extern NTSTATUS NTAPI ArbCommitAllocation(_In_ PARBITER_INSTANCE Arbiter);
extern NTSTATUS NTAPI ArbRollbackAllocation(_In_ PARBITER_INSTANCE Arbiter);
extern NTSTATUS NTAPI ArbPreprocessEntry(_In_ PARBITER_INSTANCE Arbiter,
                                         _Inout_ PARBITER_ALLOCATION_STATE ArbState);
extern NTSTATUS NTAPI ArbAllocateEntry(_In_ PARBITER_INSTANCE Arbiter,
                                       _Inout_ PARBITER_ALLOCATION_STATE ArbState);

/* GLOBALS ********************************************************************/

PCI_INTERFACE ArbiterInterfaceMemory =
{
    &GUID_ARBITER_INTERFACE_STANDARD,
    sizeof(ARBITER_INTERFACE),
    0,
    0,
    PCI_INTERFACE_FDO,
    0,
    PciArb_Memory,
    armem_Constructor,
    armem_Initializer
};

PCI_INTERFACE ArbiterInterfaceIo =
{
    &GUID_ARBITER_INTERFACE_STANDARD,
    sizeof(ARBITER_INTERFACE),
    0,
    0,
    PCI_INTERFACE_FDO,
    0,
    PciArb_Io,
    ario_Constructor,
    ario_Initializer
};

NTSTATUS
NTAPI
arint_Initializer(IN PVOID Instance);
NTSTATUS
NTAPI
arint_Constructor(IN PVOID DeviceExtension,
                  IN PVOID PciInterface,
                  IN PVOID InterfaceData,
                  IN USHORT Version,
                  IN USHORT Size,
                  IN PINTERFACE Interface);
PCI_INTERFACE ArbiterInterfaceInterrupt =
{
    &GUID_ARBITER_INTERFACE_STANDARD,
    sizeof(ARBITER_INTERFACE),
    0,
    0,
    PCI_INTERFACE_FDO,
    0,
    PciArb_Interrupt,
    arint_Constructor,
    arint_Initializer
};

static NTSTATUS NTAPI arint_UnpackRequirement(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDesc,
    _Out_ PULONGLONG Min,
    _Out_ PULONGLONG Max,
    _Out_ PULONG Length,
    _Out_ PULONG Alignment)
{
    UNREFERENCED_PARAMETER(IoDesc);
    /* Legacy INTx are unit-size tokens; let core arbiter manage sharing */
    *Min = 0;
    *Max = 0xFF;
    *Length = 1;
    *Alignment = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI arint_PackResource(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDesc,
    _In_ ULONGLONG Start,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc)
{
    RtlZeroMemory(CmDesc, sizeof(*CmDesc));
    CmDesc->Type = CmResourceTypeInterrupt;
    CmDesc->ShareDisposition = CmResourceShareShared;
    CmDesc->Flags = CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
    CmDesc->u.Interrupt.Level = (ULONG)Start;
    CmDesc->u.Interrupt.Vector = (ULONG)Start;
    CmDesc->u.Interrupt.Affinity = (KAFFINITY)-1;
    UNREFERENCED_PARAMETER(IoDesc);
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI arint_UnpackResource(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc,
    _Out_ PULONGLONG Start,
    _Out_ PULONG Length)
{
    *Start = CmDesc->u.Interrupt.Vector;
    *Length = 1;
    return STATUS_SUCCESS;
}

static LONG NTAPI arint_ScoreRequirement(_In_ PIO_RESOURCE_DESCRIPTOR IoDesc)
{
    UNREFERENCED_PARAMETER(IoDesc);
    return 0;
}

NTSTATUS
NTAPI
arint_Initializer(IN PVOID Instance)
{
    PPCI_ARBITER_INSTANCE Arbiter = Instance;
    PPCI_FDO_EXTENSION FdoExtension = Arbiter->BusFdoExtension;

    PAGED_CODE();

    RtlZeroMemory(&Arbiter->CommonInstance, sizeof(Arbiter->CommonInstance));

    Arbiter->CommonInstance.UnpackRequirement = arint_UnpackRequirement;
    Arbiter->CommonInstance.PackResource = arint_PackResource;
    Arbiter->CommonInstance.UnpackResource = arint_UnpackResource;
    Arbiter->CommonInstance.ScoreRequirement = arint_ScoreRequirement;
    Arbiter->CommonInstance.TestAllocation = ArbTestAllocation;
    Arbiter->CommonInstance.RetestAllocation = ArbRetestAllocation;
    Arbiter->CommonInstance.CommitAllocation = ArbCommitAllocation;
    Arbiter->CommonInstance.RollbackAllocation = ArbRollbackAllocation;
    Arbiter->CommonInstance.BootAllocation = ArbBootAllocation;
    Arbiter->CommonInstance.StartArbiter = NULL;
    Arbiter->CommonInstance.PreprocessEntry = ArbPreprocessEntry;
    Arbiter->CommonInstance.AllocateEntry = ArbAllocateEntry;
    Arbiter->CommonInstance.GetNextAllocationRange = ArbGetNextAllocationRange;
    Arbiter->CommonInstance.FindSuitableRange = ArbFindSuitableRange;
    Arbiter->CommonInstance.AddAllocation = ArbAddAllocation;
    Arbiter->CommonInstance.BacktrackAllocation = ArbBacktrackAllocation;
    Arbiter->CommonInstance.OverrideConflict = NULL;

    return ArbInitializeArbiterInstance(&Arbiter->CommonInstance,
                                        FdoExtension->FunctionalDeviceObject,
                                        CmResourceTypeInterrupt,
                                        Arbiter->InstanceName,
                                        L"Pci",
                                        NULL);
}

NTSTATUS
NTAPI
arint_Constructor(IN PVOID DeviceExtension,
                  IN PVOID PciInterface,
                  IN PVOID InterfaceData,
                  IN USHORT Version,
                  IN USHORT Size,
                  IN PINTERFACE Interface)
{
    PPCI_FDO_EXTENSION FdoExtension = (PPCI_FDO_EXTENSION)DeviceExtension;
    PPCI_ARBITER_INSTANCE PciArbiter = (PPCI_ARBITER_INSTANCE)PciInterface;
    NTSTATUS Status;
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Version);
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Interface);

    //
    // Validate parameters
    //
    if ((ULONG_PTR)InterfaceData != CmResourceTypeInterrupt)
    {
        DPRINT1("PCI: arint_Constructor called with non-interrupt resource type: %p\n", InterfaceData);
        return STATUS_INVALID_PARAMETER;
    }

    DPRINT("PCI: Initializing interrupt arbiter for FDO %p\n", FdoExtension);

    //
    // Initialize the arbiter instance
    //
    Status = ArbInitializeArbiterInstance(&PciArbiter->CommonInstance,
                                          FdoExtension->FunctionalDeviceObject,
                                          CmResourceTypeInterrupt,
                                          PciArbiter->InstanceName,
                                          L"Pci",
                                          NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PCI: Failed to initialize interrupt arbiter instance: 0x%lx\n", Status);
        return Status;
    }

    //
    // Initialize ACPI/APIC interrupt ranges if this is the root bus
    //
    if (PCI_IS_ROOT_FDO(FdoExtension))
    {
        Status = PciInitializeAcpiApicInterruptRanges(&PciArbiter->CommonInstance);
        if (!NT_SUCCESS(Status))
        {
            DPRINT("PCI: Failed to initialize ACPI/APIC interrupt ranges: 0x%lx\n", Status);
            // Continue - let HAL handle interrupt allocation
        }
        else
        {
            DPRINT("PCI: Successfully initialized ACPI/APIC interrupt ranges\n");
        }
    }

    DPRINT("PCI: Successfully initialized interrupt arbiter for FDO %p\n", FdoExtension);
    return STATUS_SUCCESS;
}

/* FUNCTIONS ******************************************************************/

static NTSTATUS NTAPI ario_UnpackRequirement(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDesc,
    _Out_ PULONGLONG Min,
    _Out_ PULONGLONG Max,
    _Out_ PULONG Length,
    _Out_ PULONG Alignment)
{
    *Length = IoDesc->u.Port.Length;
    *Alignment = IoDesc->u.Port.Alignment ? IoDesc->u.Port.Alignment : 1;
    *Min = IoDesc->u.Port.MinimumAddress.QuadPart;
    *Max = IoDesc->u.Port.MaximumAddress.QuadPart;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI ario_PackResource(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDesc,
    _In_ ULONGLONG Start,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc)
{
    RtlZeroMemory(CmDesc, sizeof(*CmDesc));
    CmDesc->Type = CmResourceTypePort;
    CmDesc->ShareDisposition = IoDesc->ShareDisposition;
    CmDesc->Flags = CM_RESOURCE_PORT_IO | CM_RESOURCE_PORT_BAR;
    CmDesc->u.Port.Start.QuadPart = Start;
    CmDesc->u.Port.Length = IoDesc->u.Port.Length;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI ario_UnpackResource(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc,
    _Out_ PULONGLONG Start,
    _Out_ PULONG Length)
{
    *Start = CmDesc->u.Port.Start.QuadPart;
    *Length = CmDesc->u.Port.Length;
    return STATUS_SUCCESS;
}

static LONG NTAPI ario_ScoreRequirement(_In_ PIO_RESOURCE_DESCRIPTOR IoDesc)
{
    UNREFERENCED_PARAMETER(IoDesc);
    return 0;
}

NTSTATUS
NTAPI
ario_Initializer(IN PVOID Instance)
{
    PPCI_ARBITER_INSTANCE Arbiter = Instance;
    PPCI_FDO_EXTENSION FdoExtension = Arbiter->BusFdoExtension;

    PAGED_CODE();

    RtlZeroMemory(&Arbiter->CommonInstance, sizeof(Arbiter->CommonInstance));

    Arbiter->CommonInstance.UnpackRequirement = ario_UnpackRequirement;
    Arbiter->CommonInstance.PackResource = ario_PackResource;
    Arbiter->CommonInstance.UnpackResource = ario_UnpackResource;
    Arbiter->CommonInstance.ScoreRequirement = ario_ScoreRequirement;
    Arbiter->CommonInstance.TestAllocation = ArbTestAllocation;
    Arbiter->CommonInstance.RetestAllocation = ArbRetestAllocation;
    Arbiter->CommonInstance.CommitAllocation = ArbCommitAllocation;
    Arbiter->CommonInstance.RollbackAllocation = ArbRollbackAllocation;
    Arbiter->CommonInstance.BootAllocation = ArbBootAllocation;
    Arbiter->CommonInstance.StartArbiter = NULL; /* optional for now */
    Arbiter->CommonInstance.PreprocessEntry = ArbPreprocessEntry;
    Arbiter->CommonInstance.AllocateEntry = ArbAllocateEntry;
    Arbiter->CommonInstance.GetNextAllocationRange = ArbGetNextAllocationRange;
    Arbiter->CommonInstance.FindSuitableRange = ArbFindSuitableRange;
    Arbiter->CommonInstance.AddAllocation = ArbAddAllocation;
    Arbiter->CommonInstance.BacktrackAllocation = ArbBacktrackAllocation;
    Arbiter->CommonInstance.OverrideConflict = NULL;

    return ArbInitializeArbiterInstance(&Arbiter->CommonInstance,
                                        FdoExtension->FunctionalDeviceObject,
                                        CmResourceTypePort,
                                        Arbiter->InstanceName,
                                        L"Pci",
                                        NULL);
}

NTSTATUS
NTAPI
ario_Constructor(IN PVOID DeviceExtension,
                 IN PVOID PciInterface,
                 IN PVOID InterfaceData,
                 IN USHORT Version,
                 IN USHORT Size,
                 IN PINTERFACE Interface)
{
    PPCI_FDO_EXTENSION FdoExtension = (PPCI_FDO_EXTENSION)DeviceExtension;
    NTSTATUS Status;
    PAGED_CODE();

    UNREFERENCED_PARAMETER(PciInterface);
    UNREFERENCED_PARAMETER(Version);
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Interface);

    /* Make sure it's the expected interface */
    if ((ULONG_PTR)InterfaceData != CmResourceTypePort)
    {
        /* Arbiter support must have been initialized first */
        if (FdoExtension->ArbitersInitialized)
        {
            /* Not yet implemented */
            UNIMPLEMENTED;
            while (TRUE);
        }
        else
        {
            /* No arbiters for this FDO */
            Status = STATUS_NOT_SUPPORTED;
        }
    }
    else
    {
        /* Not the right interface */
        Status = STATUS_INVALID_PARAMETER_5;
    }

    /* Return the status */
    return Status;
}

VOID
NTAPI
ario_ApplyBrokenVideoHack(IN PPCI_FDO_EXTENSION FdoExtension)
{
    PPCI_ARBITER_INSTANCE PciArbiter;
    //PARBITER_INSTANCE CommonInstance;
    //NTSTATUS Status;

    /* Only valid for root FDOs who are being applied the hack for the first time */
    ASSERT(!FdoExtension->BrokenVideoHackApplied);
    ASSERT(PCI_IS_ROOT_FDO(FdoExtension));

    /* Find the I/O arbiter */
    PciArbiter = (PVOID)PciFindNextSecondaryExtension(FdoExtension->
                                                      SecondaryExtension.Next,
                                                      PciArb_Io);
    ASSERT(PciArbiter);
#if 0 // when arb exist
    /* Get the Arb instance */
    CommonInstance = &PciArbiter->CommonInstance;

    /* Free the two lists, enabling full VGA access */
    ArbFreeOrderingList(&CommonInstance->OrderingList);
    ArbFreeOrderingList(&CommonInstance->ReservedList);

    /* Build the ordering for broken video PCI access */
    Status = ArbBuildAssignmentOrdering(CommonInstance,
                                        L"Pci",
                                        L"BrokenVideo",
                                        NULL);
    ASSERT(NT_SUCCESS(Status));
#else
    //Status = STATUS_SUCCESS;
    UNIMPLEMENTED;
    while (TRUE);
#endif
    /* Now the hack has been applied */
    FdoExtension->BrokenVideoHackApplied = TRUE;
}

static NTSTATUS NTAPI armem_UnpackRequirement(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDesc,
    _Out_ PULONGLONG Min,
    _Out_ PULONGLONG Max,
    _Out_ PULONG Length,
    _Out_ PULONG Alignment)
{
    *Length = IoDesc->u.Memory.Length;
    *Alignment = IoDesc->u.Memory.Alignment ? IoDesc->u.Memory.Alignment : 1;
    *Min = IoDesc->u.Memory.MinimumAddress.QuadPart;
    *Max = IoDesc->u.Memory.MaximumAddress.QuadPart;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI armem_PackResource(
    _In_ PIO_RESOURCE_DESCRIPTOR IoDesc,
    _In_ ULONGLONG Start,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc)
{
    RtlZeroMemory(CmDesc, sizeof(*CmDesc));
    CmDesc->Type = CmResourceTypeMemory;
    CmDesc->ShareDisposition = IoDesc->ShareDisposition;
    CmDesc->Flags = CM_RESOURCE_MEMORY_READ_WRITE | CM_RESOURCE_MEMORY_BAR;
    CmDesc->u.Memory.Start.QuadPart = Start;
    CmDesc->u.Memory.Length = IoDesc->u.Memory.Length;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI armem_UnpackResource(
    _In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDesc,
    _Out_ PULONGLONG Start,
    _Out_ PULONG Length)
{
    *Start = CmDesc->u.Memory.Start.QuadPart;
    *Length = CmDesc->u.Memory.Length;
    return STATUS_SUCCESS;
}

static LONG NTAPI armem_ScoreRequirement(_In_ PIO_RESOURCE_DESCRIPTOR IoDesc)
{
    UNREFERENCED_PARAMETER(IoDesc);
    return 0;
}

NTSTATUS
NTAPI
armem_Initializer(IN PVOID Instance)
{
    PPCI_ARBITER_INSTANCE Arbiter = Instance;
    PPCI_FDO_EXTENSION FdoExtension = Arbiter->BusFdoExtension;

    PAGED_CODE();

    RtlZeroMemory(&Arbiter->CommonInstance, sizeof(Arbiter->CommonInstance));

    Arbiter->CommonInstance.UnpackRequirement = armem_UnpackRequirement;
    Arbiter->CommonInstance.PackResource = armem_PackResource;
    Arbiter->CommonInstance.UnpackResource = armem_UnpackResource;
    Arbiter->CommonInstance.ScoreRequirement = armem_ScoreRequirement;
    Arbiter->CommonInstance.TestAllocation = ArbTestAllocation;
    Arbiter->CommonInstance.RetestAllocation = ArbRetestAllocation;
    Arbiter->CommonInstance.CommitAllocation = ArbCommitAllocation;
    Arbiter->CommonInstance.RollbackAllocation = ArbRollbackAllocation;
    Arbiter->CommonInstance.BootAllocation = ArbBootAllocation;
    Arbiter->CommonInstance.StartArbiter = NULL; /* optional for now */
    Arbiter->CommonInstance.PreprocessEntry = ArbPreprocessEntry;
    Arbiter->CommonInstance.AllocateEntry = ArbAllocateEntry;
    Arbiter->CommonInstance.GetNextAllocationRange = ArbGetNextAllocationRange;
    Arbiter->CommonInstance.FindSuitableRange = ArbFindSuitableRange;
    Arbiter->CommonInstance.AddAllocation = ArbAddAllocation;
    Arbiter->CommonInstance.BacktrackAllocation = ArbBacktrackAllocation;
    Arbiter->CommonInstance.OverrideConflict = NULL;

    return ArbInitializeArbiterInstance(&Arbiter->CommonInstance,
                                        FdoExtension->FunctionalDeviceObject,
                                        CmResourceTypeMemory,
                                        Arbiter->InstanceName,
                                        L"Pci",
                                        NULL);
}

NTSTATUS
NTAPI
armem_Constructor(IN PVOID DeviceExtension,
                  IN PVOID PciInterface,
                  IN PVOID InterfaceData,
                  IN USHORT Version,
                  IN USHORT Size,
                  IN PINTERFACE Interface)
{
    PPCI_FDO_EXTENSION FdoExtension = (PPCI_FDO_EXTENSION)DeviceExtension;
    NTSTATUS Status;
    PAGED_CODE();

    UNREFERENCED_PARAMETER(PciInterface);
    UNREFERENCED_PARAMETER(Version);
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Interface);

    /* Make sure it's the expected interface */
    if ((ULONG_PTR)InterfaceData != CmResourceTypeMemory)
    {
        /* Arbiter support must have been initialized first */
        if (FdoExtension->ArbitersInitialized)
        {
            /* Not yet implemented */
            UNIMPLEMENTED;
            while (TRUE);
        }
        else
        {
            /* No arbiters for this FDO */
            Status = STATUS_NOT_SUPPORTED;
        }
    }
    else
    {
        /* Not the right interface */
        Status = STATUS_INVALID_PARAMETER_5;
    }

    /* Return the status */
    return Status;
}

/* EOF */
