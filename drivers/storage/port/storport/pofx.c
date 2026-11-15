/*
 * PROJECT:     ReactOS Storport Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Adapter registration with the power framework
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define NDEBUG
#include <debug.h>

#define TAG_STORPORT_POFX 'XFoP'

static
VOID
NTAPI
PortPoFxComponentActive(
    _In_ PVOID Context,
    _In_ ULONG Component)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Component);
}

static
VOID
NTAPI
PortPoFxComponentIdle(
    _In_ PVOID Context,
    _In_ ULONG Component)
{
    PFDO_DEVICE_EXTENSION DeviceExtension = Context;

    PoFxCompleteIdleCondition(DeviceExtension->PoFxHandle, Component);
}

static
VOID
NTAPI
PortPoFxComponentIdleState(
    _In_ PVOID Context,
    _In_ ULONG Component,
    _In_ ULONG State)
{
    PFDO_DEVICE_EXTENSION DeviceExtension = Context;

    DPRINT("Storport adapter %p component %lu to F%lu\n", DeviceExtension, Component, State);
    PoFxCompleteIdleState(DeviceExtension->PoFxHandle, Component);
}

static
VOID
NTAPI
PortPoFxPowerRequired(
    _In_ PVOID Context)
{
    PFDO_DEVICE_EXTENSION DeviceExtension = Context;

    PoFxReportDevicePoweredOn(DeviceExtension->PoFxHandle);
}

static
VOID
NTAPI
PortPoFxPowerNotRequired(
    _In_ PVOID Context)
{
    PFDO_DEVICE_EXTENSION DeviceExtension = Context;

    PoFxCompleteDevicePowerNotRequired(DeviceExtension->PoFxHandle);
}

/* Each component carries its own F-state array, so the next one starts after it */
static
PSTOR_POFX_COMPONENT
PortPoFxNextComponent(
    _In_ PSTOR_POFX_COMPONENT Component)
{
    return (PSTOR_POFX_COMPONENT)((PUCHAR)Component +
                                  FIELD_OFFSET(STOR_POFX_COMPONENT, FStates[Component->FStateCount]));
}

/**
 * @brief Registers the adapter's components, then brings them up through an idle
 * pass so the platform applies each one's F0 resources before any I/O is issued.
 */
ULONG
PortInitializeAdapterPoFx(
    _In_ PFDO_DEVICE_EXTENSION DeviceExtension,
    _In_ PSTOR_POFX_DEVICE StorDevice,
    _Out_opt_ PBOOLEAN D3ColdEnabled)
{
    PPO_FX_DEVICE_V1 FxDevice;
    PSTOR_POFX_COMPONENT StorComponent;
    PPO_FX_COMPONENT_IDLE_STATE States;
    ULONG Index, State, StateTotal = 0;
    SIZE_T Size;
    NTSTATUS Status;

    if (D3ColdEnabled)
        *D3ColdEnabled = FALSE;

    if (!StorDevice || StorDevice->ComponentCount == 0)
        return STOR_STATUS_INVALID_PARAMETER;

    if (DeviceExtension->PoFxHandle)
        return STOR_STATUS_SUCCESS;

    StorComponent = StorDevice->Components;
    for (Index = 0; Index < StorDevice->ComponentCount; Index++)
    {
        StateTotal += StorComponent->FStateCount;
        StorComponent = PortPoFxNextComponent(StorComponent);
    }

    Size = FIELD_OFFSET(PO_FX_DEVICE_V1, Components[StorDevice->ComponentCount]) +
           StateTotal * sizeof(PO_FX_COMPONENT_IDLE_STATE);
    FxDevice = ExAllocatePoolZero(NonPagedPool, Size, TAG_STORPORT_POFX);
    if (!FxDevice)
        return STOR_STATUS_INSUFFICIENT_RESOURCES;

    FxDevice->Version = PO_FX_VERSION_V1;
    FxDevice->ComponentCount = StorDevice->ComponentCount;
    FxDevice->ComponentActiveConditionCallback = PortPoFxComponentActive;
    FxDevice->ComponentIdleConditionCallback = PortPoFxComponentIdle;
    FxDevice->ComponentIdleStateCallback = PortPoFxComponentIdleState;
    FxDevice->DevicePowerRequiredCallback = PortPoFxPowerRequired;
    FxDevice->DevicePowerNotRequiredCallback = PortPoFxPowerNotRequired;
    FxDevice->DeviceContext = DeviceExtension;

    States = (PPO_FX_COMPONENT_IDLE_STATE)&FxDevice->Components[StorDevice->ComponentCount];
    StorComponent = StorDevice->Components;
    for (Index = 0; Index < StorDevice->ComponentCount; Index++)
    {
        FxDevice->Components[Index].Id = StorComponent->Id;
        FxDevice->Components[Index].DeepestWakeableIdleState = StorComponent->DeepestWakeableFState;
        FxDevice->Components[Index].IdleStateCount = StorComponent->FStateCount;
        FxDevice->Components[Index].IdleStates = States;

        for (State = 0; State < StorComponent->FStateCount; State++)
        {
            States[State].TransitionLatency = StorComponent->FStates[State].TransitionLatency;
            States[State].ResidencyRequirement = StorComponent->FStates[State].ResidencyRequirement;
            States[State].NominalPower = StorComponent->FStates[State].NominalPower;
        }

        States += StorComponent->FStateCount;
        StorComponent = PortPoFxNextComponent(StorComponent);
    }

    Status = PoFxRegisterDevice(DeviceExtension->PhysicalDevice, FxDevice, &DeviceExtension->PoFxHandle);
    ExFreePoolWithTag(FxDevice, TAG_STORPORT_POFX);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PoFxRegisterDevice failed 0x%lx\n", Status);
        DeviceExtension->PoFxHandle = NULL;
        return STOR_STATUS_UNSUCCESSFUL;
    }

    /* Idle first, then active, so the platform walks F0 in for every component */
    PoFxStartDevicePowerManagement(DeviceExtension->PoFxHandle);
    for (Index = 0; Index < StorDevice->ComponentCount; Index++)
        PoFxActivateComponent(DeviceExtension->PoFxHandle, Index, PO_FX_FLAG_BLOCKING);

    DPRINT("Storport adapter %p registered %lu power component(s)\n",
            DeviceExtension, StorDevice->ComponentCount);
    return STOR_STATUS_SUCCESS;
}
