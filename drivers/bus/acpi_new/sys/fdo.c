/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PnP and power handling for the ACPI motherboard FDO
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"

// Storage is emitted in guid.c.
extern const GUID GUID_TRANSLATOR_INTERFACE_STANDARD;

// Trace the HAL's interrupt band. Not the SCI; nothing is captured here.
static VOID
UacpiTraceStartInterrupts(PUACPI_FDO Fdo, PCM_RESOURCE_LIST Translated)
{
    ULONG i, j, count = 0;
    ULONG first = 0, last = 0;

    UNREFERENCED_PARAMETER(Fdo);

    if (Translated == NULL)
    {
        return;
    }
    for (i = 0; i < Translated->Count; i++)
    {
        PCM_FULL_RESOURCE_DESCRIPTOR full = &Translated->List[i];
        PCM_PARTIAL_RESOURCE_LIST part = &full->PartialResourceList;

        for (j = 0; j < part->Count; j++)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &part->PartialDescriptors[j];

            if (d->Type != CmResourceTypeInterrupt)
            {
                continue;
            }
            if (count == 0)
            {
                first = d->u.Interrupt.Vector;
            }
            last = d->u.Interrupt.Vector;
            count++;
        }
    }

    if (count != 0)
    {
        UacpiTrace("[acpi] HAL reserved interrupt band: %u vector(s) "
                  "0x%X..0x%X\n", count, first, last);
    }
    else
    {
        UacpiTrace("[acpi] HAL reported no interrupt vectors\n");
    }
}

// IRP_MN_START_DEVICE
static NTSTATUS
UacpiFdoStart(PUACPI_FDO Fdo, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    status = UacpiForwardAndWait(Fdo->LowerDevice, Irp);
    if (!NT_SUCCESS(status))
    {
        UacpiTrace("[acpi] lower START failed 0x%X\n", status);
        return UacpiCompleteIrp(Irp, status, 0);
    }

    // The SCI comes from the FADT; UacpiBringUpInterpreter wires it.
    UacpiTraceStartInterrupts(Fdo, sp->Parameters.StartDevice.AllocatedResourcesTranslated);

    // The IDT allocator may only hand out vectors from this grant.
    UacpiIrqLibSetHalVectors(sp->Parameters.StartDevice.AllocatedResources);

    status = UacpiBringUpInterpreter(Fdo);
    if (!NT_SUCCESS(status))
    {
        return UacpiCompleteIrp(Irp, status, 0);
    }

    g_AcpiFdo = Fdo;                     // the Notify router / interfaces use this
    (void)UacpiEnumerateNamespace(Fdo);   // populate the child PDO list
    (void)UacpiIrqArbiterInitialize(Fdo); // the GSIV arbiter (non-fatal if it fails)

    Fdo->Started = TRUE;
    return UacpiCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

// PnP dispatch
NTSTATUS
UacpiFdoPnp(PUACPI_FDO Fdo, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);

    switch (sp->MinorFunction)
    {
    case IRP_MN_START_DEVICE:
        return UacpiFdoStart(Fdo, Irp);

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (sp->Parameters.QueryDeviceRelations.Type == BusRelations)
        {
            // The lower raw PDO has no relations to add; complete here.
            NTSTATUS status = UacpiBuildBusRelations(Fdo, Irp);
            return UacpiCompleteIrp(Irp, status, Irp->IoStatus.Information);
        }
        return UacpiForwardAndForget(Fdo->LowerDevice, Irp);

    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_QUERY_STOP_DEVICE:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        return UacpiForwardAndForget(Fdo->LowerDevice, Irp);

    case IRP_MN_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        return UacpiForwardAndForget(Fdo->LowerDevice, Irp);

    case IRP_MN_QUERY_INTERFACE:
        // Serve the interrupt arbiter and translator; forward everything else.
        {
            NTSTATUS st = UacpiQueryIrqArbiter(sp);

            if (st != STATUS_NOT_SUPPORTED)
            {
                Irp->IoStatus.Status = st;
            }
            else if (IsEqualGUID(sp->Parameters.QueryInterface.InterfaceType,
                                   &GUID_TRANSLATOR_INTERFACE_STANDARD) &&
                       (ULONG_PTR)sp->Parameters.QueryInterface.InterfaceSpecificData ==
                           CmResourceTypeInterrupt)
                           {
                Irp->IoStatus.Status = UacpiBuildIrqTranslator("ACPI root", sp);
            }
        }
        return UacpiForwardAndForget(Fdo->LowerDevice, Irp);

    case IRP_MN_REMOVE_DEVICE:
    {
        PDEVICE_OBJECT lower = Fdo->LowerDevice;
        PDEVICE_OBJECT self  = Fdo->Common.Self;
        UacpiTearDownInterpreter(Fdo);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        {
            NTSTATUS status = IoCallDriver(lower, Irp);
            IoDetachDevice(lower);
            IoDeleteDevice(self);
            return status;
        }
    }

    default:
        return UacpiForwardAndForget(Fdo->LowerDevice, Irp);
    }
}

// Power: pass down; no power policy yet.
NTSTATUS
UacpiFdoPower(PUACPI_FDO Fdo, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);

    // System sleep/shutdown transitions run through the ACPI _PTS/_WAK path.
    if (sp->MinorFunction == IRP_MN_SET_POWER &&
        sp->Parameters.Power.Type == SystemPowerState)
        {
        return UacpiSystemSetPower(Fdo, Irp);
    }

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(Fdo->LowerDevice, Irp);
}
