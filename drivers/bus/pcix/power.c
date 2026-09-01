/*
 * PROJECT:         ReactOS PCI Bus Driver
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            drivers/bus/pci/power.c
 * PURPOSE:         Bus/Device Power Management
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <pci.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

ULONG PciPowerDelayTable[PowerDeviceD3 * PowerDeviceD3] =
{
    0,      // D0 -> D0
    0,      // D1 -> D0
    200,    // D2 -> D0
    10000,  // D3 -> D0

    0,      // D0 -> D1
    0,      // D1 -> D1
    200,    // D2 -> D1
    10000,  // D3 -> D1

    200,    // D0 -> D2
    200,    // D1 -> D2
    0,      // D2 -> D2
    10000,  // D3 -> D2

    10000,  // D0 -> D3
    10000,  // D1 -> D3
    10000,  // D2 -> D3
    0       // D3 -> D3
};

/* FUNCTIONS ******************************************************************/

NTSTATUS
NTAPI
PciStallForPowerChange(IN PPCI_PDO_EXTENSION PdoExtension,
                       IN DEVICE_POWER_STATE PowerState,
                       IN ULONG_PTR CapOffset)
{
    ULONG PciState, TimeoutEntry, PmcsrOffset, TryCount;
    PPCI_VERIFIER_DATA VerifierData;
    LARGE_INTEGER Interval;
    PCI_PMCSR Pmcsr;
    KIRQL Irql;

    /* Make sure the power state is valid, and the device can support it */
    ASSERT((PdoExtension->PowerState.CurrentDeviceState >= PowerDeviceD0) &&
           (PdoExtension->PowerState.CurrentDeviceState <= PowerDeviceD3));
    ASSERT((PowerState >= PowerDeviceD0) && (PowerState <= PowerDeviceD3));
    ASSERT(!(PdoExtension->HackFlags & PCI_HACK_NO_PM_CAPS));

    /* Save the current IRQL */
    Irql = KeGetCurrentIrql();

    /* Pick the expected timeout for this transition */
    TimeoutEntry = PciPowerDelayTable[PowerState * PdoExtension->PowerState.CurrentDeviceState];

    /* PCI power states are one less than NT power states */
    PciState = PowerState - 1;

    /* The state status is stored in the PMCSR offset */
    PmcsrOffset = CapOffset + FIELD_OFFSET(PCI_PM_CAPABILITY, PMCSR);

    /* Try changing the power state up to 100 times */
    TryCount = 100;
    while (--TryCount)
    {
        /* Check if this state transition will take time */
        if (TimeoutEntry > 0)
        {
            /* Check if this is happening at high IRQL */
            if (Irql >= DISPATCH_LEVEL)
            {
                /* Can't wait at high IRQL, stall the processor */
                KeStallExecutionProcessor(TimeoutEntry);
            }
            else
            {
                /* Do a wait for the timeout specified instead */
                Interval.QuadPart = -10 * TimeoutEntry;
                Interval.QuadPart -= KeQueryTimeIncrement() - 1;
                KeDelayExecutionThread(KernelMode, FALSE, &Interval);
            }
        }

        /* Read the PMCSR and see if the state has changed */
        PciReadDeviceConfig(PdoExtension, &Pmcsr, PmcsrOffset, sizeof(PCI_PMCSR));
        if (Pmcsr.PowerState == PciState) return STATUS_SUCCESS;

        /* Try again, forcing a timeout of 1ms */
        TimeoutEntry = 1000;
    }

    /* Call verifier with this error */
    VerifierData = PciVerifierRetrieveFailureData(2);
    ASSERT(VerifierData);
    VfFailDeviceNode(PdoExtension->PhysicalDeviceObject,
                     PCI_VERIFIER_DETECTED_VIOLATION,
                     2, // The PMCSR register was not updated within the spec-mandated time.
                     VerifierData->FailureClass,
                     &VerifierData->AssertionControl,
                     VerifierData->DebuggerMessageText,
                     "%DevObj%Ulong",
                     PdoExtension->PhysicalDeviceObject,
                     PciState);

    return STATUS_DEVICE_PROTOCOL_ERROR;
}

NTSTATUS
NTAPI
PciSetPowerManagedDevicePowerState(IN PPCI_PDO_EXTENSION DeviceExtension,
                                   IN DEVICE_POWER_STATE DeviceState,
                                   IN BOOLEAN IrpSet)
{
    NTSTATUS Status;
    PCI_PM_CAPABILITY PmCaps;
    ULONG CapsOffset;

    /* Assume success */
    Status = STATUS_SUCCESS;

    /* Check if this device can support low power states */
    if (!(PciCanDisableDecodes(DeviceExtension, NULL, 0, TRUE)) &&
         (DeviceState != PowerDeviceD0))
    {
        /* Simply return success, ignoring this request */
        DPRINT1("Cannot disable decodes on this device, ignoring PM request...\n");
        return Status;
    }

    /* Does the device support power management at all? */
    if (!(DeviceExtension->HackFlags & PCI_HACK_NO_PM_CAPS))
    {
        /* Get the PM capabilities register */
        CapsOffset = PciReadDeviceCapability(DeviceExtension,
                                             DeviceExtension->CapabilitiesPtr,
                                             PCI_CAPABILITY_ID_POWER_MANAGEMENT,
                                             &PmCaps.Header,
                                             sizeof(PCI_PM_CAPABILITY));
        ASSERT(CapsOffset);
        ASSERT(DeviceState != PowerDeviceUnspecified);

        /* Check if the device is being powered up */
        if (DeviceState == PowerDeviceD0)
        {
            /* Set full power state */
            PmCaps.PMCSR.ControlStatus.PowerState = 0;

            /* Check if the device supports Cold-D3 poweroff */
            if (PmCaps.PMC.Capabilities.Support.PMED3Cold)
            {
                /* If there was a pending PME, clear it */
                PmCaps.PMCSR.ControlStatus.PMEStatus = 1;
            }
        }
        else
        {
            /* Otherwise, just set the new power state, converting from NT */
            PmCaps.PMCSR.ControlStatus.PowerState = DeviceState - 1;
        }

        /* Write the new power state in the PMCSR */
        PciWriteDeviceConfig(DeviceExtension,
                             &PmCaps.PMCSR,
                             CapsOffset + FIELD_OFFSET(PCI_PM_CAPABILITY, PMCSR),
                             sizeof(PCI_PMCSR));

        /* Now wait for the change to "stick" based on the spec-mandated time */
        Status = PciStallForPowerChange(DeviceExtension, DeviceState, CapsOffset);
        if (!NT_SUCCESS(Status)) return Status;
    }
    else
    {
        /* Nothing to do! */
        DPRINT1("No PM on this device, ignoring request\n");
    }

    /* Check if new resources have to be assigned */
    if (IrpSet)
    {
        /* Check if the new device state is lower (higher power) than now */
        if (DeviceState < DeviceExtension->PowerState.CurrentDeviceState)
        {
            /*
             * A device coming back up may well have forgotten everything that
             * was written into it while it was down - leaving D3 is allowed to
             * reset a function - so its configuration is programmed again.
             */
            Status = PciSetResources(DeviceExtension, FALSE, FALSE);
        }
    }

    /* Return the power state change status */
    return Status;
}

NTSTATUS
NTAPI
PciFdoWaitWake(IN PIRP Irp,
               IN PIO_STACK_LOCATION IoStackLocation,
               IN PPCI_FDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    /*
     * Arming a bus to wake the machine would mean keeping its power event
     * signal live across the transition, which nothing here does. Say so
     * plainly: a caller told the bus cannot wake will simply not rely on it.
     */
    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    PoStartNextPowerIrp(Irp);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
PciFdoSetPowerState(IN PIRP Irp,
                    IN PIO_STACK_LOCATION IoStackLocation,
                    IN PPCI_FDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Irp);

    /*
     * Both a host bridge and a PCI-PCI bridge are powered by whatever sits
     * above them, so there is nothing in configuration space to write for
     * this. What matters is remembering where the bus now is, because how far
     * down a device on it may go is bounded by the state of its bus.
     */
    if (IoStackLocation->Parameters.Power.Type == SystemPowerState)
    {
        DeviceExtension->PowerState.CurrentSystemState =
            IoStackLocation->Parameters.Power.State.SystemState;
    }
    else
    {
        DeviceExtension->PowerState.CurrentDeviceState =
            IoStackLocation->Parameters.Power.State.DeviceState;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PciFdoIrpQueryPower(IN PIRP Irp,
                    IN PIO_STACK_LOCATION IoStackLocation,
                    IN PPCI_FDO_EXTENSION DeviceExtension)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(IoStackLocation);
    UNREFERENCED_PARAMETER(DeviceExtension);

    /*
     * A bus has no state of its own that a power transition would lose, and
     * the devices on it are asked separately, so there is nothing here that
     * could be a reason to refuse one.
     */
    return STATUS_SUCCESS;
}

/* EOF */
