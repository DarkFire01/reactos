/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Synaptics RMI4 touch controller as a mouse class pointer port
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcomtouch.h"
#include <wmistr.h>
#include <wmiguid.h>
#include <wmidata.h>
#include <dmilib.h>

static const GUID QcTouchSmbiosGuid = SMBIOS_DATA_GUID;

/*
 * Each board is told apart by its firmware SMBIOS identity. The OnePlus 6 and
 * 6T are the same SDM845 wiring: a Synaptics S3706 on the QUPv3 wrap1 S4 I2C
 * engine, the 1.8 V rail gated by one GPIO and the attention line on another.
 */
static const QCTOUCH_BOARD QcTouchBoards[] =
{
    {
        "OnePlus", "6",
        0x00A90000, 0x00100000, 0x5200C, 0x04300000, 0x184D4, 0x184D8, 400000,
        0x20, 0x03400000, 0xC00000,
        88, QCTOUCH_TLMM_EAST, 125, QCTOUCH_TLMM_EAST, 99, QCTOUCH_TLMM_NORTH,
        49, 50, QCTOUCH_TLMM_NORTH, 1, 200,
        "ldoa28", 3000000, 1664000, 8000, 7
    },
    {
        "OnePlus", "6T",
        0x00A90000, 0x00100000, 0x5200C, 0x04300000, 0x184D4, 0x184D8, 400000,
        0x20, 0x03400000, 0xC00000,
        88, QCTOUCH_TLMM_EAST, 125, QCTOUCH_TLMM_EAST, 99, QCTOUCH_TLMM_NORTH,
        49, 50, QCTOUCH_TLMM_NORTH, 1, 200,
        "ldoa28", 3000000, 1664000, 8000, 7
    },
};

static
PCQCTOUCH_BOARD
QcTouchDetectBoard(VOID)
{
    PVOID DataBlock;
    PWNODE_ALL_DATA AllData;
    PCHAR Strings[ID_STRINGS_MAX] = { 0 };
    PCQCTOUCH_BOARD Board = NULL;
    ULONG BufferSize = 0;
    ULONG Index;
    NTSTATUS Status;

    Status = IoWMIOpenBlock((LPGUID)&QcTouchSmbiosGuid, WMIGUID_QUERY, &DataBlock);
    if (!NT_SUCCESS(Status))
    {
        QC_LOG("cannot open the SMBIOS data block, %08lx", Status);
        return NULL;
    }

    Status = IoWMIQueryAllData(DataBlock, &BufferSize, NULL);
    if (Status != STATUS_BUFFER_TOO_SMALL && !NT_SUCCESS(Status))
    {
        ObDereferenceObject(DataBlock);
        return NULL;
    }

    AllData = ExAllocatePoolWithTag(PagedPool, BufferSize, QCTOUCH_POOL_TAG);
    if (!AllData)
    {
        ObDereferenceObject(DataBlock);
        return NULL;
    }

    Status = IoWMIQueryAllData(DataBlock, &BufferSize, AllData);
    if (NT_SUCCESS(Status))
    {
        ParseSMBiosTables(AllData + 1, AllData->WnodeHeader.BufferSize, Strings);

        QC_LOG("SMBIOS vendor '%s' product '%s'",
               Strings[SYS_VENDOR] ? Strings[SYS_VENDOR] : "?",
               Strings[SYS_PRODUCT] ? Strings[SYS_PRODUCT] : "?");

        for (Index = 0; Index < RTL_NUMBER_OF(QcTouchBoards); Index++)
        {
            if (Strings[SYS_VENDOR] && Strings[SYS_PRODUCT] &&
                !strcmp(Strings[SYS_VENDOR], QcTouchBoards[Index].Manufacturer) &&
                !strcmp(Strings[SYS_PRODUCT], QcTouchBoards[Index].Product))
            {
                Board = &QcTouchBoards[Index];
                break;
            }
        }
    }

    ExFreePoolWithTag(AllData, QCTOUCH_POOL_TAG);
    ObDereferenceObject(DataBlock);
    return Board;
}

static
VOID
QcTouchConfigurePin(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ ULONG Number,
    _In_ ULONG TileOffset,
    _In_ ULONG Function,
    _In_ QCOM_TLMM_PULL Pull,
    _In_ ULONG DriveMilliamps,
    _In_ QCOM_TLMM_DIRECTION Direction)
{
    QCOM_TLMM_PIN Pin;

    RtlZeroMemory(&Pin, sizeof(Pin));
    Pin.Number = Number;
    Pin.TileOffset = TileOffset;
    Pin.Function = Function;
    Pin.Pull = Pull;
    Pin.DriveMilliamps = DriveMilliamps;
    Pin.Direction = Direction;
    QcTlmmConfigurePin(Device->Tlmm, &Pin);
}

static
NTSTATUS
QcTouchBringUp(
    _In_ PQCTOUCH_DEVICE Device)
{
    QCOM_GENI_I2C_CONFIG Config;
    PHYSICAL_ADDRESS TlmmBase;
    LARGE_INTEGER Delay;
    PCQCTOUCH_BOARD Board = Device->Board;
    NTSTATUS Status;

    /* The 3.0 V core rail is an RPMh regulator, bring it up before anything else */
    if (Board->VddResource)
    {
        QCOM_RPMH_VRM_REQUEST Vdd;

        Vdd.Resource = Board->VddResource;
        Vdd.TargetMicrovolts = Board->VddMicrovolts;
        Vdd.MinMicrovolts = Board->VddMinMicrovolts;
        Vdd.StepMicrovolts = Board->VddStepMicrovolts;
        Vdd.Mode = Board->VddMode;

        Status = QcRpmhEnableRegulator(&Vdd);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* Pin control next, the rails power the chip the I2C engine then talks to */
    TlmmBase.QuadPart = Board->TlmmBase;
    Status = QcTlmmConnect(TlmmBase, Board->TlmmLength, &Device->Tlmm);
    if (!NT_SUCCESS(Status))
        return Status;

    QcTouchConfigurePin(Device, Board->PowerPin, Board->PowerTile, 0, TlmmPullNone, 2, TlmmOutputHigh);
    QcTouchConfigurePin(Device, Board->InterruptPin, Board->InterruptTile, 0, TlmmPullUp, 16, TlmmInput);
    QcTouchConfigurePin(Device, Board->ResetPin, Board->ResetTile, 0, TlmmPullUp, 16, TlmmInput);

    /* Hand the SDA and SCL lines to the QUP engine */
    QcTouchConfigurePin(Device, Board->SdaPin, Board->I2cTile, Board->I2cFunction, TlmmPullUp, 2, TlmmInput);
    QcTouchConfigurePin(Device, Board->SclPin, Board->I2cTile, Board->I2cFunction, TlmmPullUp, 2, TlmmInput);

    /* Let the rail settle before the first transfer */
    Delay.QuadPart = -10000LL * 10;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);

    RtlZeroMemory(&Config, sizeof(Config));
    Config.EngineBase.QuadPart = Board->EngineBase;
    Config.ClockControllerBase.QuadPart = Board->ClockBase;
    Config.ClockVoteOffset = Board->ClockVoteOffset;
    Config.ClockVoteMask = Board->ClockVoteMask;
    Config.EngineBranchOffset = Board->EngineBranchOffset;
    Config.EngineRcgOffset = Board->EngineRcgOffset;
    Config.BusSpeedHz = Board->BusSpeedHz;

    Status = QcGeniI2cOpen(&Config, &Device->I2c);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Unknown page after power on, force the first page select to go out */
    Device->Page = 0xFF;

    Status = RmiScanFunctions(Device);
    if (!NT_SUCCESS(Status))
        return Status;

    return RmiConfigure(Device);
}

static
VOID
QcTouchReport(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ ULONG X,
    _In_ ULONG Y,
    _In_ BOOLEAN Down)
{
    MOUSE_INPUT_DATA Input;
    PSERVICE_CALLBACK_ROUTINE Callback;
    ULONG Consumed = 0;
    KIRQL OldIrql;
    LONG ScaledX;
    LONG ScaledY;

    if (X >= Device->MaxX)
        X = Device->MaxX - 1;
    if (Y >= Device->MaxY)
        Y = Device->MaxY - 1;

    ScaledX = (LONG)(((ULONGLONG)X * 65535) / (Device->MaxX - 1));
    ScaledY = (LONG)(((ULONGLONG)Y * 65535) / (Device->MaxY - 1));

    /* Only a change in the contact or a move while down is worth a packet */
    if (Down == Device->FingerDown &&
        (!Down || (ScaledX == Device->LastX && ScaledY == Device->LastY)))
    {
        return;
    }

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Flags = MOUSE_MOVE_ABSOLUTE;
    Input.LastX = Down ? ScaledX : Device->LastX;
    Input.LastY = Down ? ScaledY : Device->LastY;

    if (Down && !Device->FingerDown)
        Input.ButtonFlags |= MOUSE_LEFT_BUTTON_DOWN;
    else if (!Down && Device->FingerDown)
        Input.ButtonFlags |= MOUSE_LEFT_BUTTON_UP;

    Device->FingerDown = Down;
    if (Down)
    {
        Device->LastX = ScaledX;
        Device->LastY = ScaledY;
    }

    Callback = (PSERVICE_CALLBACK_ROUTINE)Device->ClassConnect.ClassService;
    if (!Callback)
        return;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    Callback(Device->ClassConnect.ClassDeviceObject, &Input, &Input + 1, &Consumed);
    KeLowerIrql(OldIrql);
}

/* The attention line fired, wake the reader through the DPC off the interrupt */
static
BOOLEAN
NTAPI
QcTouchIsr(
    _In_ PKINTERRUPT Interrupt,
    _In_ PVOID Context)
{
    PQCTOUCH_DEVICE Device = Context;

    UNREFERENCED_PARAMETER(Interrupt);

    /* The summary line is shared, only claim it when our pin latched */
    if (!QcTlmmInterruptActive(Device->Tlmm, &Device->InterruptPin))
        return FALSE;

    QcTlmmClearInterrupt(Device->Tlmm, &Device->InterruptPin);
    KeInsertQueueDpc(&Device->Dpc, NULL, NULL);
    return TRUE;
}

static
VOID
NTAPI
QcTouchDpc(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2)
{
    PQCTOUCH_DEVICE Device = Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    KeSetEvent(&Device->DataReady, IO_NO_INCREMENT, FALSE);
}

/*
 * The reader waits on the attention event and reads the controller when it
 * fires. A short fall-back timeout keeps a drag smooth and covers a missed
 * attention, a longer one while idle keeps the bus quiet.
 */
static
VOID
NTAPI
QcTouchPollThread(
    _In_ PVOID Context)
{
    PQCTOUCH_DEVICE Device = Context;
    PVOID Objects[2];
    LARGE_INTEGER Period;
    ULONG X;
    ULONG Y;
    BOOLEAN Down;
    NTSTATUS Status;

    Objects[0] = &Device->StopEvent;
    Objects[1] = &Device->DataReady;

    for (;;)
    {
        Period.QuadPart = -10000LL * (Device->FingerDown ? 10 : 50);
        Status = KeWaitForMultipleObjects(2, Objects, WaitAny, Executive, KernelMode, FALSE, &Period, NULL);
        if (Status == STATUS_WAIT_0)
            break;

        if (RmiReadFinger(Device, &X, &Y, &Down))
            QcTouchReport(Device, X, Y, Down);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

static
NTSTATUS
NTAPI
QcTouchInternalDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PQCTOUCH_DEVICE Device = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    switch (Stack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_INTERNAL_MOUSE_CONNECT:
            if (Device->ClassConnect.ClassService)
            {
                Status = STATUS_SHARING_VIOLATION;
                break;
            }
            Device->ClassConnect = *(PCONNECT_DATA)Stack->Parameters.DeviceIoControl.Type3InputBuffer;
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_INTERNAL_MOUSE_DISCONNECT:
            Device->ClassConnect.ClassService = NULL;
            Device->ClassConnect.ClassDeviceObject = NULL;
            Status = STATUS_SUCCESS;
            break;

        case IOCTL_MOUSE_QUERY_ATTRIBUTES:
            if (Stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUSE_ATTRIBUTES))
            {
                Status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            *(PMOUSE_ATTRIBUTES)Irp->AssociatedIrp.SystemBuffer = Device->Attributes;
            Irp->IoStatus.Information = sizeof(MOUSE_ATTRIBUTES);
            Status = STATUS_SUCCESS;
            break;

        default:
            return QcPnpPassDown(&Device->Pnp, Irp);
    }

    return QcPnpComplete(Irp, Status);
}

/* SDM845 funnels every TLMM interrupt into one summary line, GIC SPI 208 */
#define QCTOUCH_TLMM_GIC_SPI    208
#define QCTOUCH_GIC_SPI_BASE    32

static
VOID
QcTouchConnectInterrupt(
    _In_ PQCTOUCH_DEVICE Device)
{
    PCQCTOUCH_BOARD Board = Device->Board;
    ULONG Intid = QCTOUCH_GIC_SPI_BASE + QCTOUCH_TLMM_GIC_SPI;
    KIRQL Irql;
    KAFFINITY Affinity;
    ULONG Vector;
    NTSTATUS Status;

    RtlZeroMemory(&Device->InterruptPin, sizeof(Device->InterruptPin));
    Device->InterruptPin.Number = Board->InterruptPin;
    Device->InterruptPin.TileOffset = Board->InterruptTile;

    QcTlmmConfigureInterrupt(Device->Tlmm, &Device->InterruptPin, TlmmTriggerFallingEdge);

    Vector = HalGetInterruptVector(Internal, 0, Intid, Intid, &Irql, &Affinity);
    if (!Vector)
    {
        QC_LOG("no vector for the attention interrupt, polling only");
        return;
    }

    Status = IoConnectInterrupt(&Device->Interrupt, QcTouchIsr, Device, NULL,
                                Vector, Irql, Irql, LevelSensitive, TRUE, Affinity, FALSE);
    if (!NT_SUCCESS(Status))
    {
        Device->Interrupt = NULL;
        QC_LOG("attention interrupt connect failed %08lx, polling only", Status);
        return;
    }

    QC_LOG("attention interrupt connected on gic %lu", Intid);
}

/* Safe on a half started device, every piece is torn down only if it exists */
static
VOID
QcTouchStop(
    _In_ PQCTOUCH_DEVICE Device)
{
    /* Stop new attentions before the reader goes away */
    if (Device->Interrupt)
    {
        IoDisconnectInterrupt(Device->Interrupt);
        Device->Interrupt = NULL;
    }
    KeFlushQueuedDpcs();

    if (Device->Thread)
    {
        KeSetEvent(&Device->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(Device->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Device->Thread);
        Device->Thread = NULL;
    }

    /* Do not leave mouclass holding the button down */
    if (Device->Started && Device->FingerDown)
        QcTouchReport(Device, 0, 0, FALSE);
    Device->Started = FALSE;

    if (Device->I2c)
    {
        QcGeniI2cClose(Device->I2c);
        Device->I2c = NULL;
    }
    if (Device->Tlmm)
    {
        QcTlmmDisconnect(Device->Tlmm);
        Device->Tlmm = NULL;
    }
}

static
NTSTATUS
QcTouchStart(
    _In_ PQCTOUCH_DEVICE Device)
{
    HANDLE Thread;
    NTSTATUS Status;

    /* ACPI does not describe the I2C and GPIO wiring to us, the board table does */
    Device->Board = QcTouchDetectBoard();
    if (!Device->Board)
        return STATUS_NO_SUCH_DEVICE;

    QC_LOG("bringing up the %s %s touch controller", Device->Board->Manufacturer, Device->Board->Product);

    KeClearEvent(&Device->StopEvent);
    KeClearEvent(&Device->DataReady);
    Device->FingerDown = FALSE;

    Status = QcTouchBringUp(Device);
    if (!NT_SUCCESS(Status))
    {
        QcTouchStop(Device);
        return Status;
    }
    Device->Started = TRUE;

    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, QcTouchPollThread, Device);
    if (!NT_SUCCESS(Status))
    {
        QcTouchStop(Device);
        return Status;
    }
    ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&Device->Thread, NULL);
    ZwClose(Thread);

    /* Drive the reader from the attention line, the poll is only a fall-back */
    QcTouchConnectInterrupt(Device);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
QcTouchDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PQCTOUCH_DEVICE Device = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = QcPnpForwardAndWait(&Device->Pnp, Irp);
            if (NT_SUCCESS(Status))
                Status = QcTouchStart(Device);
            return QcPnpComplete(Irp, Status);

        /* The controller has no resources to move, so a rebalance is refused */
        case IRP_MN_QUERY_STOP_DEVICE:
            return QcPnpComplete(Irp, STATUS_UNSUCCESSFUL);

        case IRP_MN_STOP_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
            QcTouchStop(Device);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            return QcPnpPassDown(&Device->Pnp, Irp);

        case IRP_MN_REMOVE_DEVICE:
            QcTouchStop(Device);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            Status = QcPnpPassDown(&Device->Pnp, Irp);
            QcPnpDeleteDevice(&Device->Pnp);
            return Status;

        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_CANCEL_STOP_DEVICE:
            Irp->IoStatus.Status = STATUS_SUCCESS;
            return QcPnpPassDown(&Device->Pnp, Irp);

        default:
            return QcPnpPassDown(&Device->Pnp, Irp);
    }
}

/* mouclass stacks on top from the Mouse class filter and connects before START */
static
NTSTATUS
NTAPI
QcTouchAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT Pdo)
{
    PQCOM_PNP_DEVICE Pnp;
    PQCTOUCH_DEVICE Device;
    NTSTATUS Status;

    Status = QcPnpCreateDevice(DriverObject, Pdo, sizeof(*Device), FILE_DEVICE_MOUSE, &Pnp);
    if (!NT_SUCCESS(Status))
        return Status;

    Pnp->Self->Flags |= DO_BUFFERED_IO;

    Device = CONTAINING_RECORD(Pnp, QCTOUCH_DEVICE, Pnp);
    KeInitializeEvent(&Device->StopEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&Device->DataReady, SynchronizationEvent, FALSE);
    KeInitializeDpc(&Device->Dpc, QcTouchDpc, Device);

    Device->Attributes.MouseIdentifier = MOUSE_HID_HARDWARE;
    Device->Attributes.NumberOfButtons = 2;
    Device->Attributes.SampleRate = 100;
    Device->Attributes.InputDataQueueLength = 16;

    return STATUS_SUCCESS;
}

static
VOID
NTAPI
QcTouchUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    /* Every device was stopped and deleted by its REMOVE */
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverExtension->AddDevice = QcTouchAddDevice;
    DriverObject->DriverUnload = QcTouchUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = QcPnpDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = QcPnpDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = QcTouchInternalDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = QcTouchDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = QcPnpDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = QcPnpDispatchPassDown;

    return STATUS_SUCCESS;
}
