/*
 * PROJECT:     ReactOS ACPI Platform Extensions
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Resource Hub create path: connection id -> controller reparse
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * This is the whole reason the hub exists. A peripheral driver formats its
 * connection id into \Device\RESOURCE_HUB\<16 hex digits> and opens it; we look
 * the id up, rewrite the file object's name to the controller that owns it, and
 * complete the create with STATUS_REPARSE. The I/O manager then reissues the
 * open against the new name, so the caller's handle lands on the controller
 * having never known the hub was involved.
 */

#include "rhpriv.h"

VOID
RhpFreeUnicodeString(
    _Inout_ PUNICODE_STRING String)
{
    if (String->Buffer != NULL)
        ExFreePoolWithTag(String->Buffer, RH_POOL_TAG);

    String->Buffer = NULL;
    String->Length = 0;
    String->MaximumLength = 0;
}

/*
 * The id parse is open coded: skip a leading backslash, require at least
 * sixteen characters, reject anything that is not hex, require the
 * seventeenth to be a backslash or the terminator, then convert the two
 * eight-character halves separately into the high and low words. That is
 * precisely RESOURCE_HUB_ID_FROM_FILE_NAME_WITH_SUBPATH, so use the header's
 * version rather than a second copy of the same loop.
 */
NTSTATUS
RhpBuildReparseName(
    _In_ WDFDEVICE Device,
    _In_ PCUNICODE_STRING FileName,
    _Out_ PUNICODE_STRING NewFileName)
{
    PRH_DEVICE_CONTEXT DeviceContext;
    PRH_CONNECTION Connection;
    PRH_PROVIDER Provider;
    PRH_TARGET_DATA TargetData;
    UNICODE_STRING NameCopy;
    LARGE_INTEGER Id;
    PWSTR Name;
    USHORT Length;
    NTSTATUS Status;

    RtlZeroMemory(NewFileName, sizeof(*NewFileName));

    Name = FileName->Buffer;
    Length = FileName->Length;

    if (Length < sizeof(WCHAR))
        return STATUS_INVALID_PARAMETER;

    /* The I/O manager hands us the name with its leading separator */
    if (*Name == L'\\')
    {
        Name++;
        Length -= sizeof(WCHAR);

        if (Length < sizeof(WCHAR))
            return STATUS_INVALID_PARAMETER;
    }

    /*
     * The helper wants a NUL-terminated string and the file name is not
     * guaranteed to be one, so copy the tail off before parsing it.
     */
    NameCopy.Length = Length;
    NameCopy.MaximumLength = Length + sizeof(UNICODE_NULL);
    NameCopy.Buffer = ExAllocatePoolWithTag(PagedPool,
                                            NameCopy.MaximumLength,
                                            RH_POOL_TAG);
    if (NameCopy.Buffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(NameCopy.Buffer, Name, Length);
    NameCopy.Buffer[Length / sizeof(WCHAR)] = UNICODE_NULL;

    Status = RESOURCE_HUB_ID_FROM_FILE_NAME(NameCopy.Buffer, &Id);
    RhpFreeUnicodeString(&NameCopy);

    if (!NT_SUCCESS(Status))
        return Status;

    DeviceContext = RhGetDeviceContext(Device);

    Connection = RhpFindAndReferenceConnectionById(DeviceContext, Id);
    if (Connection == NULL)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    Provider = Connection->Provider;
    if (Provider == NULL)
    {
        RhpDereferenceConnection(Connection);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    InterlockedIncrement(&Provider->ReferenceCount);
    RhpDereferenceConnection(Connection);

    TargetData = RhpReferenceProviderTargetData(DeviceContext, Provider);
    if (TargetData == NULL)
    {
        RhpDereferenceProvider(Provider);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    /*
     * Hand back a private copy: the caller replaces the file object name with it
     * and then frees it, while the target data stays owned by the provider.
     */
    Length = TargetData->ControllerName.Length;
    NewFileName->Buffer = ExAllocatePoolWithTag(PagedPool,
                                                Length + sizeof(UNICODE_NULL),
                                                RH_POOL_TAG);
    if (NewFileName->Buffer == NULL)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
    }
    else
    {
        RtlCopyMemory(NewFileName->Buffer, TargetData->ControllerName.Buffer, Length);
        NewFileName->Buffer[Length / sizeof(WCHAR)] = UNICODE_NULL;
        NewFileName->Length = Length;
        NewFileName->MaximumLength = Length + sizeof(UNICODE_NULL);
        Status = STATUS_SUCCESS;
    }

    RhpDereferenceTargetData(TargetData);
    RhpDereferenceProvider(Provider);

    return Status;
}

/*
 * The IRP_MJ_CREATE preprocess callback.
 *
 * A create with no file name is somebody opening the hub device itself; that
 * goes to the framework untouched. A create *with* a name is a connection open,
 * and never reaches WDF at all: it is completed here, either with STATUS_REPARSE
 * or with the failure that stopped us resolving it.
 */
NTSTATUS
NTAPI
RhWdmDeviceFileCreate(
    _In_ WDFDEVICE Device,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION IoStack = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IoStack->FileObject;
    UNICODE_STRING NewFileName;
    NTSTATUS Status;

    if (FileObject == NULL ||
        FileObject->FileName.Length == 0 ||
        FileObject->FileName.Buffer == NULL)
    {
        return WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
    }

    Status = RhpBuildReparseName(Device, &FileObject->FileName, &NewFileName);
    if (NT_SUCCESS(Status))
    {
        Status = IoReplaceFileObjectName(FileObject,
                                         NewFileName.Buffer,
                                         NewFileName.Length);
        if (NT_SUCCESS(Status))
        {
            /*
             * Not a success code the caller ever sees: the I/O manager consumes
             * STATUS_REPARSE and reissues the open against the name we just
             * wrote. Information must be zero, because a non-zero value means a
             * mount-point style reparse and would be misread.
             */
            Status = STATUS_REPARSE;
        }

        RhpFreeUnicodeString(&NewFileName);
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return Status;
}
