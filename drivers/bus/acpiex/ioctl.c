/*
 * PROJECT:     ReactOS ACPI Platform Extensions
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Resource Hub IOCTL surface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * Only the query is built so far. It is the one a bus controller's class
 * extension needs, to recover the slave address and bus speed behind a
 * connection id the moment a peripheral opens a target. The allocate, free and
 * update codes belong to the ACPI translator path and land with it; they are
 * refused rather than faked, so a caller sees the same STATUS_NOT_SUPPORTED a
 * hub that never registered them would return.
 */

#include "rhpriv.h"

/*
 * Note the two-stage answer on overflow: Version and PropertiesLength are filled
 * in before returning STATUS_BUFFER_TOO_SMALL, because that is how a caller sizes
 * its second attempt. Information is left at the eight header bytes it did
 * manage to write.
 */
static
NTSTATUS
RhpProcessQueryConnectionPropertiesIoctl(
    _In_ PRH_DEVICE_CONTEXT DeviceContext,
    _In_ WDFREQUEST Request,
    _Out_ PULONG_PTR Information)
{
    PRH_QUERY_CONNECTION_PROPERTIES_INPUT_BUFFER Input;
    PRH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER Output;
    PRH_CONNECTION Connection;
    PVOID Properties = NULL;
    ULONG PropertiesLength = 0;
    size_t InputLength;
    size_t OutputLength;
    KIRQL OldIrql;
    NTSTATUS Status;

    *Information = 0;

    Status = WdfRequestRetrieveInputBuffer(Request,
                                           sizeof(*Input),
                                           (PVOID *)&Input,
                                           &InputLength);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WdfRequestRetrieveOutputBuffer(Request,
                                            FIELD_OFFSET(RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER,
                                                         ConnectionProperties),
                                            (PVOID *)&Output,
                                            &OutputLength);
    if (!NT_SUCCESS(Status))
        return Status;

    /*
     * Two ways in. A peripheral driver holds a connection id, which is what it
     * was handed in its own resource list. A GPIO class extension holds only
     * the vector the pin was given, because that is all the HAL tells it when
     * an interrupt on that pin is connected, so it asks by vector to recover
     * which controller and which pin the vector stands for.
     */
    if (Input->QueryType == ConnectionIdType)
    {
        Connection = RhpFindAndReferenceConnectionById(DeviceContext,
                                                       Input->u.ConnectionId);
    }
    else if (Input->QueryType == InterruptVectorType)
    {
        Connection = RhpFindAndReferenceConnectionByVector(DeviceContext,
                                                           Input->u.InterruptVector);
    }
    else
    {
        return STATUS_NOT_SUPPORTED;
    }

    if (Connection == NULL)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    /*
     * Copy the descriptor out from under the lock rather than holding it across
     * the copy into the caller's buffer, because the connection can be torn down
     * by ACPI at any point after we drop our reference.
     */
    KeAcquireSpinLock(&DeviceContext->Lock, &OldIrql);

    if (Connection->ConnectionProperties != NULL && Connection->PropertiesLength != 0)
    {
        PropertiesLength = Connection->PropertiesLength;
        Properties = ExAllocatePoolWithTag(NonPagedPool, PropertiesLength, RH_POOL_TAG);
        if (Properties != NULL)
            RtlCopyMemory(Properties, Connection->ConnectionProperties, PropertiesLength);
        else
            Status = STATUS_INSUFFICIENT_RESOURCES;
    }

    KeReleaseSpinLock(&DeviceContext->Lock, OldIrql);
    RhpDereferenceConnection(Connection);

    if (!NT_SUCCESS(Status))
        return Status;

    Output->Version = RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_VERSION;
    Output->PropertiesLength = PropertiesLength;
    *Information = FIELD_OFFSET(RH_QUERY_CONNECTION_PROPERTIES_OUTPUT_BUFFER,
                                ConnectionProperties);

    if ((OutputLength - *Information) < PropertiesLength)
    {
        /* PropertiesLength above is what the caller resizes to */
        Status = STATUS_BUFFER_TOO_SMALL;
    }
    else
    {
        RtlCopyMemory(Output->ConnectionProperties, Properties, PropertiesLength);
        *Information += PropertiesLength;
        Status = STATUS_SUCCESS;
    }

    if (Properties != NULL)
        ExFreePoolWithTag(Properties, RH_POOL_TAG);

    return Status;
}

VOID
NTAPI
RhEvtProcessDeviceIoControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    PRH_DEVICE_CONTEXT DeviceContext;
    ULONG_PTR Information = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    DeviceContext = RhGetDeviceContext(WdfIoQueueGetDevice(Queue));

    switch (IoControlCode)
    {
        case IOCTL_RH_QUERY_CONNECTION_PROPERTIES:
            Status = RhpProcessQueryConnectionPropertiesIoctl(DeviceContext,
                                                              Request,
                                                              &Information);
            break;

        case IOCTL_UACPINT_RH_QUERY_TRANSLATION:
            Status = RhpProcessTranslationInterfaceQueryIoctl(DeviceContext,
                                                              Request,
                                                              &Information);
            break;

        default:
            Status = STATUS_NOT_SUPPORTED;
            break;
    }

    WdfRequestCompleteWithInformation(Request, Status, Information);
}
