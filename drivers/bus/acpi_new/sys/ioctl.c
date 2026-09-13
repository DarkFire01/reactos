/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ACPI IOCTLs on PDOs and filter DOs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"
#include <acpiioct.h>      // the NTDDI-pinning wrapper in build_support/include/ucx
#include <uacpi/internal/namespace.h>   // uacpi_namespace_node_get_object_typed
#include <uacpi/tables.h>               // MADT walk for the InitialApicId
#include <uacpi/acpi.h>

// Package-nesting depth guard for both directions of conversion.
#define UACPI_EVAL_MAX_PACKAGE_DEPTH 8

// Input side: ACPI_METHOD_ARGUMENT(s) -> uacpi_object(s)

static VOID
UacpipFreeObjectArray(uacpi_object_array *Arr)
{
    uacpi_size i;
    if (Arr->objects == NULL)
    {
        return;
    }
    for (i = 0; i < Arr->count; i++)
    {
        if (Arr->objects[i] != NULL)
        {
            uacpi_object_unref(Arr->objects[i]);
        }
    }
    ExFreePoolWithTag(Arr->objects, UACPI_POOL_TAG);
    Arr->objects = NULL;
    Arr->count = 0;
}

static NTSTATUS
UacpipArgumentsToObjects(PACPI_METHOD_ARGUMENT First, ULONG Count,
                        ULONG BytesAvailable, ULONG Depth,
                        uacpi_object_array *Out);

// One wire argument -> one uACPI object.  *Consumed reports the wire size.
static NTSTATUS
UacpipArgumentToObject(PACPI_METHOD_ARGUMENT Arg, ULONG BytesAvailable,
                      ULONG Depth, uacpi_object **Out, PULONG Consumed)
{
    ULONG argLen;
    uacpi_data_view view;

    if (BytesAvailable < (ULONG)FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data))
    {
        return STATUS_INVALID_PARAMETER;
    }
    argLen = ACPI_METHOD_ARGUMENT_LENGTH(Arg->DataLength);
    if (argLen > BytesAvailable)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *Consumed = argLen;

    switch (Arg->Type)
    {

    case ACPI_METHOD_ARGUMENT_INTEGER:
        *Out = uacpi_object_create_integer(Arg->Argument);
        break;

    case ACPI_METHOD_ARGUMENT_STRING:
        // DataLength includes the NUL; verify it.
        if (Arg->DataLength == 0 || Arg->Data[Arg->DataLength - 1] != '\0')
        {
            return STATUS_INVALID_PARAMETER;
        }
        *Out = uacpi_object_create_cstring((const uacpi_char *)Arg->Data);
        break;

    case ACPI_METHOD_ARGUMENT_BUFFER:
        view.bytes  = (uacpi_u8 *)Arg->Data;
        view.length = Arg->DataLength;
        *Out = uacpi_object_create_buffer(view);
        break;

    case ACPI_METHOD_ARGUMENT_PACKAGE:
    {
        // Data holds nested arguments back-to-back; count, recurse, wrap.
        uacpi_object_array nested = { 0 };
        PACPI_METHOD_ARGUMENT walk;
        ULONG remaining, n = 0;
        NTSTATUS status;

        if (Depth >= UACPI_EVAL_MAX_PACKAGE_DEPTH)
        {
            return STATUS_INVALID_PARAMETER;
        }
        remaining = Arg->DataLength;
        walk = (PACPI_METHOD_ARGUMENT)Arg->Data;
        while (remaining >= (ULONG)FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data))
        {
            ULONG l = ACPI_METHOD_ARGUMENT_LENGTH(walk->DataLength);
            if (l > remaining)
            {
                return STATUS_INVALID_PARAMETER;
            }
            n++;
            remaining -= l;
            walk = ACPI_METHOD_NEXT_ARGUMENT(walk);
        }

        status = UacpipArgumentsToObjects((PACPI_METHOD_ARGUMENT)Arg->Data, n,
                                         Arg->DataLength, Depth + 1, &nested);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        *Out = uacpi_object_create_package(nested);
        UacpipFreeObjectArray(&nested);   // package took its own references
        break;
    }

    default:
        return STATUS_INVALID_PARAMETER;
    }

    return (*Out != NULL) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

static NTSTATUS
UacpipArgumentsToObjects(PACPI_METHOD_ARGUMENT First, ULONG Count,
                        ULONG BytesAvailable, ULONG Depth,
                        uacpi_object_array *Out)
{
    PACPI_METHOD_ARGUMENT arg = First;
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;

    Out->objects = NULL;
    Out->count = 0;
    if (Count == 0)
    {
        return STATUS_SUCCESS;
    }

    Out->objects = (uacpi_object **)ExAllocatePoolWithTag(
                       NonPagedPool, Count * sizeof(uacpi_object *), UACPI_POOL_TAG);
    if (Out->objects == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Out->objects, Count * sizeof(uacpi_object *));
    Out->count = Count;

    for (i = 0; i < Count; i++)
    {
        ULONG consumed = 0;
        status = UacpipArgumentToObject(arg, BytesAvailable, Depth,
                                       &Out->objects[i], &consumed);
        if (!NT_SUCCESS(status))
        {
            UacpipFreeObjectArray(Out);
            return status;
        }
        BytesAvailable -= consumed;
        arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
    }
    return STATUS_SUCCESS;
}

// Output side: wire size of a uacpi_object's serialization.
static NTSTATUS
UacpipObjectWireSize(uacpi_object *Obj, ULONG Depth, PULONG Size)
{
    uacpi_data_view view;
    uacpi_object_array pkg;
    NTSTATUS status;
    ULONG total, i;

    switch (uacpi_object_get_type(Obj))
    {

    case UACPI_OBJECT_INTEGER:
        *Size = ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG));
        return STATUS_SUCCESS;

    case UACPI_OBJECT_STRING:
        if (uacpi_unlikely_error(uacpi_object_get_string(Obj, &view)))
        {
            return STATUS_UNSUCCESSFUL;
        }
        if (view.length + 1 > MAXUSHORT)
        {
            return STATUS_ACPI_INVALID_DATA;   // DataLength is a USHORT
        }
        // Wire strings carry the NUL (the view length excludes it).
        *Size = ACPI_METHOD_ARGUMENT_LENGTH((ULONG)view.length + 1);
        return STATUS_SUCCESS;

    case UACPI_OBJECT_BUFFER:
        if (uacpi_unlikely_error(uacpi_object_get_buffer(Obj, &view)))
        {
            return STATUS_UNSUCCESSFUL;
        }
        if (view.length > MAXUSHORT)
        {
            return STATUS_ACPI_INVALID_DATA;   // DataLength is a USHORT
        }
        *Size = ACPI_METHOD_ARGUMENT_LENGTH((ULONG)view.length);
        return STATUS_SUCCESS;

    case UACPI_OBJECT_PACKAGE:
        if (Depth >= UACPI_EVAL_MAX_PACKAGE_DEPTH)
        {
            return STATUS_INVALID_PARAMETER;
        }
        if (uacpi_unlikely_error(uacpi_object_get_package(Obj, &pkg)))
        {
            return STATUS_UNSUCCESSFUL;
        }
        total = 0;
        for (i = 0; i < pkg.count; i++)
        {
            ULONG s;
            status = UacpipObjectWireSize(pkg.objects[i], Depth + 1, &s);
            if (!NT_SUCCESS(status))
            {
                return status;
            }
            total += s;
        }
        if (total > MAXUSHORT)
        {
            return STATUS_ACPI_INVALID_DATA;   // DataLength is a USHORT
        }
        *Size = ACPI_METHOD_ARGUMENT_LENGTH(total);
        return STATUS_SUCCESS;

    default:
        // Other object types (fields, mutexes, etc.) have no wire form.
        return STATUS_ACPI_INVALID_DATA;
    }
}

static NTSTATUS
UacpipObjectToArgument(uacpi_object *Obj, ULONG Depth,
                      PACPI_METHOD_ARGUMENT Arg, PULONG Written)
{
    uacpi_data_view view;
    uacpi_object_array pkg;
    uacpi_u64 value;
    NTSTATUS status;
    ULONG i;

    switch (uacpi_object_get_type(Obj))
    {

    case UACPI_OBJECT_INTEGER:
        (void)uacpi_object_get_integer(Obj, &value);
        // V1 wire integers are 32-bit; 64-bit values are truncated.
        Arg->Type = ACPI_METHOD_ARGUMENT_INTEGER;
        Arg->DataLength = sizeof(ULONG);
        Arg->Argument = (ULONG)value;
        break;

    case UACPI_OBJECT_STRING:
        if (uacpi_unlikely_error(uacpi_object_get_string(Obj, &view)))
        {
            return STATUS_UNSUCCESSFUL;
        }
        Arg->Type = ACPI_METHOD_ARGUMENT_STRING;
        Arg->DataLength = (USHORT)(view.length + 1);
        RtlCopyMemory(Arg->Data, view.const_bytes, view.length);
        Arg->Data[view.length] = '\0';
        break;

    case UACPI_OBJECT_BUFFER:
        if (uacpi_unlikely_error(uacpi_object_get_buffer(Obj, &view)))
        {
            return STATUS_UNSUCCESSFUL;
        }
        Arg->Type = ACPI_METHOD_ARGUMENT_BUFFER;
        Arg->DataLength = (USHORT)view.length;
        RtlCopyMemory(Arg->Data, view.const_bytes, view.length);
        break;

    case UACPI_OBJECT_PACKAGE:
    {
        PACPI_METHOD_ARGUMENT nested;
        ULONG total = 0;

        if (uacpi_unlikely_error(uacpi_object_get_package(Obj, &pkg)))
        {
            return STATUS_UNSUCCESSFUL;
        }
        nested = (PACPI_METHOD_ARGUMENT)Arg->Data;
        for (i = 0; i < pkg.count; i++)
        {
            ULONG w = 0;
            status = UacpipObjectToArgument(pkg.objects[i], Depth + 1, nested, &w);
            if (!NT_SUCCESS(status))
            {
                return status;
            }
            total += w;
            nested = (PACPI_METHOD_ARGUMENT)((PUCHAR)nested + w);
        }
        Arg->Type = ACPI_METHOD_ARGUMENT_PACKAGE;
        Arg->DataLength = (USHORT)total;
        break;
    }

    default:
        return STATUS_ACPI_INVALID_DATA;
    }

    *Written = ACPI_METHOD_ARGUMENT_LENGTH(Arg->DataLength);
    return STATUS_SUCCESS;
}

// Evaluation core for the eval IOCTLs; serves PDOs and filter DOs.
static NTSTATUS
UacpipEvalIoctl(uacpi_namespace_node *Node, const char *TraceName,
               PVOID InBuf, ULONG InLen,
               PVOID OutBuf, ULONG OutLen, PULONG_PTR Information)
{
    CHAR path[260];
    uacpi_object_array args = { 0 };
    uacpi_object *ret = NULL;
    uacpi_status ust;
    NTSTATUS status;
    ULONG sig;

    *Information = 0;

    if (Node == NULL || g_AcpiFdo == NULL || !g_AcpiFdo->UacpiUp)
    {
        return STATUS_DEVICE_NOT_READY;
    }
    if (InLen < sizeof(ULONG))
    {
        return STATUS_INVALID_PARAMETER;
    }
    sig = *(ULONG UNALIGNED *)InBuf;

    // decode the input buffer by signature
    switch (sig)
    {

    case ACPI_EVAL_INPUT_BUFFER_SIGNATURE:
    {
        PACPI_EVAL_INPUT_BUFFER in = (PACPI_EVAL_INPUT_BUFFER)InBuf;
        if (InLen < sizeof(*in))
        {
            return STATUS_INVALID_PARAMETER;
        }
        RtlCopyMemory(path, in->MethodName, 4);
        path[4] = '\0';
        break;
    }

    case ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE:
    {
        PACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER in =
            (PACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER)InBuf;
        if (InLen < sizeof(*in))
        {
            return STATUS_INVALID_PARAMETER;
        }
        RtlCopyMemory(path, in->MethodName, 4);
        path[4] = '\0';

        args.objects = (uacpi_object **)ExAllocatePoolWithTag(
                           NonPagedPool, sizeof(uacpi_object *), UACPI_POOL_TAG);
        if (args.objects == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        args.objects[0] = uacpi_object_create_integer(in->IntegerArgument);
        args.count = 1;
        if (args.objects[0] == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto done;
        }
        break;
    }

    case ACPI_EVAL_INPUT_BUFFER_SIMPLE_STRING_SIGNATURE:
    {
        PACPI_EVAL_INPUT_BUFFER_SIMPLE_STRING in =
            (PACPI_EVAL_INPUT_BUFFER_SIMPLE_STRING)InBuf;
        if (InLen < (ULONG)FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_SIMPLE_STRING, String) ||
            InLen < (ULONG)FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_SIMPLE_STRING, String) +
                    in->StringLength ||
            in->StringLength == 0 ||
            in->String[in->StringLength - 1] != '\0')
            {
            return STATUS_INVALID_PARAMETER;
        }
        RtlCopyMemory(path, in->MethodName, 4);
        path[4] = '\0';

        args.objects = (uacpi_object **)ExAllocatePoolWithTag(
                           NonPagedPool, sizeof(uacpi_object *), UACPI_POOL_TAG);
        if (args.objects == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        args.objects[0] = uacpi_object_create_cstring((const uacpi_char *)in->String);
        args.count = 1;
        if (args.objects[0] == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto done;
        }
        break;
    }

    case ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE:
    {
        PACPI_EVAL_INPUT_BUFFER_COMPLEX in =
            (PACPI_EVAL_INPUT_BUFFER_COMPLEX)InBuf;
        if (InLen < (ULONG)FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX, Argument) ||
            InLen < in->Size)
            {
            return STATUS_INVALID_PARAMETER;
        }
        RtlCopyMemory(path, in->MethodName, 4);
        path[4] = '\0';

        status = UacpipArgumentsToObjects(
                     (PACPI_METHOD_ARGUMENT)in->Argument, in->ArgumentCount,
                     InLen - FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX, Argument),
                     0, &args);
        if (!NT_SUCCESS(status))
        {
            goto done;
        }
        break;
    }

    // the _EX variants (NUL-terminated, possibly absolute, path)
    case ACPI_EVAL_INPUT_BUFFER_SIGNATURE_EX:
    {
        PACPI_EVAL_INPUT_BUFFER_EX in = (PACPI_EVAL_INPUT_BUFFER_EX)InBuf;
        if (InLen < sizeof(*in))
        {
            return STATUS_INVALID_PARAMETER;
        }
        RtlStringCbCopyA(path, sizeof(path), in->MethodName);
        break;
    }

    case ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE_EX:
    {
        PACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_EX in =
            (PACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_EX)InBuf;
        if (InLen < sizeof(*in))
        {
            return STATUS_INVALID_PARAMETER;
        }
        RtlStringCbCopyA(path, sizeof(path), in->MethodName);

        args.objects = (uacpi_object **)ExAllocatePoolWithTag(
                           NonPagedPool, sizeof(uacpi_object *), UACPI_POOL_TAG);
        if (args.objects == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        args.objects[0] = uacpi_object_create_integer(in->IntegerArgument);
        args.count = 1;
        if (args.objects[0] == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto done;
        }
        break;
    }

    case ACPI_EVAL_INPUT_BUFFER_SIMPLE_STRING_SIGNATURE_EX:
    {
        PACPI_EVAL_INPUT_BUFFER_SIMPLE_STRING_EX in =
            (PACPI_EVAL_INPUT_BUFFER_SIMPLE_STRING_EX)InBuf;
        if (InLen < (ULONG)FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_SIMPLE_STRING_EX, String) ||
            InLen < (ULONG)FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_SIMPLE_STRING_EX, String) +
                    in->StringLength ||
            in->StringLength == 0 ||
            in->String[in->StringLength - 1] != '\0')
            {
            return STATUS_INVALID_PARAMETER;
        }
        RtlStringCbCopyA(path, sizeof(path), in->MethodName);

        args.objects = (uacpi_object **)ExAllocatePoolWithTag(
                           NonPagedPool, sizeof(uacpi_object *), UACPI_POOL_TAG);
        if (args.objects == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        args.objects[0] = uacpi_object_create_cstring((const uacpi_char *)in->String);
        args.count = 1;
        if (args.objects[0] == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto done;
        }
        break;
    }

    case ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE_EX:
    {
        PACPI_EVAL_INPUT_BUFFER_COMPLEX_EX in =
            (PACPI_EVAL_INPUT_BUFFER_COMPLEX_EX)InBuf;
        if (InLen < (ULONG)FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX_EX, Argument) ||
            InLen < in->Size)
            {
            return STATUS_INVALID_PARAMETER;
        }
        RtlStringCbCopyA(path, sizeof(path), in->MethodName);

        status = UacpipArgumentsToObjects(
                     (PACPI_METHOD_ARGUMENT)in->Argument, in->ArgumentCount,
                     InLen - FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX_EX, Argument),
                     0, &args);
        if (!NT_SUCCESS(status))
        {
            goto done;
        }
        break;
    }

    default:
        return STATUS_INVALID_PARAMETER;
    }

    // Trim trailing space padding from a 4-char name.
    {
        int i;
        for (i = (int)strlen(path) - 1; i >= 0 && path[i] == ' '; i--)
        {
            path[i] = '\0';
        }
    }

    // evaluate
    UacpiTrace("[acpi] EVAL %s on %s\n", path, TraceName);
    ust = uacpi_eval(Node, path, args.count ? &args : NULL, &ret);
    if (uacpi_unlikely_error(ust))
    {
        UacpiTrace("[acpi] EVAL %s failed: %s\n", path, uacpi_status_to_string(ust));
        status = (ust == UACPI_STATUS_NOT_FOUND) ? STATUS_OBJECT_NAME_NOT_FOUND
                                                 : STATUS_UNSUCCESSFUL;
        goto done;
    }

    // serialize the result
    if (ret == NULL)
    {
        status = STATUS_SUCCESS;     // method returned nothing
        goto done;
    }

    {
        PACPI_EVAL_OUTPUT_BUFFER out = (PACPI_EVAL_OUTPUT_BUFFER)OutBuf;
        ULONG required = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER_V1, Argument);
        ULONG count, i;
        uacpi_object_array pkg;
        BOOLEAN flatten = FALSE;

        // A top-level package is flattened into Count arguments.
        if (uacpi_object_get_type(ret) == UACPI_OBJECT_PACKAGE &&
            uacpi_likely_success(uacpi_object_get_package(ret, &pkg)))
            {
            flatten = TRUE;
            count = (ULONG)pkg.count;
            for (i = 0; i < count; i++)
            {
                ULONG s;
                status = UacpipObjectWireSize(pkg.objects[i], 1, &s);
                if (!NT_SUCCESS(status))
                {
                    goto done;
                }
                required += s;
            }
        }
        else
        {
            ULONG s;
            count = 1;
            status = UacpipObjectWireSize(ret, 0, &s);
            if (!NT_SUCCESS(status))
            {
                goto done;
            }
            required += s;
        }

        if (OutBuf == NULL ||
            OutLen < (ULONG)FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER_V1, Argument))
            {
            status = STATUS_BUFFER_TOO_SMALL;
            goto done;
        }
        out->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE;
        out->Length    = required;
        if (OutLen < required)
        {
            // Two-pass client sizing: report the requirement and overflow.
            out->Count = 0;
            *Information = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER_V1, Argument);
            status = STATUS_BUFFER_OVERFLOW;
            goto done;
        }
        out->Count = count;

        {
            PACPI_METHOD_ARGUMENT arg = (PACPI_METHOD_ARGUMENT)out->Argument;
            if (flatten)
            {
                for (i = 0; i < count; i++)
                {
                    ULONG w = 0;
                    status = UacpipObjectToArgument(pkg.objects[i], 1, arg, &w);
                    if (!NT_SUCCESS(status))
                    {
                        goto done;
                    }
                    arg = (PACPI_METHOD_ARGUMENT)((PUCHAR)arg + w);
                }
            }
            else
            {
                ULONG w = 0;
                status = UacpipObjectToArgument(ret, 0, arg, &w);
                if (!NT_SUCCESS(status))
                {
                    goto done;
                }
            }
        }
        *Information = required;
        status = STATUS_SUCCESS;
    }

done:
    if (ret != NULL)
    {
        uacpi_object_unref(ret);
    }
    UacpipFreeObjectArray(&args);
    return status;
}

// Above PASSIVE_LEVEL the IRP is pended and evaluated from a work item.
typedef struct _ACPI_EVAL_WORK
{
    PIO_WORKITEM          WorkItem;
    uacpi_namespace_node *Node;
    CHAR                  Name[8];
    PIRP                  Irp;
} UACPI_EVAL_WORK, *PUACPI_EVAL_WORK;

static NTSTATUS
UacpipEvalIrp(uacpi_namespace_node *Node, const char *Name, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG_PTR info = 0;
    NTSTATUS status;

    // All four eval IOCTLs are METHOD_BUFFERED: one SystemBuffer both ways.
    status = UacpipEvalIoctl(Node, Name,
                            Irp->AssociatedIrp.SystemBuffer,
                            sp->Parameters.DeviceIoControl.InputBufferLength,
                            Irp->AssociatedIrp.SystemBuffer,
                            sp->Parameters.DeviceIoControl.OutputBufferLength,
                            &info);
    Irp->IoStatus.Information = info;
    return status;
}

static VOID
NTAPI
UacpipEvalWorker(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    PUACPI_EVAL_WORK work = (PUACPI_EVAL_WORK)Context;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(DeviceObject);

    status = UacpipEvalIrp(work->Node, work->Name, work->Irp);
    work->Irp->IoStatus.Status = status;
    IoCompleteRequest(work->Irp, IO_NO_INCREMENT);

    IoFreeWorkItem(work->WorkItem);
    ExFreePoolWithTag(work, UACPI_POOL_TAG);
}

// FILE_DEVICE_ACPI control codes missing from acpiioct.h at this NTDDI floor.

#ifndef IOCTL_ACPI_REGISTER_OPREGION_HANDLER
#define IOCTL_ACPI_REGISTER_OPREGION_HANDLER \
    CTL_CODE(FILE_DEVICE_ACPI, 2, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_ACPI_UNREGISTER_OPREGION_HANDLER
#define IOCTL_ACPI_UNREGISTER_OPREGION_HANDLER \
    CTL_CODE(FILE_DEVICE_ACPI, 3, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_ACPI_QUERY_DEVICE_BIOS_NAME
#define IOCTL_ACPI_QUERY_DEVICE_BIOS_NAME \
    CTL_CODE(FILE_DEVICE_ACPI, 9, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_ACPI_GET_DEVICE_INFORMATION
#define IOCTL_ACPI_GET_DEVICE_INFORMATION     CTL_CODE(FILE_DEVICE_ACPI, 10, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_ACPI_TRANSLATE_BIOS_RESOURCES
#define IOCTL_ACPI_TRANSLATE_BIOS_RESOURCES \
    CTL_CODE(FILE_DEVICE_ACPI, 11, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_ACPI_REGISTER_DEVICE_FIRMWARE_LOCK
#define IOCTL_ACPI_REGISTER_DEVICE_FIRMWARE_LOCK \
    CTL_CODE(FILE_DEVICE_ACPI, 12, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_ACPI_UNREGISTER_DEVICE_FIRMWARE_LOCK
#define IOCTL_ACPI_UNREGISTER_DEVICE_FIRMWARE_LOCK \
    CTL_CODE(FILE_DEVICE_ACPI, 13, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA
#define IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA \
    CTL_CODE(FILE_DEVICE_ACPI, 14, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_ACPI_EVAL_METHOD_V2
#define IOCTL_ACPI_EVAL_METHOD_V2 \
    CTL_CODE(FILE_DEVICE_ACPI, 15, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_ACPI_ASYNC_EVAL_METHOD_V2
#define IOCTL_ACPI_ASYNC_EVAL_METHOD_V2 \
    CTL_CODE(FILE_DEVICE_ACPI, 16, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_ACPI_EVAL_METHOD_V2_EX
#define IOCTL_ACPI_EVAL_METHOD_V2_EX \
    CTL_CODE(FILE_DEVICE_ACPI, 17, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_ACPI_ASYNC_EVAL_METHOD_V2_EX
#define IOCTL_ACPI_ASYNC_EVAL_METHOD_V2_EX \
    CTL_CODE(FILE_DEVICE_ACPI, 18, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif
#ifndef IOCTL_ACPI_GET_DEVICE_INFORMATION_SIGNATURE
#define IOCTL_ACPI_GET_DEVICE_INFORMATION_SIGNATURE 'JieA'
#endif
// Gated behind NTDDI_WIN10_RS1 in acpiioct.h; the wrapper pins NTDDI below that.
#ifndef IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA_SIGNATURE
#define IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA_SIGNATURE 'HieA'
#endif

// IOCTL_ACPI_GET_DEVICE_INFORMATION; a short buffer gets Size and OVERFLOW.
static NTSTATUS
UacpipGetDeviceInformation(uacpi_namespace_node *Node, const char *Name,
                          PIRP Irp, PULONG_PTR Information)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PACPI_DEVICE_INFORMATION_OUTPUT_BUFFER out;
    ULONG  outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG  needed, instLen;
    uacpi_object_name n;
    char   inst[8];

    *Information = 0;
    if (Node == NULL)
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    n = uacpi_namespace_node_name(Node);
    inst[0] = n.text[0]; inst[1] = n.text[1];
    inst[2] = n.text[2]; inst[3] = n.text[3]; inst[4] = 0;
    instLen = 5;

    needed = sizeof(ACPI_DEVICE_INFORMATION_OUTPUT_BUFFER) + instLen;
    if (outLen < needed)
    {
        // Write Size only if Signature and Size fit in the buffer.
        if (outLen >= FIELD_OFFSET(ACPI_DEVICE_INFORMATION_OUTPUT_BUFFER, Size) +
                      sizeof(USHORT))
                      {
            out = (PACPI_DEVICE_INFORMATION_OUTPUT_BUFFER)Irp->AssociatedIrp.SystemBuffer;
            out->Size = (USHORT)needed;
            *Information = FIELD_OFFSET(ACPI_DEVICE_INFORMATION_OUTPUT_BUFFER, Size) +
                           sizeof(USHORT);
        }
        else
        {
            *Information = 0;
        }
        UacpiTrace("[acpi] %s: GET_DEVICE_INFORMATION probe, need %u have %u -> OVERFLOW\n",
                  Name, needed, outLen);
        return STATUS_BUFFER_OVERFLOW;
    }

    out = (PACPI_DEVICE_INFORMATION_OUTPUT_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    RtlZeroMemory(out, needed);
    out->Signature = IOCTL_ACPI_GET_DEVICE_INFORMATION_SIGNATURE;
    out->Size      = (USHORT)needed;
    out->Revision  = 1;

    // Only the instance name is reported; vendor/device/class offsets stay 0.
    out->InstanceIdOffset = (USHORT)sizeof(ACPI_DEVICE_INFORMATION_OUTPUT_BUFFER);
    out->InstanceIdLength = (USHORT)instLen;
    RtlCopyMemory((PUCHAR)out + out->InstanceIdOffset, inst, instLen);

    *Information = needed;
    UacpiTrace("[acpi] %s: GET_DEVICE_INFORMATION -> instance '%s' (%u bytes)\n",
              Name, inst, needed);
    return STATUS_SUCCESS;
}

// IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA: no _DSD accessor, so report not found.
static NTSTATUS
UacpipGetDeviceSpecificData(uacpi_namespace_node *Node, const char *Name, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PACPI_GET_DEVICE_SPECIFIC_DATA in;

    if (Node == NULL)
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (sp->Parameters.DeviceIoControl.InputBufferLength <
            (ULONG)FIELD_OFFSET(ACPI_GET_DEVICE_SPECIFIC_DATA, PropertyName))
            {
        return STATUS_INVALID_PARAMETER;
    }
    in = (PACPI_GET_DEVICE_SPECIFIC_DATA)Irp->AssociatedIrp.SystemBuffer;
    if (in == NULL ||
        in->Signature != IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA_SIGNATURE)
        {
        return STATUS_INVALID_PARAMETER;
    }
    UacpiTrace("[acpi] %s: GET_DEVICE_SPECIFIC_DATA - no _DSD support, "
              "reporting not found\n", Name);
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

// IOCTL_ACPI_QUERY_DEVICE_BIOS_NAME: absolute path, NUL-terminated WCHAR.
static NTSTATUS
UacpipQueryDeviceBiosName(uacpi_namespace_node *Node, const char *Name, PIRP Irp,
                          PULONG_PTR Information)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    const uacpi_char *absolutePath;
    UNICODE_STRING wide;
    ANSI_STRING path;
    NTSTATUS status;
    USHORT required;

    *Information = 0;

    if (Node == NULL)
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    absolutePath = uacpi_namespace_node_generate_absolute_path(Node);
    if (absolutePath == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlInitAnsiString(&path, absolutePath);

    // RtlAnsiStringToUnicodeSize already counts the terminator
    required = (USHORT)RtlAnsiStringToUnicodeSize(&path);

    if (sp->Parameters.DeviceIoControl.OutputBufferLength < required)
    {
        uacpi_free_absolute_path(absolutePath);
        return STATUS_BUFFER_TOO_SMALL;
    }

    wide.Buffer = (PWCH)Irp->AssociatedIrp.SystemBuffer;
    wide.Length = 0;
    wide.MaximumLength = required;

    status = RtlAnsiStringToUnicodeString(&wide, &path, FALSE);
    uacpi_free_absolute_path(absolutePath);

    if (!NT_SUCCESS(status))
    {
        return status;
    }

    *Information = wide.Length + sizeof(WCHAR);

    UacpiTrace("[acpi] %s: BIOS name is %wZ\n", Name, &wide);
    return STATUS_SUCCESS;
}

// IOCTL_ACPI_ENUM_CHILDREN: immediate children only, by 4-char name.

// Records are packed, not ULONG-aligned; the caller does not round the stride.
#define UACPI_ENUM_CHILD_NAME_LENGTH    5   // 4 name characters plus the NUL
#define UACPI_ENUM_CHILD_RECORD_LENGTH  \
    ((ULONG)(FIELD_OFFSET(ACPI_ENUM_CHILD, Name) + UACPI_ENUM_CHILD_NAME_LENGTH))

#ifndef ENUM_CHILDREN_IMMEDIATE_ONLY
#define ENUM_CHILDREN_IMMEDIATE_ONLY 0x1
#endif

// Source of ACPI_OBJECT_HAS_CHILDREN; USB hubs skip ports without the bit.
static BOOLEAN
UacpipNodeHasChildren(uacpi_namespace_node *Node)
{
    uacpi_namespace_node *child = UACPI_NULL;

    return (BOOLEAN)(uacpi_likely_success(uacpi_namespace_node_next(Node, &child)) &&
                     child != UACPI_NULL);
}

static NTSTATUS
UacpipEnumChildren(uacpi_namespace_node *Node, const char *Name, PIRP Irp,
                  PULONG_PTR Information)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PACPI_ENUM_CHILDREN_INPUT_BUFFER  in;
    PACPI_ENUM_CHILDREN_OUTPUT_BUFFER out;
    uacpi_namespace_node *child;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG inLen  = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG used, count, needed;
    PUCHAR cursor;

    *Information = 0;

    if (Node == NULL)
    {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if (inLen < sizeof(ULONG) * 2)
    {
        return STATUS_INVALID_PARAMETER;
    }
    in = (PACPI_ENUM_CHILDREN_INPUT_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    if (in == NULL || in->Signature != ACPI_ENUM_CHILDREN_INPUT_BUFFER_SIGNATURE)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Size the walk, then fill it. A short buffer reports the required size.
    count  = 0;
    needed = FIELD_OFFSET(ACPI_ENUM_CHILDREN_OUTPUT_BUFFER, Children);
    child  = NULL;
    while (uacpi_likely_success(uacpi_namespace_node_next(Node, &child)) &&
           child != NULL)
           {
        count++;
        needed += UACPI_ENUM_CHILD_RECORD_LENGTH;
    }

    if (outLen < needed)
    {
        // Required size goes in NumberOfChildren; OVERFLOW if the header fits.
        if (outLen >= sizeof(ACPI_ENUM_CHILDREN_OUTPUT_BUFFER))
        {
            out = (PACPI_ENUM_CHILDREN_OUTPUT_BUFFER)Irp->AssociatedIrp.SystemBuffer;
            RtlZeroMemory(out, outLen);
            out->Signature        = ACPI_ENUM_CHILDREN_OUTPUT_BUFFER_SIGNATURE;
            out->NumberOfChildren = needed;
            *Information = sizeof(ACPI_ENUM_CHILDREN_OUTPUT_BUFFER);
            UacpiTrace("[acpi] %s: ENUM_CHILDREN %u child(ren), need %u bytes, "
                      "got %u -> OVERFLOW\n", Name, count, needed, outLen);
            return STATUS_BUFFER_OVERFLOW;
        }
        *Information = 0;
        UacpiTrace("[acpi] %s: ENUM_CHILDREN got %u bytes, too small for the header\n",
                  Name, outLen);
        return STATUS_BUFFER_TOO_SMALL;
    }

    out = (PACPI_ENUM_CHILDREN_OUTPUT_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    RtlZeroMemory(out, needed);
    out->Signature        = ACPI_ENUM_CHILDREN_OUTPUT_BUFFER_SIGNATURE;
    out->NumberOfChildren = count;

    cursor = (PUCHAR)out + FIELD_OFFSET(ACPI_ENUM_CHILDREN_OUTPUT_BUFFER, Children);
    used   = FIELD_OFFSET(ACPI_ENUM_CHILDREN_OUTPUT_BUFFER, Children);

    child = NULL;
    while (uacpi_likely_success(uacpi_namespace_node_next(Node, &child)) &&
           child != NULL)
           {
        PACPI_ENUM_CHILD entry = (PACPI_ENUM_CHILD)cursor;
        uacpi_object_name n = uacpi_namespace_node_name(child);
        ULONG rec;

        entry->Flags      = UacpipNodeHasChildren(child)
                                ? ACPI_OBJECT_HAS_CHILDREN : 0;
        entry->NameLength = UACPI_ENUM_CHILD_NAME_LENGTH;
        entry->Name[0] = n.text[0];
        entry->Name[1] = n.text[1];
        entry->Name[2] = n.text[2];
        entry->Name[3] = n.text[3];
        entry->Name[4] = 0;

        // Step by the same length the caller's ACPI_ENUM_CHILD_NEXT() uses.
        rec = (ULONG)ACPI_ENUM_CHILD_LENGTH_FROM_CHILD(entry);

        cursor += rec;
        used   += rec;
    }

    *Information = used;
    UacpiTrace("[acpi] %s: ENUM_CHILDREN -> %u child(ren), %u bytes\n",
              Name, count, used);
    return STATUS_SUCCESS;
}

// ACPI global lock; uACPI owns the FACS protocol, so hand off directly.
static NTSTATUS
UacpipManipulateGlobalLock(BOOLEAN Acquire, const char *Name, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PACPI_MANIPULATE_GLOBAL_LOCK_BUFFER buf;
    static uacpi_u32 UacpiGlobalLockSeq;
    uacpi_status st;

    if (sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(ULONG))
    {
        return STATUS_INVALID_PARAMETER;
    }
    buf = (PACPI_MANIPULATE_GLOBAL_LOCK_BUFFER)Irp->AssociatedIrp.SystemBuffer;
    if (buf == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (Acquire)
    {
        st = uacpi_acquire_global_lock(0xFFFF, &UacpiGlobalLockSeq);
    }
    else
    {
        st = uacpi_release_global_lock(UacpiGlobalLockSeq);
    }
    UacpiTrace("[acpi] %s: global lock %s -> %d\n",
              Name, Acquire ? "acquire" : "release", (int)st);
    return uacpi_unlikely_error(st) ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

// Shared by PDOs and filters. TRUE if the IRP was completed or pended.
static BOOLEAN
UacpipEvalDeviceControl(uacpi_namespace_node *Node, const char *Name,
                       PDEVICE_OBJECT Self, PIRP Irp, PNTSTATUS Disposition)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;

    switch (code)
    {

    case IOCTL_ACPI_GET_DEVICE_INFORMATION:
    {
        ULONG_PTR info = 0;
        NTSTATUS  st   = UacpipGetDeviceInformation(Node, Name, Irp, &info);

        *Disposition = UacpiCompleteIrp(Irp, st, info);
        return TRUE;
    }

    case IOCTL_ACPI_GET_DEVICE_SPECIFIC_DATA:
        *Disposition = UacpiCompleteIrp(Irp,
                           UacpipGetDeviceSpecificData(Node, Name, Irp), 0);
        return TRUE;

    case IOCTL_ACPI_QUERY_DEVICE_BIOS_NAME:
    {
        ULONG_PTR info = 0;
        NTSTATUS  st   = UacpipQueryDeviceBiosName(Node, Name, Irp, &info);

        *Disposition = UacpiCompleteIrp(Irp, st, info);
        return TRUE;
    }

    // No public structures for these; trace and fail.
    case IOCTL_ACPI_REGISTER_OPREGION_HANDLER:
    case IOCTL_ACPI_UNREGISTER_OPREGION_HANDLER:
    case IOCTL_ACPI_TRANSLATE_BIOS_RESOURCES:
    case IOCTL_ACPI_REGISTER_DEVICE_FIRMWARE_LOCK:
    case IOCTL_ACPI_UNREGISTER_DEVICE_FIRMWARE_LOCK:
        UacpiTrace("[acpi] %s: ACPI ioctl fn %u (internal, no public contract) "
                  "- not implemented\n",
                  Name, (ULONG)((code >> 2) & 0xFFF));
        *Disposition = UacpiCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0);
        return TRUE;

    case IOCTL_ACPI_ENUM_CHILDREN:
    {
        ULONG_PTR info = 0;
        NTSTATUS  st   = UacpipEnumChildren(Node, Name, Irp, &info);

        *Disposition = UacpiCompleteIrp(Irp, st, info);
        return TRUE;
    }

    case IOCTL_ACPI_ACQUIRE_GLOBAL_LOCK:
        *Disposition = UacpiCompleteIrp(Irp,
                           UacpipManipulateGlobalLock(TRUE, Name, Irp), 0);
        return TRUE;

    case IOCTL_ACPI_RELEASE_GLOBAL_LOCK:
        *Disposition = UacpiCompleteIrp(Irp,
                           UacpipManipulateGlobalLock(FALSE, Name, Irp), 0);
        return TRUE;

    case IOCTL_ACPI_EVAL_METHOD:
    case IOCTL_ACPI_EVAL_METHOD_EX:
    case IOCTL_ACPI_ASYNC_EVAL_METHOD:
    case IOCTL_ACPI_ASYNC_EVAL_METHOD_EX:
    // V2 codes share the parser; unknown input signatures are rejected.
    case IOCTL_ACPI_EVAL_METHOD_V2:
    case IOCTL_ACPI_EVAL_METHOD_V2_EX:
    case IOCTL_ACPI_ASYNC_EVAL_METHOD_V2:
    case IOCTL_ACPI_ASYNC_EVAL_METHOD_V2_EX:
        // AML must run at PASSIVE_LEVEL; above it, defer to a work item.
        if (KeGetCurrentIrql() == PASSIVE_LEVEL)
        {
            // Evaluate first; UacpipEvalIrp writes Irp->IoStatus.Information.
            NTSTATUS evalStatus = UacpipEvalIrp(Node, Name, Irp);

            *Disposition = UacpiCompleteIrp(Irp, evalStatus,
                                           Irp->IoStatus.Information);
            return TRUE;
        }
        {
            PUACPI_EVAL_WORK work = (PUACPI_EVAL_WORK)ExAllocatePoolWithTag(
                                       NonPagedPool, sizeof(*work), UACPI_POOL_TAG);
            if (work == NULL)
            {
                *Disposition = UacpiCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
                return TRUE;
            }
            work->WorkItem = IoAllocateWorkItem(Self);
            if (work->WorkItem == NULL)
            {
                ExFreePoolWithTag(work, UACPI_POOL_TAG);
                *Disposition = UacpiCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
                return TRUE;
            }
            work->Node = Node;
            RtlStringCbCopyA(work->Name, sizeof(work->Name), Name);
            work->Irp  = Irp;
            IoMarkIrpPending(Irp);
            IoQueueWorkItem(work->WorkItem, UacpipEvalWorker, DelayedWorkQueue, work);
            *Disposition = STATUS_PENDING;
            return TRUE;
        }

    default:
        return FALSE;
    }
}

// Processor object-info IOCTL, same code and layout on Vista and Win10. The CPU
// driver issues it at START to learn its identity.
#define UACPI_IOCTL_GET_PROCESSOR_OBJ_INFO 0x00294180u

// InitialApicId for the 16-byte reply: the APIC id whose MADT entry's ACPI UID
// matches this processor's ID.
static BOOLEAN
UacpipProcessorInitialApicId(uacpi_u8 ProcId, PULONG ApicId)
{
    uacpi_table tbl;
    struct acpi_madt *madt;
    uacpi_u8 *p, *end;
    BOOLEAN found = FALSE;

    if (uacpi_unlikely_error(
            uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &tbl)) ||
        tbl.ptr == NULL)
        {
        return FALSE;
    }
    madt = (struct acpi_madt *)tbl.ptr;
    p   = (uacpi_u8 *)madt->entries;
    end = (uacpi_u8 *)madt + madt->hdr.length;

    while (p + sizeof(struct acpi_entry_hdr) <= end)
    {
        struct acpi_entry_hdr *h = (struct acpi_entry_hdr *)p;

        if (h->length < sizeof(struct acpi_entry_hdr) || p + h->length > end)
        {
            break;
        }
        if (h->type == ACPI_MADT_ENTRY_TYPE_LAPIC &&
            h->length >= sizeof(struct acpi_madt_lapic))
            {
            struct acpi_madt_lapic *l = (struct acpi_madt_lapic *)p;
            if (l->uid == ProcId)
            {
                *ApicId = l->id;
                found = TRUE;
                break;
            }
        }
        else if (h->type == ACPI_MADT_ENTRY_TYPE_LOCAL_X2APIC &&
                 h->length >= sizeof(struct acpi_madt_x2apic))
                 {
            struct acpi_madt_x2apic *x = (struct acpi_madt_x2apic *)p;
            if (x->uid == ProcId)
            {
                *ApicId = x->id;
                found = TRUE;
                break;
            }
        }
        p += h->length;
    }
    uacpi_table_unref(&tbl);
    return found;
}

// Serve the processor obj-info IOCTL on a legacy Processor() PDO. TRUE if handled.
static BOOLEAN
UacpipProcessorObjInfoIoctl(PUACPI_PDO Pdo, PIRP Irp, PNTSTATUS Disposition)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    PUCHAR buf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    uacpi_object *obj;
    uacpi_processor_info info;

    if (sp->Parameters.DeviceIoControl.IoControlCode !=
            UACPI_IOCTL_GET_PROCESSOR_OBJ_INFO ||
        !Pdo->IsProcessor)
        {
        return FALSE;
    }

    // acpi.sys serves this for kernel-mode callers only.
    if (Irp->RequestorMode != KernelMode)
    {
        *Disposition = UacpiCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        return TRUE;
    }
    // {PhysicalID(4), PBlkAddress(4), PBlkLength(1)} = 12; +InitialApicId(4) = 16.
    if (outLen < 12 || buf == NULL)
    {
        *Disposition = UacpiCompleteIrp(Irp, STATUS_INFO_LENGTH_MISMATCH, 0);
        return TRUE;
    }

    RtlZeroMemory(buf, (outLen >= 16) ? 16 : 12);

    obj = (Pdo->Node != NULL)
              ? uacpi_namespace_node_get_object_typed(Pdo->Node, UACPI_OBJECT_PROCESSOR_BIT)
              : NULL;
    if (obj != NULL &&
        uacpi_likely_success(uacpi_object_get_processor_info(obj, &info)))
        {
        *(PULONG)(buf + 0) = info.id;                  // PhysicalID
        *(PULONG)(buf + 4) = info.block_address;       // PBlkAddress
        buf[8]             = info.block_length;        // PBlkLength
    }

    if (outLen >= 16)
    {
        ULONG apic = 0xFFFFFFFFu;
        if (obj != NULL)
        {
            (void)UacpipProcessorInitialApicId((uacpi_u8)info.id, &apic);
        }
        *(PULONG)(buf + 12) = apic;                    // InitialApicId
        *Disposition = UacpiCompleteIrp(Irp, STATUS_SUCCESS, 16);
    }
    else
    {
        *Disposition = UacpiCompleteIrp(Irp, STATUS_SUCCESS, 12);
    }
    return TRUE;
}

NTSTATUS
UacpiPdoDeviceControl(PUACPI_PDO Pdo, PIRP Irp)
{
    NTSTATUS disp;
    BOOLEAN  handled;

    // Processor PDOs answer the object-info IOCTL the CPU driver needs at START.
    if (Pdo->IsProcessor && UacpipProcessorObjInfoIoctl(Pdo, Irp, &disp))
    {
        return disp;
    }

    // Button PDOs answer the SYS_BUTTON IOCTLs before the eval path.
    if (Pdo->ButtonCaps != 0)
    {
        disp = UacpiButtonDeviceControl(Pdo, Irp, &handled);
        if (handled)
        {
            return disp;
        }
    }

    // The EC answers _Qxx handler registration before the eval path.
    if (Pdo->Ec != NULL)
    {
        disp = UacpiEcDeviceControl(Pdo, Irp, &handled);
        if (handled)
        {
            return disp;
        }
    }

    // Thermal zones answer the thermal manager's IOCTLs before the eval path.
    if (Pdo->IsThermalZone)
    {
        disp = UacpiThermalDeviceControl(Pdo, Irp, &handled);
        if (handled)
        {
            return disp;
        }
    }

    if (UacpipEvalDeviceControl(Pdo->Node, Pdo->Name, Pdo->Common.Self, Irp, &disp))
    {
        return disp;
    }
    // Bottom of the stack: trace and fail unrecognized codes.
    UacpiTrace("[acpi] %s: IOCTL 0x%08lX is not implemented -> NOT_SUPPORTED\n",
               Pdo->Name,
               IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.IoControlCode);

    return UacpiCompleteIrp(Irp, STATUS_NOT_SUPPORTED, 0);
}

// Filter DO: serve ACPI IOCTLs off the filter's node, forward the rest.
NTSTATUS
UacpiFilterDeviceControl(PUACPI_FILTER Filter, PIRP Irp)
{
    NTSTATUS disp;
    char name[8] = "FILT";

    if (Filter->Node != NULL)
    {
        uacpi_object_name n = uacpi_namespace_node_name(Filter->Node);
        name[0] = n.text[0]; name[1] = n.text[1];
        name[2] = n.text[2]; name[3] = n.text[3]; name[4] = '\0';
        if (UacpipEvalDeviceControl(Filter->Node, name, Filter->Common.Self, Irp, &disp))
        {
            return disp;
        }
    }
    return UacpiForwardAndForget(Filter->LowerDevice, Irp);
}
