/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/pci/rbar.c
 * PURPOSE:         PCI Express Resizable BAR Support
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/*
 * An ordinary BAR decodes a fixed amount of space, fixed by the hardware and
 * discoverable only by writing ones to it. A resizable BAR instead advertises
 * every size it could decode and lets software choose one before the window is
 * assigned - which is how a device with a large local memory can expose all of
 * it to the processor, rather than the small fixed aperture it would otherwise
 * have to default to for the benefit of systems that cannot spare the space.
 *
 * The choice is made by the arbiter rather than here: each resizable BAR is
 * offered as a large requirement with its ordinary size as the alternative, so
 * a machine with room gets the large window and one without still gets a
 * working device. Whatever comes back is then written into the BAR size field,
 * which must happen while the memory decode is off, because the size field
 * changes what the BAR decodes.
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

VOID
NTAPI
PciResizableBarInitialize(IN PPCI_PDO_EXTENSION PdoExtension)
{
    PPCI_RESIZABLE_BAR_INFO ResizableBar = &PdoExtension->ResizableBar;
    PCI_EXPRESS_ENHANCED_CAPABILITY_HEADER Header;
    ULONG Offset, Entry, Count, Control, Capability, BarIndex;
    PAGED_CODE();

    /* Assume this function has no resizable BARs */
    RtlZeroMemory(ResizableBar, sizeof(*ResizableBar));

    /* The capability is an extended one, so only Express functions have it */
    Offset = PciReadDeviceExtendedCapability(PdoExtension,
                                             PCI_EXPRESS_RESIZABLE_BAR_CAP_ID,
                                             &Header,
                                             sizeof(Header));
    if (!Offset) return;

    /*
     * The number of entries is only meaningful in the first control register,
     * so that one has to be read before the rest can be walked.
     */
    PciReadDeviceConfig(PdoExtension,
                        &Control,
                        Offset + PCI_RBAR_ENTRY_CONTROL(0),
                        sizeof(Control));

    Count = (Control & PCI_RBAR_CONTROL_COUNT_MASK) >> PCI_RBAR_CONTROL_COUNT_SHIFT;
    if ((!Count) || (Count > PCI_RESIZABLE_BAR_COUNT_MAX))
    {
        DPRINT1("PCI - device %p claims %u resizable BARs, ignoring them\n",
                PdoExtension, Count);
        return;
    }

    ResizableBar->CapabilityPtr = (USHORT)Offset;

    /* Record which BAR each entry resizes, and what sizes it offers */
    for (Entry = 0; Entry < Count; Entry++)
    {
        PciReadDeviceConfig(PdoExtension,
                            &Capability,
                            Offset + PCI_RBAR_ENTRY_CAPABILITY(Entry),
                            sizeof(Capability));
        PciReadDeviceConfig(PdoExtension,
                            &Control,
                            Offset + PCI_RBAR_ENTRY_CONTROL(Entry),
                            sizeof(Control));

        BarIndex = Control & PCI_RBAR_CONTROL_BAR_INDEX_MASK;
        Capability >>= PCI_RBAR_CAPABILITY_SIZES_SHIFT;

        /* Ignore an entry that names no sizes, or a BAR that cannot exist */
        if (!Capability) continue;
        if (BarIndex >= PCI_RESIZABLE_BAR_COUNT_MAX) continue;

        /* Two entries naming one BAR is nonsense, so keep only the first */
        if (ResizableBar->SizesSupported[BarIndex]) continue;

        ResizableBar->SizesSupported[BarIndex] = Capability;
        ResizableBar->ControlRegister[BarIndex] = (UCHAR)Entry;

        DPRINT1("PCI - BAR %u of %p is resizable, sizes 0x%08lx\n",
                BarIndex, PdoExtension, Capability);
    }
}

ULONGLONG
NTAPI
PciResizableBarLargestSize(IN PPCI_PDO_EXTENSION PdoExtension,
                           IN ULONG BarIndex,
                           IN ULONGLONG MaximumLength)
{
    PPCI_RESIZABLE_BAR_INFO ResizableBar = &PdoExtension->ResizableBar;
    ULONGLONG Length;
    ULONG Sizes, Bit;

    /* Only the first six BARs can be resized at all */
    if (BarIndex >= PCI_RESIZABLE_BAR_COUNT_MAX) return 0;

    Sizes = ResizableBar->SizesSupported[BarIndex];
    if (!Sizes) return 0;

    /*
     * Bit N says the BAR can decode two-to-the-N megabytes. Walk down from the
     * largest until one is found that still fits the window it has to live in.
     */
    for (Bit = 31; (LONG)Bit >= 0; Bit--)
    {
        if (!(Sizes & (1UL << Bit))) continue;

        /* A size that cannot be expressed on this architecture is no use */
        if ((Bit + 20) >= (sizeof(ULONGLONG) * 8)) continue;

        Length = 1ULL << (Bit + 20);
        if (Length <= MaximumLength) return Length;
    }

    return 0;
}


/*
 * The largest size a resizable BAR could be given in place of the window it
 * reads back as, or zero when it is not resizable or has nothing bigger to
 * offer. A memory descriptor carries its length in a ULONG, so a window that
 * could not be described that way is not offered at all.
 */
ULONGLONG
NTAPI
PciResizableBarRequirement(IN PPCI_PDO_EXTENSION PdoExtension,
                           IN ULONG BarIndex,
                           IN PIO_RESOURCE_DESCRIPTOR Limit)
{
    ULONGLONG Ceiling, Length;

    if (Limit->Type != CmResourceTypeMemory) return 0;

    /* The window still has to sit under the highest address the BAR decodes */
    Ceiling = (ULONGLONG)Limit->u.Memory.MaximumAddress.QuadPart + 1;
    if ((!Ceiling) || (Ceiling > PCI_MAX_MEMORY_DESCRIPTOR_LENGTH))
    {
        Ceiling = PCI_MAX_MEMORY_DESCRIPTOR_LENGTH;
    }

    Length = PciResizableBarLargestSize(PdoExtension, BarIndex, Ceiling);
    if (Length <= Limit->u.Memory.Length) return 0;

    return Length;
}

NTSTATUS
NTAPI
PciResizableBarSetSize(IN PPCI_PDO_EXTENSION PdoExtension,
                       IN ULONG BarIndex,
                       IN ULONGLONG Length)
{
    PPCI_RESIZABLE_BAR_INFO ResizableBar = &PdoExtension->ResizableBar;
    ULONGLONG Megabytes;
    ULONG Control, Offset, Size, Sizes;

    /* Only a BAR that was found to be resizable can be resized */
    if (BarIndex >= PCI_RESIZABLE_BAR_COUNT_MAX) return STATUS_NOT_SUPPORTED;

    Sizes = ResizableBar->SizesSupported[BarIndex];
    if (!Sizes) return STATUS_NOT_SUPPORTED;

    /* The size field counts megabytes, so anything smaller cannot be named */
    if (Length < (1024 * 1024)) return STATUS_INVALID_PARAMETER;
    Megabytes = Length >> 20;

    /* And it names them by their power of two, so nothing else can be named */
    if (Megabytes & (Megabytes - 1)) return STATUS_INVALID_PARAMETER;

    for (Size = 0; Size < 32; Size++)
    {
        if (Megabytes == (1ULL << Size)) break;
    }

    if ((Size >= 32) || !(Sizes & (1UL << Size))) return STATUS_INVALID_PARAMETER;

    /* Write the chosen size back, leaving the rest of the register alone */
    Offset = ResizableBar->CapabilityPtr +
             PCI_RBAR_ENTRY_CONTROL(ResizableBar->ControlRegister[BarIndex]);

    PciReadDeviceConfig(PdoExtension, &Control, Offset, sizeof(Control));
    if (((Control & PCI_RBAR_CONTROL_SIZE_MASK) >> PCI_RBAR_CONTROL_SIZE_SHIFT) == Size)
    {
        /* It already decodes what was asked for */
        return STATUS_SUCCESS;
    }

    Control &= ~PCI_RBAR_CONTROL_SIZE_MASK;
    Control |= (Size << PCI_RBAR_CONTROL_SIZE_SHIFT) & PCI_RBAR_CONTROL_SIZE_MASK;
    PciWriteDeviceConfig(PdoExtension, &Control, Offset, sizeof(Control));

    DPRINT1("PCI - BAR %u of %p resized to %u MB\n",
            BarIndex, PdoExtension, (ULONG)Megabytes);
    return STATUS_SUCCESS;
}

VOID
NTAPI
PciResizableBarApplySettings(IN PPCI_PDO_EXTENSION PdoExtension)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Current;
    ULONG BarIndex;
    PAGED_CODE();

    /* Nothing to do for a function with no resizable BARs, or no resources */
    if (!PdoExtension->ResizableBar.CapabilityPtr) return;
    if (!PdoExtension->Resources) return;

    for (BarIndex = 0; BarIndex < PCI_RESIZABLE_BAR_COUNT_MAX; BarIndex++)
    {
        if (!PdoExtension->ResizableBar.SizesSupported[BarIndex]) continue;

        /* Match the BAR to whatever length the arbiter settled on for it */
        Current = &PdoExtension->Resources->Current[BarIndex];
        if (Current->Type != CmResourceTypeMemory) continue;

        PciResizableBarSetSize(PdoExtension,
                               BarIndex,
                               Current->u.Memory.Length);
    }
}

/* EOF */
