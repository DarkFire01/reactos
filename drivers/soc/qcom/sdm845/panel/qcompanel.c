/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SDM845 DSI panel power, sleeps and wakes the panel the firmware left running
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <drivers/video/panelpower.h>
#include <qcom/qcompnp.h>

#define QCDBG_COMPONENT "qcompanel"
#include <qcom/qcomdbg.h>

#define QCPANEL_POOL_TAG            'nPcQ'

/* The first DSI controller sits inside the display subsystem the ACPI node describes */
#define QCPANEL_DSI0_OFFSET         0x00094000
#define QCPANEL_DSI0_LENGTH         0x00000400

/* The 6G controllers keep their version first and every other register 4 bytes on */
#define DSI_HW_VERSION              0x000
#define DSI_HW_VERSION_MAJOR(v)     ((v) >> 28)
#define DSI_HW_VERSION_MINOR(v)     (((v) >> 16) & 0xFFF)
#define DSI_REGS                    0x004

#define DSI_CTRL                    0x000
#define DSI_CTRL_ENABLE             0x00000001
#define DSI_CTRL_CMD_MODE_EN        0x00000004
#define DSI_STATUS0                 0x004
#define DSI_STATUS0_CMD_DMA_BUSY    0x00000002
#define DSI_CMD_DMA_CTRL            0x038
#define DSI_CMD_DMA_FROM_MEMORY     0x10000000
#define DSI_CMD_DMA_LOW_POWER       0x04000000
#define DSI_DMA_BASE                0x044
#define DSI_DMA_LEN                 0x048
#define DSI_TRIG_CTRL               0x080
#define DSI_TRIG_DMA_MASK           0x00000007
#define DSI_TRIG_DMA_SW             0x00000004
#define DSI_TRIG_DMA_OUT_OF_FRAME   0x00001000
#define DSI_TRIG_DMA                0x08C
#define DSI_INTR_CTRL               0x10C
#define DSI_INTR_CMD_DMA_DONE       0x00000001

/* A command goes out as one packet, header first, the way the DMA engine fetches it */
#define DSI_PACKET_LAST             0x80
#define DSI_DT_DCS_SHORT_WRITE      0x05
#define DSI_DT_DCS_SHORT_WRITE_PARAM 0x15
#define DSI_DMA_TIMEOUT_US          200000

/* MIPI DCS commands every panel understands */
#define DCS_ENTER_SLEEP_MODE        0x10
#define DCS_EXIT_SLEEP_MODE         0x11
#define DCS_SET_DISPLAY_OFF         0x28
#define DCS_SET_DISPLAY_ON          0x29

typedef struct _QCPANEL_DEVICE
{
    QCOM_PNP_DEVICE Pnp;
    PUCHAR Dsi;
    PUCHAR Packet;
    PHYSICAL_ADDRESS PacketAddress;
    PCALLBACK_OBJECT Callback;
    PVOID Registration;
    FAST_MUTEX Lock;
    BOOLEAN PanelOff;
} QCPANEL_DEVICE, *PQCPANEL_DEVICE;

static
ULONG
QcPanelRead(
    _In_ PQCPANEL_DEVICE Device,
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(Device->Dsi + DSI_REGS + Offset));
}

static
VOID
QcPanelWrite(
    _In_ PQCPANEL_DEVICE Device,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(Device->Dsi + DSI_REGS + Offset), Value);
}

static
VOID
QcPanelDelay(
    _In_ ULONG Milliseconds)
{
    LARGE_INTEGER Delay;

    Delay.QuadPart = -10000LL * Milliseconds;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
}

/*
 * Sends one DCS short write in low power mode. The video stream keeps
 * running, the controller holds the packet back until the next blanking.
 */
static
NTSTATUS
QcPanelSendDcs(
    _In_ PQCPANEL_DEVICE Device,
    _In_ UCHAR Command,
    _In_ BOOLEAN HasParameter,
    _In_ UCHAR Parameter)
{
    ULONG SavedCtrl;
    ULONG SavedDmaCtrl;
    ULONG SavedTrigCtrl;
    ULONG Waited;
    BOOLEAN Done = FALSE;

    Device->Packet[0] = Command;
    Device->Packet[1] = HasParameter ? Parameter : 0;
    Device->Packet[2] = HasParameter ? DSI_DT_DCS_SHORT_WRITE_PARAM : DSI_DT_DCS_SHORT_WRITE;
    Device->Packet[3] = DSI_PACKET_LAST;
    KeMemoryBarrier();

    SavedCtrl = QcPanelRead(Device, DSI_CTRL);
    SavedDmaCtrl = QcPanelRead(Device, DSI_CMD_DMA_CTRL);
    SavedTrigCtrl = QcPanelRead(Device, DSI_TRIG_CTRL);

    QcPanelWrite(Device, DSI_CMD_DMA_CTRL, SavedDmaCtrl | DSI_CMD_DMA_FROM_MEMORY | DSI_CMD_DMA_LOW_POWER);
    QcPanelWrite(Device, DSI_TRIG_CTRL,
                 (SavedTrigCtrl & ~DSI_TRIG_DMA_MASK) | DSI_TRIG_DMA_SW | DSI_TRIG_DMA_OUT_OF_FRAME);
    QcPanelWrite(Device, DSI_CTRL, SavedCtrl | DSI_CTRL_CMD_MODE_EN | DSI_CTRL_ENABLE);

    /* Clear a stale completion, the status bit is write one to clear */
    QcPanelWrite(Device, DSI_INTR_CTRL, QcPanelRead(Device, DSI_INTR_CTRL) | DSI_INTR_CMD_DMA_DONE);

    QcPanelWrite(Device, DSI_DMA_BASE, Device->PacketAddress.LowPart);
    QcPanelWrite(Device, DSI_DMA_LEN, 4);
    QcPanelWrite(Device, DSI_TRIG_DMA, 1);

    for (Waited = 0; Waited < DSI_DMA_TIMEOUT_US; Waited += 10)
    {
        if (QcPanelRead(Device, DSI_INTR_CTRL) & DSI_INTR_CMD_DMA_DONE)
        {
            Done = TRUE;
            break;
        }
        KeStallExecutionProcessor(10);
    }

    QcPanelWrite(Device, DSI_INTR_CTRL, QcPanelRead(Device, DSI_INTR_CTRL) | DSI_INTR_CMD_DMA_DONE);
    QcPanelWrite(Device, DSI_CTRL, SavedCtrl);
    QcPanelWrite(Device, DSI_TRIG_CTRL, SavedTrigCtrl);
    QcPanelWrite(Device, DSI_CMD_DMA_CTRL, SavedDmaCtrl);

    if (!Done)
    {
        QC_LOG("DCS %02x did not complete, status0 %08lx", Command, QcPanelRead(Device, DSI_STATUS0));
        return STATUS_IO_TIMEOUT;
    }

    return STATUS_SUCCESS;
}

static
VOID
QcPanelSetPower(
    _In_ PQCPANEL_DEVICE Device,
    _In_ BOOLEAN PowerOn)
{
    ExAcquireFastMutex(&Device->Lock);

    if (Device->Dsi && PowerOn == Device->PanelOff)
    {
        if (PowerOn)
        {
            /* Sleep out restores the settings the panel kept, then the picture comes back */
            if (NT_SUCCESS(QcPanelSendDcs(Device, DCS_EXIT_SLEEP_MODE, FALSE, 0)))
            {
                QcPanelDelay(120);
                QcPanelSendDcs(Device, DCS_SET_DISPLAY_ON, FALSE, 0);
            }
            Device->PanelOff = FALSE;
            QC_LOG("panel on");
        }
        else
        {
            /* The same timing the panel vendor asks for between the two */
            if (NT_SUCCESS(QcPanelSendDcs(Device, DCS_SET_DISPLAY_OFF, FALSE, 0)))
            {
                QcPanelDelay(40);
                QcPanelSendDcs(Device, DCS_ENTER_SLEEP_MODE, FALSE, 0);
                QcPanelDelay(160);
            }
            Device->PanelOff = TRUE;
            QC_LOG("panel off");
        }
    }

    ExReleaseFastMutex(&Device->Lock);
}

static
VOID
NTAPI
QcPanelPowerCallback(
    _In_opt_ PVOID Context,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2)
{
    UNREFERENCED_PARAMETER(Argument2);

    QcPanelSetPower(Context, (BOOLEAN)(ULONG_PTR)Argument1);
}

/* Safe on a half started device, every piece is torn down only if it exists */
static
VOID
QcPanelStop(
    _In_ PQCPANEL_DEVICE Device)
{
    if (Device->Registration)
    {
        ExUnregisterCallback(Device->Registration);
        Device->Registration = NULL;
    }
    if (Device->Callback)
    {
        ObDereferenceObject(Device->Callback);
        Device->Callback = NULL;
    }

    /* Never leave the phone dark once nothing can wake the panel again */
    QcPanelSetPower(Device, TRUE);

    ExAcquireFastMutex(&Device->Lock);
    if (Device->Packet)
    {
        MmFreeContiguousMemorySpecifyCache(Device->Packet, PAGE_SIZE, MmNonCached);
        Device->Packet = NULL;
    }
    if (Device->Dsi)
    {
        MmUnmapIoSpace(Device->Dsi, QCPANEL_DSI0_LENGTH);
        Device->Dsi = NULL;
    }
    ExReleaseFastMutex(&Device->Lock);
}

static
NTSTATUS
QcPanelStart(
    _In_ PQCPANEL_DEVICE Device,
    _In_opt_ PCM_RESOURCE_LIST Resources)
{
    UNICODE_STRING CallbackName = RTL_CONSTANT_STRING(DISPLAY_PANEL_POWER_CALLBACK);
    OBJECT_ATTRIBUTES ObjectAttributes;
    PCM_PARTIAL_RESOURCE_LIST List;
    PHYSICAL_ADDRESS Base;
    PHYSICAL_ADDRESS Lowest;
    PHYSICAL_ADDRESS Highest;
    PHYSICAL_ADDRESS Boundary;
    ULONG Version;
    ULONG Index;
    NTSTATUS Status;

    /* The display subsystem is the first memory range of the node */
    Base.QuadPart = 0;
    if (Resources && Resources->Count)
    {
        List = &Resources->List[0].PartialResourceList;
        for (Index = 0; Index < List->Count; Index++)
        {
            if (List->PartialDescriptors[Index].Type == CmResourceTypeMemory)
            {
                Base = List->PartialDescriptors[Index].u.Memory.Start;
                break;
            }
        }
    }
    if (!Base.QuadPart)
    {
        QC_LOG("no display subsystem range in the resources");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Base.QuadPart += QCPANEL_DSI0_OFFSET;
    Device->Dsi = MmMapIoSpace(Base, QCPANEL_DSI0_LENGTH, MmNonCached);
    if (!Device->Dsi)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Only the SDM845 DSI controller, 6G v2.2, is driven this way */
    Version = READ_REGISTER_ULONG((PULONG)(Device->Dsi + DSI_HW_VERSION));
    if (DSI_HW_VERSION_MAJOR(Version) != 2 || DSI_HW_VERSION_MINOR(Version) != 2)
    {
        QC_LOG("DSI version %08lx is not the SDM845 one", Version);
        QcPanelStop(Device);
        return STATUS_NOT_SUPPORTED;
    }

    /* The DMA engine fetches the packet with 32 bit addresses */
    Lowest.QuadPart = 0;
    Highest.QuadPart = 0xFFFFFFFF;
    Boundary.QuadPart = 0;
    Device->Packet = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, Lowest, Highest, Boundary, MmNonCached);
    if (!Device->Packet)
    {
        QcPanelStop(Device);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Device->PacketAddress = MmGetPhysicalAddress(Device->Packet);
    Device->PanelOff = FALSE;

    InitializeObjectAttributes(&ObjectAttributes,
                               &CallbackName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    Status = ExCreateCallback(&Device->Callback, &ObjectAttributes, TRUE, TRUE);
    if (!NT_SUCCESS(Status))
    {
        QcPanelStop(Device);
        return Status;
    }

    Device->Registration = ExRegisterCallback(Device->Callback, QcPanelPowerCallback, Device);
    if (!Device->Registration)
    {
        QcPanelStop(Device);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    QC_LOG("DSI %08lx ready, panel power follows the display", Version);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
QcPanelDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PQCPANEL_DEVICE Device = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = QcPnpForwardAndWait(&Device->Pnp, Irp);
            if (NT_SUCCESS(Status))
                Status = QcPanelStart(Device, Stack->Parameters.StartDevice.AllocatedResourcesTranslated);
            return QcPnpComplete(Irp, Status);

        /* The panel has no resources to move, so a rebalance is refused */
        case IRP_MN_QUERY_STOP_DEVICE:
            return QcPnpComplete(Irp, STATUS_UNSUCCESSFUL);

        case IRP_MN_STOP_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
            QcPanelStop(Device);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            return QcPnpPassDown(&Device->Pnp, Irp);

        case IRP_MN_REMOVE_DEVICE:
            QcPanelStop(Device);
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
QcPanelAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT Pdo)
{
    PQCOM_PNP_DEVICE Pnp;
    PQCPANEL_DEVICE Device;
    NTSTATUS Status;

    Status = QcPnpCreateDevice(DriverObject, Pdo, sizeof(*Device), FILE_DEVICE_UNKNOWN, &Pnp);
    if (!NT_SUCCESS(Status))
        return Status;

    Device = CONTAINING_RECORD(Pnp, QCPANEL_DEVICE, Pnp);
    ExInitializeFastMutex(&Device->Lock);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
QcPanelUnload(
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

    DriverObject->DriverExtension->AddDevice = QcPanelAddDevice;
    DriverObject->DriverUnload = QcPanelUnload;
    DriverObject->MajorFunction[IRP_MJ_PNP] = QcPanelDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = QcPnpDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = QcPnpDispatchPassDown;

    return STATUS_SUCCESS;
}
