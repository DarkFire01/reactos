/*
 * PROJECT:     ReactOS Kernel Streaming
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Stream resource registration
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "private.hpp"

#define NDEBUG
#include <debug.h>

#define TAG_STREAM_RESOURCE 'rSoP'

/* Nothing in this port class schedules by stream resource yet, so a handle only remembers its owner */
typedef struct _PC_STREAM_RESOURCE
{
    PDEVICE_OBJECT DeviceObject;
    PVOID Descriptor;
} PC_STREAM_RESOURCE, *PPC_STREAM_RESOURCE;

extern "C"
NTSTATUS
NTAPI
PcAddStreamResource(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Reserved,
    _In_ PVOID ResourceDescriptor,
    _Out_ PVOID *ResourceHandle)
{
    PPC_STREAM_RESOURCE Resource;

    if (!DeviceObject || Reserved || !ResourceDescriptor || !ResourceHandle)
        return STATUS_INVALID_PARAMETER;

    /* The descriptor starts with its size */
    if (*(PULONG)ResourceDescriptor == 0)
        return STATUS_INVALID_PARAMETER;

    *ResourceHandle = NULL;

    Resource = (PPC_STREAM_RESOURCE)ExAllocatePoolZero(NonPagedPool, sizeof(*Resource), TAG_STREAM_RESOURCE);
    if (!Resource)
        return STATUS_INSUFFICIENT_RESOURCES;

    Resource->DeviceObject = DeviceObject;
    Resource->Descriptor = ResourceDescriptor;
    *ResourceHandle = Resource;

    return STATUS_SUCCESS;
}

extern "C"
NTSTATUS
NTAPI
PcRemoveStreamResource(
    _In_ PVOID ResourceHandle)
{
    if (!ResourceHandle)
        return STATUS_INVALID_PARAMETER;

    ExFreePoolWithTag(ResourceHandle, TAG_STREAM_RESOURCE);
    return STATUS_SUCCESS;
}
