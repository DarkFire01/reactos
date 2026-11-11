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
#define SMB_CMD_OTG                 0x1140
#define SMB_CMD_OTG_EN              0x01
#define SMB_OTG_CFG                 0x1153
#define SMB_OTG_CFG_EN_SRC          0x02

/* SMB2 charger: USB input block, where the Type-C state machine lives */
#define SMB_TYPEC_STATUS_2          0x130C
#define SMB_TYPEC_DFP_RD_OPEN       0x08
#define SMB_TYPEC_DFP_RD_RA_VCONN   0x04
#define SMB_TYPEC_STATUS_4          0x130E
#define SMB_TYPEC_DFP_MODE          0x80
#define SMB_TYPEC_ATTACHED          0x01
#define SMB_USBIN_RT_STS            0x1310
#define SMB_USBIN_PLUGIN            0x10
#define SMB_TYPEC_SW_CTRL           0x1368
#define SMB_TYPEC_ROLE_MASK         0x07

typedef struct _QCTYPEC_DEVICE
{
    QCOM_PNP_DEVICE Pnp;
    PKTHREAD Thread;
    KEVENT StopEvent;
    BOOLEAN VbusOn;
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
QcTypecSetVbus(
    _In_ PQCTYPEC_DEVICE Device,
    _In_ BOOLEAN On)
{
    NTSTATUS Status;

    Status = QcTypecModify(SMB_CMD_OTG, SMB_CMD_OTG_EN, On ? SMB_CMD_OTG_EN : 0);
    if (!NT_SUCCESS(Status))
    {
        QC_LOG("cannot turn VBUS %s, %08lx", On ? "on" : "off", Status);
        return;
    }

    Device->VbusOn = On;
    QC_LOG("VBUS %s", On ? "on" : "off");
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

    if (SinkAttached && !Device->VbusOn)
    {
        /* Our own boost also raises USBIN, so this only holds before it is on */
        if (Usbin & SMB_USBIN_PLUGIN)
            return;

        QcTypecSetVbus(Device, TRUE);
    }
    else if (!SinkAttached && Device->VbusOn)
    {
        QcTypecSetVbus(Device, FALSE);
    }
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
        QcTypecSetVbus(Device, FALSE);
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

    Status = QcTypecRead(SMB_CMD_OTG, &Value);
    if (!NT_SUCCESS(Status))
        return Status;
    Device->VbusOn = (Value & SMB_CMD_OTG_EN) != 0;

    Device->LastStatus2 = 0xFF;
    Device->LastStatus4 = 0xFF;
    Device->LastUsbin = 0xFF;
    KeClearEvent(&Device->StopEvent);

    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, QcTypecThread, Device);
    if (!NT_SUCCESS(Status))
        return Status;

    ObReferenceObjectByHandle(Thread, THREAD_ALL_ACCESS, NULL, KernelMode, (PVOID *)&Device->Thread, NULL);
    ZwClose(Thread);

    QC_LOG("Type-C port ready, VBUS %s", Device->VbusOn ? "on" : "off");
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
