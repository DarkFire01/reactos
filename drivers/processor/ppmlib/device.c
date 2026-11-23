/*
 * PROJECT:     ReactOS Processor Power Management Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Processor device start, PnP and power handling, ACPI notifications
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ppmlib.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* {ECBE47A8-C498-4BB9-BD70-E867E0940D22} */
static const GUID PpmLibCoolingInterfaceGuid =
    { 0xecbe47a8, 0xc498, 0x4bb9, { 0xbd, 0x70, 0xe8, 0x67, 0xe0, 0x94, 0x0d, 0x22 } };

/* {DBE4373D-3C81-40CB-ACE4-E0E5D05F0C9F} */
static const GUID PpmLibCoolingDeviceInterfaceGuid =
    { 0xdbe4373d, 0x3c81, 0x40cb, { 0xac, 0xe4, 0xe0, 0xe5, 0xd0, 0x5f, 0x0c, 0x9f } };

/* Processor number property, {5724C81D-D5AF-4C1F-A103-A06E28F204C6}, 1 */
static const DEVPROPKEY PpmLibProcessorNumberKey =
    { { 0x5724c81d, 0xd5af, 0x4c1f, { 0xa1, 0x03, 0xa0, 0x6e, 0x28, 0xf2, 0x04, 0xc6 } }, 1 };

/* Thermal cooling interface the thermal zone driver queries */
typedef
VOID
(NTAPI *PPPMLIB_COOLING_CALLBACK)(
    _In_ PVOID Context,
    _In_ ULONG Value);

typedef struct _PPMLIB_COOLING_INTERFACE
{
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PINTERFACE_REFERENCE InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;
    ULONG Flags;
    PPPMLIB_COOLING_CALLBACK ActiveCooling;
    PPPMLIB_COOLING_CALLBACK PassiveCooling;
} PPMLIB_COOLING_INTERFACE, *PPPMLIB_COOLING_INTERFACE;

/* Notify() codes the processor object can raise */
#define PPMLIB_NOTIFY_OSC               0x08
#define PPMLIB_NOTIFY_PPC               0x80
#define PPMLIB_NOTIFY_CST               0x81
#define PPMLIB_NOTIFY_TPC               0x82
#define PPMLIB_NOTIFY_GUARANTEED        0x83
#define PPMLIB_NOTIFY_HIGHEST           0x85

/* Features that need the device notifications */
#define PPMLIB_NOTIFY_USERS             (PPMLIB_CST_IDLE | PPMLIB_TSS_MASK | PPMLIB_CPPC | PPMLIB_PSTATE_MASK)

NTHALAPI
NTSTATUS
NTAPI
HalGetProcessorIdByNtNumber(
    _In_ ULONG ProcessorNumber,
    _Out_ PULONG ProcessorId);

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief Maps the processor to its NT number through the initial APIC ID.
 */
static
NTSTATUS
NTAPI
PpmLibLookupProcessor(
    _Inout_ PPPMLIB_DEVICE Device)
{
    ULONG Count = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG ApicId;
    ULONG Index;

    Device->NtNumber = MAXULONG;
    for (Index = 0; Index < Count; Index++)
    {
        if (NT_SUCCESS(HalGetProcessorIdByNtNumber(Index, &ApicId)) && ApicId == Device->ApicId)
            break;
    }

    if (Index == Count)
        return STATUS_NOT_FOUND;

    PpmLibAcquireGlobal();
    if (Index >= PPMLIB_MAX_PROCESSORS || (PpmLib.Enumerated & AFFINITY_MASK(Index)))
    {
        Status = STATUS_INVALID_PARAMETER;
    }
    else
    {
        PpmLib.Enumerated |= AFFINITY_MASK(Index);
        PpmLib.Devices[Index] = Device;
        Device->NtNumber = Index;
        InsertTailList(&PpmLib.DeviceList, &Device->DeviceLink);
    }
    PpmLibReleaseGlobal();

    return Status;
}

/* Binds the P and T state handlers the surviving features call for */
static
VOID
NTAPI
PpmLibBindHandlers(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_VENDOR Vendor = &PpmLib.Vendor;

    if ((Device->Enabled & (PPMLIB_PSS_FFH | PPMLIB_XPSS)) && Device->Pct.Control.SpaceId == PPMLIB_SPACE_FFH)
    {
        if (Device->Enabled & PPMLIB_PSS_FFH)
            Device->SetPState = Vendor->SetPStateFfh;
    }
    else if (Device->Pct.Control.SpaceId == PPMLIB_SPACE_IO && (Device->Enabled & PPMLIB_PSS_IO))
    {
        Device->SetPState = PpmLibSetStateIo;
    }
    else
    {
        Device->Enabled &= ~PPMLIB_PSTATE_MASK;
    }

    if (Device->Enabled & PPMLIB_FEEDBACK)
        Device->PStateFeedback = Vendor->PStateFeedback;

    /* Boost hooks only go with FFH P-states, CPPC or PCC */
    if (Device->Enabled & (PPMLIB_CPPC | PPMLIB_PSS_FFH | PPMLIB_PCC))
    {
        Device->SetBoostPolicy = Vendor->BoostPolicyHandler;
        Device->SetBoostMode = Vendor->BoostModeHandler;
    }

    if ((Device->Enabled & PPMLIB_TSS_FFH) && Device->Ptc.Control.SpaceId == PPMLIB_SPACE_FFH)
    {
        Device->SetTState = Vendor->SetTStateFfh;
    }
    else if (Device->Ptc.Control.SpaceId == PPMLIB_SPACE_IO &&
             (Device->Enabled & (PPMLIB_TSS_IO | PPMLIB_ACPI1_THROTTLE)))
    {
        Device->SetTState = PpmLibSetStateIo;
    }
    else
    {
        Device->Enabled &= ~PPMLIB_THROTTLE_MASK;
    }

    if (Vendor->GetFfhThrottleState)
        Device->GetFfhThrottleState = Vendor->GetFfhThrottleState;

    /* Feedback alone is only worth registering when something can use it */
    if ((Device->Enabled & PPMLIB_PERF_MASK) == PPMLIB_FEEDBACK &&
        !(Device->Enabled & PPMLIB_PEP_PERF) &&
        Vendor->BoostModeHandler == NULL)
    {
        Device->Enabled &= ~PPMLIB_FEEDBACK;
    }
}

static
VOID
NTAPI
PpmLibQueryLimits(
    _In_ PPPMLIB_DEVICE Device,
    _Out_ PULONG Ppc,
    _Out_ PULONG Tpc)
{
    *Ppc = 0;
    if ((Device->Enabled & PPMLIB_PSTATE_MASK) && !NT_SUCCESS(PpmLibEvaluatePpc(Device, Ppc)))
        *Ppc = 0;

    *Tpc = 0;
    if ((Device->Enabled & PPMLIB_TSS_MASK) && !NT_SUCCESS(PpmLibEvaluateTpc(Device, Tpc)))
        *Tpc = 0;
}

/**
 * @brief Brings up power management on one processor, in the order the
 * firmware expects: handshake, idle, CPPC, P-states, throttling, then
 * registration and the SMI handoff.
 */
static
NTSTATUS
NTAPI
PpmLibStartDevice(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PPMLIB_PROCESSOR_OBJECT Info;
    GROUP_AFFINITY Previous;
    GROUP_AFFINITY Affinity;
    PROCESSOR_NUMBER Number;
    BOOLEAN Affinitized = FALSE;
    NTSTATUS Status;
    ULONG Ppc;
    ULONG Tpc;

    PAGED_CODE();

    Device->Capabilities = PpmLib.Capabilities;
    Device->Enabled = PpmLib.Capabilities;
    Device->Found = 0;

    Status = PpmLibGetProcessorObject(Device, &Info);
    if (!NT_SUCCESS(Status))
    {
        RtlZeroMemory(&Info, sizeof(Info));
        Info.InitialApicId = MAXULONG;
        Device->Enabled &= ~(PPMLIB_ACPI1_C2 | PPMLIB_ACPI1_C3 | PPMLIB_ACPI1_THROTTLE_MASK);
    }
    else if (Info.BlockAddress != 0)
    {
        Device->PBlk.SpaceId = PPMLIB_SPACE_IO;
        Device->PBlk.BitWidth = Info.BlockLength;
        Device->PBlk.BitOffset = 0;
        Device->PBlk.AccessBits = 32;
        Device->PBlk.Address = Info.BlockAddress;
    }

    Device->ApicId = Info.InitialApicId;
    Device->AcpiId = Info.PhysicalId;

    Status = PpmLibLookupProcessor(Device);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PPM: APIC ID %lu has no NT processor, 0x%08lx\n", Device->ApicId, Status);
        Device->Enabled = 0;
        return Status;
    }

    /* MSR probes below must run on the processor being started */
    IoSetDevicePropertyData(Device->Pdo,
                            &PpmLibProcessorNumberKey,
                            0,
                            0,
                            DEVPROP_TYPE_UINT32,
                            sizeof(Device->NtNumber),
                            &Device->NtNumber);

    KeGetProcessorNumberFromIndex(Device->NtNumber, &Number);
    RtlZeroMemory(&Affinity, sizeof(Affinity));
    Affinity.Group = Number.Group;
    Affinity.Mask = AFFINITY_MASK(Number.Number);
    KeSetSystemGroupAffinityThread(&Affinity, &Previous);
    Affinitized = TRUE;

    /* A failed _OSC falls back to _PDC, an _OSC that answers with errors does not */
    if (PpmLib.Vendor.Osc == NULL || !NT_SUCCESS(PpmLibEvaluateOsc(Device, PpmLib.Vendor.Osc)))
    {
        if (PpmLib.Vendor.Pdc)
            PpmLibEvaluatePdc(Device, PpmLib.Vendor.Pdc, PpmLib.Vendor.PdcSize);
    }

    /* No platform extension plug-in is supported */
    Device->Enabled &= ~PPMLIB_PEP_ANY;

    PpmLibInitCStates(Device);

    if (Device->Enabled & PPMLIB_CPPC)
    {
        if (NT_SUCCESS(PpmLibInitCpc(Device)))
            Device->Enabled &= ~(PPMLIB_PCC | PPMLIB_PSTATE_MASK | PPMLIB_THROTTLE_MASK);
        else
            Device->Enabled &= ~PPMLIB_CPPC;
    }

    /* Legacy PCC is not supported */
    Device->Enabled &= ~PPMLIB_PCC;

    if ((Device->Enabled & PPMLIB_PSTATE_MASK) && !NT_SUCCESS(PpmLibInitPerfStates(Device)))
        Device->Enabled &= ~PPMLIB_PSTATE_MASK;

    if ((Device->Enabled & PPMLIB_THROTTLE_MASK) && !NT_SUCCESS(PpmLibInitThrottleStates(Device)))
        Device->Enabled &= ~PPMLIB_THROTTLE_MASK;

    if (PpmLib.Vendor.DeviceStart)
    {
        Status = PpmLib.Vendor.DeviceStart(Device);
        if (!NT_SUCCESS(Status))
            goto Exit;
    }

    PpmLibBindHandlers(Device);
    PpmLibInitDomains(Device);

    if (Device->Enabled & PPMLIB_NOTIFY_USERS)
    {
        Status = PpmLibAcquireAcpiInterface(Device);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("PPM: no ACPI interface, 0x%08lx\n", Status);
            PpmLibReleaseAcpiInterface(Device);
            goto Exit;
        }
    }

    PpmLibAcquireAcpi(Device);
    PpmLibRegisterIdleStates(Device);
    PpmLibReleaseAcpi(Device);

    PpmLibQueryLimits(Device, &Ppc, &Tpc);
    PpmLibAcquireGlobal();
    PpmLibCapChange(Device, Ppc, Tpc, 100);
    PpmLibReleaseGlobal();

    PpmLibRegisterPerformance(Device);
    PpmLibAcquireBiosControl(Device);
    Status = STATUS_SUCCESS;

Exit:
    if (Affinitized)
        KeRevertToUserGroupAffinityThread(&Previous);

    DPRINT("PPM: processor %lu enabled 0x%I64x\n", Device->NtNumber, Device->Enabled);
    return Status;
}

/**
 * @brief Re-reads the platform limits on return from sleep.
 */
static
VOID
NTAPI
PpmLibEnterD0(
    _Inout_ PPPMLIB_DEVICE Device)
{
    ULONG Ppc;
    ULONG Tpc;

    PAGED_CODE();

    if ((Device->Enabled & PPMLIB_PCC) || !Device->ResumeFromSleep)
        return;

    Device->ResumeFromSleep = FALSE;
    PpmLibQueryLimits(Device, &Ppc, &Tpc);

    PpmLibAcquireGlobal();
    if (Ppc != Device->Ppc || Tpc != Device->Tpc)
    {
        PpmLibCapChange(Device, Ppc, Tpc, 100);
        PpmLibRegisterCap(Device);
    }
    PpmLibReleaseGlobal();
}

static
VOID
NTAPI
PpmLibResumeWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    IoFreeWorkItem((PIO_WORKITEM)Context);
    PpmLibEnterD0(DeviceObject->DeviceExtension);
}

static
NTSTATUS
NTAPI
PpmLibSystemPowerComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    PIO_WORKITEM WorkItem;

    UNREFERENCED_PARAMETER(Context);

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    if (NT_SUCCESS(Irp->IoStatus.Status))
    {
        WorkItem = IoAllocateWorkItem(DeviceObject);
        if (WorkItem)
            IoQueueWorkItem(WorkItem, PpmLibResumeWorker, DelayedWorkQueue, WorkItem);
    }

    PoStartNextPowerIrp(Irp);
    return STATUS_CONTINUE_COMPLETION;
}

/* Opens the PDO once so the stack stays referenced while the kernel uses it */
static
VOID
NTAPI
PpmLibOpenPdo(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PDEVICE_OBJECT DeviceObject;
    UNICODE_STRING Name;
    ULONG Length = 0;
    NTSTATUS Status;

    PAGED_CODE();

    Status = IoGetDeviceProperty(Device->Pdo, DevicePropertyPhysicalDeviceObjectName, 0, NULL, &Length);
    if (Status != STATUS_BUFFER_TOO_SMALL || Length == 0 || Length > MAXUSHORT)
        return;

    Name.Buffer = ExAllocatePoolWithTag(PagedPool, Length, PPMLIB_TAG);
    if (Name.Buffer == NULL)
        return;

    Status = IoGetDeviceProperty(Device->Pdo,
                                 DevicePropertyPhysicalDeviceObjectName,
                                 Length,
                                 Name.Buffer,
                                 &Length);
    if (NT_SUCCESS(Status))
    {
        Name.MaximumLength = (USHORT)Length;
        Name.Length = (USHORT)(Length - sizeof(WCHAR));
        IoGetDeviceObjectPointer(&Name, FILE_ALL_ACCESS, &Device->PdoFile, &DeviceObject);
    }

    ExFreePoolWithTag(Name.Buffer, PPMLIB_TAG);
}

static
VOID
NTAPI
PpmLibCoolingNop(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
}

/**
 * @brief Passive cooling request from a thermal zone, a percent of full speed.
 */
static
VOID
NTAPI
PpmLibPassiveCooling(
    _In_ PVOID Context,
    _In_ ULONG Percentage)
{
    PPPMLIB_DEVICE Device = Context;

    PpmLibAcquireGlobal();
    if (Device->ThermalCap != Percentage)
    {
        Device->ThermalCap = Percentage;
        PpmLibRegisterCap(Device);
    }
    PpmLibReleaseGlobal();
}

static
NTSTATUS
NTAPI
PpmLibQueryCooling(
    _In_ PPPMLIB_DEVICE Device,
    _In_ PIO_STACK_LOCATION Stack)
{
    PPPMLIB_COOLING_INTERFACE Interface;

    if (Stack->Parameters.QueryInterface.Size < sizeof(*Interface))
        return STATUS_INVALID_PARAMETER;

    Interface = (PPPMLIB_COOLING_INTERFACE)Stack->Parameters.QueryInterface.Interface;
    RtlZeroMemory(Interface, sizeof(*Interface));
    Interface->Size = sizeof(*Interface);
    Interface->Version = 1;
    Interface->Context = Device;
    Interface->InterfaceReference = PpmLibCoolingNop;
    Interface->InterfaceDereference = PpmLibCoolingNop;
    Interface->PassiveCooling = PpmLibPassiveCooling;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
PpmLibSyncComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static
NTSTATUS
NTAPI
PpmLibForwardSync(
    _In_ PPPMLIB_DEVICE Device,
    _In_ PIRP Irp)
{
    NTSTATUS Status;
    KEVENT Event;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, PpmLibSyncComplete, &Event, TRUE, TRUE, TRUE);

    Status = IoCallDriver(Device->LowerDevice, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Irp->IoStatus.Status;
    }

    return Status;
}

/* ACPI notifications **********************************************************/

static
VOID
NTAPI
PpmLibOscWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    IoFreeWorkItem((PIO_WORKITEM)Context);
}

static
VOID
NTAPI
PpmLibPpcWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PPPMLIB_DEVICE Device = DeviceObject->DeviceExtension;
    ULONG Ppc;

    IoFreeWorkItem((PIO_WORKITEM)Context);

    if (!(Device->Enabled & PPMLIB_PSTATE_MASK) || !NT_SUCCESS(PpmLibEvaluatePpc(Device, &Ppc)))
        return;

    PpmLibAcquireGlobal();
    PpmLibCapChange(Device, Ppc, Device->Tpc, 100);
    PpmLibRegisterCap(Device);
    PpmLibReleaseGlobal();
}

static
VOID
NTAPI
PpmLibTpcWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PPPMLIB_DEVICE Device = DeviceObject->DeviceExtension;
    ULONG Tpc;

    IoFreeWorkItem((PIO_WORKITEM)Context);

    if (!(Device->Enabled & PPMLIB_TSS_MASK) || !NT_SUCCESS(PpmLibEvaluateTpc(Device, &Tpc)))
        return;

    PpmLibAcquireGlobal();
    PpmLibCapChange(Device, Device->Ppc, Tpc, 100);
    PpmLibRegisterCap(Device);
    PpmLibReleaseGlobal();
}

static
VOID
NTAPI
PpmLibCstWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    IoFreeWorkItem((PIO_WORKITEM)Context);
    PpmLibCStateChange(DeviceObject->DeviceExtension);
}

static
VOID
NTAPI
PpmLibGuaranteedWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    IoFreeWorkItem((PIO_WORKITEM)Context);
    PpmLibCpcGuaranteedChange(DeviceObject->DeviceExtension);
}

static
VOID
NTAPI
PpmLibHighestWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    IoFreeWorkItem((PIO_WORKITEM)Context);
    PpmLibCpcHighestChange(DeviceObject->DeviceExtension);
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief Notify() handler of the processor object, may run above PASSIVE_LEVEL.
 */
VOID
NTAPI
PpmLibAcpiNotify(
    _In_ PVOID Context,
    _In_ ULONG NotifyCode)
{
    PPPMLIB_DEVICE Device = Context;
    PIO_WORKITEM_ROUTINE Routine;
    PIO_WORKITEM WorkItem;

    WorkItem = IoAllocateWorkItem(Device->Self);
    if (WorkItem == NULL)
        return;

    switch (NotifyCode)
    {
        case PPMLIB_NOTIFY_OSC:
            Routine = PpmLibOscWorker;
            break;
        case PPMLIB_NOTIFY_PPC:
            Routine = PpmLibPpcWorker;
            break;
        case PPMLIB_NOTIFY_CST:
            Routine = PpmLibCstWorker;
            break;
        case PPMLIB_NOTIFY_TPC:
            Routine = PpmLibTpcWorker;
            break;
        case PPMLIB_NOTIFY_GUARANTEED:
            Routine = PpmLibGuaranteedWorker;
            break;
        case PPMLIB_NOTIFY_HIGHEST:
            Routine = PpmLibHighestWorker;
            break;
        default:
            IoFreeWorkItem(WorkItem);
            return;
    }

    IoQueueWorkItem(WorkItem, Routine, CriticalWorkQueue, WorkItem);
}

/**
 * @brief Records new _PPC, _TPC and platform limit values. Global lock held.
 */
VOID
NTAPI
PpmLibCapChange(
    _Inout_ PPPMLIB_DEVICE Device,
    _In_ ULONG Ppc,
    _In_ ULONG Tpc,
    _In_ ULONG PlatformCap)
{
    Device->Ppc = Ppc;
    Device->Tpc = Tpc;
    Device->PlatformCap = PlatformCap;
}

/**
 * @brief Takes idle and performance control over from SMM through SMI_CMD.
 */
VOID
NTAPI
PpmLibAcquireBiosControl(
    _In_ PPPMLIB_DEVICE Device)
{
    PPPMLIB_FADT_INFO Fadt = &PpmLib.Fadt;

    if (Fadt->SmiCommand == 0)
        return;

    if ((Device->Enabled & PPMLIB_CST_IDLE) && Fadt->CStateControl != 0)
        WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)Fadt->SmiCommand, Fadt->CStateControl);

    if ((Device->Enabled & PPMLIB_PSTATE_MASK) && Fadt->PStateControl != 0)
        WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)Fadt->SmiCommand, Fadt->PStateControl);
}

/**
 * @brief Firmware takes control back across hibernation, hand it over again.
 */
NTSTATUS
NTAPI
PpmLibSleepNotify(
    _In_ POWER_STATE_HANDLER_TYPE State,
    _In_ PVOID Context,
    _In_ BOOLEAN Entering)
{
    UNREFERENCED_PARAMETER(Context);

    if ((State == PowerStateSleeping4 || State == PowerStateSleeping4Firmware) &&
        !Entering &&
        !IsListEmpty(&PpmLib.DeviceList))
    {
        PpmLibAcquireBiosControl(CONTAINING_RECORD(PpmLib.DeviceList.Flink, PPMLIB_DEVICE, DeviceLink));
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PpmLibAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PDEVICE_OBJECT DeviceObject;
    PPPMLIB_DEVICE Device;
    NTSTATUS Status;

    PAGED_CODE();

    Status = IoCreateDevice(DriverObject,
                            sizeof(*Device),
                            NULL,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &DeviceObject);
    if (!NT_SUCCESS(Status))
        return Status;

    Device = DeviceObject->DeviceExtension;
    RtlZeroMemory(Device, sizeof(*Device));
    Device->Self = DeviceObject;
    Device->Pdo = PhysicalDeviceObject;
    Device->NtNumber = MAXULONG;
    Device->ThermalCap = 100;
    Device->PlatformCap = 100;
    KeInitializeMutex(&Device->AcpiLock, 0);
    InitializeListHead(&Device->DeviceLink);
    InitializeListHead(&Device->DomainLink);
    InitializeListHead(&Device->PackageLink);

    Status = IoRegisterDeviceInterface(PhysicalDeviceObject,
                                       &PpmLibCoolingDeviceInterfaceGuid,
                                       NULL,
                                       &Device->CoolingInterface);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(DeviceObject);
        return Status;
    }

    Device->LowerDevice = IoAttachDeviceToDeviceStack(DeviceObject, PhysicalDeviceObject);
    if (Device->LowerDevice == NULL)
    {
        RtlFreeUnicodeString(&Device->CoolingInterface);
        IoDeleteDevice(DeviceObject);
        return STATUS_NO_SUCH_DEVICE;
    }

    DeviceObject->Flags |= DO_POWER_PAGABLE;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PpmLibDispatchPnp(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PPPMLIB_DEVICE Device = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status;

    PAGED_CODE();

    switch (Stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            Status = PpmLibForwardSync(Device, Irp);
            if (NT_SUCCESS(Status))
            {
                /* A processor that fails power management setup still runs */
                PpmLibStartDevice(Device);
                IoSetDeviceInterfaceState(&Device->CoolingInterface, TRUE);

                /* The kernel keeps pointers into this extension from now on */
                if (!Device->Started)
                    ObReferenceObject(DeviceObject);
                Device->Started = TRUE;
                Status = STATUS_SUCCESS;
            }

            Irp->IoStatus.Status = Status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return Status;

        case IRP_MN_QUERY_STOP_DEVICE:
            Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_UNSUCCESSFUL;

        case IRP_MN_QUERY_PNP_DEVICE_STATE:
            if (Device->PdoFile == NULL)
                PpmLibOpenPdo(Device);
            break;

        case IRP_MN_QUERY_INTERFACE:
            if (IsEqualGUID(Stack->Parameters.QueryInterface.InterfaceType, &PpmLibCoolingInterfaceGuid))
            {
                Status = PpmLibQueryCooling(Device, Stack);
                Irp->IoStatus.Status = Status;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return Status;
            }
            break;

        case IRP_MN_REMOVE_DEVICE:
            IoSetDeviceInterfaceState(&Device->CoolingInterface, FALSE);
            PpmLibReleaseAcpiInterface(Device);

            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(Irp);
            Status = IoCallDriver(Device->LowerDevice, Irp);

            IoDetachDevice(Device->LowerDevice);
            IoDeleteDevice(DeviceObject);
            return Status;

        default:
            break;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Device->LowerDevice, Irp);
}

NTSTATUS
NTAPI
PpmLibDispatchPower(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PPPMLIB_DEVICE Device = DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);

    if (Stack->MinorFunction == IRP_MN_SET_POWER &&
        Stack->Parameters.Power.Type == SystemPowerState &&
        Stack->Parameters.Power.State.SystemState == PowerSystemWorking &&
        Stack->Parameters.Power.SystemPowerStateContext.CurrentSystemState != PowerSystemWorking)
    {
        Device->ResumeFromSleep = TRUE;

        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp, PpmLibSystemPowerComplete, NULL, TRUE, TRUE, TRUE);
        return PoCallDriver(Device->LowerDevice, Irp);
    }

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(Device->LowerDevice, Irp);
}

NTSTATUS
NTAPI
PpmLibDispatchCreateClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
PpmLibDispatchForward(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PPPMLIB_DEVICE Device = DeviceObject->DeviceExtension;

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(Device->LowerDevice, Irp);
}

/* EOF */
