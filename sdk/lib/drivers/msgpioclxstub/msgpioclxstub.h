/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     GpioClx client binding for drivers built in tree
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <gpioclx.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Entry points handed out by msgpioclx, indexed by GPIO_CLX_EXPORT_INDEX. */
extern PGPIO_CLX_EXPORTED_INTERFACES GpioClxFunctions[GPIO_CLX_TOTAL_EXPORTS];

/**
 * Binds the calling driver to msgpioclx and fills GpioClxFunctions.
 * Call from DriverEntry after WdfDriverCreate.
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
GpioClxStubBindClass(VOID);

/** Drops the binding taken by GpioClxStubBindClass. Safe to call when not bound. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
GpioClxStubUnbindClass(VOID);

FORCEINLINE
NTSTATUS
GPIO_CLX_RegisterClient(
    _In_ WDFDRIVER Driver,
    _Inout_ PGPIO_CLIENT_REGISTRATION_PACKET RegistrationPacket,
    _In_ PUNICODE_STRING RegistryPath)
{
    return ((PGPIO_CLX_REGISTER_CLIENT)GpioClxFunctions[RegisterClientIndex])(Driver,
                                                                               RegistrationPacket,
                                                                               RegistryPath);
}

FORCEINLINE
NTSTATUS
GPIO_CLX_UnregisterClient(
    _In_ WDFDRIVER Driver)
{
    return ((PGPIO_CLX_UNREGISTER_CLIENT)GpioClxFunctions[UnregisterClientIndex])(Driver);
}

FORCEINLINE
NTSTATUS
GPIO_CLX_ProcessAddDevicePreDeviceCreate(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit,
    _Out_ PWDF_OBJECT_ATTRIBUTES FdoAttributes)
{
    return ((PGPIO_CLX_PROCESS_ADD_DEVICE_PRE_DEVICE_CREATE)
                GpioClxFunctions[AddDevicePreDeviceCreateIndex])(Driver, DeviceInit, FdoAttributes);
}

FORCEINLINE
NTSTATUS
GPIO_CLX_ProcessAddDevicePostDeviceCreate(
    _In_ WDFDRIVER Driver,
    _In_ WDFDEVICE Device)
{
    return ((PGPIO_CLX_PROCESS_ADD_DEVICE_POST_DEVICE_CREATE)
                GpioClxFunctions[AddDevicePostDeviceCreateIndex])(Driver, Device);
}

FORCEINLINE
VOID
GPIO_CLX_AcquireInterruptLock(
    _In_ PVOID Context,
    _In_ BANK_ID BankId)
{
    ((PGPIO_CLX_ACQUIRE_INTERRUPT_LOCK)GpioClxFunctions[AcquireInterruptLockIndex])(Context, BankId);
}

FORCEINLINE
VOID
GPIO_CLX_ReleaseInterruptLock(
    _In_ PVOID Context,
    _In_ BANK_ID BankId)
{
    ((PGPIO_CLX_RELEASE_INTERRUPT_LOCK)GpioClxFunctions[ReleaseInterruptLockIndex])(Context, BankId);
}

#ifdef __cplusplus
}
#endif
