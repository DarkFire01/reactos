/*
 * PROJECT:     ReactOS Qualcomm subsystem manager
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     CHLD child devices and their wait wake queues
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "subsys.h"
#include <ntstrsafe.h>

#define QCDBG_COMPONENT "qcomsubsys"
#include <qcom/qcomdbg.h>

#define SS_METHOD_CHLD              'DLHC'
#define SS_CHLD_FIELDS              5
#define SS_LOCALE_EN_US             0x409

static EVT_WDF_CHILD_LIST_CREATE_DEVICE SsEvtChildCreateDevice;

static
PACPI_METHOD_ARGUMENT
SsNextArgument(
    _In_ PACPI_METHOD_ARGUMENT Argument)
{
    return (PACPI_METHOD_ARGUMENT)((PUCHAR)Argument + ACPI_METHOD_ARGUMENT_LENGTH(Argument->DataLength));
}

/* Turns one CHLD package into a child record */
static
NTSTATUS
SsParseChild(
    _In_ PACPI_METHOD_ARGUMENT Package,
    _Out_ PSS_CHILD Child)
{
    PACPI_METHOD_ARGUMENT Field;
    ANSI_STRING Ansi;
    ULONG Values[SS_CHLD_FIELDS - 1];
    ULONG i;

    if (Package->Type != ACPI_METHOD_ARGUMENT_PACKAGE)
        return STATUS_ACPI_INVALID_DATA;

    Field = (PACPI_METHOD_ARGUMENT)Package->Data;
    if (Field->Type != ACPI_METHOD_ARGUMENT_STRING)
        return STATUS_ACPI_INVALID_DATA;

    RtlInitAnsiString(&Ansi, (PCSZ)Field->Data);
    if (!NT_SUCCESS(RtlAnsiStringToUnicodeString(&Child->DeviceId, &Ansi, TRUE)))
        return STATUS_INSUFFICIENT_RESOURCES;

    for (i = 0; i < RTL_NUMBER_OF(Values); i++)
    {
        Field = SsNextArgument(Field);
        if (Field->Type != ACPI_METHOD_ARGUMENT_INTEGER)
        {
            RtlFreeUnicodeString(&Child->DeviceId);
            return STATUS_ACPI_INVALID_DATA;
        }
        Values[i] = Field->Argument;
    }

    Child->DeviceType = Values[0];
    Child->Instance = Values[1];
    Child->RemovalPolicy = (Values[2] >= 3) ? SS_CHILD_POLICY_KEEP : Values[2];
    Child->RemovalHook = Values[3];

    InitializeListHead(&Child->WakeIrps);
    KeInitializeSpinLock(&Child->WakeLock);
    return STATUS_SUCCESS;
}

NTSTATUS
SsReadChildren(
    _Inout_ PSS_DEVICE Device)
{
    PACPI_EVAL_OUTPUT_BUFFER Output;
    PACPI_METHOD_ARGUMENT Package;
    NTSTATUS Status;
    ULONG i;

    Status = SsEvaluateMethod(Device, SS_METHOD_CHLD, &Output);
    if (!NT_SUCCESS(Status))
        return STATUS_SUCCESS;

    if (Output->Count == 0)
    {
        ExFreePoolWithTag(Output, SS_TAG);
        return STATUS_SUCCESS;
    }

    Device->Children = ExAllocatePoolZero(NonPagedPool, Output->Count * sizeof(SS_CHILD), SS_TAG);
    if (!Device->Children)
    {
        ExFreePoolWithTag(Output, SS_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Package = Output->Argument;
    for (i = 0; i < Output->Count; i++)
    {
        Status = SsParseChild(Package, &Device->Children[i]);
        if (!NT_SUCCESS(Status))
        {
            Device->ChildCount = i;
            ExFreePoolWithTag(Output, SS_TAG);
            SsFreeChildren(Device);
            return Status;
        }
        Device->Children[i].Index = i;
        Package = SsNextArgument(Package);
    }

    Device->ChildCount = Output->Count;
    ExFreePoolWithTag(Output, SS_TAG);
    return STATUS_SUCCESS;
}

/* IO_CSQ over one child's held wait wake IRPs */
static IO_CSQ_INSERT_IRP SsCsqInsert;
static IO_CSQ_REMOVE_IRP SsCsqRemove;
static IO_CSQ_PEEK_NEXT_IRP SsCsqPeek;
static IO_CSQ_ACQUIRE_LOCK SsCsqAcquire;
static IO_CSQ_RELEASE_LOCK SsCsqRelease;
static IO_CSQ_COMPLETE_CANCELED_IRP SsCsqComplete;

static
VOID
NTAPI
SsCsqInsert(
    _In_ PIO_CSQ Csq,
    _In_ PIRP Irp)
{
    PSS_CHILD Child = CONTAINING_RECORD(Csq, SS_CHILD, WakeQueue);

    InsertTailList(&Child->WakeIrps, &Irp->Tail.Overlay.ListEntry);
}

static
VOID
NTAPI
SsCsqRemove(
    _In_ PIO_CSQ Csq,
    _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);

    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
}

static
PIRP
NTAPI
SsCsqPeek(
    _In_ PIO_CSQ Csq,
    _In_ PIRP Irp,
    _In_ PVOID PeekContext)
{
    PSS_CHILD Child = CONTAINING_RECORD(Csq, SS_CHILD, WakeQueue);
    PLIST_ENTRY Next;

    UNREFERENCED_PARAMETER(PeekContext);

    Next = Irp ? Irp->Tail.Overlay.ListEntry.Flink : Child->WakeIrps.Flink;
    if (Next == &Child->WakeIrps)
        return NULL;

    return CONTAINING_RECORD(Next, IRP, Tail.Overlay.ListEntry);
}

static
VOID
NTAPI
SsCsqAcquire(
    _In_ PIO_CSQ Csq,
    _Out_ PKIRQL Irql)
{
    PSS_CHILD Child = CONTAINING_RECORD(Csq, SS_CHILD, WakeQueue);

    KeAcquireSpinLock(&Child->WakeLock, Irql);
}

static
VOID
NTAPI
SsCsqRelease(
    _In_ PIO_CSQ Csq,
    _In_ KIRQL Irql)
{
    PSS_CHILD Child = CONTAINING_RECORD(Csq, SS_CHILD, WakeQueue);

    KeReleaseSpinLock(&Child->WakeLock, Irql);
}

static
VOID
NTAPI
SsCsqComplete(
    _In_ PIO_CSQ Csq,
    _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static EVT_WDFDEVICE_WDM_IRP_PREPROCESS SsPreprocessPower;

/* Holds a wait wake request until the modem IOCTL drains it */
static
NTSTATUS
SsPreprocessPower(
    _In_ WDFDEVICE WdfDevice,
    _Inout_ PIRP Irp)
{
    PSS_PDO Pdo = SsGetPdo(WdfDevice);
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
    PSS_CHILD Child;

    if (Stack->MinorFunction != IRP_MN_WAIT_WAKE)
        return WdfDeviceWdmDispatchPreprocessedIrp(WdfDevice, Irp);

    Child = &Pdo->Parent->Children[Pdo->Index];
    QC_LOG("child %lu holds a wait wake IRP", Pdo->Index);
    IoMarkIrpPending(Irp);
    IoCsqInsertIrp(&Child->WakeQueue, Irp, NULL);
    return STATUS_PENDING;
}

static
NTSTATUS
SsEvtChildCreateDevice(
    _In_ WDFCHILDLIST ChildList,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER IdHeader,
    _In_ PWDFDEVICE_INIT ChildInit)
{
    PSS_CHILD_DESCRIPTION Id = CONTAINING_RECORD(IdHeader, SS_CHILD_DESCRIPTION, Header);
    PSS_DEVICE Device = SsGetDevice(WdfChildListGetDevice(ChildList));
    DECLARE_UNICODE_STRING_SIZE(Instance, 16);
    DECLARE_UNICODE_STRING_SIZE(Text, 32);
    UNICODE_STRING Location = RTL_CONSTANT_STRING(L"AMSS Bus 0");
    WDF_OBJECT_ATTRIBUTES Attributes;
    PSS_CHILD Child;
    WDFDEVICE Pdo;
    NTSTATUS Status;

    if (Id->Index >= Device->ChildCount)
        return STATUS_INVALID_PARAMETER;

    Child = &Device->Children[Id->Index];
    QC_LOG("create child %lu '%wZ' type 0x%lx instance %lu",
           Id->Index, &Child->DeviceId, Child->DeviceType, Child->Instance);

    Status = WdfPdoInitAssignDeviceID(ChildInit, &Child->DeviceId);
    if (NT_SUCCESS(Status))
        Status = WdfPdoInitAddHardwareID(ChildInit, &Child->DeviceId);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlIntegerToUnicodeString(Child->Instance, 10, &Instance);
    Status = WdfPdoInitAssignInstanceID(ChildInit, &Instance);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = RtlStringCchPrintfW(Text.Buffer, Text.MaximumLength / sizeof(WCHAR), L"AMSS_Child%02lu", Id->Index);
    if (NT_SUCCESS(Status))
    {
        RtlInitUnicodeString(&Text, Text.Buffer);
        WdfPdoInitAddDeviceText(ChildInit, &Text, &Location, SS_LOCALE_EN_US);
        WdfPdoInitSetDefaultLocale(ChildInit, SS_LOCALE_EN_US);
    }

    /* Modem children hold wait wake, so the power IRP is intercepted */
    if (Child->DeviceType == SS_CHILD_TYPE_MODEM)
    {
        Status = WdfDeviceInitAssignWdmIrpPreprocessCallback(ChildInit,
                                                             SsPreprocessPower,
                                                             IRP_MJ_POWER,
                                                             NULL,
                                                             0);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SS_PDO);
    Status = WdfDeviceCreate(&ChildInit, &Attributes, &Pdo);
    if (!NT_SUCCESS(Status))
        return Status;

    SsGetPdo(Pdo)->Parent = Device;
    SsGetPdo(Pdo)->Index = Id->Index;

    if (Child->DeviceType == SS_CHILD_TYPE_MODEM)
    {
        IoCsqInitialize(&Child->WakeQueue,
                        SsCsqInsert,
                        SsCsqRemove,
                        SsCsqPeek,
                        SsCsqAcquire,
                        SsCsqRelease,
                        SsCsqComplete);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
SsSetupChildren(
    _Inout_ PSS_DEVICE Device,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_CHILD_LIST_CONFIG Config;

    UNREFERENCED_PARAMETER(Device);

    WDF_CHILD_LIST_CONFIG_INIT(&Config, sizeof(SS_CHILD_DESCRIPTION), SsEvtChildCreateDevice);
    WdfFdoInitSetDefaultChildListConfig(DeviceInit, &Config, WDF_NO_OBJECT_ATTRIBUTES);
    return STATUS_SUCCESS;
}

VOID
SsAnnounceChildren(
    _Inout_ PSS_DEVICE Device)
{
    SS_CHILD_DESCRIPTION Description;
    ULONG i;

    Device->ChildList = WdfFdoGetDefaultChildList(Device->WdfDevice);
    if (!Device->ChildList)
        return;

    QC_LOG("announcing %lu children", Device->ChildCount);
    WdfChildListBeginScan(Device->ChildList);
    for (i = 0; i < Device->ChildCount; i++)
    {
        WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&Description.Header, sizeof(Description));
        Description.Index = i;
        WdfChildListAddOrUpdateChildDescriptionAsPresent(Device->ChildList, &Description.Header, NULL);
    }
    WdfChildListEndScan(Device->ChildList);
}

ULONG
SsRemoveChildren(
    _Inout_ PSS_DEVICE Device)
{
    SS_CHILD_DESCRIPTION Description;
    BOOLEAN Pending = FALSE;
    ULONG i;

    if (!Device->ChildList)
        return SS_EV_CHILDREN_GONE;

    WdfChildListBeginScan(Device->ChildList);
    for (i = 0; i < Device->ChildCount; i++)
    {
        /* Policy 0 keeps the child present across a restart */
        if (Device->Children[i].RemovalPolicy == SS_CHILD_POLICY_KEEP)
            continue;

        if (Device->Children[i].RemovalPolicy == SS_CHILD_POLICY_SHUTDOWN &&
            !Device->ShutdownInProgress)
        {
            continue;
        }

        WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT(&Description.Header, sizeof(Description));
        Description.Index = i;
        WdfChildListUpdateChildDescriptionAsMissing(Device->ChildList, &Description.Header);
        Pending = TRUE;
    }
    WdfChildListEndScan(Device->ChildList);

    return Pending ? SS_EV_NONE : SS_EV_CHILDREN_GONE;
}

VOID
SsReleaseWakeIrps(
    _Inout_ PSS_DEVICE Device,
    _In_ ULONG Index)
{
    PSS_CHILD Child;
    PIRP Irp;

    if (Index >= Device->ChildCount)
        return;

    Child = &Device->Children[Index];
    while ((Irp = IoCsqRemoveNextIrp(&Child->WakeQueue, NULL)) != NULL)
    {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
}

VOID
SsFreeChildren(
    _Inout_ PSS_DEVICE Device)
{
    ULONG i;

    if (!Device->Children)
        return;

    for (i = 0; i < Device->ChildCount; i++)
        RtlFreeUnicodeString(&Device->Children[i].DeviceId);

    ExFreePoolWithTag(Device->Children, SS_TAG);
    Device->Children = NULL;
    Device->ChildCount = 0;
}
