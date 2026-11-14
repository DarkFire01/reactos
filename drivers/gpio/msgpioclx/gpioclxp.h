/*
 * PROJECT:     ReactOS GPIO framework extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private declarations
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <gpioclx.h>

#include <debug.h>

#define GCX_POOL_TAG 'xCpG'

/* Stamped on every controller so a context handed back by a client can be checked */
#define GCX_CONTROLLER_SIGNATURE 'rCpG'

/* Version of the class library published to wdfldr */
#define GCX_CLASS_MAJOR_VERSION 1
#define GCX_CLASS_MINOR_VERSION 0

/**
 * @brief
 * One registered controller driver, kept as a context on its WDFDRIVER.
 */
typedef struct _GCX_CLIENT
{
    BOOLEAN Registered;
    GPIO_CLIENT_REGISTRATION_PACKET Registration;
} GCX_CLIENT, *PGCX_CLIENT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GCX_CLIENT, GcxGetClient);

typedef enum _GCX_CONTROLLER_STATE
{
    GcxControllerStopped = 0,
    GcxControllerPreparing,
    GcxControllerFailed,
    GcxControllerReady
} GCX_CONTROLLER_STATE;

/* The pin is armed and the rest of its entry is meaningful */
#define GCX_PIN_INTERRUPT_ENABLED 0x0001

typedef struct _GCX_PIN
{
    ULONG Flags;
    KINTERRUPT_MODE InterruptMode;
    KINTERRUPT_POLARITY Polarity;

    /* GSIV the pin was connected as, and the HAL token to quote back on delivery */
    ULONG Gsiv;
    PVOID TargetContext;
} GCX_PIN, *PGCX_PIN;

typedef struct _GCX_BANK
{
    BANK_ID BankId;

    /* Pins armed as interrupt sources */
    ULONG64 EnabledMask;

    /* Locks behind GPIO_CLX_AcquireInterruptLock when there is no interrupt object */
    WDFSPINLOCK SpinLock;
    WDFWAITLOCK WaitLock;
    PKTHREAD LockOwner;

    /* NumberOfPinsPerBank entries, in the same allocation as the bank array */
    PGCX_PIN Pins;
} GCX_BANK, *PGCX_BANK;

/* The controller has no interrupt line of its own */
#define GCX_NO_PRIMARY_GSIV MAXULONG

/**
 * @brief
 * One GPIO controller, as a context on the client's WDFDEVICE.
 *
 * The client's own controller context follows this block, at
 * GCX_CLIENT_CONTEXT_OFFSET, in the same context allocation.
 */
typedef struct _GCX_CONTROLLER
{
    ULONG Signature;
    WDFDEVICE Device;
    GPIO_CLIENT_REGISTRATION_PACKET Registration;
    CLIENT_CONTROLLER_BASIC_INFORMATION Information;

    USHORT BankCount;
    PGCX_BANK Banks;
    WDFMEMORY BankMemory;

    WDFINTERRUPT Interrupt;
    PKTHREAD InterruptLockOwner;
    ULONG InterruptLockDepth;

    /* The line the controller sits on, as the HAL describes it */
    ULONG PrimaryGsiv;
    KIRQL PrimaryIrql;
    KAFFINITY PrimaryAffinity;
    KINTERRUPT_MODE PrimaryMode;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR InterruptRaw;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR InterruptTranslated;

    /* ACPI path of the controller, which GpioInt descriptors name it by */
    UNICODE_STRING BiosName;
    LIST_ENTRY ControllerLink;

    volatile LONG State;
} GCX_CONTROLLER, *PGCX_CONTROLLER;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GCX_CONTROLLER, GcxGetController);

#define GCX_CLIENT_CONTEXT_OFFSET \
    ALIGN_UP_BY(sizeof(GCX_CONTROLLER), MEMORY_ALLOCATION_ALIGNMENT)

/**
 * @brief
 * The client's storage, which every client callback gets as its Context.
 */
FORCEINLINE
PVOID
GcxClientContext(
    _In_ PGCX_CONTROLLER Controller)
{
    return (PUCHAR)Controller + GCX_CLIENT_CONTEXT_OFFSET;
}

/**
 * @brief
 * One pin's interrupt entry, or NULL if the controller has no such pin.
 */
FORCEINLINE
PGCX_PIN
GcxPin(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber)
{
    if (Controller->Banks == NULL || BankId >= Controller->BankCount)
        return NULL;

    if (PinNumber >= Controller->Information.NumberOfPinsPerBank)
        return NULL;

    return &Controller->Banks[BankId].Pins[PinNumber];
}

/* HAL secondary interrupt controller interface */

typedef struct _GCX_PRIMARY_INTERRUPT
{
    USHORT Size;
    USHORT Version;
    ULONG PrimaryGsiv;
    KIRQL PrimaryIrql;
    GROUP_AFFINITY PrimaryAffinity;
    KINTERRUPT_MODE InterruptMode;
    KINTERRUPT_POLARITY Polarity;
} GCX_PRIMARY_INTERRUPT, *PGCX_PRIMARY_INTERRUPT;

typedef
NTSTATUS
(NTAPI *PGCX_SECONDARY_ENABLE)(
    _In_ PVOID Context,
    _In_ ULONG Gsiv,
    _In_ KINTERRUPT_MODE Mode,
    _In_ KINTERRUPT_POLARITY Polarity,
    _In_ PVOID ControllerContext);

typedef
NTSTATUS
(NTAPI *PGCX_SECONDARY_DISABLE)(
    _In_ PVOID Context,
    _In_ ULONG Gsiv);

typedef
NTSTATUS
(NTAPI *PGCX_SECONDARY_MASK)(
    _In_ PVOID Context,
    _In_ ULONG Flags,
    _In_ ULONG Gsiv);

typedef
NTSTATUS
(NTAPI *PGCX_SECONDARY_QUERY_PRIMARY)(
    _In_ PVOID Context,
    _In_ ULONG Gsiv,
    _Out_ PGCX_PRIMARY_INTERRUPT PrimaryInformation);

#define GCX_SECONDARY_PROVIDER_VERSION 1

/* Registered with HalSetSystemInformation(HalRegisterSecondaryInterruptInterface) */
typedef struct _GCX_SECONDARY_PROVIDER
{
    USHORT Size;
    USHORT Version;
    PVOID Context;
    ULONG GsivBase;
    USHORT GsivSize;
    PDRIVER_OBJECT DriverObject;
    PGCX_SECONDARY_ENABLE EnableInterrupt;
    PGCX_SECONDARY_DISABLE DisableInterrupt;
    PGCX_SECONDARY_MASK MaskInterrupt;
    PGCX_SECONDARY_MASK UnmaskInterrupt;
    PGCX_SECONDARY_QUERY_PRIMARY QueryPrimaryInterrupt;
    PVOID RequestInterrupt;
    PVOID QueryLineInformation;
} GCX_SECONDARY_PROVIDER, *PGCX_SECONDARY_PROVIDER;

typedef
BOOLEAN
(NTAPI *PGCX_HAL_INVOKE_ISR)(
    _In_ ULONG Gsiv,
    _In_ PVOID ControllerContext);

typedef
NTSTATUS
(NTAPI *PGCX_HAL_UNREGISTER)(
    _In_ ULONG GsivBase,
    _In_ ULONG GsivSize,
    _In_ PDRIVER_OBJECT DriverObject);

/* Returned by HalQuerySystemInformation(HalSecondaryInterruptInformation) */
typedef struct _GCX_HAL_SECONDARY_INFO
{
    USHORT Version;
    ULONG GsivRangeStart;
    ULONG GsivRangeSize;
    PVOID MaskInterrupt;
    PVOID UnmaskInterrupt;
    PGCX_HAL_INVOKE_ISR InvokeIsrForGsiv;
    PGCX_HAL_UNREGISTER UnregisterInterface;
    PVOID RequestInterrupt;
} GCX_HAL_SECONDARY_INFO, *PGCX_HAL_SECONDARY_INFO;

/* client.c */
GPIO_CLX_REGISTER_CLIENT GcxRegisterClient;
GPIO_CLX_UNREGISTER_CLIENT GcxUnregisterClient;
GPIO_CLX_PROCESS_ADD_DEVICE_PRE_DEVICE_CREATE GcxProcessAddDevicePreDeviceCreate;
GPIO_CLX_PROCESS_ADD_DEVICE_POST_DEVICE_CREATE GcxProcessAddDevicePostDeviceCreate;
GPIO_CLX_ACQUIRE_INTERRUPT_LOCK GcxAcquireInterruptLock;
GPIO_CLX_RELEASE_INTERRUPT_LOCK GcxReleaseInterruptLock;

/* controller.c */
EVT_WDF_DEVICE_PREPARE_HARDWARE GcxEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE GcxEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY GcxEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT GcxEvtDeviceD0Exit;

/* banks.c */
NTSTATUS
NTAPI
GcxInitializeBanks(
    _In_ PGCX_CONTROLLER Controller);

VOID
NTAPI
GcxFreeBanks(
    _In_ PGCX_CONTROLLER Controller);

EVT_WDF_INTERRUPT_ISR GcxEvtInterruptIsr;

/* pins.c */
BOOLEAN
NTAPI
GcxPinToBank(
    _In_ PGCX_CONTROLLER Controller,
    _In_ ULONG Pin,
    _Out_ PBANK_ID BankId,
    _Out_ PPIN_NUMBER Offset);

NTSTATUS
NTAPI
GcxConnectIoPins(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_reads_(PinCount) PPIN_NUMBER PinNumberTable,
    _In_ USHORT PinCount,
    _In_ GPIO_CONNECT_IO_PINS_MODE ConnectMode,
    _In_ UCHAR PullConfiguration);

NTSTATUS
NTAPI
GcxDisconnectIoPins(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_reads_(PinCount) PPIN_NUMBER PinNumberTable,
    _In_ USHORT PinCount,
    _In_ GPIO_CONNECT_IO_PINS_MODE DisconnectMode);

NTSTATUS
NTAPI
GcxReadGpioPins(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_reads_(PinCount) PPIN_NUMBER PinNumberTable,
    _In_ ULONG PinCount,
    _Out_ PULONG64 Buffer);

NTSTATUS
NTAPI
GcxWriteGpioPins(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_reads_(PinCount) PPIN_NUMBER PinNumberTable,
    _In_ ULONG PinCount,
    _In_ PULONG64 Buffer);

/* interrupt.c */
NTSTATUS
NTAPI
GcxEnableInterrupt(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber,
    _In_ KINTERRUPT_MODE InterruptMode,
    _In_ KINTERRUPT_POLARITY Polarity,
    _In_ UCHAR PullConfiguration);

NTSTATUS
NTAPI
GcxDisableInterrupt(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber);

NTSTATUS
NTAPI
GcxMaskInterrupts(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ ULONG64 PinMask,
    _Out_ PULONG64 FailedMask);

NTSTATUS
NTAPI
GcxUnmaskInterrupt(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ PIN_NUMBER PinNumber);

NTSTATUS
NTAPI
GcxQueryActiveInterrupts(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ ULONG64 EnabledMask,
    _Out_ PULONG64 ActiveMask);

NTSTATUS
NTAPI
GcxClearActiveInterrupts(
    _In_ PGCX_CONTROLLER Controller,
    _In_ BANK_ID BankId,
    _In_ ULONG64 ClearMask);

/* secint.c */
VOID
NTAPI
GcxInitializeGlobals(VOID);

VOID
NTAPI
GcxAddController(
    _In_ PGCX_CONTROLLER Controller);

VOID
NTAPI
GcxRemoveController(
    _In_ PGCX_CONTROLLER Controller);

PGCX_CONTROLLER
NTAPI
GcxFindControllerByBiosName(
    _In_ PCUNICODE_STRING BiosName);

NTSTATUS
NTAPI
GcxRegisterSecondaryInterruptController(
    _In_ PDRIVER_OBJECT DriverObject);

VOID
NTAPI
GcxUnregisterSecondaryInterruptController(VOID);

VOID
NTAPI
GcxRemoveVirqMappings(
    _In_ PGCX_CONTROLLER Controller);

BOOLEAN
NTAPI
GcxDeliverPinInterrupt(
    _In_ PGCX_PIN Pin);

/* reshub.c */
NTSTATUS
NTAPI
GcxQueryDeviceBiosName(
    _In_ WDFDEVICE Device,
    _Out_ PUNICODE_STRING BiosName);

NTSTATUS
NTAPI
GcxResolveGsivToPin(
    _In_ ULONG Gsiv,
    _Outptr_ PGCX_CONTROLLER *Controller,
    _Out_ PBANK_ID BankId,
    _Out_ PPIN_NUMBER PinNumber);

VOID
NTAPI
GcxCloseResourceHub(VOID);
