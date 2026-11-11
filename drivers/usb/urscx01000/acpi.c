/*
 * PROJECT:     ReactOS USB Role Switch Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ACPI queries on the controller
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "urscx.h"

#define NDEBUG
#include <debug.h>

#define URS_ACPI_ATTEMPTS           2
#define URS_ACPI_FIRST_REPLY_SIZE   512
#define URS_PLD_SIZE                16

static
NTSTATUS
UrsAcpiCreateBuffer(
    _In_ WDFDEVICE Device,
    _In_ size_t Size,
    _Out_ WDFMEMORY *Memory,
    _Outptr_ PVOID *Buffer)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;
    Status = WdfMemoryCreate(&Attributes, NonPagedPool, 0, Size, Memory, Buffer);
    if (NT_SUCCESS(Status))
        RtlZeroMemory(*Buffer, Size);

    return Status;
}

/**
 * @brief
 * Lists the ACPI namespace below the controller.
 */
NTSTATUS
NTAPI
UrsAcpiEnumerateChildren(
    _In_ WDFDEVICE Device,
    _Out_ WDFMEMORY *Children)
{
    ACPI_ENUM_CHILDREN_INPUT_BUFFER Input;
    PACPI_ENUM_CHILDREN_OUTPUT_BUFFER Output;
    WDF_MEMORY_DESCRIPTOR InputDescriptor, OutputDescriptor;
    WDFMEMORY Memory;
    size_t Size = sizeof(ACPI_ENUM_CHILDREN_OUTPUT_BUFFER);
    ULONG Attempt;
    NTSTATUS Status = STATUS_BUFFER_OVERFLOW;

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Signature = ACPI_ENUM_CHILDREN_INPUT_BUFFER_SIGNATURE;
    Input.Flags = ENUM_CHILDREN_MULTILEVEL;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InputDescriptor, &Input, sizeof(Input));

    /* The first reply only says how much room the list needs */
    for (Attempt = 0; Attempt < URS_ACPI_ATTEMPTS; Attempt++)
    {
        Status = UrsAcpiCreateBuffer(Device, Size, &Memory, (PVOID *)&Output);
        if (!NT_SUCCESS(Status))
            return Status;

        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&OutputDescriptor, Output, (ULONG)Size);
        Status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device),
                                                   NULL,
                                                   IOCTL_ACPI_ENUM_CHILDREN,
                                                   &InputDescriptor,
                                                   &OutputDescriptor,
                                                   NULL,
                                                   NULL);
        if (NT_SUCCESS(Status))
        {
            if (Output->Signature != ACPI_ENUM_CHILDREN_OUTPUT_BUFFER_SIGNATURE || !Output->NumberOfChildren)
            {
                WdfObjectDelete(Memory);
                return STATUS_ACPI_INVALID_DATA;
            }

            *Children = Memory;
            return STATUS_SUCCESS;
        }

        if (Status != STATUS_BUFFER_OVERFLOW || Output->Signature != ACPI_ENUM_CHILDREN_OUTPUT_BUFFER_SIGNATURE)
        {
            WdfObjectDelete(Memory);
            return (Status == STATUS_BUFFER_OVERFLOW) ? STATUS_ACPI_INVALID_DATA : Status;
        }

        Size = Output->NumberOfChildren;
        WdfObjectDelete(Memory);
    }

    return Status;
}

/**
 * @brief
 * Evaluates a method by namespace path, relative to the controller or absolute.
 */
NTSTATUS
NTAPI
UrsAcpiEvaluate(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR Path,
    _Out_ WDFMEMORY *Result)
{
    ACPI_EVAL_INPUT_BUFFER_EX Input;
    PACPI_EVAL_OUTPUT_BUFFER Output;
    WDF_MEMORY_DESCRIPTOR InputDescriptor, OutputDescriptor;
    WDFMEMORY Memory;
    ULONG_PTR Returned = 0;
    size_t Size = URS_ACPI_FIRST_REPLY_SIZE;
    ULONG Attempt;
    NTSTATUS Status;

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE_EX;
    Status = RtlStringCbCopyA(Input.MethodName, sizeof(Input.MethodName), Path);
    if (!NT_SUCCESS(Status))
        return STATUS_BUFFER_OVERFLOW;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InputDescriptor, &Input, sizeof(Input));

    for (Attempt = 0; Attempt < URS_ACPI_ATTEMPTS; Attempt++)
    {
        Status = UrsAcpiCreateBuffer(Device, Size, &Memory, (PVOID *)&Output);
        if (!NT_SUCCESS(Status))
            return Status;

        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&OutputDescriptor, Output, (ULONG)Size);
        Status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device),
                                                   NULL,
                                                   IOCTL_ACPI_EVAL_METHOD_EX,
                                                   &InputDescriptor,
                                                   &OutputDescriptor,
                                                   NULL,
                                                   &Returned);
        if (NT_SUCCESS(Status))
            break;

        Size = Output->Length;
        WdfObjectDelete(Memory);
        if (Status != STATUS_BUFFER_OVERFLOW)
            return Status;
    }

    if (!NT_SUCCESS(Status))
        return Status;

    if (!Returned)
    {
        WdfObjectDelete(Memory);
        return STATUS_UNSUCCESSFUL;
    }

    if (Output->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE)
    {
        WdfObjectDelete(Memory);
        return STATUS_ACPI_INVALID_DATA;
    }

    *Result = Memory;
    return STATUS_SUCCESS;
}

static
NTSTATUS
UrsAcpiReadPld(
    _In_ WDFDEVICE Device,
    _In_z_ PCSTR Name,
    _Out_writes_bytes_(URS_PLD_SIZE) PULONG Pld)
{
    PACPI_EVAL_OUTPUT_BUFFER Output;
    CHAR Path[256];
    WDFMEMORY Memory;
    NTSTATUS Status;

    Status = RtlStringCbPrintfA(Path, sizeof(Path), "%s._PLD", Name);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UrsAcpiEvaluate(Device, Path, &Memory);
    if (!NT_SUCCESS(Status))
        return Status;

    Output = WdfMemoryGetBuffer(Memory, NULL);
    if (!Output->Count ||
        Output->Argument[0].Type != ACPI_METHOD_ARGUMENT_BUFFER ||
        Output->Argument[0].DataLength < URS_PLD_SIZE)
    {
        Status = STATUS_ACPI_INVALID_DATA;
    }
    else
    {
        RtlCopyMemory(Pld, Output->Argument[0].Data, URS_PLD_SIZE);
    }

    WdfObjectDelete(Memory);
    return Status;
}

/* Group position in the low byte, group token above it */
static
ULONG64
UrsAcpiPldGroup(
    _In_reads_(4) PULONG Pld)
{
    return ((Pld[2] >> 23) & 0xFF) | (((Pld[2] >> 15) & 0xFF) << 8);
}

static
BOOLEAN
UrsAcpiHasPldGroup(
    _In_ PURS_FDO Fdo,
    _In_ ULONG64 Group,
    _Out_ PNTSTATUS Status)
{
    ULONG Count = WdfCollectionGetCount(Fdo->Plds), i;
    WDFMEMORY Entry;

    *Status = STATUS_SUCCESS;
    for (i = 0; i < Count; i++)
    {
        Entry = WdfCollectionGetItem(Fdo->Plds, i);
        if (!Entry)
        {
            *Status = STATUS_UNSUCCESSFUL;
            return FALSE;
        }

        if (*(PULONG64)WdfMemoryGetBuffer(Entry, NULL) == Group)
            return TRUE;
    }

    return FALSE;
}

/**
 * @brief
 * Collects the distinct port groups the ACPI children below the controller describe.
 */
NTSTATUS
NTAPI
UrsAcpiEnumeratePlds(
    _In_ PURS_FDO Fdo)
{
    PACPI_ENUM_CHILDREN_OUTPUT_BUFFER Children;
    WDF_OBJECT_ATTRIBUTES Attributes;
    PACPI_ENUM_CHILD Child;
    ULONG Pld[URS_PLD_SIZE / sizeof(ULONG)];
    WDFMEMORY Memory, Entry;
    PULONG64 Group;
    ULONG i;
    NTSTATUS Status;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Fdo->Device;
    Status = WdfCollectionCreate(&Attributes, &Fdo->Plds);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = UrsAcpiEnumerateChildren(Fdo->Device, &Memory);
    if (!NT_SUCCESS(Status))
        return Status;

    Children = WdfMemoryGetBuffer(Memory, NULL);
    Child = Children->Children;

    /* The first entry is the controller itself */
    for (i = 1; i < Children->NumberOfChildren; i++)
    {
        Child = (PACPI_ENUM_CHILD)((PUCHAR)Child->Name + Child->NameLength);
        if (!(Child->Flags & ACPI_OBJECT_HAS_CHILDREN))
            continue;

        if (!NT_SUCCESS(UrsAcpiReadPld(Fdo->Device, Child->Name, Pld)))
            continue;

        if (UrsAcpiHasPldGroup(Fdo, UrsAcpiPldGroup(Pld), &Status))
            continue;
        if (!NT_SUCCESS(Status))
            break;

        WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
        Attributes.ParentObject = Fdo->Plds;
        Status = WdfMemoryCreate(&Attributes, NonPagedPool, URS_POOL_TAG, sizeof(*Group), &Entry, (PVOID *)&Group);
        if (!NT_SUCCESS(Status))
            break;

        *Group = UrsAcpiPldGroup(Pld);
        Status = WdfCollectionAdd(Fdo->Plds, Entry);
        if (!NT_SUCCESS(Status))
        {
            WdfObjectDelete(Entry);
            break;
        }
    }

    WdfObjectDelete(Memory);
    return Status;
}
