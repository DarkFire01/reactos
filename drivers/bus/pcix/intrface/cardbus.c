/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/intrface/cardbus.c
 * PURPOSE:         CardBus Interface
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PCI_INTERFACE PciCardbusPrivateInterface =
{
    &GUID_PCI_CARDBUS_INTERFACE_PRIVATE,
    sizeof(PCI_CARDBUS_INTERFACE_PRIVATE),
    PCI_CB_INTRF_VERSION,
    PCI_CB_INTRF_VERSION,
    PCI_INTERFACE_PDO,
    0,
    PciInterface_PciCb,
    pcicbintrf_Constructor,
    pcicbintrf_Initializer
};

/* FUNCTIONS ******************************************************************/

/*
 * CardBus (PCI header type 2) resource configurator.
 *
 * The only PCI-arbitrated resource of a CardBus bridge is its socket (ExCA)
 * registers BAR at offset 0x10. The four card-side memory/I/O decode windows
 * (the type2 Range[] registers) are read/write registers programmed by the
 * CardBus controller driver, not sizeable BARs, so they are not treated as
 * device BARs here.
 */

VOID
NTAPI
Cardbus_MassageHeaderForLimitsDetermination(IN PPCI_CONFIGURATOR_CONTEXT Context)
{
    PPCI_COMMON_HEADER PciData, Current;

    PciData = Context->PciData;
    Current = Context->Current;

    /* Write all 1s to the socket-registers BAR; the read-back shows which bits
       are hardwired to 0, which gives the BAR length. */
    PciData->u.type2.SocketRegistersBaseAddress = 0xFFFFFFFF;

    /* Preserve the bus-number and latency fields across the discovery write */
    PciData->u.type2.PrimaryBus = Current->u.type2.PrimaryBus;
    PciData->u.type2.SecondaryBus = Current->u.type2.SecondaryBus;
    PciData->u.type2.SubordinateBus = Current->u.type2.SubordinateBus;
    PciData->u.type2.SecondaryLatency = Current->u.type2.SecondaryLatency;

    /* The card decode windows (Range[]) are not sizeable BARs; leave them at
       their current values so they are written back unchanged. */

    /* Save and clear the write-1-to-clear secondary status register */
    Context->SecondaryStatus = Current->u.type2.SecondaryStatus;
    Current->u.type2.SecondaryStatus = 0;
    PciData->u.type2.SecondaryStatus = 0;
}

VOID
NTAPI
Cardbus_SaveLimits(IN PPCI_CONFIGURATOR_CONTEXT Context)
{
    PPCI_COMMON_HEADER PciData;
    PPCI_FUNCTION_RESOURCES Resources;

    /* PciData holds the sized BAR mask read back after the all-1s write */
    PciData = Context->PciData;
    Resources = Context->PdoExtension->Resources;

    /* Build the limit descriptor for the socket-registers memory BAR. The
       remaining limit descriptors are left NULL (the caller zeroed them). */
    PciCreateIoDescriptorFromBarLimit(&Resources->Limit[0],
                                      &PciData->u.type2.SocketRegistersBaseAddress,
                                      FALSE);
}

VOID
NTAPI
Cardbus_SaveCurrentSettings(IN PPCI_CONFIGURATOR_CONTEXT Context)
{
    PPCI_COMMON_HEADER Current;
    PPCI_FUNCTION_RESOURCES Resources;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR CmDescriptor;
    PIO_RESOURCE_DESCRIPTOR IoDescriptor;

    Current = Context->Current;
    Resources = Context->PdoExtension->Resources;

    /* Mirror the socket BAR's limit descriptor into a current-setting one,
       using the live (programmed) base address. */
    CmDescriptor = &Resources->Current[0];
    IoDescriptor = &Resources->Limit[0];

    CmDescriptor->Type = IoDescriptor->Type;
    if (CmDescriptor->Type != CmResourceTypeNull)
    {
        CmDescriptor->Flags = IoDescriptor->Flags;
        CmDescriptor->ShareDisposition = IoDescriptor->ShareDisposition;
        CmDescriptor->u.Generic.Start.HighPart = 0;
        CmDescriptor->u.Generic.Length = IoDescriptor->u.Generic.Length;
        CmDescriptor->u.Memory.Start.LowPart =
            Current->u.type2.SocketRegistersBaseAddress & PCI_ADDRESS_MEMORY_ADDRESS_MASK;
    }
}

VOID
NTAPI
Cardbus_RestoreCurrent(IN PPCI_CONFIGURATOR_CONTEXT Context)
{
    /* Restore the secondary status register saved during massaging */
    Context->Current->u.type2.SecondaryStatus = Context->SecondaryStatus;
}

VOID
NTAPI
Cardbus_GetAdditionalResourceDescriptors(IN PPCI_CONFIGURATOR_CONTEXT Context,
                                         IN PPCI_COMMON_HEADER PciData,
                                         IN PIO_RESOURCE_DESCRIPTOR IoDescriptor)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(PciData);
    UNREFERENCED_PARAMETER(IoDescriptor);

    /* A CardBus bridge exposes no additional fixed legacy resources here; the
       card-side windows are owned by the CardBus controller driver. */
}

VOID
NTAPI
Cardbus_ResetDevice(IN PPCI_PDO_EXTENSION PdoExtension,
                    IN PPCI_COMMON_HEADER PciData)
{
    UNREFERENCED_PARAMETER(PdoExtension);
    UNREFERENCED_PARAMETER(PciData);

    /* No special reset handling required for a CardBus bridge. */
}

VOID
NTAPI
Cardbus_ChangeResourceSettings(IN PPCI_PDO_EXTENSION PdoExtension,
                               IN PPCI_COMMON_HEADER PciData)
{
    UNREFERENCED_PARAMETER(PdoExtension);
    UNREFERENCED_PARAMETER(PciData);

    /*
     * PciData already holds the bridge's live configuration (read by
     * PciSetResources). The socket BAR and the card decode windows keep their
     * firmware-programmed values, so there is nothing to reprogram here.
     */
}

NTSTATUS
NTAPI
pcicbintrf_Initializer(IN PVOID Instance)
{
    UNREFERENCED_PARAMETER(Instance);
    /* PnP Interfaces don't get Initialized */
    ASSERTMSG("PCI pcicbintrf_Initializer, unexpected call.\n", FALSE);
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS
NTAPI
pcicbintrf_Constructor(IN PVOID DeviceExtension,
                       IN PVOID Instance,
                       IN PVOID InterfaceData,
                       IN USHORT Version,
                       IN USHORT Size,
                       IN PINTERFACE Interface)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(InterfaceData);
    UNREFERENCED_PARAMETER(Version);
    UNREFERENCED_PARAMETER(Size);
    UNREFERENCED_PARAMETER(Interface);
    DPRINT1("PCI pcicbintrf_Constructor, unexpected call.\n");
    return STATUS_NOT_IMPLEMENTED;
}

/* EOF */
