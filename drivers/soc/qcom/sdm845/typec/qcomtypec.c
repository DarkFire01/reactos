/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PMI8998 Type-C port, sources VBUS when a USB device is attached
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <qcom/spmiif.h>
#include <qcom/qcompnp.h>

#define QCDBG_COMPONENT "qcomtypec"
#include <qcom/qcomdbg.h>

#define QCTYPEC_POOL_TAG            'pTcQ'
#define QCTYPEC_POLL_MS             250

/* PMI8998 sits at SPMI slave 2, its revision block names the part */
#define PMI8998_SID                 2
#define PMI8998_REVID_SUBTYPE       0x0105
#define PMI8998_SUBTYPE             0x15

/* SMB2 charger: OTG boost block */
#define SMB_OTG_STATUS              0x1109
#define SMB_OTG_SOFTSTART_DONE      0x08
#define SMB_OTG_RT_STS              0x1110
#define SMB_OTG_OVERCURRENT         0x02
#define SMB_CMD_OTG                 0x1140
#define SMB_CMD_OTG_EN              0x01
#define SMB_OTG_CURRENT_LIMIT       0x1152
#define SMB_OTG_CURRENT_MASK        0x07
#define SMB_OTG_CFG                 0x1153
#define SMB_OTG_CFG_EN_SRC          0x02
#define SMB_OTG_ENG_CFG             0x11C0
#define SMB_OTG_ENG_HALT_1_IN_8     0x01

/* 250 mA steps from 250 mA, 1.5 A is the charger's own default for OTG */
#define SMB_OTG_CURRENT_1P5A        0x05
#define SMB_OTG_SOFTSTART_TRIES     15
#define SMB_OTG_SOFTSTART_MS        5

/* SMB2 charger: USB input block, where the Type-C state machine lives */
#define SMB_TYPEC_STATUS_2          0x130C
#define SMB_TYPEC_DFP_RD_OPEN       0x08
#define SMB_TYPEC_DFP_RD_RA_VCONN   0x04
#define SMB_TYPEC_STATUS_4          0x130E
#define SMB_TYPEC_DFP_MODE          0x80
#define SMB_TYPEC_ATTACHED          0x01
#define SMB_USBIN_RT_STS            0x1310
#define SMB_USBIN_PLUGIN            0x10
#define SMB_USBIN_CMD_IL            0x1340
#define SMB_USBIN_SUSPEND           0x01
#define SMB_TYPEC_SW_CTRL           0x1368
#define SMB_TYPEC_ROLE_MASK         0x07

typedef struct _QCTYPEC_DEVICE
{
    QCOM_PNP_DEVICE Pnp;
    PKTHREAD Thread;
    KEVENT StopEvent;
    BOOLEAN VbusOn;
    BOOLEAN Faulted;
    BOOLEAN SuspendedUsbin;
    UCHAR LastStatus2;
    UCHAR LastStatus4;
    UCHAR LastUsbin;
} QCTYPEC_DEVICE, *PQCTYPEC_DEVICE;

static
NTSTATUS
QcTypecRead(
    _In_ USHORT Address,
    _Out_ PUCHAR Value)
{
    return QcSpmiRead(PMI8998_SID, Address, Value, sizeof(*Value));
}

static
NTSTATUS
QcTypecModify(
    _In_ USHORT Address,
    _In_ UCHAR Clear,
    _In_ UCHAR Set)
{
    UCHAR Value;
    UCHAR Updated;
    NTSTATUS Status;

    Status = QcTypecRead(Address, &Value);
    if (!NT_SUCCESS(Status))
        return Status;

    Updated = (UCHAR)((Value & ~Clear) | Set);
    if (Updated == Value)
        return STATUS_SUCCESS;

    return QcSpmiWrite(PMI8998_SID, Address, &Updated, sizeof(Updated));
}

static
VOID
QcTypecDelay(
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER Delay;

    Delay.QuadPart = -10000LL * Milliseconds;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
}

/* Lets the boost ramp, a missing soft start is logged but the boost stays on */
static
VOID
QcTypecWaitSoftStart(VOID)
{
    UCHAR Value = 0;
    ULONG Try;

    for (Try = 0; Try < SMB_OTG_SOFTSTART_TRIES; Try++)
    {
        QcTypecDelay(SMB_OTG_SOFTSTART_MS);
        if (NT_SUCCESS(QcTypecRead(SMB_OTG_STATUS, &Value)) && (Value & SMB_OTG_SOFTSTART_DONE))
            return;
    }

    QC_LOG("boost soft start not reported, otg status %02x", Value);
}

static
VOID
QcTypecVbusOff(
    _In_ PQCTYPEC_DEVICE Device)
{
    NTSTATUS Status;

    Status = QcTypecModify(SMB_CMD_OTG, SMB_CMD_OTG_EN, 0);
    if (!NT_SUCCESS(Status))
        QC_LOG("cannot turn VBUS off, %08lx", Status);

    QcTypecModify(SMB_OTG_ENG_CFG, SMB_OTG_ENG_HALT_1_IN_8, 0);

    /* Hand the input back to charging only if this driver took it */
    if (Device->SuspendedUsbin)
    {
        QcTypecModify(SMB_USBIN_CMD_IL, SMB_USBIN_SUSPEND, 0);
        Device->SuspendedUsbin = FALSE;
    }

    Device->VbusOn = FALSE;
    QC_LOG("VBUS off");
}

static
VOID
QcTypecVbusOn(
    _In_ PQCTYPEC_DEVICE Device)
{
    UCHAR Value;
    NTSTATUS Status;

    /* The charger must not try to draw from the boost it is feeding */
    Status = QcTypecRead(SMB_USBIN_CMD_IL, &Value);
    if (!NT_SUCCESS(Status))
        return;
    if (!(Value & SMB_USBIN_SUSPEND))
    {
        Status = QcTypecModify(SMB_USBIN_CMD_IL, 0, SMB_USBIN_SUSPEND);
        if (!NT_SUCCESS(Status))
            return;
        Device->SuspendedUsbin = TRUE;
    }

    Status = QcTypecModify(SMB_OTG_ENG_CFG, 0, SMB_OTG_ENG_HALT_1_IN_8);
    if (NT_SUCCESS(Status))
        Status = QcTypecModify(SMB_CMD_OTG, 0, SMB_CMD_OTG_EN);
    if (!NT_SUCCESS(Status))
    {
        QC_LOG("cannot turn VBUS on, %08lx", Status);
        QcTypecVbusOff(Device);
        return;
    }

    Device->VbusOn = TRUE;
    QcTypecWaitSoftStart();
    QC_LOG("VBUS on");
}

/*
 * One look at the port. VBUS goes on only while the port is the source and a
 * sink terminates CC with Rd, and never while something else drives USBIN.
 */
static
VOID
QcTypecPoll(
    _In_ PQCTYPEC_DEVICE Device)
{
    UCHAR Status2;
    UCHAR Status4;
    UCHAR Usbin;
    BOOLEAN SinkAttached;

    if (!NT_SUCCESS(QcTypecRead(SMB_TYPEC_STATUS_2, &Status2)) ||
        !NT_SUCCESS(QcTypecRead(SMB_TYPEC_STATUS_4, &Status4)) ||
        !NT_SUCCESS(QcTypecRead(SMB_USBIN_RT_STS, &Usbin)))
    {
        return;
    }

    if (Status2 != Device->LastStatus2 || Status4 != Device->LastStatus4 || Usbin != Device->LastUsbin)
    {
        QC_LOG("port status2 %02x status4 %02x usbin %02x", Status2, Status4, Usbin);
        Device->LastStatus2 = Status2;
        Device->LastStatus4 = Status4;
        Device->LastUsbin = Usbin;
    }

    SinkAttached = (Status4 & SMB_TYPEC_ATTACHED) &&
                   (Status4 & SMB_TYPEC_DFP_MODE) &&
                   (Status2 & (SMB_TYPEC_DFP_RD_OPEN | SMB_TYPEC_DFP_RD_RA_VCONN));

    /* A fault holds VBUS off until the device is unplugged */
    if (!SinkAttached)
        Device->Faulted = FALSE;

    if (Device->VbusOn)
    {
        UCHAR Otg;

        if (NT_SUCCESS(QcTypecRead(SMB_OTG_RT_STS, &Otg)) && (Otg & SMB_OTG_OVERCURRENT))
        {
            QC_LOG("VBUS overcurrent, holding it off until the device is removed");
            Device->Faulted = TRUE;
            QcTypecVbusOff(Device);
            return;
        }

        if (!SinkAttached)
            QcTypecVbusOff(Device);
        return;
    }

    /* Our own boost also raises USBIN, so this only holds before it is on */
    if (SinkAttached && !Device->Faulted && !(Usbin & SMB_USBIN_PLUGIN))
        QcTypecVbusOn(Device);
}

static
VOID
NTAPI
QcTypecThread(
    _In_ PVOID Context)
{
    PQCTYPEC_DEVICE Device = Context;
    LARGE_INTEGER Period;

    Period.QuadPart = -10000LL * QCTYPEC_POLL_MS;
    do
    {
        QcTypecPoll(Device);
    } while (KeWaitForSingleObject(&Device->StopEvent, Executive, KernelMode, FALSE, &Period) == STATUS_TIMEOUT);

    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* Safe on a half started device, every piece is torn down only if it exists */
static
VOID
QcTypecStop(
    _In_ PQCTYPEC_DEVICE Device)
{
    if (Device->Thread)
    {
        KeSetEvent(&Device->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(Device->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Device->Thread);
        Device->Thread = NULL;
    }

    if (Device->VbusOn)
        QcTypecVbusOff(Device);
}

static
NTSTATUS
QcTypecStart(
    _In_ PQCTYPEC_DEVICE Device)
{
    HANDLE Thread;
    UCHAR Value;
    NTSTATUS Status;

    /* The register map below is the PMI8998 charger, refuse any other PMIC */
    Status = QcTypecRead(PMI8998_REVID_SUBTYPE, &Value);
    if (!NT_SUCCESS(Status))
        return Status;
    if (Value != PMI8998_SUBTYPE)
    {
        QC_LOG("PMIC at slave %u is subtype %02x, not a PMI8998", PMI8998_SID, Value);
        return STATUS_NOT_SUPPORTED;
    }

    /* Software decides when VBUS is sourced, not the Type-C hardware */
    Status = QcTypecModify(SMB_OTG_CFG, SMB_OTG_CFG_EN_SRC, 0);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Let the port toggle between sink and source so a device can attach */
    if (NT_SUCCESS(QcTypecRead(SMB_TYPEC_SW_CTRL, &Value)) && (Value & SMB_TYPEC_ROLE_MASK))
    {
        QC_LOG("port forced to role %x, switching to dual role", Value & SMB_TYPEC_ROLE_MASK);
        Status = QcTypecModify(SMB_TYPEC_SW_CTRL, SMB_TYPEC_ROLE_MASK, 0);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Status = QcTypecModify(SMB_OTG_CURRENT_LIMIT, SMB_OTG_CURRENT_MASK, SMB_OTG_CURRENT_1P5A);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Start from a known off state, the poll turns it back on if a device is there */
    Status = QcTypecRead(SMB_CMD_OTG, &Value);
    if (!NT_SUCCESS(Status))
        return Status;
    Device->VbusOn = (Value & SMB_CMD_OTG_EN) != 0;
    Device->SuspendedUsbin = FALSE;
    Device->Faulted = FALSE;
    if (Device->VbusOn)
        QcTypecVbusOff(Device);

    Device->LastStatus2 = 0xFF;
    Device->LastStatus4 = 0xFF;
    Device->LastUsbin = 0xFF;
    KeClearEvent(&Device->StopEvent);

    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, QcTypecThread, Device);
    if (!NT_SUCCESS(Status))
        return Status;

    ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&Device->Thread, NULL);
    ZwClose(Thread);

    QC_LOG("Type-C port ready");
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
QcTypecDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PQCTYPEC_DEVICE Device = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = QcPnpForwardAndWait(&Device->Pnp, Irp);
            if (NT_SUCCESS(Status))
                Status = QcTypecStart(Device);
            return QcPnpComplete(Irp, Status);

        /* The port has no resources to move, so a rebalance is refused */
        case IRP_MN_QUERY_STOP_DEVICE:
            return QcPnpComplete(Irp, STATUS_UNSUCCESSFUL);

        case IRP_MN_STOP_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
            QcTypecStop(Device);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            return QcPnpPassDown(&Device->Pnp, Irp);

        case IRP_MN_REMOVE_DEVICE:
            QcTypecStop(Device);
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

static
NTSTATUS
NTAPI
QcTypecAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT Pdo)
{
    PQCOM_PNP_DEVICE Pnp;
    PQCTYPEC_DEVICE Device;
    NTSTATUS Status;

    Status = QcPnpCreateDevice(DriverObject, Pdo, sizeof(*Device), FILE_DEVICE_UNKNOWN, &Pnp);
    if (!NT_SUCCESS(Status))
        return Status;

    Device = CONTAINING_RECORD(Pnp, QCTYPEC_DEVICE, Pnp);
    KeInitializeEvent(&Device->StopEvent, NotificationEvent, FALSE);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
QcTypecUnload(
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

    DriverObject->DriverExtension->AddDevice = QcTypecAddDevice;
    DriverObject->DriverUnload = QcTypecUnload;
    DriverObject->MajorFunction[IRP_MJ_PNP] = QcTypecDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = QcPnpDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = QcPnpDispatchPassDown;

    return STATUS_SUCCESS;
}
