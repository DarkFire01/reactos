/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/pci/osc.c
 * PURPOSE:         PCI Express Platform Capability Negotiation (_OSC)
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/*
 * On a PCI Express machine the firmware keeps driving several features of the
 * hierarchy - native hot plug, advanced error reporting, power management
 * events - until an operating system asks for them. That handover is _OSC, a
 * method on the host bridge which is handed what the operating system can do
 * and what it would like to take over, and answers with what it is actually
 * being given. The firmware is free to grant less than was asked for, and what
 * it grants is binding: a feature the firmware kept must not be touched.
 *
 * What is asked for here is deliberately narrow. This driver reads extended
 * configuration space and programs message interrupts, so it says so; it does
 * not implement native hot plug, error reporting or power management events,
 * so it does not ask to take those away from the firmware that does.
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* The UUID that names the PCI Express host bridge capabilities of _OSC */
static const UCHAR PciOscUuid[16] =
{
    0x5B, 0x4D, 0xDB, 0x33, 0xF7, 0x1F, 0x1C, 0x40,
    0x96, 0x57, 0x74, 0x41, 0xC0, 0x3D, 0xD7, 0x66
};

/* FUNCTIONS ******************************************************************/

VOID
NTAPI
PciEvaluateOsc(IN PPCI_FDO_EXTENSION FdoExtension)
{
    PACPI_EVAL_INPUT_BUFFER_COMPLEX InputBuffer;
    PACPI_EVAL_OUTPUT_BUFFER OutputBuffer;
    PACPI_METHOD_ARGUMENT Argument;
    ULONG Capabilities[3];
    ULONG InputLength, OutputLength, Support, Control, Granted;
    NTSTATUS Status;
    PAGED_CODE();
    ASSERT(PCI_IS_ROOT_FDO(FdoExtension));

    /*
     * Four arguments: the UUID naming which _OSC this is, the revision of that
     * UUID's capability layout, how many dwords of capabilities follow, and
     * the capabilities themselves.
     */
    InputLength = FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX, Argument) +
                  ACPI_METHOD_ARGUMENT_LENGTH(sizeof(PciOscUuid)) +
                  ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG)) +
                  ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG)) +
                  ACPI_METHOD_ARGUMENT_LENGTH(sizeof(Capabilities));

    InputBuffer = ExAllocatePoolWithTag(PagedPool, InputLength, PCI_POOL_TAG);
    if (!InputBuffer) return;

    /* The answer is the same capability buffer, with the words rewritten */
    OutputLength = sizeof(ACPI_EVAL_OUTPUT_BUFFER) +
                   ACPI_METHOD_ARGUMENT_LENGTH(sizeof(Capabilities));

    OutputBuffer = ExAllocatePoolWithTag(PagedPool, OutputLength, PCI_POOL_TAG);
    if (!OutputBuffer)
    {
        ExFreePoolWithTag(InputBuffer, 0);
        return;
    }

    /*
     * The support word says what this driver understands. Extended
     * configuration space is claimed only where an aperture was actually
     * found, since claiming it otherwise would be a promise it cannot keep.
     */
    Support = PCI_OSC_SUPPORT_MSI;
    if (PciEcamEnabled) Support |= PCI_OSC_SUPPORT_EXTENDED_CONFIG;

    /*
     * The control word says what it wants to take over. Native hot plug, error
     * reporting and power management events are all left with the firmware,
     * because nothing here would service them.
     */
    Control = 0;

    Capabilities[0] = 0;
    Capabilities[1] = Support;
    Capabilities[2] = Control;

    RtlZeroMemory(InputBuffer, InputLength);
    InputBuffer->Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    *(PULONG)InputBuffer->MethodName = 'CSO_';
    InputBuffer->Size = InputLength;
    InputBuffer->ArgumentCount = 4;

    Argument = InputBuffer->Argument;
    ACPI_METHOD_SET_ARGUMENT_BUFFER(Argument, PciOscUuid, sizeof(PciOscUuid));
    Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(Argument, PCI_OSC_REVISION);
    Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(Argument, RTL_NUMBER_OF(Capabilities));
    Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    ACPI_METHOD_SET_ARGUMENT_BUFFER(Argument, Capabilities, sizeof(Capabilities));

    RtlZeroMemory(OutputBuffer, OutputLength);
    Status = PciSendIoctl(FdoExtension->PhysicalDeviceObject,
                          IOCTL_ACPI_EVAL_METHOD,
                          InputBuffer,
                          InputLength,
                          OutputBuffer,
                          OutputLength);
    ExFreePoolWithTag(InputBuffer, 0);

    if (!NT_SUCCESS(Status))
    {
        /* A host bridge with no _OSC keeps everything, which is no error */
        DPRINT1("PCI - _OSC unavailable on FDO ext 0x%p (0x%08lx)\n",
                FdoExtension, Status);
        ExFreePoolWithTag(OutputBuffer, 0);
        return;
    }

    /* The one thing that comes back is the capability buffer, rewritten */
    Argument = OutputBuffer->Argument;
    if ((OutputBuffer->Count < 1) ||
        (Argument->Type != ACPI_METHOD_ARGUMENT_BUFFER) ||
        (Argument->DataLength < sizeof(Capabilities)))
    {
        DPRINT1("PCI - _OSC on FDO ext 0x%p returned nothing usable\n", FdoExtension);
        ExFreePoolWithTag(OutputBuffer, 0);
        return;
    }

    RtlCopyMemory(Capabilities, Argument->Data, sizeof(Capabilities));

    /*
     * The first word is how the call went. Anything but a clean run means the
     * firmware kept everything, so nothing may be assumed to have been granted.
     */
    if (Capabilities[0] & (PCI_OSC_STATUS_FAILURE |
                           PCI_OSC_STATUS_UNRECOGNISED_UUID |
                           PCI_OSC_STATUS_UNRECOGNISED_REVISION))
    {
        DPRINT1("PCI - _OSC on FDO ext 0x%p refused the request (0x%08lx)\n",
                FdoExtension, Capabilities[0]);
        ExFreePoolWithTag(OutputBuffer, 0);
        return;
    }

    /* Whatever survived in the control word is what this driver now owns */
    Granted = Capabilities[2] & Control;
    FdoExtension->OscControlGranted = Granted;
    FdoExtension->OscEvaluated = TRUE;

    if (Capabilities[0] & PCI_OSC_STATUS_CAPABILITIES_MASKED)
    {
        DPRINT1("PCI - _OSC on FDO ext 0x%p granted less than was asked for\n",
                FdoExtension);
    }

    DPRINT1("PCI - _OSC on FDO ext 0x%p: support 0x%08lx, control 0x%08lx\n",
            FdoExtension, Support, Granted);

    ExFreePoolWithTag(OutputBuffer, 0);
}

/* EOF */
