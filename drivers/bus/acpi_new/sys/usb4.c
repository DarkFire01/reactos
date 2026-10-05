/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     USB4 _OSC ownership and the host router request on the ACPI root
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include <uacpint.h>
#include <drivers/acpi/usb4osc.h>
#include <debug.h>

/* Platform _OSC support bit asking for native USB4 control */
#define UACPINT_PLATFORM_OSC_USB4           0x00040000u

/* Nonzero lets the platform _OSC ask for USB4 control */
ULONG UacpiNtUsb4NativeCmPresent = 1;

static struct
{
    KMUTEX Lock;

    /* What the last committed platform _OSC granted */
    ULONG PlatformGranted;

    /* Firmware handed the OS USB4 control at boot */
    BOOLEAN ControlGranted;

    /* The platform _OSC ran again after hibernate */
    BOOLEAN ReEvaluated;

    /* The grant survived the last platform _OSC */
    BOOLEAN ControlRetained;

    /* Control bits a committed host router request holds */
    ULONG Committed;
} UacpiNtUsb4;

VOID
NTAPI
UacpiNtUsb4Initialize(VOID)
{
    KeInitializeMutex(&UacpiNtUsb4.Lock, 0);
}

ULONG
NTAPI
UacpiNtUsb4PlatformSupport(VOID)
{
    return UacpiNtUsb4NativeCmPresent ? UACPINT_PLATFORM_OSC_USB4 : 0;
}

static
VOID
NTAPI
UacpiNtUsb4SaveGrant(VOID)
{
    UNICODE_STRING ValueName = RTL_CONSTANT_STRING(L"Usb4ControlGranted");
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE ServiceKey;
    HANDLE ParametersKey;
    ULONG Value = UacpiNtUsb4.ControlGranted;

    if (!GlobalDriverRegPath.Length)
        return;

    InitializeObjectAttributes(&ObjectAttributes,
                               &GlobalDriverRegPath,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    if (!NT_SUCCESS(ZwOpenKey(&ServiceKey, KEY_WRITE, &ObjectAttributes)))
        return;

    if (NT_SUCCESS(UacpiNtRegOpenKey(ServiceKey, L"Parameters", KEY_SET_VALUE, &ParametersKey)))
    {
        ZwSetValueKey(ParametersKey, &ValueName, 0, REG_DWORD, &Value, sizeof(Value));
        ZwClose(ParametersKey);
    }

    ZwClose(ServiceKey);
}

VOID
NTAPI
UacpiNtUsb4PlatformNegotiated(
    _In_ uacpi_namespace_node *SbNode,
    _In_ ULONG Granted)
{
    ULONG Capabilities[3];
    NTSTATUS Status;

    UacpiNtUsb4.PlatformGranted = Granted;

    if (Granted & UACPINT_PLATFORM_OSC_USB4)
    {
        /* Query only: the host router commits once it starts */
        Capabilities[0] = 1;
        Capabilities[1] = 0;
        Capabilities[2] = UACPINT_USB4_CONTROL_ALL;

        Status = UacpiNtEvaluateOscDwords(SbNode,
                                          &GUID_UACPINT_USB4_OSC,
                                          Capabilities,
                                          RTL_NUMBER_OF(Capabilities));

        UacpiNtUsb4.ControlGranted = NT_SUCCESS(Status);
        UacpiNtUsb4.ControlRetained = NT_SUCCESS(Status);
        DPRINT("USB4 _OSC query 0x%lx, control 0x%lx\n", Status, Capabilities[2]);
    }

    UacpiNtUsb4SaveGrant();
}

VOID
NTAPI
UacpiNtUsb4ResumeFromHibernate(VOID)
{
    uacpi_namespace_node *SbNode;
    ULONG Capabilities[2];
    NTSTATUS Status;

    if (!UacpiNtUsb4.ControlGranted)
        return;

    SbNode = uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB);
    if (!SbNode)
        return;

    /* Firmware may have taken USB4 back across the hibernate */
    Capabilities[0] = 0;
    Capabilities[1] = UacpiNtUsb4.PlatformGranted;
    Status = UacpiNtEvaluateOscDwords(SbNode, &UacpiNtSbOscUuid, Capabilities, RTL_NUMBER_OF(Capabilities));

    /* Only a run that returned both DWORDs counts, even when it reports an error */
    if (!NT_SUCCESS(Status) && Status != STATUS_REQUEST_NOT_ACCEPTED)
        return;

    KeWaitForSingleObject(&UacpiNtUsb4.Lock, Executive, KernelMode, FALSE, NULL);

    UacpiNtUsb4.ReEvaluated = TRUE;
    if (NT_SUCCESS(Status))
    {
        UacpiNtUsb4.PlatformGranted = Capabilities[1];
        if (Capabilities[1] & UACPINT_PLATFORM_OSC_USB4)
        {
            UacpiNtUsb4.ControlRetained = TRUE;
        }
        else
        {
            DPRINT1("Firmware took USB4 control back after hibernate\n");
            UacpiNtUsb4.ControlGranted = FALSE;
            UacpiNtUsb4.ControlRetained = FALSE;
        }
    }

    KeReleaseMutex(&UacpiNtUsb4.Lock, FALSE);
}

static
NTSTATUS
NTAPI
UacpiNtUsb4HandleRequest(
    _In_opt_ uacpi_namespace_node *Node,
    _Inout_ PUACPINT_USB4_OSC_REQUEST Request)
{
    ULONG Capabilities[3];
    NTSTATUS Status;

    if (Request->Signature != UACPINT_USB4_OSC_SIGNATURE || Request->Revision == 0)
        return STATUS_ACPI_INVALID_DATA;

    /* A request may never drop control that was committed before */
    if ((Request->ControlRequested & UacpiNtUsb4.Committed) != UacpiNtUsb4.Committed)
        return STATUS_UNSUCCESSFUL;

    if (!Node)
    {
        Request->Usb4Present = FALSE;
        return STATUS_UNSUCCESSFUL;
    }

    Capabilities[0] = Request->Query ? 1 : 0;
    Capabilities[1] = Request->Support;
    Capabilities[2] = Request->ControlRequested;

    Status = UacpiNtEvaluateOscDwords(Node, &GUID_UACPINT_USB4_OSC, Capabilities, RTL_NUMBER_OF(Capabilities));
    if (!NT_SUCCESS(Status))
    {
        Request->Usb4Present = FALSE;
        if (Status == STATUS_NOT_IMPLEMENTED || Status == STATUS_REQUEST_NOT_ACCEPTED)
            Status = STATUS_UNSUCCESSFUL;
        return Status;
    }

    Request->Usb4Present = TRUE;
    Request->ReEvaluated = UacpiNtUsb4.ReEvaluated;
    Request->ControlRetained = UacpiNtUsb4.ControlRetained;

    /* Windows bugchecks on this; drop the bits nobody asked for instead */
    if ((Capabilities[2] | Request->ControlRequested) != Request->ControlRequested)
    {
        DPRINT1("USB4 _OSC granted 0x%lx beyond the request 0x%lx\n",
                Capabilities[2], Request->ControlRequested);
        Capabilities[2] &= Request->ControlRequested;
    }

    if (!Request->Query)
        UacpiNtUsb4.Committed = Capabilities[2];

    Request->ControlGranted = Capabilities[2];
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
UacpiNtUsb4DeviceControl(
    _In_opt_ uacpi_namespace_node *Node,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    ULONG_PTR Information = sizeof(UACPINT_USB4_OSC_REQUEST);
    NTSTATUS Status;

    /* Windows checks only the input length; the system buffer is at least that big */
    if (IoStack->Parameters.DeviceIoControl.InputBufferLength < sizeof(UACPINT_USB4_OSC_REQUEST))
    {
        Status = STATUS_INFO_LENGTH_MISMATCH;
    }
    else
    {
        KeWaitForSingleObject(&UacpiNtUsb4.Lock, Executive, KernelMode, FALSE, NULL);
        Status = UacpiNtUsb4HandleRequest(Node, Irp->AssociatedIrp.SystemBuffer);
        KeReleaseMutex(&UacpiNtUsb4.Lock, FALSE);
    }

    /* Windows reports 24 bytes even to a smaller output buffer, which the copy back overruns */
    if (Information > IoStack->Parameters.DeviceIoControl.OutputBufferLength)
        Information = IoStack->Parameters.DeviceIoControl.OutputBufferLength;

    return UacpiNtCompleteIrp(Irp, Status, Information);
}

VOID
NTAPI
UacpiNtUsb4RegisterInterface(
    _Inout_ PUACPINT_FDO Fdo)
{
    NTSTATUS Status;

    Status = IoRegisterDeviceInterface(Fdo->PhysicalDeviceObject,
                                       &GUID_DEVINTERFACE_UACPINT_ROOT,
                                       NULL,
                                       &Fdo->RootInterfaceName);
    if (NT_SUCCESS(Status))
        Status = IoSetDeviceInterfaceState(&Fdo->RootInterfaceName, TRUE);

    if (!NT_SUCCESS(Status))
        DPRINT1("ACPI root interface unavailable 0x%lx\n", Status);
}

VOID
NTAPI
UacpiNtUsb4UnregisterInterface(
    _Inout_ PUACPINT_FDO Fdo)
{
    if (!Fdo->RootInterfaceName.Buffer)
        return;

    IoSetDeviceInterfaceState(&Fdo->RootInterfaceName, FALSE);
    RtlFreeUnicodeString(&Fdo->RootInterfaceName);
}
