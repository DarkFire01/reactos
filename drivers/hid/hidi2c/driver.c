/*
 * PROJECT:     ReactOS HID Stack
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HID over I2C driver entry, PnP and power
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "hidi2c.h"
#include "hidi2c_msg.h"
#include <ntstrsafe.h>
#include <initguid.h>
#include <devpkey.h>

#define NDEBUG
#include <debug.h>

/* Resource type of the message table compiled from hidi2c_msg.mc */
#define HIDI2C_MESSAGE_TABLE 11

/* One message table entry, laid out as MESSAGE_RESOURCE_ENTRY */
typedef struct _HIDI2C_MESSAGE_ENTRY
{
    USHORT Length;
    USHORT Flags;
    UCHAR Text[ANYSIZE_ARRAY];
} HIDI2C_MESSAGE_ENTRY, *PHIDI2C_MESSAGE_ENTRY;

/* Entry text is UTF-16 */
#define HIDI2C_MESSAGE_UNICODE 0x0001

NTSYSAPI
NTSTATUS
NTAPI
RtlFindMessage(
    _In_ PVOID BaseAddress,
    _In_ ULONG Type,
    _In_ ULONG Language,
    _In_ ULONG MessageId,
    _Out_ PHIDI2C_MESSAGE_ENTRY *MessageEntry);

/* Indirect string prefix naming this image, followed by ",#<id>;" */
#define HIDI2C_PROBLEM_SOURCE L"@System32\\drivers\\hidi2c.sys"

/* Room for the prefix, the separators, a signed decimal id and the terminator */
#define HIDI2C_PROBLEM_EXTRA_BYTES 86

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD HidI2cEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE HidI2cEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE HidI2cEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY HidI2cEvtD0Entry;
static EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED HidI2cEvtD0EntryPostInterruptsEnabled;
static EVT_WDF_DEVICE_D0_EXIT HidI2cEvtD0Exit;
static EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND HidI2cEvtSelfManagedIoSuspend;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#endif

/**
 * @brief
 * Publishes why the device failed as its driver problem description, using the
 * message table entry for Problem.
 */
_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
HidI2cReportProblem(
    _In_ PHIDI2C_DEVICE Context,
    _In_ NTSTATUS Problem)
{
    PHIDI2C_MESSAGE_ENTRY Entry;
    PDRIVER_OBJECT DriverObject;
    PCWSTR Text;
    SIZE_T MaxChars;
    SIZE_T TextChars;
    SIZE_T Size;
    size_t Length;
    PWSTR Description;
    NTSTATUS Status;

    PAGED_CODE();

    DPRINT1("hidi2c: reporting problem 0x%08lx\n", Problem);

    DriverObject = WdfDriverWdmGetDriverObject(WdfGetDriver());
    Status = RtlFindMessage(DriverObject->DriverStart, HIDI2C_MESSAGE_TABLE, 0, Problem, &Entry);
    if (!NT_SUCCESS(Status) ||
        !(Entry->Flags & HIDI2C_MESSAGE_UNICODE) ||
        Entry->Length < FIELD_OFFSET(HIDI2C_MESSAGE_ENTRY, Text))
    {
        return;
    }

    /* The text has to end inside the entry */
    Text = (PCWSTR)Entry->Text;
    MaxChars = (Entry->Length - FIELD_OFFSET(HIDI2C_MESSAGE_ENTRY, Text)) / sizeof(WCHAR);
    TextChars = 0;
    while (TextChars < MaxChars && Text[TextChars] != UNICODE_NULL)
        TextChars++;

    if (TextChars == MaxChars)
        return;

    Size = TextChars * sizeof(WCHAR) + HIDI2C_PROBLEM_EXTRA_BYTES;
    Description = ExAllocatePoolZero(NonPagedPoolNx, Size, HIDI2C_POOL_TAG);
    if (Description == NULL)
        return;

    Status = RtlStringCbPrintfW(Description,
                                Size,
                                L"%s,#%d;%s",
                                HIDI2C_PROBLEM_SOURCE,
                                Problem,
                                Text);
    if (NT_SUCCESS(Status))
        Status = RtlStringCbLengthW(Description, Size, &Length);

    if (NT_SUCCESS(Status))
    {
        Status = IoSetDevicePropertyData(WdfDeviceWdmGetPhysicalDevice(Context->Device),
                                         &DEVPKEY_Device_DriverProblemDesc,
                                         LOCALE_NEUTRAL,
                                         0,
                                         DEVPROP_TYPE_STRING_INDIRECT,
                                         (ULONG)(Length + sizeof(WCHAR)),
                                         Description);
        if (!NT_SUCCESS(Status))
            DPRINT1("hidi2c: cannot set the problem description 0x%08lx\n", Status);
    }

    ExFreePoolWithTag(Description, HIDI2C_POOL_TAG);
}

/**
 * @brief
 * Needs both the I2C connection and the interrupt; opens the connection and asks ACPI
 * where the HID descriptor lives.
 */
static
NTSTATUS
NTAPI
HidI2cEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PHIDI2C_DEVICE Context = HidI2cGetDevice(Device);
    NTSTATUS Status;

    PAGED_CODE();

    Status = HidI2cSpbInitialize(Context, ResourcesRaw, ResourcesTranslated);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = HidI2cQueryDescriptorRegister(Context);
    if (!NT_SUCCESS(Status))
        HidI2cReportProblem(Context, HIDI2C_PROBLEM_ACPI);

    return Status;
}

static
NTSTATUS
NTAPI
HidI2cEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    PAGED_CODE();

    HidI2cSpbRelease(HidI2cGetDevice(Device));
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
HidI2cEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PHIDI2C_DEVICE Context = HidI2cGetDevice(Device);
    NTSTATUS Status;

    Status = HidI2cInitialize(Context, PreviousState);
    if (NT_SUCCESS(Status))
        HidI2cCompleteIdleRequest(Context);

    return Status;
}

/* The reset answer is an interrupt, so the reset waits until they are connected */
static
NTSTATUS
NTAPI
HidI2cEvtD0EntryPostInterruptsEnabled(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PHIDI2C_DEVICE Context = HidI2cGetDevice(Device);
    NTSTATUS Status;

    if (PreviousState != WdfPowerDeviceD3Final)
        return STATUS_SUCCESS;

    Status = HidI2cReset(Context);
    if (!NT_SUCCESS(Status))
        HidI2cReportProblem(Context, HIDI2C_PROBLEM_RESET);

    return Status;
}

static
NTSTATUS
NTAPI
HidI2cEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    return HidI2cShutdown(HidI2cGetDevice(Device), TargetState);
}

static
NTSTATUS
NTAPI
HidI2cEvtSelfManagedIoSuspend(
    _In_ WDFDEVICE Device)
{
    PHIDI2C_DEVICE Context = HidI2cGetDevice(Device);

    if (Context->ResetTimer != NULL)
        WdfTimerStop(Context->ResetTimer, TRUE);

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Creates a manual queue that keeps handing requests back while the device is below D0.
 */
static
NTSTATUS
NTAPI
HidI2cCreateManualQueue(
    _In_ PHIDI2C_DEVICE Context,
    _Out_ WDFQUEUE *Queue)
{
    WDF_IO_QUEUE_CONFIG QueueConfig;

    WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchManual);
    QueueConfig.PowerManaged = WdfFalse;

    return WdfIoQueueCreate(Context->Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, Queue);
}

/**
 * @brief
 * The default queue for hidclass requests, and the manual queues requests wait in.
 */
static
NTSTATUS
NTAPI
HidI2cCreateQueues(
    _Inout_ PHIDI2C_DEVICE Context)
{
    WDF_IO_QUEUE_CONFIG QueueConfig;
    NTSTATUS Status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.PowerManaged = WdfTrue;
    QueueConfig.EvtIoInternalDeviceControl = HidI2cEvtInternalDeviceControl;
    QueueConfig.EvtIoStop = HidI2cEvtIoStop;

    Status = WdfIoQueueCreate(Context->Device,
                              &QueueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              &Context->DefaultQueue);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = HidI2cCreateManualQueue(Context, &Context->ReadQueue);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = HidI2cCreateManualQueue(Context, &Context->CompletionQueue);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = HidI2cCreateManualQueue(Context, &Context->IdleQueue);
    if (!NT_SUCCESS(Status))
        return Status;

    return HidI2cCreateManualQueue(Context, &Context->ResetNotifyQueue);
}

static
NTSTATUS
NTAPI
HidI2cEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpCallbacks;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_TIMER_CONFIG TimerConfig;
    WDF_DEVICE_STATE DeviceState;
    PHIDI2C_DEVICE Context;
    WDFDEVICE Device;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    /* hidclass above owns power policy for the stack */
    WdfDeviceInitSetPowerPolicyOwnership(DeviceInit, FALSE);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpCallbacks);
    PnpCallbacks.EvtDevicePrepareHardware = HidI2cEvtPrepareHardware;
    PnpCallbacks.EvtDeviceReleaseHardware = HidI2cEvtReleaseHardware;
    PnpCallbacks.EvtDeviceD0Entry = HidI2cEvtD0Entry;
    PnpCallbacks.EvtDeviceD0EntryPostInterruptsEnabled = HidI2cEvtD0EntryPostInterruptsEnabled;
    PnpCallbacks.EvtDeviceD0Exit = HidI2cEvtD0Exit;
    PnpCallbacks.EvtDeviceSelfManagedIoSuspend = HidI2cEvtSelfManagedIoSuspend;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, HIDI2C_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("hidi2c: WdfDeviceCreate failed 0x%08lx\n", Status);
        return Status;
    }

    Context = HidI2cGetDevice(Device);
    Context->Device = Device;

    /* Passive, since giving up on the reset writes the registry */
    WDF_TIMER_CONFIG_INIT(&TimerConfig, HidI2cEvtResetTimer);
    TimerConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Attributes.ExecutionLevel = WdfExecutionLevelPassive;

    Status = WdfTimerCreate(&TimerConfig, &Attributes, &Context->ResetTimer);
    if (!NT_SUCCESS(Status))
        return Status;

    WDF_DEVICE_STATE_INIT(&DeviceState);
    DeviceState.NotDisableable = WdfFalse;
    WdfDeviceSetDeviceState(Device, &DeviceState);

    return HidI2cCreateQueues(Context);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT(&Config, HidI2cEvtDeviceAdd);
    Config.DriverPoolTag = HIDI2C_POOL_TAG;

    return WdfDriverCreate(DriverObject,
                           RegistryPath,
                           WDF_NO_OBJECT_ATTRIBUTES,
                           &Config,
                           WDF_NO_HANDLE);
}
