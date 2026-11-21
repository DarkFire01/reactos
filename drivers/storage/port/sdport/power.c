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

/* POWER FRAMEWORK ************************************************************/

static
VOID
NTAPI
SdPortPoFxComponentActive(
    _In_ PVOID Context,
    _In_ ULONG Component)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Component);
}

static
VOID
NTAPI
SdPortPoFxComponentIdle(
    _In_ PVOID Context,
    _In_ ULONG Component)
{
    PSDPORT_FDO_EXTENSION FdoExtension = Context;

    PoFxCompleteIdleCondition(FdoExtension->PoFxHandle, Component);
}

static
VOID
NTAPI
SdPortPoFxComponentIdleState(
    _In_ PVOID Context,
    _In_ ULONG Component,
    _In_ ULONG State)
{
    PSDPORT_FDO_EXTENSION FdoExtension = Context;

    UNREFERENCED_PARAMETER(State);
    PoFxCompleteIdleState(FdoExtension->PoFxHandle, Component);
}

static
VOID
NTAPI
SdPortPoFxPowerRequired(
    _In_ PVOID Context)
{
    PSDPORT_FDO_EXTENSION FdoExtension = Context;

    PoFxReportDevicePoweredOn(FdoExtension->PoFxHandle);
}

static
VOID
NTAPI
SdPortPoFxPowerNotRequired(
    _In_ PVOID Context)
{
    PSDPORT_FDO_EXTENSION FdoExtension = Context;

    PoFxCompleteDevicePowerNotRequired(FdoExtension->PoFxHandle);
}

/**
 * @brief Registers the host controller with the power framework as one component that only knows F0.
 *
 * On boards with a platform extension plugin the plugin owns the controller's
 * clocks and supplies and turns them on only for an active component, so the
 * component is activated before any register is touched and stays active.
 * Without a plugin the registration fails or does nothing, and the controller
 * runs on what firmware left on.
 */
VOID
NTAPI
SdPortRegisterPoFx(
    _Inout_ PSDPORT_FDO_EXTENSION FdoExtension)
{
    PO_FX_COMPONENT_IDLE_STATE State;
    PO_FX_DEVICE_V1 FxDevice;
    NTSTATUS Status;

    if (FdoExtension->PoFxHandle != NULL)
        return;

    RtlZeroMemory(&State, sizeof(State));
    RtlZeroMemory(&FxDevice, sizeof(FxDevice));
    FxDevice.Version = PO_FX_VERSION_V1;
    FxDevice.ComponentCount = 1;
    FxDevice.ComponentActiveConditionCallback = SdPortPoFxComponentActive;
    FxDevice.ComponentIdleConditionCallback = SdPortPoFxComponentIdle;
    FxDevice.ComponentIdleStateCallback = SdPortPoFxComponentIdleState;
    FxDevice.DevicePowerRequiredCallback = SdPortPoFxPowerRequired;
    FxDevice.DevicePowerNotRequiredCallback = SdPortPoFxPowerNotRequired;
    FxDevice.DeviceContext = FdoExtension;
    FxDevice.Components[0].IdleStateCount = 1;
    FxDevice.Components[0].IdleStates = &State;

    Status = PoFxRegisterDevice(FdoExtension->Pdo, (PPO_FX_DEVICE)&FxDevice, &FdoExtension->PoFxHandle);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("sdport: no power framework registration (0x%lx), using the firmware's clocks\n", Status);
        FdoExtension->PoFxHandle = NULL;
        return;
    }

    PoFxStartDevicePowerManagement(FdoExtension->PoFxHandle);
    PoFxActivateComponent(FdoExtension->PoFxHandle, 0, PO_FX_FLAG_BLOCKING);
}

VOID
NTAPI
SdPortUnregisterPoFx(
    _Inout_ PSDPORT_FDO_EXTENSION FdoExtension)
{
    if (FdoExtension->PoFxHandle == NULL)
        return;

    PoFxIdleComponent(FdoExtension->PoFxHandle, 0, PO_FX_FLAG_BLOCKING);
    PoFxUnregisterDevice(FdoExtension->PoFxHandle);
    FdoExtension->PoFxHandle = NULL;
}
