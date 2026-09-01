/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/pci/express.c
 * PURPOSE:         PCI Express Capability Support
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/*
 * A PCI Express function carries two capability lists. The first is the
 * ordinary one in the low 256 bytes, where the Express capability itself lives
 * and describes what kind of port the function is. The second starts at 0x100
 * and holds the extended capabilities, which are only reachable through the
 * enhanced configuration mechanism - so everything here degrades to "no
 * extended capabilities" on a machine where that aperture was not found.
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

ULONG
NTAPI
PciReadDeviceExtendedCapability(IN PPCI_PDO_EXTENSION DeviceExtension,
                                IN ULONG CapabilityId,
                                OUT PPCI_EXPRESS_ENHANCED_CAPABILITY_HEADER Buffer,
                                IN ULONG Length)
{
    PCI_EXPRESS_ENHANCED_CAPABILITY_HEADER Header;
    ULONG Offset, CapabilityCount;

    ASSERT(DeviceExtension->ExtensionType == PciPdoExtensionType);
    ASSERT(Length >= sizeof(PCI_EXPRESS_ENHANCED_CAPABILITY_HEADER));

    /* The extended list only exists where the enhanced mechanism can reach */
    if (!PciEcamEnabled) return 0;

    /* The list always starts at the top of the legacy configuration space */
    Offset = PCI_LEGACY_CONFIG_LENGTH;
    CapabilityCount = 0;

    while (Offset)
    {
        /* Entries are dword aligned and live entirely in extended space */
        if ((Offset < PCI_LEGACY_CONFIG_LENGTH) ||
            (Offset > (PCI_EXTENDED_CONFIG_LENGTH - sizeof(Header))) ||
            (Offset & 0x3))
        {
            return 0;
        }

        /* Read this entry's header */
        PciReadDeviceConfig(DeviceExtension, &Header, Offset, sizeof(Header));

        /*
         * A function without extended capabilities answers the whole aperture
         * with ones, or with zeroes, so neither can start a list.
         */
        if ((Header.CapabilityID == 0xFFFF) || !(Header.CapabilityID)) return 0;

        /* Check if this is the capability being looked up */
        if (Header.CapabilityID == CapabilityId)
        {
            /* Hand back as much of it as the caller asked for */
            PciReadDeviceConfig(DeviceExtension, Buffer, Offset, Length);
            return Offset;
        }

        /* Try the next capability instead */
        Offset = Header.Next;

        /* There can only be so many entries in 4KB of configuration space */
        CapabilityCount++;
        if (CapabilityCount > (PCI_EXTENDED_CONFIG_LENGTH / sizeof(Header)))
        {
            DPRINT1("PCI device %p extended capabilities list is broken.\n",
                    DeviceExtension);
            return 0;
        }
    }

    return 0;
}

VOID
NTAPI
PciGetExpressCapabilities(IN PPCI_PDO_EXTENSION PdoExtension)
{
    PCI_EXPRESS_CAPABILITY Express;
    ULONG Offset;
    PAGED_CODE();

    /* Assume this is not an Express function */
    PdoExtension->ExpressCapabilityPtr = 0;
    PdoExtension->ExpressDeviceType = 0;

    /* A function with no capabilities at all cannot be one */
    if (!PdoExtension->CapabilitiesPtr) return;

    /*
     * Only the header and the capabilities register are wanted here, and a
     * capability sitting near the top of the list would not have room for the
     * whole structure below the extended space anyway.
     */
    Offset = PciReadDeviceCapability(PdoExtension,
                                     PdoExtension->CapabilitiesPtr,
                                     PCI_CAPABILITY_ID_PCI_EXPRESS,
                                     &Express.Header,
                                     RTL_SIZEOF_THROUGH_FIELD(PCI_EXPRESS_CAPABILITY,
                                                              ExpressCapabilities));
    if (!Offset) return;

    /* Remember where it is, and what kind of port this function turned out to be */
    PdoExtension->ExpressCapabilityPtr = (USHORT)Offset;
    PdoExtension->ExpressDeviceType = (UCHAR)Express.ExpressCapabilities.DeviceType;

    DPRINT1("PCI - Express capability at 0x%x, port type %u, version %u\n",
            PdoExtension->ExpressCapabilityPtr,
            PdoExtension->ExpressDeviceType,
            Express.ExpressCapabilities.CapabilityVersion);
}

/* EOF */
