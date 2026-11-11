/*
 * PROJECT:     ReactOS Storage Stack / SD host controller port driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Power and WMI dispatch
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

NTSTATUS
NTAPI
SdPortDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PSDPORT_FDO_EXTENSION FdoExtension = DeviceObject->DeviceExtension;

    PoStartNextPowerIrp(Irp);

    if (FdoExtension->ExtensionType == SdportExtensionFdo)
    {
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(FdoExtension->LowerDevice, Irp);
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
SdPortDispatchSystemControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PSDPORT_FDO_EXTENSION FdoExtension = DeviceObject->DeviceExtension;

    if (FdoExtension->ExtensionType == SdportExtensionFdo)
    {
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(FdoExtension->LowerDevice, Irp);
    }

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}
