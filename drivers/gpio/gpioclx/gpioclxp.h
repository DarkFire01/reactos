/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private declarations
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/**
 * @file
 * @brief
 * GpioClx internals.
 *
 * Structured the same way as the
 * SpbCx reconstruction in this tree, which is the other KMDF class extension
 * ReactOS builds: a class library published under a device name, a per-client
 * globals block handed back through the bind, and one controller object per
 * client device.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <wdfcx.h>
#include <gpioclx.h>

#include <debug.h>

#define GCX_POOL_TAG 'xCpG'   /* "GpCx" */

/*
 * The signature guards the walk back from the pointer a client holds to our
 * own block. A client bound against a different class extension would
 * otherwise have us reading its structures as ours.
 */
#define GCX_CLIENT_GLOBALS_SIGNATURE 'XoiG'   /* "GioX" */
#define GCX_CLIENT_GLOBALS_OFFSET    16

typedef struct _GCX_CLIENT_GLOBALS
{
    ULONG Signature;
    WDF_CLASS_VERSION Version;
    UCHAR Reserved[GCX_CLIENT_GLOBALS_OFFSET - sizeof(ULONG)];

    /* The client's registration, copied at GPIO_CLX_RegisterClient */
    GPIO_CLIENT_REGISTRATION_PACKET Registration;
    BOOLEAN Registered;

    /* What the client actually sees; its address is the value we hand back */
    WDF_DRIVER_GLOBALS ClientGlobals;
} GCX_CLIENT_GLOBALS, *PGCX_CLIENT_GLOBALS;

/*
 * How far the controller has got. The reference keeps this as an interlocked
 * word and moves it to 3 on entry to PrepareHardware and 5 once the banks are
 * up.
 */
typedef enum _GCX_CONTROLLER_STATE
{
    GcxControllerStopped = 0,
    GcxControllerPreparing = 3,
    GcxControllerReady = 5
} GCX_CONTROLLER_STATE;

/* The pin is armed, and the rest of its registration is meaningful */
#define GCX_PIN_INTERRUPT_ENABLED 0x0001

/**
 * @brief
 * One pin's interrupt registration.
 *
 * The reference keeps the same things in its pin entry and reads all three
 * back on the delivery path: the flags at +192, the target's context at +248
 * and the GSIV the pin was given at +256.
 */
typedef struct _GCX_PIN
{
    ULONG Flags;

    /* What the pin was armed for */
    KINTERRUPT_MODE InterruptMode;
    KINTERRUPT_POLARITY Polarity;

    /* The GSIV this pin was given, and what its target asked to be handed */
    ULONG Gsiv;
    PVOID TargetContext;
} GCX_PIN, *PGCX_PIN;

/**
 * @brief
 * One bank's interrupt state.
 *
 * A bank is the unit the hardware raises interrupts in, so the masks are kept
 * per bank, while what a pin was armed for is kept per pin.
 */
typedef struct _GCX_BANK
{
    BANK_ID BankId;

    /* Pins armed as interrupt sources */
    ULONG64 EnabledMask;

    /* Pins seen asserted and not yet delivered */
    ULONG64 ActiveMask;

    /* NumberOfPinsPerBank entries, allocated alongside the bank array */
    PGCX_PIN Pins;
} GCX_BANK, *PGCX_BANK;

/* The controller has not been given an interrupt of its own */
#define GCX_NO_PRIMARY_GSIV MAXULONG

/**
 * @brief
 * One GPIO controller, as a context on the client's WDFDEVICE.
 *
 * The client's own per-controller storage follows this block in the same
 * allocation, which is why the device is created with a ContextSizeOverride of
 * sizeof(GCX_CONTROLLER) + ControllerContextSize. The client context sits at a
 * fixed offset inside the controller object rather than in an allocation of its
 * own.
 */
typedef struct _GCX_CONTROLLER
{
    WDFDEVICE Device;

    /* The client that owns this controller */
    PGCX_CLIENT_GLOBALS Client;

    /* The client's callbacks, copied so a controller never chases the globals */
    GPIO_CLIENT_REGISTRATION_PACKET Registration;

    /* What the client told us about its silicon */
    CLIENT_CONTROLLER_BASIC_INFORMATION Information;

    /* How many banks the pin count works out to, and their state */
    ULONG BankCount;
    PGCX_BANK Banks;

    /* The controller's own interrupt line, when it has one */
    WDFINTERRUPT Interrupt;

    /*
     * What that line is, in the terms the HAL uses. A device on one of this
     * controller's pins runs its ISR from inside this interrupt, so this is
     * the answer to what IRQL such a device runs at.
     */
    ULONG PrimaryGsiv;
    KIRQL PrimaryIrql;
    KAFFINITY PrimaryAffinity;

    /* The descriptor pair WdfInterruptCreate needs, kept from
       PrepareHardware's resource lists - see GcxRecordPrimaryInterrupt. */
    PCM_PARTIAL_RESOURCE_DESCRIPTOR InterruptRaw;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR InterruptTranslated;

    /*
     * What the firmware calls this controller. A GpioInt somewhere else in the
     * namespace names its controller by exactly this string, so it is what ties
     * a pin described on another device back to this one.
     */
    UNICODE_STRING BiosName;
    LIST_ENTRY ControllerLink;

    volatile LONG State;
} GCX_CONTROLLER, *PGCX_CONTROLLER;

/**
 * @brief
 * The client's own storage, which follows the controller block.
 *
 * @param[in] Controller
 * The controller object.
 *
 * @return
 * What every client callback is handed as its Context.
 */
FORCEINLINE PVOID
GcxClientContext(
    _In_ PGCX_CONTROLLER Controller)
{
    return (PVOID)((PUCHAR)Controller + sizeof(GCX_CONTROLLER));
}

/**
 * @brief
 * One pin's interrupt registration.
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The bank the pin is in.
 *
 * @param[in] PinNumber
 * The pin, as an offset within that bank.
 *
 * @return
 * Its entry, or NULL if the pin is not one this controller has.
 */
FORCEINLINE PGCX_PIN
GcxPin(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    if (Controller->Banks == NULL ||
        BankId >= Controller->BankCount ||
        PinNumber >= Controller->Information.NumberOfPinsPerBank)
        return NULL;

    return &Controller->Banks[BankId].Pins[PinNumber];
}

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GCX_CONTROLLER, GcxGetController);

PGCX_CLIENT_GLOBALS
GcxGlobalsFromClient(
    _In_ PGPIO_DRIVER_GLOBALS ClientGlobals);

/* The four entry points published to a client, in table order */
NTSTATUS NTAPI GcxRegisterClient(
    _In_ PGPIO_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDRIVER Driver,
    _In_ PGPIO_CLIENT_REGISTRATION_PACKET RegistrationPacket,
    _In_opt_ PCUNICODE_STRING RegistryPath);

NTSTATUS NTAPI GcxUnregisterClient(
    _In_ PGPIO_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDRIVER Driver);

NTSTATUS NTAPI GcxProcessAddDevicePreDeviceCreate(
    _In_ PGPIO_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDRIVER Driver,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _Out_ PWDF_OBJECT_ATTRIBUTES DeviceAttributes);

NTSTATUS NTAPI GcxProcessAddDevicePostDeviceCreate(
    _In_ PGPIO_DRIVER_GLOBALS DriverGlobals,
    _In_ WDFDRIVER Driver,
    _In_ WDFDEVICE Device);

/* controller.c */
NTSTATUS GcxQueryControllerInformation(_In_ PGCX_CONTROLLER Controller);

EVT_WDF_DEVICE_PREPARE_HARDWARE GcxEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE GcxEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY GcxEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT GcxEvtDeviceD0Exit;

/* pins.c */
BOOLEAN GcxPinToBank(_In_ PGCX_CONTROLLER Controller, _In_ ULONG Pin,
                     _Out_ PBANK_ID BankId, _Out_ PPIN_NUMBER Offset);
NTSTATUS GcxConnectIoPins(_In_ PGCX_CONTROLLER Controller, _In_ BANK_ID BankId,
                          _In_ PPIN_NUMBER PinNumberTable, _In_ USHORT PinCount,
                          _In_ UCHAR ConnectMode, _In_ UCHAR PullConfiguration);
NTSTATUS GcxDisconnectIoPins(_In_ PGCX_CONTROLLER Controller, _In_ BANK_ID BankId,
                             _In_ PPIN_NUMBER PinNumberTable, _In_ USHORT PinCount);
NTSTATUS GcxReadGpioPins(_In_ PGCX_CONTROLLER Controller, _In_ BANK_ID BankId,
                         _In_ PPIN_NUMBER PinNumberTable, _In_ ULONG PinCount,
                         _Out_ PULONG64 Buffer);
NTSTATUS GcxWriteGpioPins(_In_ PGCX_CONTROLLER Controller, _In_ BANK_ID BankId,
                          _In_ PPIN_NUMBER PinNumberTable, _In_ ULONG PinCount,
                          _In_ PULONG64 Buffer);

/* interrupt.c */
NTSTATUS GcxEnableInterrupt(_In_ PGCX_CONTROLLER Controller, _In_ BANK_ID BankId,
                            _In_ PIN_NUMBER PinNumber, _In_ KINTERRUPT_MODE InterruptMode,
                            _In_ KINTERRUPT_POLARITY Polarity, _In_ UCHAR PullConfiguration);
NTSTATUS GcxDisableInterrupt(_In_ PGCX_CONTROLLER Controller, _In_ BANK_ID BankId,
                             _In_ PIN_NUMBER PinNumber);
NTSTATUS GcxMaskInterrupts(_In_ PGCX_CONTROLLER Controller, _In_ BANK_ID BankId,
                           _In_ ULONG64 PinMask, _Out_ PULONG64 FailedMask);
NTSTATUS GcxUnmaskInterrupt(_In_ PGCX_CONTROLLER Controller, _In_ BANK_ID BankId,
                            _In_ PIN_NUMBER PinNumber);
NTSTATUS GcxQueryActiveInterrupts(_In_ PGCX_CONTROLLER Controller, _In_ BANK_ID BankId,
                                  _In_ ULONG64 EnabledMask, _Out_ PULONG64 ActiveMask);
NTSTATUS GcxClearActiveInterrupts(_In_ PGCX_CONTROLLER Controller, _In_ BANK_ID BankId,
                                  _In_ ULONG64 ClearMask);

/* banks.c */
NTSTATUS GcxInitializeBanks(_In_ PGCX_CONTROLLER Controller);
VOID GcxInvokeTargetIsr(_In_ PGCX_CONTROLLER Controller, _In_ PGCX_BANK Bank,
                        _In_ ULONG64 ActiveMask);

/* secint.c */
VOID GcxAddController(_In_ PGCX_CONTROLLER Controller);
VOID GcxRemoveController(_In_ PGCX_CONTROLLER Controller);
PGCX_CONTROLLER GcxFindControllerByBiosName(_In_ PCUNICODE_STRING BiosName);
NTSTATUS GcxRegisterSecondaryInterruptController(_In_ PDRIVER_OBJECT DriverObject);
VOID GcxUnregisterSecondaryInterruptController(VOID);
NTSTATUS GcxAddVirqMapping(_In_ PGCX_CONTROLLER Controller, _In_ ULONG Gsiv,
                           _In_ BANK_ID BankId, _In_ PIN_NUMBER PinNumber);
VOID GcxRemoveVirqMappings(_In_ PGCX_CONTROLLER Controller);
BOOLEAN GcxDeliverPinInterrupt(_In_ PGCX_PIN Pin);

/* reshub.c */
NTSTATUS GcxQueryDeviceBiosName(_In_ WDFDEVICE Device, _Out_ PUNICODE_STRING BiosName);
NTSTATUS GcxResolveGsivToPin(_In_ ULONG Gsiv, _Outptr_ PGCX_CONTROLLER *Controller,
                             _Out_ PBANK_ID BankId, _Out_ PPIN_NUMBER PinNumber);
VOID GcxCloseResourceHub(VOID);
NTSTATUS GcxCreateControllerDevice(_In_ PGCX_CONTROLLER Controller);
EVT_WDF_INTERRUPT_ISR GcxEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC GcxEvtInterruptDpc;
