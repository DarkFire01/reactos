/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared PnP plumbing for the Qualcomm function drivers (qcompnp.lib)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Every Qualcomm FDO extension starts with this */
typedef struct _QCOM_PNP_DEVICE
{
    PDEVICE_OBJECT Self;
    PDEVICE_OBJECT Pdo;
    PDEVICE_OBJECT Lower;
} QCOM_PNP_DEVICE, *PQCOM_PNP_DEVICE;

NTSTATUS
NTAPI
QcPnpCreateDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT Pdo,
    _In_ ULONG ExtensionSize,
    _In_ DEVICE_TYPE DeviceType,
    _Outptr_ PQCOM_PNP_DEVICE *Device);

VOID
NTAPI
QcPnpDeleteDevice(
    _In_ PQCOM_PNP_DEVICE Device);

NTSTATUS
NTAPI
QcPnpForwardAndWait(
    _In_ PQCOM_PNP_DEVICE Device,
    _Inout_ PIRP Irp);

NTSTATUS
NTAPI
QcPnpPassDown(
    _In_ PQCOM_PNP_DEVICE Device,
    _Inout_ PIRP Irp);

NTSTATUS
NTAPI
QcPnpComplete(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status);

NTSTATUS
NTAPI
QcPnpDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp);

NTSTATUS
NTAPI
QcPnpDispatchPassDown(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp);

NTSTATUS
NTAPI
QcPnpDispatchCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp);

VOID
NTAPI
QcPnpLogResources(
    _In_z_ PCSTR Component,
    _In_opt_ PCM_RESOURCE_LIST Resources);

/* For export drivers that only claim their ACPI node so it shows as installed */
VOID
NTAPI
QcPnpInitializeClaimDriver(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_z_ PCSTR Component);
