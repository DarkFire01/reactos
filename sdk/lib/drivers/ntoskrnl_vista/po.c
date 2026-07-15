/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Po functions of Vista+
 * COPYRIGHT:   Copyright 2020 Victor Perevertkin (victor.perevertkin@reactos.org)
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ntoskrnl_vista.h"

/**
 * @brief
 * Registers a callback that is notified when a power setting changes.
 *
 * @param[in] DeviceObject
 * Optional device object associated with the registration.
 *
 * @param[in] SettingGuid
 * The GUID of the power setting to monitor.
 *
 * @param[in] Callback
 * The callback invoked when the setting changes.
 *
 * @param[in] Context
 * Optional context passed to @p Callback.
 *
 * @param[out] Handle
 * Receives the registration handle.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement power setting notifications.
 */
NTSTATUS
NTAPI
PoRegisterPowerSettingCallback(
    _In_opt_ PDEVICE_OBJECT DeviceObject,
    _In_ LPCGUID SettingGuid,
    _In_ PPOWER_SETTING_CALLBACK Callback,
    _In_opt_ PVOID Context,
    _Outptr_opt_ PVOID *Handle)
{
    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Unregisters a power setting change callback.
 *
 * @param[in,out] Handle
 * The registration handle returned by PoRegisterPowerSettingCallback().
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement power setting notifications.
 */
_IRQL_requires_max_(APC_LEVEL)
NTSTATUS
NTAPI
PoUnregisterPowerSettingCallback(
    _Inout_ PVOID Handle)
{
    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Queries the remaining watchdog time for a device power transition.
 *
 * @param[in] Pdo
 * The physical device object being tracked.
 *
 * @param[out] SecondsRemaining
 * Receives the number of seconds before the watchdog expires.
 *
 * @return
 * TRUE if a watchdog is active, FALSE otherwise.
 *
 * @unimplemented
 * ReactOS does not implement power transition watchdogs.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
NTAPI
PoQueryWatchdogTime(
    _In_ PDEVICE_OBJECT Pdo,
    _Out_ PULONG SecondsRemaining)
{
    return FALSE;
}

/**
 * @brief
 * Marks an IRP as the cause of a system wake.
 *
 * @param[in,out] Irp
 * The power IRP that triggered the wake.
 *
 * @unimplemented
 * ReactOS does not track system wake sources.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
NTAPI
PoSetSystemWake(
    _Inout_ struct _IRP *Irp)
{
}

/**
 * @brief
 * Determines whether an IRP was flagged as a system wake source.
 *
 * @param[in] Irp
 * The power IRP to test.
 *
 * @return
 * TRUE if the IRP is a wake source, FALSE otherwise.
 *
 * @unimplemented
 * ReactOS does not track system wake sources.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
NTAPI
PoGetSystemWake(
    _In_ struct _IRP *Irp)
{
    return FALSE;
}

/**
 * @brief
 * Notifies the power manager that a device was powered on unexpectedly.
 *
 * @param[in] Pdo
 * The physical device object that was surprise-powered-on.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement surprise power-on notifications.
 */
NTSTATUS
NTAPI
PoFxNotifySurprisePowerOn(
    _In_ PDEVICE_OBJECT Pdo)
{
    UNREFERENCED_PARAMETER(Pdo);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Creates a thermal management request for a device.
 *
 * @param[out] ThermalRequest
 * Receives the created thermal request object.
 *
 * @param[in] TargetDeviceObject
 * The device object whose thermal state is managed.
 *
 * @param[in] PolicyDeviceObject
 * The device object providing the thermal policy.
 *
 * @param[in] Callback
 * The callback invoked when the thermal request changes.
 *
 * @param[in] Context
 * Optional context passed to @p Callback.
 *
 * @param[in] Flags
 * Thermal request flags.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement active thermal management.
 */
NTSTATUS
NTAPI
PoCreateThermalRequest(
    _Outptr_ PVOID *ThermalRequest,
    _In_ PDEVICE_OBJECT TargetDeviceObject,
    _In_ PDEVICE_OBJECT PolicyDeviceObject,
    _In_ PVOID Callback,
    _In_opt_ PVOID Context,
    _In_ ULONG Flags)
{
    UNREFERENCED_PARAMETER(TargetDeviceObject);
    UNREFERENCED_PARAMETER(PolicyDeviceObject);
    UNREFERENCED_PARAMETER(Callback);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Flags);

    if (ThermalRequest != NULL)
        *ThermalRequest = NULL;

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Deletes a thermal management request.
 *
 * @param[in] ThermalRequest
 * The thermal request object to delete.
 *
 * @unimplemented
 * ReactOS does not implement active thermal management.
 */
VOID
NTAPI
PoDeleteThermalRequest(
    _In_ PVOID ThermalRequest)
{
    UNREFERENCED_PARAMETER(ThermalRequest);
}

/**
 * @brief
 * Queries whether a thermal request supports active or passive cooling.
 *
 * @param[in] ThermalRequest
 * The thermal request object to query.
 *
 * @param[out] Support
 * Receives the supported cooling capabilities.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement active thermal management.
 */
NTSTATUS
NTAPI
PoGetThermalRequestSupport(
    _In_ PVOID ThermalRequest,
    _Out_ PULONG Support)
{
    UNREFERENCED_PARAMETER(ThermalRequest);

    if (Support != NULL)
        *Support = 0;

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Engages or disengages active cooling for a thermal request.
 *
 * @param[in] ThermalRequest
 * The thermal request object.
 *
 * @param[in] Engaged
 * Non-zero to engage active cooling, zero to disengage it.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement active thermal management.
 */
NTSTATUS
NTAPI
PoSetThermalActiveCooling(
    _In_ PVOID ThermalRequest,
    _In_ ULONG Engaged)
{
    UNREFERENCED_PARAMETER(ThermalRequest);
    UNREFERENCED_PARAMETER(Engaged);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Sets the passive cooling throttle level for a thermal request.
 *
 * @param[in] ThermalRequest
 * The thermal request object.
 *
 * @param[in] Throttle
 * The passive cooling throttle percentage.
 *
 * @return
 * STATUS_SUCCESS on success, or an appropriate NTSTATUS error code.
 *
 * @unimplemented
 * ReactOS does not implement passive thermal management.
 */
NTSTATUS
NTAPI
PoSetThermalPassiveCooling(
    _In_ PVOID ThermalRequest,
    _In_ ULONG Throttle)
{
    UNREFERENCED_PARAMETER(ThermalRequest);
    UNREFERENCED_PARAMETER(Throttle);

    return STATUS_NOT_IMPLEMENTED;
}
