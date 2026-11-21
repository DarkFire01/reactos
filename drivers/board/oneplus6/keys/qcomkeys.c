/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PMIC power and volume keys as a keyboard class port
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <kbdmou.h>
#include <ntddkbd.h>
#include <qcom/spmiif.h>
#include <wmistr.h>
#include <wmiguid.h>
#include <wmidata.h>
#include <dmilib.h>

#define QCDBG_COMPONENT "qcomkeys"
#include <qcom/qcomdbg.h>

#define QCKEYS_POOL_TAG     'yKcQ'

static const GUID QcKeysSmbiosGuid = SMBIOS_DATA_GUID;

/*
 * One physical key. The power key reads back set while pressed, the volume
 * keys are PMIC GPIOs wired active low so they read clear while pressed.
 */
typedef struct _QCKEYS_BUTTON
{
    UCHAR Sid;
    USHORT Address;
    UCHAR Mask;
    BOOLEAN ActiveLow;
    USHORT MakeCode;
    USHORT ExtraFlags;
    BOOLEAN Down;
} QCKEYS_BUTTON, *PQCKEYS_BUTTON;

/* OnePlus 6: power on the PON of the PM8998, volume on its GPIOs 5 and 6 */
static QCKEYS_BUTTON QcKeysButtons[] =
{
    { 0, 0x0810, 0x01, FALSE, 0x1C, 0,      FALSE },  /* power down enter */
    { 0, 0xC510, 0x01, TRUE,  0x48, KEY_E0, FALSE },  /* volume up */
    { 0, 0xC410, 0x01, TRUE,  0x50, KEY_E0, FALSE },  /* volume down */
};

typedef struct _QCKEYS_DEVICE
{
    PDEVICE_OBJECT DeviceObject;
    CONNECT_DATA ClassConnect;
    KEYBOARD_ATTRIBUTES Attributes;
    PKTHREAD Thread;
    KEVENT StopEvent;
} QCKEYS_DEVICE, *PQCKEYS_DEVICE;

static UNICODE_STRING QcKeysDeviceName = RTL_CONSTANT_STRING(L"\\Device\\KeyboardPort0");

static
BOOLEAN
QcKeysIsSupportedBoard(VOID)
{
    PVOID DataBlock;
    PWNODE_ALL_DATA AllData;
    PCHAR Strings[ID_STRINGS_MAX] = { 0 };
    BOOLEAN Supported = FALSE;
    ULONG BufferSize = 0;
    NTSTATUS Status;

    Status = IoWMIOpenBlock((LPGUID)&QcKeysSmbiosGuid, WMIGUID_QUERY, &DataBlock);
    if (!NT_SUCCESS(Status))
        return FALSE;

    Status = IoWMIQueryAllData(DataBlock, &BufferSize, NULL);
    if (Status != STATUS_BUFFER_TOO_SMALL && !NT_SUCCESS(Status))
    {
        ObDereferenceObject(DataBlock);
        return FALSE;
    }

    AllData = ExAllocatePoolWithTag(PagedPool, BufferSize, QCKEYS_POOL_TAG);
    if (!AllData)
    {
        ObDereferenceObject(DataBlock);
        return FALSE;
    }

    Status = IoWMIQueryAllData(DataBlock, &BufferSize, AllData);
    if (NT_SUCCESS(Status))
    {
        ParseSMBiosTables(AllData + 1, AllData->WnodeHeader.BufferSize, Strings);
        if (Strings[SYS_VENDOR] && !strcmp(Strings[SYS_VENDOR], "OnePlus") &&
            Strings[SYS_PRODUCT] &&
            (!strcmp(Strings[SYS_PRODUCT], "6") || !strcmp(Strings[SYS_PRODUCT], "6T")))
        {
            Supported = TRUE;
        }
    }

    ExFreePoolWithTag(AllData, QCKEYS_POOL_TAG);
    ObDereferenceObject(DataBlock);
    return Supported;
}

static
VOID
QcKeysReport(
    _In_ PQCKEYS_DEVICE Device,
    _In_ PQCKEYS_BUTTON Button,
    _In_ BOOLEAN Pressed)
{
    KEYBOARD_INPUT_DATA Input;
    PSERVICE_CALLBACK_ROUTINE Callback;
    ULONG Consumed = 0;
    KIRQL OldIrql;

    Callback = (PSERVICE_CALLBACK_ROUTINE)Device->ClassConnect.ClassService;
    if (!Callback)
        return;

    RtlZeroMemory(&Input, sizeof(Input));
    Input.MakeCode = Button->MakeCode;
    Input.Flags = Button->ExtraFlags | (Pressed ? KEY_MAKE : KEY_BREAK);

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Callback(Device->ClassConnect.ClassDeviceObject, &Input, &Input + 1, &Consumed);
    KeLowerIrql(OldIrql);
}

static
VOID
NTAPI
QcKeysPollThread(
    _In_ PVOID Context)
{
    PQCKEYS_DEVICE Device = Context;
    LARGE_INTEGER Period;

    Period.QuadPart = -10000LL * 30;
    while (KeWaitForSingleObject(&Device->StopEvent, Executive, KernelMode, FALSE, &Period) == STATUS_TIMEOUT)
    {
        ULONG Index;

        for (Index = 0; Index < RTL_NUMBER_OF(QcKeysButtons); Index++)
        {
            PQCKEYS_BUTTON Button = &QcKeysButtons[Index];
            UCHAR Value;
            BOOLEAN Level;
            BOOLEAN Pressed;

            if (!NT_SUCCESS(QcSpmiRead(Button->Sid, Button->Address, &Value, sizeof(Value))))
                continue;

            Level = (Value & Button->Mask) != 0;
            Pressed = Button->ActiveLow ? !Level : Level;

            if (Pressed != Button->Down)
            {
                Button->Down = Pressed;
                QcKeysReport(Device, Button, Pressed);
            }
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
NTSTATUS
NTAPI
QcKeysCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
QcKeysInternalDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PQCKEYS_DEVICE Device = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    switch (Stack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_INTERNAL_KEYBOARD_CONNECT:
            if (Device->ClassConnect.ClassService)
            {
                Status = STATUS_SHARING_VIOLATION;
                break;
            }
            Device->ClassConnect = *(PCONNECT_DATA)Stack->Parameters.DeviceIoControl.Type3InputBuffer;
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_INTERNAL_KEYBOARD_DISCONNECT:
            Device->ClassConnect.ClassService = NULL;
            Device->ClassConnect.ClassDeviceObject = NULL;
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_KEYBOARD_QUERY_ATTRIBUTES:
            if (Stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(KEYBOARD_ATTRIBUTES))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            *(PKEYBOARD_ATTRIBUTES)Irp->AssociatedIrp.SystemBuffer = Device->Attributes;
            Irp->IoStatus.Information = sizeof(KEYBOARD_ATTRIBUTES);
            Status = STATUS_SUCCESS;
            break;

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static
VOID
QcKeysRegisterDeviceMap(VOID)
{
    static UNICODE_STRING KeyName =
        RTL_CONSTANT_STRING(L"\\Registry\\Machine\\HARDWARE\\DEVICEMAP\\KeyboardPort");
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Key;
    NTSTATUS Status;

    InitializeObjectAttributes(&Attributes, &KeyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateKey(&Key, KEY_SET_VALUE, &Attributes, 0, NULL, REG_OPTION_VOLATILE, NULL);
    if (!NT_SUCCESS(Status))
        return;

    /* kbdclass reads the value name as the port device to connect to */
    ZwSetValueKey(Key, &QcKeysDeviceName, 0, REG_SZ, L"qcomkeys", sizeof(L"qcomkeys"));
    ZwClose(Key);
}

static
VOID
NTAPI
QcKeysUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PDEVICE_OBJECT DeviceObject = DriverObject->DeviceObject;
    PQCKEYS_DEVICE Device;

    if (!DeviceObject)
        return;

    Device = DeviceObject->DeviceExtension;
    if (Device->Thread)
    {
        KeSetEvent(&Device->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(Device->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Device->Thread);
    }

    IoDeleteDevice(DeviceObject);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    PDEVICE_OBJECT DeviceObject;
    PQCKEYS_DEVICE Device;
    HANDLE Thread;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RegistryPath);

    if (!QcKeysIsSupportedBoard())
        return STATUS_NO_SUCH_DEVICE;

    Status = IoCreateDevice(DriverObject, sizeof(QCKEYS_DEVICE), &QcKeysDeviceName,
                            FILE_DEVICE_KEYBOARD, 0, FALSE, &DeviceObject);
    if (!NT_SUCCESS(Status))
        return Status;

    DeviceObject->Flags |= DO_BUFFERED_IO;

    Device = DeviceObject->DeviceExtension;
    RtlZeroMemory(Device, sizeof(*Device));
    Device->DeviceObject = DeviceObject;
    KeInitializeEvent(&Device->StopEvent, NotificationEvent, FALSE);

    Device->Attributes.KeyboardIdentifier.Type = 4;
    Device->Attributes.KeyboardMode = 1;
    Device->Attributes.NumberOfFunctionKeys = 12;
    Device->Attributes.NumberOfIndicators = 3;
    Device->Attributes.NumberOfKeysTotal = 101;
    Device->Attributes.InputDataQueueLength = 16;

    DriverObject->MajorFunction[IRP_MJ_CREATE] = QcKeysCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = QcKeysCreateClose;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = QcKeysInternalDeviceControl;
    DriverObject->DriverUnload = QcKeysUnload;

    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, QcKeysPollThread, Device);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(DeviceObject);
        return Status;
    }
    ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&Device->Thread, NULL);
    ZwClose(Thread);

    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    QC_LOG("power and volume keys ready");
    QcKeysRegisterDeviceMap();

    return STATUS_SUCCESS;
}
