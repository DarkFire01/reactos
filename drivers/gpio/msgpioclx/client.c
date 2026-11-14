/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The entry points a controller driver calls
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#define NDEBUG
#include "gpioclxp.h"

#include <ntstrsafe.h>

/* Kernel and administrators get full access, user mode drivers too */
static const UNICODE_STRING GcxControllerSddl =
    RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;UD)");

static volatile LONG GcxControllerInstance;

/* Registration flags this class extension understands */
#define GCX_REGISTRATION_EXTENDED_FLAG 0x80000000UL
#define GCX_REGISTRATION_KNOWN_FLAGS   GPIO_CLIENT_REGISTRATION_FLAGS_PAGING_PATH_SUPPORT

/**
 * @brief
 * Checks a registration packet before it is accepted.
 */
static
BOOLEAN
NTAPI
GcxIsRegistrationValid(
    _In_ PGPIO_CLIENT_REGISTRATION_PACKET Packet)
{
    const GPIO_CLIENT_REGISTRATION_PACKET *Callbacks = Packet;
    ULONG Flags;

    if (Packet->Version == 0)
        return FALSE;

    Flags = Packet->Flags & ~GCX_REGISTRATION_EXTENDED_FLAG;
    if (Packet->Version >= 4 && (Flags & ~GCX_REGISTRATION_KNOWN_FLAGS) != 0)
        return FALSE;

    /* Version 1 ends after CLIENT_ReconfigureInterrupt */
    if (Packet->Size < RTL_SIZEOF_THROUGH_FIELD(GPIO_CLIENT_REGISTRATION_PACKET,
                                                CLIENT_ReconfigureInterrupt))
    {
        return FALSE;
    }

    if (Callbacks->CLIENT_PrepareController == NULL ||
        Callbacks->CLIENT_ReleaseController == NULL ||
        Callbacks->CLIENT_StartController == NULL ||
        Callbacks->CLIENT_StopController == NULL ||
        Callbacks->CLIENT_QueryControllerBasicInformation == NULL)
    {
        return FALSE;
    }

    /* Interrupt support is all or nothing for the calls the demultiplexer needs */
    if (Callbacks->CLIENT_EnableInterrupt != NULL)
    {
        if (Callbacks->CLIENT_DisableInterrupt == NULL ||
            Callbacks->CLIENT_MaskInterrupts == NULL ||
            Callbacks->CLIENT_QueryActiveInterrupts == NULL)
        {
            return FALSE;
        }
    }

    if (Callbacks->CLIENT_ConnectIoPins != NULL)
    {
        if (Callbacks->CLIENT_DisconnectIoPins == NULL)
            return FALSE;

        if (Callbacks->CLIENT_ReadGpioPins == NULL && Callbacks->CLIENT_WriteGpioPins == NULL)
            return FALSE;
    }

    return TRUE;
}

/**
 * @brief
 * The registration of a driver, or NULL if it never registered.
 */
static
PGCX_CLIENT
NTAPI
GcxLookupClient(
    _In_ WDFDRIVER Driver)
{
    PGCX_CLIENT Client;

    Client = GcxGetClient(Driver);
    if (Client == NULL || !Client->Registered)
        return NULL;

    return Client;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
GcxRegisterClient(
    WDFDRIVER Driver,
    PGPIO_CLIENT_REGISTRATION_PACKET RegistrationPacket,
    PUNICODE_STRING RegistryPath)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    PGCX_CLIENT Client;
    ULONG CopyLength;
    NTSTATUS Status;

    if (Driver == NULL || RegistrationPacket == NULL || RegistryPath == NULL)
        return STATUS_INVALID_PARAMETER;

    if (!GcxIsRegistrationValid(RegistrationPacket))
    {
        DPRINT1("GpioClx: rejected packet version %u size %u\n",
                RegistrationPacket->Version,
                RegistrationPacket->Size);
        return STATUS_GPIO_INVALID_REGISTRATION_PACKET;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, GCX_CLIENT);
    Status = WdfObjectAllocateContext(Driver, &Attributes, (PVOID *)&Client);
    if (Status == STATUS_OBJECT_NAME_EXISTS)
        Status = STATUS_SUCCESS;

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: no client context 0x%08lx\n", Status);
        return Status;
    }

    /* Only what the client's version describes is taken; the rest stays NULL */
    RtlZeroMemory(&Client->Registration, sizeof(Client->Registration));
    CopyLength = min(RegistrationPacket->Size, (USHORT)sizeof(Client->Registration));
    RtlCopyMemory(&Client->Registration, RegistrationPacket, CopyLength);
    Client->Registration.Size = (USHORT)CopyLength;

    if (RegistrationPacket->Version < 2)
        Client->Registration.CLIENT_QueryEnabledInterrupts = NULL;

    if (RegistrationPacket->Version < 3)
    {
        Client->Registration.CLIENT_ConnectFunctionConfigPins = NULL;
        Client->Registration.CLIENT_DisconnectFunctionConfigPins = NULL;
    }

    Client->Registered = TRUE;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
GcxUnregisterClient(
    WDFDRIVER Driver)
{
    PGCX_CLIENT Client;

    Client = GcxLookupClient(Driver);
    if (Client == NULL)
        return STATUS_NOT_FOUND;

    Client->Registered = FALSE;
    RtlZeroMemory(&Client->Registration, sizeof(Client->Registration));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
GcxProcessAddDevicePreDeviceCreate(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT DeviceInit,
    PWDF_OBJECT_ATTRIBUTES FdoAttributes)
{
    DECLARE_UNICODE_STRING_SIZE(DeviceName, 32);
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    PGCX_CLIENT Client;
    NTSTATUS Status;

    Client = GcxLookupClient(Driver);
    if (Client == NULL)
        return STATUS_INVALID_PARAMETER;

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDevicePrepareHardware = GcxEvtDevicePrepareHardware;
    PnpPowerCallbacks.EvtDeviceReleaseHardware = GcxEvtDeviceReleaseHardware;
    PnpPowerCallbacks.EvtDeviceD0Entry = GcxEvtDeviceD0Entry;
    PnpPowerCallbacks.EvtDeviceD0Exit = GcxEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpPowerCallbacks);

    if (Client->Registration.Version >= 4 &&
        (Client->Registration.Flags & GPIO_CLIENT_REGISTRATION_FLAGS_PAGING_PATH_SUPPORT) != 0)
    {
        WdfDeviceInitSetPowerNotPageable(DeviceInit);
    }

    Status = RtlUnicodeStringPrintf(&DeviceName,
                                    L"\\Device\\GPIO_%ld",
                                    InterlockedIncrement(&GcxControllerInstance));
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfDeviceInitAssignName(DeviceInit, &DeviceName);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GpioClx: could not name %wZ 0x%08lx\n", &DeviceName, Status);
        return Status;
    }

    Status = WdfDeviceInitAssignSDDLString(DeviceInit, &GcxControllerSddl);
    if (!NT_SUCCESS(Status))
        return Status;

    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    /* The client's context rides behind ours in the same allocation */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(FdoAttributes, GCX_CONTROLLER);
    FdoAttributes->ContextSizeOverride = GCX_CLIENT_CONTEXT_OFFSET +
                                         Client->Registration.ControllerContextSize;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
NTAPI
GcxProcessAddDevicePostDeviceCreate(
    WDFDRIVER Driver,
    WDFDEVICE Device)
{
    PGCX_CONTROLLER Controller;
    PGCX_CLIENT Client;

    Client = GcxLookupClient(Driver);
    if (Client == NULL || Device == NULL)
        return STATUS_INVALID_PARAMETER;

    Controller = GcxGetController(Device);
    if (Controller == NULL)
    {
        DPRINT1("GpioClx: device %p lacks the controller context\n", Device);
        return STATUS_INVALID_DEVICE_STATE;
    }

    Controller->Signature = GCX_CONTROLLER_SIGNATURE;
    Controller->Device = Device;
    Controller->Registration = Client->Registration;
    Controller->PrimaryGsiv = GCX_NO_PRIMARY_GSIV;
    InitializeListHead(&Controller->ControllerLink);
    Controller->State = GcxControllerStopped;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Recovers a controller from the context a client was handed, or crashes on a bad one.
 */
static
PGCX_CONTROLLER
NTAPI
GcxControllerFromContext(
    _In_ PVOID Context,
    _In_ BANK_ID BankId)
{
    PGCX_CONTROLLER Controller;

    if (Context == NULL)
        KeBugCheckEx(GPIO_CONTROLLER_DRIVER_ERROR, 2, (ULONG_PTR)Context, 1, 0);

    Controller = (PGCX_CONTROLLER)((PUCHAR)Context - GCX_CLIENT_CONTEXT_OFFSET);
    if (Controller->Signature != GCX_CONTROLLER_SIGNATURE)
        KeBugCheckEx(GPIO_CONTROLLER_DRIVER_ERROR, 2, (ULONG_PTR)Context, 1, 0);

    if (Controller->Banks == NULL || BankId >= Controller->BankCount)
    {
        KeBugCheckEx(GPIO_CONTROLLER_DRIVER_ERROR,
                     2,
                     (ULONG_PTR)Controller,
                     2,
                     BankId | ((ULONG_PTR)Controller->BankCount << 16));
    }

    return Controller;
}

_Use_decl_annotations_
VOID
NTAPI
GcxAcquireInterruptLock(
    PVOID Context,
    BANK_ID BankId)
{
    PGCX_CONTROLLER Controller;
    PKTHREAD Self;
    PGCX_BANK Bank;

    Controller = GcxControllerFromContext(Context, BankId);
    Bank = &Controller->Banks[BankId];
    Self = KeGetCurrentThread();

    if (Bank->LockOwner == Self)
        KeBugCheckEx(GPIO_CONTROLLER_DRIVER_ERROR, 2, (ULONG_PTR)Controller, 3, 0);

    if (!Controller->Information.Flags.MemoryMappedController)
    {
        WdfWaitLockAcquire(Bank->WaitLock, NULL);
    }
    else if (Controller->Interrupt == NULL)
    {
        WdfSpinLockAcquire(Bank->SpinLock);
    }
    else
    {
        /* Every bank shares the one interrupt, so nest instead of deadlocking */
        if (Controller->InterruptLockOwner != Self)
        {
            WdfInterruptAcquireLock(Controller->Interrupt);
            Controller->InterruptLockOwner = Self;
        }
        Controller->InterruptLockDepth++;
    }

    Bank->LockOwner = Self;
}

_Use_decl_annotations_
VOID
NTAPI
GcxReleaseInterruptLock(
    PVOID Context,
    BANK_ID BankId)
{
    PGCX_CONTROLLER Controller;
    PGCX_BANK Bank;

    Controller = GcxControllerFromContext(Context, BankId);
    Bank = &Controller->Banks[BankId];

    if (Bank->LockOwner != KeGetCurrentThread())
        KeBugCheckEx(GPIO_CONTROLLER_DRIVER_ERROR, 2, (ULONG_PTR)Controller, 4, 0);

    Bank->LockOwner = NULL;

    if (!Controller->Information.Flags.MemoryMappedController)
    {
        WdfWaitLockRelease(Bank->WaitLock);
    }
    else if (Controller->Interrupt == NULL)
    {
        WdfSpinLockRelease(Bank->SpinLock);
    }
    else if (--Controller->InterruptLockDepth == 0)
    {
        Controller->InterruptLockOwner = NULL;
        WdfInterruptReleaseLock(Controller->Interrupt);
    }
}
