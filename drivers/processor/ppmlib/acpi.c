/*
 * PROJECT:     ReactOS Processor Power Management Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     ACPI method evaluation and parsing of the processor objects
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ppmlib.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* Enough for the longest package the driver reads, _CPC revision 3 and up */
#define PPMLIB_MAX_ITEMS                32

/* First output size tried, the reply grows it when needed */
#define PPMLIB_EVAL_FIRST_SIZE          1024

/* Descriptor of a register buffer, ACPI "Generic Register" */
#define PPMLIB_REGISTER_DESCRIPTOR      0x82
#define PPMLIB_REGISTER_BUFFER_SIZE     15
#define PPMLIB_REGISTER_BODY_SIZE       12

/* The integer packages are read straight into these layouts */
C_ASSERT(sizeof(PPMLIB_PSTATE) == 6 * sizeof(ULONG));
C_ASSERT(sizeof(PPMLIB_TSTATE) == 5 * sizeof(ULONG));
C_ASSERT(sizeof(PPMLIB_IDLE_DEPENDENCY) == 6 * sizeof(ULONG));
C_ASSERT(sizeof(PPMLIB_DEPENDENCY) == 5 * sizeof(ULONG));

typedef struct _PPMLIB_ITEMS
{
    ULONG Count;
    PACPI_METHOD_ARGUMENT Item[PPMLIB_MAX_ITEMS];
} PPMLIB_ITEMS, *PPPMLIB_ITEMS;

/* PRIVATE FUNCTIONS **********************************************************/

static
NTSTATUS
NTAPI
PpmLibSendIoctl(
    _In_ PDEVICE_OBJECT Target,
    _In_ ULONG IoControlCode,
    _In_reads_bytes_opt_(InputSize) PVOID Input,
    _In_ ULONG InputSize,
    _Out_writes_bytes_opt_(OutputSize) PVOID Output,
    _In_ ULONG OutputSize,
    _Out_opt_ PULONG_PTR Returned)
{
    IO_STATUS_BLOCK IoStatus;
    NTSTATUS Status;
    KEVENT Event;
    PIRP Irp;

    PAGED_CODE();

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IoControlCode,
                                        Target,
                                        Input,
                                        InputSize,
                                        Output,
                                        OutputSize,
                                        FALSE,
                                        &Event,
                                        &IoStatus);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(Target, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (Returned)
        *Returned = NT_ERROR(Status) ? 0 : IoStatus.Information;

    return Status;
}

/* Splits an argument run into its elements, Count keeps counting past the array */
static
VOID
NTAPI
PpmLibCollect(
    _In_ PACPI_METHOD_ARGUMENT First,
    _In_ PUCHAR End,
    _Out_ PPPMLIB_ITEMS Items)
{
    PACPI_METHOD_ARGUMENT Argument = First;

    Items->Count = 0;
    while ((PUCHAR)Argument + FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) <= End)
    {
        if (Items->Count < PPMLIB_MAX_ITEMS)
            Items->Item[Items->Count] = Argument;

        Items->Count++;
        Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    }
}

static
VOID
NTAPI
PpmLibCollectOutput(
    _In_ PACPI_EVAL_OUTPUT_BUFFER Output,
    _Out_ PPPMLIB_ITEMS Items)
{
    PpmLibCollect((PACPI_METHOD_ARGUMENT)Output->Argument, (PUCHAR)Output + Output->Length, Items);
}

static
VOID
NTAPI
PpmLibCollectPackage(
    _In_ PACPI_METHOD_ARGUMENT Package,
    _Out_ PPPMLIB_ITEMS Items)
{
    PpmLibCollect((PACPI_METHOD_ARGUMENT)Package->Data, Package->Data + Package->DataLength, Items);
}

/**
 * @brief Checks the element count of a parsed package.
 *
 * @param[in] AllowExtra
 * TRUE when trailing elements a newer revision adds are acceptable.
 */
static
NTSTATUS
NTAPI
PpmLibCheckCount(
    _In_ PPPMLIB_ITEMS Items,
    _In_ ULONG Expected,
    _In_ BOOLEAN AllowExtra)
{
    if (Items->Count < Expected)
        return STATUS_ACPI_INCORRECT_ARGUMENT_COUNT;

    if (Items->Count > Expected && !AllowExtra)
        return STATUS_ACPI_INCORRECT_ARGUMENT_COUNT;

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
PpmLibGetInteger(
    _In_ PACPI_METHOD_ARGUMENT Argument,
    _Out_ PULONG Value)
{
    if (Argument->Type != ACPI_METHOD_ARGUMENT_INTEGER)
        return STATUS_ACPI_INVALID_ARGTYPE;

    *Value = Argument->Argument;
    return STATUS_SUCCESS;
}

/* Reads a run of integers into consecutive ULONG fields */
static
NTSTATUS
NTAPI
PpmLibGetIntegers(
    _In_ PACPI_METHOD_ARGUMENT Package,
    _In_ ULONG Count,
    _Out_writes_(Count) PULONG Values)
{
    PPMLIB_ITEMS Items;
    NTSTATUS Status;
    ULONG Index;

    PpmLibCollectPackage(Package, &Items);
    for (Index = 0; Index < min(Count, Items.Count); Index++)
    {
        Status = PpmLibGetInteger(Items.Item[Index], &Values[Index]);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    return PpmLibCheckCount(&Items, Count, FALSE);
}

/* Unpacks a Generic Register descriptor buffer */
static
NTSTATUS
NTAPI
PpmLibGetRegister(
    _In_ PACPI_METHOD_ARGUMENT Argument,
    _Out_ PPPMLIB_GAS Register)
{
    PUCHAR Data = Argument->Data;

    if (Argument->Type != ACPI_METHOD_ARGUMENT_BUFFER ||
        Argument->DataLength < PPMLIB_REGISTER_BUFFER_SIZE ||
        Data[0] != PPMLIB_REGISTER_DESCRIPTOR ||
        *(USHORT UNALIGNED *)&Data[1] < PPMLIB_REGISTER_BODY_SIZE)
    {
        return STATUS_ACPI_INVALID_ARGTYPE;
    }

    Register->SpaceId = Data[3];
    Register->BitWidth = Data[4];
    Register->BitOffset = Data[5];
    Register->AccessBits = Data[6];
    Register->Address = *(ULONG64 UNALIGNED *)&Data[7];
    return STATUS_SUCCESS;
}

/* A _CPC capability field may be a plain value instead of a register */
static
NTSTATUS
NTAPI
PpmLibGetRegisterOrValue(
    _In_ PACPI_METHOD_ARGUMENT Argument,
    _Out_ PPPMLIB_GAS Register)
{
    if (Argument->Type == ACPI_METHOD_ARGUMENT_INTEGER)
    {
        RtlZeroMemory(Register, sizeof(*Register));
        Register->SpaceId = PPMLIB_SPACE_STATIC;
        Register->Address = Argument->Argument;
        return STATUS_SUCCESS;
    }

    return PpmLibGetRegister(Argument, Register);
}

static
NTSTATUS
NTAPI
PpmLibEvaluateName(
    _In_ PPPMLIB_DEVICE Device,
    _In_reads_(4) PCSTR Name,
    _Outptr_result_maybenull_ PACPI_EVAL_OUTPUT_BUFFER *Output)
{
    ACPI_EVAL_INPUT_BUFFER Input;

    Input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(Input.MethodName, Name, sizeof(Input.MethodName));
    return PpmLibEvaluate(Device, &Input, sizeof(Input), Output);
}

static
VOID
NTAPI
PpmLibInvalidate(
    _In_ PPPMLIB_DEVICE Device,
    _In_ ULONG Error)
{
    InterlockedOr((PLONG)&Device->Invalidated, (LONG)Error);
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief Evaluates a method on the processor's ACPI device.
 *
 * @param[out] Output
 * Receives the reply, NULL when the method returned nothing. Free with the
 * library pool tag.
 */
NTSTATUS
NTAPI
PpmLibEvaluate(
    _In_ PPPMLIB_DEVICE Device,
    _In_reads_bytes_(InputSize) PVOID Input,
    _In_ ULONG InputSize,
    _Outptr_result_maybenull_ PACPI_EVAL_OUTPUT_BUFFER *Output)
{
    PACPI_EVAL_OUTPUT_BUFFER Buffer;
    ULONG Size = PPMLIB_EVAL_FIRST_SIZE;
    ULONG_PTR Returned;
    NTSTATUS Status;

    PAGED_CODE();

    *Output = NULL;
    for (;;)
    {
        Buffer = ExAllocatePoolWithTag(PagedPool, Size, PPMLIB_TAG);
        if (Buffer == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;

        Status = PpmLibSendIoctl(Device->LowerDevice,
                                 IOCTL_ACPI_EVAL_METHOD,
                                 Input,
                                 InputSize,
                                 Buffer,
                                 Size,
                                 &Returned);
        if (Status != STATUS_BUFFER_OVERFLOW || Buffer->Length <= Size)
            break;

        /* The header tells how much the full reply needs */
        Size = Buffer->Length;
        ExFreePoolWithTag(Buffer, PPMLIB_TAG);
    }

    if (NT_SUCCESS(Status) && Returned != 0)
        *Output = Buffer;
    else
        ExFreePoolWithTag(Buffer, PPMLIB_TAG);

    return Status;
}

/**
 * @brief Asks the ACPI driver which processor object this device is.
 */
NTSTATUS
NTAPI
PpmLibGetProcessorObject(
    _In_ PPPMLIB_DEVICE Device,
    _Out_ PPPMLIB_PROCESSOR_OBJECT Info)
{
    RtlZeroMemory(Info, sizeof(*Info));
    return PpmLibSendIoctl(Device->LowerDevice,
                           IOCTL_GET_PROCESSOR_OBJ_INFO,
                           NULL,
                           0,
                           Info,
                           sizeof(*Info),
                           NULL);
}

/**
 * @brief Runs _OSC with the vendor capabilities, keeps the reply when accepted.
 */
NTSTATUS
NTAPI
PpmLibEvaluateOsc(
    _Inout_ PPPMLIB_DEVICE Device,
    _In_ PPPMLIB_OSC_REQUEST Request)
{
    PACPI_EVAL_INPUT_BUFFER_COMPLEX Input;
    PACPI_EVAL_OUTPUT_BUFFER Output;
    PACPI_METHOD_ARGUMENT Argument;
    ULONG CapsSize;
    ULONG Size;
    NTSTATUS Status;

    if (Request->Count == 0 || Request->Count > 5)
    {
        PpmLibInvalidate(Device, PPMLIB_ERROR_OSC);
        return STATUS_UNSUCCESSFUL;
    }

    /* UUID, revision, count and the capability dwords */
    CapsSize = Request->Count * sizeof(ULONG);
    Size = FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX, Argument) +
           ACPI_METHOD_ARGUMENT_LENGTH(sizeof(GUID)) +
           2 * ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG)) +
           ACPI_METHOD_ARGUMENT_LENGTH(CapsSize);

    Input = ExAllocatePoolWithTag(PagedPool, Size, PPMLIB_TAG);
    if (Input == NULL)
    {
        PpmLibInvalidate(Device, PPMLIB_ERROR_OSC);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Input, Size);
    Input->Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    RtlCopyMemory(Input->MethodName, "_OSC", sizeof(Input->MethodName));
    Input->Size = Size;
    Input->ArgumentCount = 4;

    Argument = Input->Argument;
    ACPI_METHOD_SET_ARGUMENT_BUFFER(Argument, &Request->Uuid, sizeof(GUID));
    Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(Argument, Request->Revision);
    Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(Argument, Request->Count);
    Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    ACPI_METHOD_SET_ARGUMENT_BUFFER(Argument, Request->Capabilities, (USHORT)CapsSize);

    Status = PpmLibEvaluate(Device, Input, Size, &Output);
    ExFreePoolWithTag(Input, PPMLIB_TAG);

    if (!NT_SUCCESS(Status))
    {
        PpmLibInvalidate(Device, PPMLIB_ERROR_OSC);
        return Status;
    }

    /* Error bits in the first dword void the handshake, it does not fall back to _PDC */
    if (Output != NULL)
    {
        Argument = Output->Argument;
        if (Output->Count != 0 &&
            Argument->DataLength >= sizeof(ULONG) &&
            ((*(PULONG)Argument->Data) & 0xE) == 0)
        {
            Device->OscReply = Output;
            Device->OscOutput = (PULONG)Argument->Data;
            Device->OscOutputLength = Argument->DataLength;
            Output = NULL;
        }

        if (Output)
            ExFreePoolWithTag(Output, PPMLIB_TAG);
    }

    return Status;
}

/**
 * @brief Runs _PDC with the vendor capability buffer.
 */
NTSTATUS
NTAPI
PpmLibEvaluatePdc(
    _In_ PPPMLIB_DEVICE Device,
    _In_ PPPMLIB_PDC_REQUEST Request,
    _In_ ULONG RequestSize)
{
    PACPI_EVAL_INPUT_BUFFER_COMPLEX Input;
    PACPI_EVAL_OUTPUT_BUFFER Output;
    ULONG Size;
    NTSTATUS Status;

    Size = FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX, Argument) + ACPI_METHOD_ARGUMENT_LENGTH(RequestSize);
    Input = ExAllocatePoolWithTag(PagedPool, Size, PPMLIB_TAG);
    if (Input == NULL)
    {
        PpmLibInvalidate(Device, PPMLIB_ERROR_PDC);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Input, Size);
    Input->Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    RtlCopyMemory(Input->MethodName, "_PDC", sizeof(Input->MethodName));
    Input->Size = Size;
    Input->ArgumentCount = 1;
    ACPI_METHOD_SET_ARGUMENT_BUFFER(Input->Argument, Request, (USHORT)RequestSize);

    Status = PpmLibEvaluate(Device, Input, Size, &Output);
    ExFreePoolWithTag(Input, PPMLIB_TAG);
    if (Output)
        ExFreePoolWithTag(Output, PPMLIB_TAG);

    if (!NT_SUCCESS(Status))
        PpmLibInvalidate(Device, PPMLIB_ERROR_PDC);

    return Status;
}

/**
 * @brief Reads _PCT, or _PTC when Throttle is set.
 */
NTSTATUS
NTAPI
PpmLibEvaluateControlPair(
    _In_ PPPMLIB_DEVICE Device,
    _In_ BOOLEAN Throttle,
    _Out_ PPPMLIB_CONTROL_PAIR Pair)
{
    PACPI_EVAL_OUTPUT_BUFFER Output;
    PPMLIB_CONTROL_PAIR Parsed;
    PPMLIB_ITEMS Items;
    NTSTATUS Status;

    Status = PpmLibEvaluateName(Device, Throttle ? "_PTC" : "_PCT", &Output);
    if (NT_SUCCESS(Status))
    {
        if (Output == NULL)
        {
            Status = STATUS_NOT_FOUND;
        }
        else
        {
            PpmLibCollectOutput(Output, &Items);
            Status = STATUS_SUCCESS;
            if (Items.Count >= 1)
                Status = PpmLibGetRegister(Items.Item[0], &Parsed.Control);
            if (NT_SUCCESS(Status) && Items.Count >= 2)
                Status = PpmLibGetRegister(Items.Item[1], &Parsed.Status);
            if (NT_SUCCESS(Status))
                Status = PpmLibCheckCount(&Items, 2, FALSE);

            if (NT_SUCCESS(Status))
            {
                PpmLibTranslateAccessSize(&Parsed.Control);
                PpmLibTranslateAccessSize(&Parsed.Status);
                *Pair = Parsed;
            }

            ExFreePoolWithTag(Output, PPMLIB_TAG);
        }
    }

    if (!NT_SUCCESS(Status))
        PpmLibInvalidate(Device, Throttle ? PPMLIB_ERROR_PTC : PPMLIB_ERROR_PCT);

    return Status;
}

/**
 * @brief Reads _PSS, each package is {Frequency, Power, Latency, BusMasterLatency, Control, Status}.
 */
NTSTATUS
NTAPI
PpmLibEvaluatePss(
    _In_ PPPMLIB_DEVICE Device,
    _Outptr_ PPPMLIB_PSS *Pss)
{
    PACPI_EVAL_OUTPUT_BUFFER Output;
    PACPI_METHOD_ARGUMENT Argument;
    PPPMLIB_PSS Table = NULL;
    NTSTATUS Status;
    PUCHAR End;
    ULONG Index;

    Status = PpmLibEvaluateName(Device, "_PSS", &Output);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Device->Found |= Device->Enabled & PPMLIB_PSS_MASK;
    if (Output == NULL || Output->Count == 0)
    {
        Status = STATUS_NOT_FOUND;
        goto Exit;
    }

    Table = ExAllocatePoolWithTag(NonPagedPoolNx,
                                  FIELD_OFFSET(PPMLIB_PSS, State[Output->Count]),
                                  PPMLIB_TAG);
    if (Table == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    RtlZeroMemory(Table, FIELD_OFFSET(PPMLIB_PSS, State[Output->Count]));
    Table->Count = (UCHAR)Output->Count;

    End = (PUCHAR)Output + Output->Length;
    Argument = (PACPI_METHOD_ARGUMENT)Output->Argument;
    for (Index = 0; Index < Output->Count; Index++)
    {
        if ((PUCHAR)Argument >= End || Argument->Type != ACPI_METHOD_ARGUMENT_PACKAGE)
        {
            Status = STATUS_ACPI_INVALID_ARGTYPE;
            goto Exit;
        }

        Status = PpmLibGetIntegers(Argument, 6, (PULONG)&Table->State[Index]);
        if (!NT_SUCCESS(Status))
            goto Exit;

        Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    }

    *Pss = Table;
    Table = NULL;

Exit:
    if (!NT_SUCCESS(Status))
        PpmLibInvalidate(Device, PPMLIB_ERROR_PSS);
    if (Table)
        ExFreePoolWithTag(Table, PPMLIB_TAG);
    if (Output)
        ExFreePoolWithTag(Output, PPMLIB_TAG);
    return Status;
}

/**
 * @brief Reads _TSS, each package is {Percent, Power, Latency, Control, Status}.
 */
NTSTATUS
NTAPI
PpmLibEvaluateTss(
    _In_ PPPMLIB_DEVICE Device,
    _Outptr_ PPPMLIB_TSS *Tss)
{
    PACPI_EVAL_OUTPUT_BUFFER Output;
    PACPI_METHOD_ARGUMENT Argument;
    PPPMLIB_TSS Table = NULL;
    NTSTATUS Status;
    PUCHAR End;
    ULONG Index;

    Status = PpmLibEvaluateName(Device, "_TSS", &Output);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Device->Found |= Device->Enabled & PPMLIB_TSS_MASK;
    if (Output == NULL || Output->Count == 0)
    {
        Status = STATUS_NOT_FOUND;
        goto Exit;
    }

    Table = ExAllocatePoolWithTag(NonPagedPoolNx,
                                  FIELD_OFFSET(PPMLIB_TSS, State[Output->Count]),
                                  PPMLIB_TAG);
    if (Table == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    RtlZeroMemory(Table, FIELD_OFFSET(PPMLIB_TSS, State[Output->Count]));
    Table->Count = Output->Count;

    End = (PUCHAR)Output + Output->Length;
    Argument = (PACPI_METHOD_ARGUMENT)Output->Argument;
    for (Index = 0; Index < Output->Count && (PUCHAR)Argument < End; Index++)
    {
        if (Argument->Type != ACPI_METHOD_ARGUMENT_PACKAGE)
        {
            Status = STATUS_ACPI_INVALID_ARGTYPE;
            goto Exit;
        }

        Status = PpmLibGetIntegers(Argument, 5, (PULONG)&Table->State[Index]);
        if (!NT_SUCCESS(Status))
            goto Exit;

        Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    }

    *Tss = Table;
    Table = NULL;

Exit:
    if (!NT_SUCCESS(Status))
        PpmLibInvalidate(Device, PPMLIB_ERROR_TSS);
    if (Table)
        ExFreePoolWithTag(Table, PPMLIB_TAG);
    if (Output)
        ExFreePoolWithTag(Output, PPMLIB_TAG);
    return Status;
}

/* _PPC and _TPC return one integer, an index limited to the table */
static
NTSTATUS
NTAPI
PpmLibEvaluateLimit(
    _In_ PPPMLIB_DEVICE Device,
    _In_reads_(4) PCSTR Name,
    _In_ ULONG TableCount,
    _In_ ULONG Error,
    _Out_ PULONG Limit)
{
    PACPI_EVAL_OUTPUT_BUFFER Output;
    PACPI_METHOD_ARGUMENT Argument;
    NTSTATUS Status;

    Status = PpmLibEvaluateName(Device, Name, &Output);
    if (NT_SUCCESS(Status))
    {
        Argument = Output ? (PACPI_METHOD_ARGUMENT)Output->Argument : NULL;
        if (Argument == NULL || Output->Count != 1 || Argument->Type != ACPI_METHOD_ARGUMENT_INTEGER)
        {
            Status = STATUS_NOT_FOUND;
        }
        else if (TableCount == 0)
        {
            Status = STATUS_UNSUCCESSFUL;
        }
        else
        {
            *Limit = min(Argument->Argument, TableCount - 1);
        }
    }

    if (Output)
        ExFreePoolWithTag(Output, PPMLIB_TAG);
    if (!NT_SUCCESS(Status))
        PpmLibInvalidate(Device, Error);

    return Status;
}

NTSTATUS
NTAPI
PpmLibEvaluatePpc(
    _In_ PPPMLIB_DEVICE Device,
    _Out_ PULONG Ppc)
{
    return PpmLibEvaluateLimit(Device,
                               "_PPC",
                               Device->Pss ? Device->Pss->Count : 0,
                               PPMLIB_ERROR_PPC,
                               Ppc);
}

NTSTATUS
NTAPI
PpmLibEvaluateTpc(
    _In_ PPPMLIB_DEVICE Device,
    _Out_ PULONG Tpc)
{
    return PpmLibEvaluateLimit(Device,
                               "_TPC",
                               Device->Tss ? Device->Tss->Count : 0,
                               PPMLIB_ERROR_TPC,
                               Tpc);
}

/* An enabled feature bit of either form is needed to keep a state of that class */
static
BOOLEAN
NTAPI
PpmLibCStateWanted(
    _In_ PPPMLIB_DEVICE Device,
    _In_ UCHAR Type)
{
    if (Type == 1)
        return (Device->Enabled & (PPMLIB_CST_C1 | PPMLIB_NATIVE_C1)) != 0;
    if (Type == 2)
        return (Device->Enabled & (PPMLIB_CST_C2 | PPMLIB_NATIVE_C2)) != 0;
    if (Type >= 3)
        return (Device->Enabled & (PPMLIB_CST_C3 | PPMLIB_NATIVE_C3)) != 0;

    return TRUE;
}

/**
 * @brief Reads _CST: {Count, {Register, Type, Latency, Power}...}.
 *
 * @remarks States of a disabled class are dropped, not rejected.
 */
NTSTATUS
NTAPI
PpmLibEvaluateCst(
    _In_ PPPMLIB_DEVICE Device,
    _Outptr_ PPPMLIB_CST *Cst)
{
    PACPI_EVAL_OUTPUT_BUFFER Output;
    PACPI_METHOD_ARGUMENT Argument;
    PPMLIB_CSTATE Entry;
    PPMLIB_ITEMS Fields;
    PPPMLIB_CST Table = NULL;
    ULONG Declared;
    ULONG Value;
    ULONG Seen;
    NTSTATUS Status;
    PUCHAR End;

    Status = PpmLibEvaluateName(Device, "_CST", &Output);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Device->Found |= Device->Enabled & PPMLIB_CST_IDLE;
    if (Output == NULL || Output->Count == 0)
    {
        Status = STATUS_NOT_FOUND;
        goto Exit;
    }

    Argument = (PACPI_METHOD_ARGUMENT)Output->Argument;
    if (Argument->Type != ACPI_METHOD_ARGUMENT_INTEGER)
    {
        Status = STATUS_ACPI_INVALID_ARGTYPE;
        goto Exit;
    }

    Declared = Argument->Argument;
    if (Declared == 0)
    {
        Status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }

    Table = ExAllocatePoolWithTag(NonPagedPoolNx, FIELD_OFFSET(PPMLIB_CST, State[Declared]), PPMLIB_TAG);
    if (Table == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    RtlZeroMemory(Table, FIELD_OFFSET(PPMLIB_CST, State[Declared]));

    End = (PUCHAR)Output + Output->Length;
    Seen = 0;
    for (Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
         (PUCHAR)Argument < End;
         Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument))
    {
        /* More states than the count promised is malformed */
        if (Seen == Declared || Argument->Type != ACPI_METHOD_ARGUMENT_PACKAGE)
        {
            Status = STATUS_ACPI_INVALID_ARGTYPE;
            goto Exit;
        }
        Seen++;

        RtlZeroMemory(&Entry, sizeof(Entry));
        PpmLibCollectPackage(Argument, &Fields);
        Status = PpmLibCheckCount(&Fields, 4, FALSE);
        if (NT_SUCCESS(Status))
            Status = PpmLibGetRegister(Fields.Item[0], &Entry.Register);
        if (NT_SUCCESS(Status))
        {
            Status = PpmLibGetInteger(Fields.Item[1], &Value);
            Entry.Type = (UCHAR)Value;
        }
        if (NT_SUCCESS(Status))
        {
            Status = PpmLibGetInteger(Fields.Item[2], &Value);
            Entry.Latency = (USHORT)Value;
        }
        if (NT_SUCCESS(Status))
            Status = PpmLibGetInteger(Fields.Item[3], &Entry.Power);

        if (!NT_SUCCESS(Status))
        {
            Status = STATUS_ACPI_INVALID_DATA;
            goto Exit;
        }

        if (Entry.Register.SpaceId != PPMLIB_SPACE_FFH)
            PpmLibTranslateAccessSize(&Entry.Register);

        if (PpmLibCStateWanted(Device, Entry.Type))
            Table->State[Table->Count++] = Entry;
    }

    if (Table->Count == 0)
    {
        Status = STATUS_UNSUCCESSFUL;
        goto Exit;
    }

    Table->Count = (UCHAR)Table->Count;
    *Cst = Table;
    Table = NULL;

Exit:
    if (!NT_SUCCESS(Status))
        PpmLibInvalidate(Device, PPMLIB_ERROR_CST);
    if (Table)
        ExFreePoolWithTag(Table, PPMLIB_TAG);
    if (Output)
        ExFreePoolWithTag(Output, PPMLIB_TAG);
    return Status;
}

/**
 * @brief Reads _CSD, each package is {Entries, Revision, Domain, Coordination, Processors, Index}.
 */
NTSTATUS
NTAPI
PpmLibEvaluateCsd(
    _In_ PPPMLIB_DEVICE Device,
    _Outptr_ PPPMLIB_CSD *Csd)
{
    PACPI_EVAL_OUTPUT_BUFFER Output;
    PACPI_METHOD_ARGUMENT Argument;
    PPPMLIB_CSD Table = NULL;
    NTSTATUS Status;
    PUCHAR End;
    ULONG Index;

    Status = PpmLibEvaluateName(Device, "_CSD", &Output);
    if (!NT_SUCCESS(Status))
        goto Exit;

    if (Output == NULL || Output->Count == 0)
    {
        Status = STATUS_NOT_FOUND;
        goto Exit;
    }

    Table = ExAllocatePoolWithTag(NonPagedPoolNx, FIELD_OFFSET(PPMLIB_CSD, Entry[Output->Count]), PPMLIB_TAG);
    if (Table == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    RtlZeroMemory(Table, FIELD_OFFSET(PPMLIB_CSD, Entry[Output->Count]));
    Table->Count = (UCHAR)Output->Count;

    End = (PUCHAR)Output + Output->Length;
    Argument = (PACPI_METHOD_ARGUMENT)Output->Argument;
    for (Index = 0; Index < Output->Count && (PUCHAR)Argument < End; Index++)
    {
        if (Argument->Type != ACPI_METHOD_ARGUMENT_PACKAGE)
        {
            Status = STATUS_ACPI_INVALID_ARGTYPE;
            goto Exit;
        }

        Status = PpmLibGetIntegers(Argument, 6, (PULONG)&Table->Entry[Index]);
        if (!NT_SUCCESS(Status))
            goto Exit;

        Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    }

    *Csd = Table;
    Table = NULL;

Exit:
    if (!NT_SUCCESS(Status))
        PpmLibInvalidate(Device, PPMLIB_ERROR_CSD);
    if (Table)
        ExFreePoolWithTag(Table, PPMLIB_TAG);
    if (Output)
        ExFreePoolWithTag(Output, PPMLIB_TAG);
    return Status;
}

/**
 * @brief Reads _PSD, or _TSD when Throttle is set.
 */
NTSTATUS
NTAPI
PpmLibEvaluateDependency(
    _In_ PPPMLIB_DEVICE Device,
    _In_ BOOLEAN Throttle,
    _Outptr_ PPPMLIB_DEPENDENCY_LIST *List)
{
    PACPI_EVAL_OUTPUT_BUFFER Output;
    PACPI_METHOD_ARGUMENT Argument;
    PPPMLIB_DEPENDENCY_LIST Table = NULL;
    NTSTATUS Status;
    PUCHAR End;
    ULONG Index;

    Status = PpmLibEvaluateName(Device, Throttle ? "_TSD" : "_PSD", &Output);
    if (!NT_SUCCESS(Status))
        goto Exit;

    if (Output == NULL || Output->Count == 0)
    {
        Status = STATUS_NOT_FOUND;
        goto Exit;
    }

    Table = ExAllocatePoolWithTag(NonPagedPoolNx,
                                  FIELD_OFFSET(PPMLIB_DEPENDENCY_LIST, Entry[Output->Count]),
                                  PPMLIB_TAG);
    if (Table == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    RtlZeroMemory(Table, FIELD_OFFSET(PPMLIB_DEPENDENCY_LIST, Entry[Output->Count]));
    Table->Count = (UCHAR)Output->Count;

    End = (PUCHAR)Output + Output->Length;
    Argument = (PACPI_METHOD_ARGUMENT)Output->Argument;
    for (Index = 0; Index < Output->Count && (PUCHAR)Argument < End; Index++)
    {
        if (Argument->Type != ACPI_METHOD_ARGUMENT_PACKAGE)
        {
            Status = STATUS_ACPI_INVALID_ARGTYPE;
            goto Exit;
        }

        Status = PpmLibGetIntegers(Argument, 5, (PULONG)&Table->Entry[Index]);
        if (!NT_SUCCESS(Status))
            goto Exit;

        Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    }

    *List = Table;
    Table = NULL;

Exit:
    if (!NT_SUCCESS(Status))
        PpmLibInvalidate(Device, Throttle ? PPMLIB_ERROR_TSD : PPMLIB_ERROR_PSD);
    if (Table)
        ExFreePoolWithTag(Table, PPMLIB_TAG);
    if (Output)
        ExFreePoolWithTag(Output, PPMLIB_TAG);
    return Status;
}

/* _CPC element order after NumEntries and Revision */
static const ULONG PpmLibCpcLayout[] =
{
    FIELD_OFFSET(PPMLIB_CPC, Highest),
    FIELD_OFFSET(PPMLIB_CPC, Nominal),
    FIELD_OFFSET(PPMLIB_CPC, LowestNonlinear),
    FIELD_OFFSET(PPMLIB_CPC, Lowest),
    FIELD_OFFSET(PPMLIB_CPC, Guaranteed),
    FIELD_OFFSET(PPMLIB_CPC, Desired),
    FIELD_OFFSET(PPMLIB_CPC, Minimum),
    FIELD_OFFSET(PPMLIB_CPC, Maximum),
    FIELD_OFFSET(PPMLIB_CPC, Tolerance),
    FIELD_OFFSET(PPMLIB_CPC, TimeWindow),
    FIELD_OFFSET(PPMLIB_CPC, Wraparound),
    FIELD_OFFSET(PPMLIB_CPC, ReferenceCounter),
    FIELD_OFFSET(PPMLIB_CPC, DeliveredCounter),
    FIELD_OFFSET(PPMLIB_CPC, Limited),
    FIELD_OFFSET(PPMLIB_CPC, Enable),
    FIELD_OFFSET(PPMLIB_CPC, AutonomousEnable),
    FIELD_OFFSET(PPMLIB_CPC, ActivityWindow),
    FIELD_OFFSET(PPMLIB_CPC, EnergyPreference),
    FIELD_OFFSET(PPMLIB_CPC, ReferencePerformance),
};

/* Elements that may be given as a plain integer */
static
BOOLEAN
NTAPI
PpmLibCpcValueAllowed(
    _In_ ULONG Offset)
{
    return (Offset == FIELD_OFFSET(PPMLIB_CPC, Highest) ||
            Offset == FIELD_OFFSET(PPMLIB_CPC, Nominal) ||
            Offset == FIELD_OFFSET(PPMLIB_CPC, LowestNonlinear) ||
            Offset == FIELD_OFFSET(PPMLIB_CPC, Lowest) ||
            Offset == FIELD_OFFSET(PPMLIB_CPC, AutonomousEnable) ||
            Offset == FIELD_OFFSET(PPMLIB_CPC, ReferencePerformance));
}

/**
 * @brief Reads _CPC. Revision 1 has 17 elements, revision 2 has 21, later
 * revisions may append more.
 */
NTSTATUS
NTAPI
PpmLibEvaluateCpc(
    _In_ PPPMLIB_DEVICE Device,
    _Outptr_ PPPMLIB_CPC *Cpc)
{
    PACPI_EVAL_OUTPUT_BUFFER Output;
    PPPMLIB_CPC Table = NULL;
    PPMLIB_ITEMS Items;
    PPPMLIB_GAS Register;
    ULONG RegisterCount;
    BOOLEAN AllowExtra;
    NTSTATUS Status;
    ULONG Index;

    Status = PpmLibEvaluateName(Device, "_CPC", &Output);
    if (!NT_SUCCESS(Status))
        goto Exit;

    Device->Found |= Device->Enabled & PPMLIB_CPPC;
    if (Output == NULL)
    {
        Status = STATUS_NOT_FOUND;
        goto Exit;
    }

    Table = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*Table), PPMLIB_TAG);
    if (Table == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    RtlZeroMemory(Table, sizeof(*Table));
    PpmLibCollectOutput(Output, &Items);

    /* The revision decides how many elements follow */
    Status = (Items.Count >= 1) ? PpmLibGetInteger(Items.Item[0], &Table->Entries) : STATUS_SUCCESS;
    if (NT_SUCCESS(Status) && Items.Count >= 2)
        Status = PpmLibGetInteger(Items.Item[1], &Table->Revision);
    if (NT_SUCCESS(Status))
        Status = PpmLibCheckCount(&Items, 2, TRUE);
    if (!NT_SUCCESS(Status))
        goto Exit;

    if (Table->Revision == 0)
    {
        Status = STATUS_ACPI_INVALID_DATA;
        goto Exit;
    }

    RegisterCount = (Table->Revision == 1) ? 15 : RTL_NUMBER_OF(PpmLibCpcLayout);
    AllowExtra = (Table->Revision > 2);

    for (Index = 0; Index < RegisterCount && Index + 2 < min(Items.Count, PPMLIB_MAX_ITEMS); Index++)
    {
        Register = (PPPMLIB_GAS)((PUCHAR)Table + PpmLibCpcLayout[Index]);
        if (PpmLibCpcValueAllowed(PpmLibCpcLayout[Index]))
            Status = PpmLibGetRegisterOrValue(Items.Item[Index + 2], Register);
        else
            Status = PpmLibGetRegister(Items.Item[Index + 2], Register);

        if (!NT_SUCCESS(Status))
            goto Exit;
    }

    Status = PpmLibCheckCount(&Items, RegisterCount + 2, AllowExtra);
    if (!NT_SUCCESS(Status))
        goto Exit;

    for (Index = 0; Index < RegisterCount; Index++)
        PpmLibTranslateAccessSize((PPPMLIB_GAS)((PUCHAR)Table + PpmLibCpcLayout[Index]));

    *Cpc = Table;
    Table = NULL;

Exit:
    if (!NT_SUCCESS(Status))
        PpmLibInvalidate(Device, PPMLIB_ERROR_CPC);
    if (Table)
        ExFreePoolWithTag(Table, PPMLIB_TAG);
    if (Output)
        ExFreePoolWithTag(Output, PPMLIB_TAG);
    return Status;
}

/**
 * @brief Gets the ACPI driver's standard interface and hooks device notifications.
 */
NTSTATUS
NTAPI
PpmLibAcquireAcpiInterface(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PIO_STACK_LOCATION Stack;
    IO_STATUS_BLOCK IoStatus;
    PDEVICE_OBJECT Target;
    NTSTATUS Status;
    KEVENT Event;
    PIRP Irp;

    PAGED_CODE();

    RtlZeroMemory(&Device->AcpiInterface, sizeof(Device->AcpiInterface));

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Target = Device->LowerDevice;
    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, Target, NULL, 0, NULL, &Event, &IoStatus);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    Stack = IoGetNextIrpStackLocation(Irp);
    Stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    Stack->Parameters.QueryInterface.InterfaceType = &GUID_ACPI_INTERFACE_STANDARD;
    Stack->Parameters.QueryInterface.Size = sizeof(Device->AcpiInterface);
    Stack->Parameters.QueryInterface.Version = 1;
    Stack->Parameters.QueryInterface.Interface = (PINTERFACE)&Device->AcpiInterface;
    Stack->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    Status = IoCallDriver(Target, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = IoStatus.Status;
    }

    if (!NT_SUCCESS(Status))
        return Status;

    Device->AcpiInterfaceHeld = TRUE;
    if (Device->AcpiInterface.RegisterForDeviceNotifications == NULL)
        return STATUS_NOT_SUPPORTED;

    Status = Device->AcpiInterface.RegisterForDeviceNotifications(Device->Pdo, PpmLibAcpiNotify, Device);
    if (!NT_SUCCESS(Status))
        return Status;

    Device->NotifyRegistered = TRUE;
    return STATUS_SUCCESS;
}

/**
 * @brief Drops the notification hook and the interface reference.
 */
VOID
NTAPI
PpmLibReleaseAcpiInterface(
    _Inout_ PPPMLIB_DEVICE Device)
{
    if (Device->NotifyRegistered && Device->AcpiInterface.UnregisterForDeviceNotifications)
        Device->AcpiInterface.UnregisterForDeviceNotifications(Device->Pdo, PpmLibAcpiNotify);

    if (Device->AcpiInterfaceHeld && Device->AcpiInterface.InterfaceDereference)
        Device->AcpiInterface.InterfaceDereference(Device->AcpiInterface.Context);

    Device->NotifyRegistered = FALSE;
    Device->AcpiInterfaceHeld = FALSE;
}

/* EOF */
