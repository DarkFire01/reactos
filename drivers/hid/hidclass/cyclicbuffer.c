/*
 * PROJECT:     ReactOS Universal Serial Bus Human Interface Device Driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Ring buffer
 * COPYRIGHT:   Copyright 2022 Roman Masanin <36927roma@gmail.com>
 */

#include "precomp.h"

NTSTATUS HidClass_CyclicBufferUpdateSize(IN PHIDCHASS_CYCLIC_BUFFER buffer, IN UINT32 elementCount)
{
    SIZE_T size;

    buffer->elementCount = elementCount + 1;
    buffer->endIndex = 0;
    buffer->startIndex = 0;

    if (buffer->buffer != NULL)
    {
        ExFreePool(buffer->buffer);
        buffer->buffer = NULL;
    }

    /*
     * A collection with no input reports has nothing to buffer, and that is an
     * ordinary shape for a HID device: an output-only or feature-only
     * collection has an InputLength of zero. Leave the buffer empty rather
     * than asking the pool for zero bytes, which it asserts on.
     */
    size = (SIZE_T)buffer->elementSize * buffer->elementCount;
    if (size == 0)
    {
        return STATUS_SUCCESS;
    }

    buffer->buffer = ExAllocatePoolWithTag(NonPagedPool, size, HIDCLASS_TAG);
    if (buffer->buffer == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

void HidClass_CyclicBufferInitialize(IN PHIDCHASS_CYCLIC_BUFFER buffer, IN UINT32 elementSize)
{
    buffer->elementSize = elementSize;
    buffer->endIndex = 0;
    buffer->startIndex = 0;
    buffer->buffer = NULL;

    HidClass_CyclicBufferUpdateSize(buffer, 16);
}

void HidClass_CyclicBufferPut(IN PHIDCHASS_CYCLIC_BUFFER buffer, IN PVOID item)
{
    PUCHAR currentItem;

    /*
     * Nothing was allocated, so there is nowhere to put the report: the
     * collection carries no input reports, or the allocation failed.
     * Returning also keeps the modulo below out of a divide by zero.
     */
    if (buffer->buffer == NULL)
    {
        return;
    }

    currentItem = buffer->buffer;
    currentItem += buffer->elementSize * buffer->endIndex;
    RtlCopyMemory(currentItem, item, buffer->elementSize);
    buffer->endIndex = (buffer->endIndex + 1) % buffer->elementCount;
    if (buffer->endIndex == buffer->startIndex)
    {
        buffer->startIndex = (buffer->startIndex + 1) % buffer->elementCount;
    }
}

BOOLEAN HidClass_CyclicBufferIsEmpty(IN PHIDCHASS_CYCLIC_BUFFER buffer)
{
    return buffer->startIndex == buffer->endIndex;
}

BOOLEAN HidClass_CyclicBufferGet(IN PHIDCHASS_CYCLIC_BUFFER buffer, OUT PVOID item)
{
    PUCHAR currentItem;
    BOOLEAN result = FALSE;

    if (buffer->startIndex != buffer->endIndex)
    {
        currentItem = buffer->buffer;
        currentItem += buffer->elementSize * buffer->startIndex;
        RtlCopyMemory(item, currentItem, buffer->elementSize);
        buffer->startIndex = (buffer->startIndex + 1) % buffer->elementCount;
        result = TRUE;
    }

    return result;
}
