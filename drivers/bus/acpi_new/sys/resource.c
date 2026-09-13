/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Convert _CRS and _PRS into NT resource lists
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"

#include "reshub_downlevel.h"

// AML resource descriptor tag byte, per ACPI 6.5 section 6.4.
// Small item: bit 7 clear, bits 6:3 name, bits 2:0 length of the data after
// the tag.  Large item: bit 7 set, followed by a 16-bit little-endian length.
#define ACPI_RSRC_LARGE_ITEM        0x80
#define ACPI_RSRC_SMALL_NAME_MASK   0x78
#define ACPI_RSRC_SMALL_NAME_END    0x78    // small name 0x0F, the end tag
#define ACPI_RSRC_SMALL_LEN_MASK    0x07
#define ACPI_RSRC_SMALL_HEADER_SIZE 1
#define ACPI_RSRC_LARGE_HEADER_SIZE 3

// Log each window handed to pci.sys and every dropped _CRS descriptor.
int UacpiResVerbose = 1;

// Emit callback: NULL 'out' counts; non-NULL fills.  Returns #descriptors.
typedef ULONG (*ACPI_EMIT_CM)(uacpi_resource *r, PCM_PARTIAL_RESOURCE_DESCRIPTOR out);
typedef ULONG (*ACPI_EMIT_IO)(uacpi_resource *r, PIO_RESOURCE_DESCRIPTOR out);

// One _CRS entry -> 0+ CM_PARTIAL_RESOURCE_DESCRIPTORs.
static ULONG
UacpiEmitCm(uacpi_resource *r, PCM_PARTIAL_RESOURCE_DESCRIPTOR out)
{
    switch (r->type)
    {

    case UACPI_RESOURCE_TYPE_IRQ:
    {
        ULONG n = r->irq.num_irqs, i;
        if (out != NULL)
        {
            for (i = 0; i < n; i++)
            {
                RtlZeroMemory(&out[i], sizeof(out[i]));
                out[i].Type = CmResourceTypeInterrupt;
                out[i].ShareDisposition =
                    r->irq.sharing == UACPI_SHARED ? CmResourceShareShared
                                                   : CmResourceShareDeviceExclusive;
                out[i].Flags = (r->irq.triggering == UACPI_TRIGGERING_EDGE)
                    ? CM_RESOURCE_INTERRUPT_LATCHED
                    : CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
                out[i].u.Interrupt.Level    = r->irq.irqs[i];
                out[i].u.Interrupt.Vector   = r->irq.irqs[i];
                out[i].u.Interrupt.Affinity = (KAFFINITY)-1;
            }
        }
        return n;
    }

    case UACPI_RESOURCE_TYPE_EXTENDED_IRQ:
    {
        ULONG n = r->extended_irq.num_irqs, i;
        if (out != NULL)
        {
            for (i = 0; i < n; i++)
            {
                RtlZeroMemory(&out[i], sizeof(out[i]));
                out[i].Type = CmResourceTypeInterrupt;
                out[i].ShareDisposition =
                    r->extended_irq.sharing == UACPI_SHARED ? CmResourceShareShared
                                                            : CmResourceShareDeviceExclusive;
                out[i].Flags = (r->extended_irq.triggering == UACPI_TRIGGERING_EDGE)
                    ? CM_RESOURCE_INTERRUPT_LATCHED
                    : CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
                out[i].u.Interrupt.Level    = r->extended_irq.irqs[i];
                out[i].u.Interrupt.Vector   = r->extended_irq.irqs[i];
                out[i].u.Interrupt.Affinity = (KAFFINITY)-1;
            }
        }
        return n;
    }

    case UACPI_RESOURCE_TYPE_IO:
        if (out != NULL)
        {
            RtlZeroMemory(out, sizeof(*out));
            out->Type = CmResourceTypePort;
            out->Flags = CM_RESOURCE_PORT_IO;
            out->ShareDisposition = CmResourceShareDeviceExclusive;
            out->u.Port.Start.QuadPart = r->io.minimum;
            out->u.Port.Length = r->io.length;
        }
        return 1;

    case UACPI_RESOURCE_TYPE_FIXED_IO:
        if (out != NULL)
        {
            RtlZeroMemory(out, sizeof(*out));
            out->Type = CmResourceTypePort;
            out->Flags = CM_RESOURCE_PORT_IO;
            out->ShareDisposition = CmResourceShareDeviceExclusive;
            out->u.Port.Start.QuadPart = r->fixed_io.address;
            out->u.Port.Length = r->fixed_io.length;
        }
        return 1;

    case UACPI_RESOURCE_TYPE_MEMORY32:
        if (out != NULL)
        {
            RtlZeroMemory(out, sizeof(*out));
            out->Type = CmResourceTypeMemory;
            out->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
            out->ShareDisposition = CmResourceShareDeviceExclusive;
            out->u.Memory.Start.QuadPart = r->memory32.minimum;
            out->u.Memory.Length = r->memory32.length;
        }
        return 1;

    case UACPI_RESOURCE_TYPE_FIXED_MEMORY32:
        if (out != NULL)
        {
            RtlZeroMemory(out, sizeof(*out));
            out->Type = CmResourceTypeMemory;
            out->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
            out->ShareDisposition = CmResourceShareDeviceExclusive;
            out->u.Memory.Start.QuadPart = r->fixed_memory32.address;
            out->u.Memory.Length = r->fixed_memory32.length;
        }
        return 1;

    case UACPI_RESOURCE_TYPE_ADDRESS16:
    case UACPI_RESOURCE_TYPE_ADDRESS32:
    case UACPI_RESOURCE_TYPE_ADDRESS64:
    case UACPI_RESOURCE_TYPE_ADDRESS64_EXTENDED:
    {
        ULONGLONG start, len;
        UCHAR rtype;
        if (r->type == UACPI_RESOURCE_TYPE_ADDRESS16)
        {
            rtype = r->address16.common.type;
            start = r->address16.minimum;
            len   = r->address16.address_length;
        }
        else if (r->type == UACPI_RESOURCE_TYPE_ADDRESS32)
        {
            rtype = r->address32.common.type;
            start = r->address32.minimum;
            len   = r->address32.address_length;
        }
        else if (r->type == UACPI_RESOURCE_TYPE_ADDRESS64)
        {
            rtype = r->address64.common.type;
            start = r->address64.minimum;
            len   = r->address64.address_length;
        }
        else
        {
            // Extended Address Space Descriptor (0x8B): same window as ADDRESS64.
            rtype = r->address64_extended.common.type;
            start = r->address64_extended.minimum;
            len   = r->address64_extended.address_length;
        }
        if (len == 0)
        {
            return 0;
        }
        if (out != NULL)
        {
            RtlZeroMemory(out, sizeof(*out));
            // Producer windows stay exclusive; resarb.c sub-allocates from them.
            out->ShareDisposition = CmResourceShareDeviceExclusive;
            if (rtype == UACPI_RANGE_MEMORY)
            {
                out->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
                if (len <= 0xFFFFFFFFull)
                {
                    out->Type = CmResourceTypeMemory;
                    out->u.Memory.Start.QuadPart = start;
                    out->u.Memory.Length = (ULONG)len;
                }
                else
                {
                    // Over 4 GB: MemoryLarge, LARGE_40 (length = Length40 << 8).
                    out->Type = CmResourceTypeMemoryLarge;
                    out->Flags |= CM_RESOURCE_MEMORY_LARGE_40;
                    out->u.Memory40.Start.QuadPart = start;
                    out->u.Memory40.Length40 = (ULONG)(len >> 8);
                }
            }
            else if (rtype == UACPI_RANGE_IO)
            {
                out->Type = CmResourceTypePort;
                out->Flags = CM_RESOURCE_PORT_IO;
                out->u.Port.Start.QuadPart = start;
                out->u.Port.Length = (ULONG)len;
            }
            else   // UACPI_RANGE_BUS
            {
                out->Type = CmResourceTypeBusNumber;
                out->u.BusNumber.Start = (ULONG)start;
                out->u.BusNumber.Length = (ULONG)len;
            }
        }
        return 1;
    }

    default:
        return 0;   // DMA / GPIO / serial / dependent / vendor: skipped
    }
}

// One _CRS entry -> 0+ IO_RESOURCE_DESCRIPTORs (fixed: min == max).
static ULONG
UacpiEmitIo(uacpi_resource *r, PIO_RESOURCE_DESCRIPTOR out)
{
    switch (r->type)
    {

    case UACPI_RESOURCE_TYPE_IRQ:
    case UACPI_RESOURCE_TYPE_EXTENDED_IRQ:
    {
        ULONG n = (r->type == UACPI_RESOURCE_TYPE_IRQ)
                    ? r->irq.num_irqs : r->extended_irq.num_irqs;
        ULONG i;
        UCHAR trig = (r->type == UACPI_RESOURCE_TYPE_IRQ)
                    ? r->irq.triggering : r->extended_irq.triggering;
        UCHAR shr  = (r->type == UACPI_RESOURCE_TYPE_IRQ)
                    ? r->irq.sharing : r->extended_irq.sharing;
        if (out != NULL)
        {
            for (i = 0; i < n; i++)
            {
                ULONG vec = (r->type == UACPI_RESOURCE_TYPE_IRQ)
                    ? r->irq.irqs[i] : r->extended_irq.irqs[i];
                RtlZeroMemory(&out[i], sizeof(out[i]));
                out[i].Option = (i == 0) ? 0 : IO_RESOURCE_ALTERNATIVE;
                out[i].Type = CmResourceTypeInterrupt;
                out[i].ShareDisposition =
                    shr == UACPI_SHARED ? CmResourceShareShared
                                        : CmResourceShareDeviceExclusive;
                out[i].Flags = (trig == UACPI_TRIGGERING_EDGE)
                    ? CM_RESOURCE_INTERRUPT_LATCHED
                    : CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
                out[i].u.Interrupt.MinimumVector = vec;
                out[i].u.Interrupt.MaximumVector = vec;
            }
        }
        return n;
    }

    case UACPI_RESOURCE_TYPE_IO:
    case UACPI_RESOURCE_TYPE_FIXED_IO:
    {
        ULONGLONG start = (r->type == UACPI_RESOURCE_TYPE_IO)
                    ? r->io.minimum : r->fixed_io.address;
        ULONG len = (r->type == UACPI_RESOURCE_TYPE_IO)
                    ? r->io.length : r->fixed_io.length;
        if (out != NULL)
        {
            RtlZeroMemory(out, sizeof(*out));
            out->Type = CmResourceTypePort;
            out->Flags = CM_RESOURCE_PORT_IO;
            out->ShareDisposition = CmResourceShareDeviceExclusive;
            out->u.Port.Length = len;
            out->u.Port.Alignment = 1;
            out->u.Port.MinimumAddress.QuadPart = start;
            out->u.Port.MaximumAddress.QuadPart = start + len - 1;
        }
        return 1;
    }

    case UACPI_RESOURCE_TYPE_MEMORY32:
    case UACPI_RESOURCE_TYPE_FIXED_MEMORY32:
    {
        ULONGLONG start = (r->type == UACPI_RESOURCE_TYPE_MEMORY32)
                    ? r->memory32.minimum : r->fixed_memory32.address;
        ULONG len = (r->type == UACPI_RESOURCE_TYPE_MEMORY32)
                    ? r->memory32.length : r->fixed_memory32.length;
        if (out != NULL)
        {
            RtlZeroMemory(out, sizeof(*out));
            out->Type = CmResourceTypeMemory;
            out->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
            out->ShareDisposition = CmResourceShareDeviceExclusive;
            out->u.Memory.Length = len;
            out->u.Memory.Alignment = 1;
            out->u.Memory.MinimumAddress.QuadPart = start;
            out->u.Memory.MaximumAddress.QuadPart = start + len - 1;
        }
        return 1;
    }

    case UACPI_RESOURCE_TYPE_ADDRESS16:
    case UACPI_RESOURCE_TYPE_ADDRESS32:
    case UACPI_RESOURCE_TYPE_ADDRESS64:
    case UACPI_RESOURCE_TYPE_ADDRESS64_EXTENDED:
    {
        ULONGLONG start, len;
        UCHAR rtype, dir;
        if (r->type == UACPI_RESOURCE_TYPE_ADDRESS16)
        {
            rtype = r->address16.common.type; dir = r->address16.common.direction;
            start = r->address16.minimum; len = r->address16.address_length;
        }
        else if (r->type == UACPI_RESOURCE_TYPE_ADDRESS32)
        {
            rtype = r->address32.common.type; dir = r->address32.common.direction;
            start = r->address32.minimum; len = r->address32.address_length;
        }
        else if (r->type == UACPI_RESOURCE_TYPE_ADDRESS64)
        {
            rtype = r->address64.common.type; dir = r->address64.common.direction;
            start = r->address64.minimum; len = r->address64.address_length;
        }
        else
        {
            rtype = r->address64_extended.common.type;
            dir = r->address64_extended.common.direction;
            start = r->address64_extended.minimum;
            len = r->address64_extended.address_length;
        }
        UCHAR isProducer = (dir == UACPI_PRODUCER) ? 1 : 0;
        if (len == 0)
        {
            return 0;
        }
        if (out != NULL)
        {
            RtlZeroMemory(out, sizeof(*out));
            // Exclusive; producer windows are marked by the DevicePrivate below.
            out->ShareDisposition = CmResourceShareDeviceExclusive;
            if (rtype == UACPI_RANGE_MEMORY)
            {
                out->Type = CmResourceTypeMemory;
                out->Flags = CM_RESOURCE_MEMORY_READ_WRITE;
                out->u.Memory.Length = (ULONG)len;
                out->u.Memory.Alignment = 1;
                out->u.Memory.MinimumAddress.QuadPart = start;
                out->u.Memory.MaximumAddress.QuadPart = start + len - 1;
            }
            else if (rtype == UACPI_RANGE_IO)
            {
                out->Type = CmResourceTypePort;
                out->Flags = CM_RESOURCE_PORT_IO;
                out->u.Port.Length = (ULONG)len;
                out->u.Port.Alignment = 1;
                out->u.Port.MinimumAddress.QuadPart = start;
                out->u.Port.MaximumAddress.QuadPart = start + len - 1;
            }
            else
            {
                out->Type = CmResourceTypeBusNumber;
                out->u.BusNumber.Length = (ULONG)len;
                out->u.BusNumber.MinBusNumber = (ULONG)start;
                out->u.BusNumber.MaxBusNumber = (ULONG)(start + len - 1);
            }
            if (isProducer)
            {
                // DevicePrivate (Flags = 1) marks the window for sub-allocation.
                RtlZeroMemory(&out[1], sizeof(out[1]));
                out[1].Type  = CmResourceTypeDevicePrivate;
                out[1].Flags = 1;
            }
        }
        return isProducer ? 2 : 1;
    }

    default:
        return 0;
    }
}

// _CRS -> CM_RESOURCE_LIST
NTSTATUS
UacpiCrsToCmList(uacpi_namespace_node *node, PDEVICE_OBJECT DeviceObject,
                 PCM_RESOURCE_LIST *out)
{
    uacpi_resources *res = NULL;
    uacpi_resource *r;
    ULONG count = 0;
    ULONG connections = 0;
    SIZE_T size;
    PCM_RESOURCE_LIST list;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d;

    *out = NULL;
    if (uacpi_unlikely_error(uacpi_get_current_resources(node, &res)) || res == NULL)
    {
        return STATUS_NOT_FOUND;
    }

    for (r = res->entries; r->type != UACPI_RESOURCE_TYPE_END_TAG;
         r = UACPI_NEXT_RESOURCE(r))
         {
        count += UacpiEmitCm(r, NULL);
    }

    // Connection descriptors are counted from the raw _CRS.
    if (DeviceObject != NULL)
    {
        connections = UacpiCrsConnectionCount(node, DeviceObject);
        count += connections;
    }

    if (count == 0)
    {
        uacpi_free_resources(res);
        return STATUS_NOT_FOUND;
    }

    size = FIELD_OFFSET(CM_RESOURCE_LIST, List[0].PartialResourceList.PartialDescriptors)
           + (SIZE_T)count * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
    list = (PCM_RESOURCE_LIST)ExAllocatePoolWithTag(PagedPool, size, UACPI_POOL_TAG);
    if (list == NULL)
    {
        uacpi_free_resources(res);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(list, size);
    list->Count = 1;
    list->List[0].InterfaceType = Internal;   // ACPI-provided
    list->List[0].BusNumber = 0;
    list->List[0].PartialResourceList.Version = 1;
    list->List[0].PartialResourceList.Revision = 1;
    list->List[0].PartialResourceList.Count = count;

    d = list->List[0].PartialResourceList.PartialDescriptors;
    for (r = res->entries; r->type != UACPI_RESOURCE_TYPE_END_TAG;
         r = UACPI_NEXT_RESOURCE(r))
         {
        d += UacpiEmitCm(r, d);
    }

    if (connections != 0)
    {
        UacpiCrsEmitConnectionsCm(node, DeviceObject, d);
    }

    if (UacpiResVerbose)
    {
        uacpi_object_name nm = uacpi_namespace_node_name(node);
        ULONG i;
        // Log _CRS descriptors that produced no CM output.
        for (r = res->entries; r->type != UACPI_RESOURCE_TYPE_END_TAG;
             r = UACPI_NEXT_RESOURCE(r))
             {
            if (UacpiEmitCm(r, NULL) == 0 &&
                r->type != UACPI_RESOURCE_TYPE_START_DEPENDENT &&
                r->type != UACPI_RESOURCE_TYPE_END_DEPENDENT)
                {
                UacpiTrace("[acpi] res: %c%c%c%c DROPPED _CRS descriptor type %u\n",
                          nm.text[0], nm.text[1], nm.text[2], nm.text[3], r->type);
            }
        }
        d = list->List[0].PartialResourceList.PartialDescriptors;
        for (i = 0; i < count; i++)
        {
            if (d[i].Type == CmResourceTypeMemory)
            {
                UacpiTrace("[acpi] res: %c%c%c%c MEM  %010I64X len %08X\n",
                          nm.text[0], nm.text[1], nm.text[2], nm.text[3],
                          d[i].u.Memory.Start.QuadPart, d[i].u.Memory.Length);
            }
            else if (d[i].Type == CmResourceTypeMemoryLarge)
            {
                UacpiTrace("[acpi] res: %c%c%c%c MEML %010I64X len40 %08X\n",
                          nm.text[0], nm.text[1], nm.text[2], nm.text[3],
                          d[i].u.Memory40.Start.QuadPart, d[i].u.Memory40.Length40);
            }
            else if (d[i].Type == CmResourceTypePort)
            {
                UacpiTrace("[acpi] res: %c%c%c%c IO   %010I64X len %08X\n",
                          nm.text[0], nm.text[1], nm.text[2], nm.text[3],
                          d[i].u.Port.Start.QuadPart, d[i].u.Port.Length);
            }
            else if (d[i].Type == CmResourceTypeBusNumber)
            {
                UacpiTrace("[acpi] res: %c%c%c%c BUS  %u..%u\n",
                          nm.text[0], nm.text[1], nm.text[2], nm.text[3],
                          d[i].u.BusNumber.Start,
                          d[i].u.BusNumber.Start + d[i].u.BusNumber.Length - 1);
            }
        }
    }

    uacpi_free_resources(res);
    *out = list;
    return STATUS_SUCCESS;
}

// _CRS -> IO_RESOURCE_REQUIREMENTS_LIST (single alternative list of fixed reqs)
NTSTATUS
UacpiPrsToRequirements(uacpi_namespace_node *node, BOOLEAN Possible,
                      PDEVICE_OBJECT DeviceObject,
                      PIO_RESOURCE_REQUIREMENTS_LIST *out)
{
    ULONG connections = 0;
    uacpi_resources *res = NULL;
    uacpi_resource *r;
    ULONG count = 0;
    SIZE_T size;
    PIO_RESOURCE_REQUIREMENTS_LIST list;
    PIO_RESOURCE_DESCRIPTOR d;
    uacpi_status ust;

    *out = NULL;

    // _PRS: choices for the arbiter, committed with _SRS. _CRS: fixed.
    ust = Possible ? uacpi_get_possible_resources(node, &res)
                   : uacpi_get_current_resources(node, &res);
    if (uacpi_unlikely_error(ust) || res == NULL)
    {
        return STATUS_NOT_FOUND;
    }

    for (r = res->entries; r->type != UACPI_RESOURCE_TYPE_END_TAG;
         r = UACPI_NEXT_RESOURCE(r))
         {
        count += UacpiEmitIo(r, NULL);
    }

    // Connection() descriptors come from the raw _CRS; _PRS never has them.
    if (!Possible && DeviceObject != NULL)
    {
        connections = UacpiCrsConnectionCount(node, DeviceObject);
        count += connections;
    }

    if (count == 0)
    {
        uacpi_free_resources(res);
        return STATUS_NOT_FOUND;
    }

    // One IO_RESOURCE_LIST with 'count' descriptors.
    size = FIELD_OFFSET(IO_RESOURCE_REQUIREMENTS_LIST, List[0].Descriptors)
           + (SIZE_T)count * sizeof(IO_RESOURCE_DESCRIPTOR);
    list = (PIO_RESOURCE_REQUIREMENTS_LIST)ExAllocatePoolWithTag(PagedPool, size,
                                                                 UACPI_POOL_TAG);
    if (list == NULL)
    {
        uacpi_free_resources(res);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(list, size);
    list->ListSize = (ULONG)size;
    list->InterfaceType = Internal;
    list->BusNumber = 0;
    list->AlternativeLists = 1;
    list->List[0].Version = 1;
    list->List[0].Revision = 1;
    list->List[0].Count = count;

    d = list->List[0].Descriptors;
    for (r = res->entries; r->type != UACPI_RESOURCE_TYPE_END_TAG;
         r = UACPI_NEXT_RESOURCE(r))
         {
        d += UacpiEmitIo(r, d);
    }

    if (connections != 0)
    {
        d += UacpiCrsEmitConnections(node, DeviceObject, d);
    }

    uacpi_free_resources(res);
    *out = list;
    return STATUS_SUCCESS;
}

NTSTATUS
UacpiCrsToRequirements(uacpi_namespace_node *node, PDEVICE_OBJECT DeviceObject,
                       PIO_RESOURCE_REQUIREMENTS_LIST *out)
{
    return UacpiPrsToRequirements(node, FALSE, DeviceObject, out);
}

// CM form of a connection from the IO form the hub returned.
static
VOID
UacpiConnectionIoToCm(
    _In_ PIO_RESOURCE_DESCRIPTOR Io,
    _Out_ PCM_PARTIAL_RESOURCE_DESCRIPTOR Cm)
{
    RtlZeroMemory(Cm, sizeof(*Cm));

    Cm->Type = CmResourceTypeConnection;
    Cm->ShareDisposition = Io->ShareDisposition;
    Cm->Flags = Io->Flags;
    Cm->u.Connection.Class = Io->u.Connection.Class;
    Cm->u.Connection.Type = Io->u.Connection.Type;
    Cm->u.Connection.IdLowPart = Io->u.Connection.IdLowPart;
    Cm->u.Connection.IdHighPart = Io->u.Connection.IdHighPart;
}

// Walk the raw _CRS for Connection() descriptors; the hub needs raw bytes.
static ULONG
UacpiForEachCrsConnection(uacpi_namespace_node *node,
                          PDEVICE_OBJECT DeviceObject,
                          PIO_RESOURCE_DESCRIPTOR IoOut,
                          PCM_PARTIAL_RESOURCE_DESCRIPTOR CmOut)
{
    uacpi_object *obj = NULL;
    uacpi_data_view view;
    ULONG count = 0;
    SIZE_T offset = 0;

    if (uacpi_unlikely_error(uacpi_eval_typed(node, "_CRS", NULL,
                                              UACPI_OBJECT_BUFFER_BIT, &obj)) ||
        obj == NULL)
    {
        return 0;
    }

    if (uacpi_unlikely_error(uacpi_object_get_buffer(obj, &view)) ||
        view.bytes == NULL)
    {
        uacpi_object_unref(obj);
        return 0;
    }

    while (offset < view.length)
    {
        const UCHAR *item = (const UCHAR *)view.bytes + offset;
        SIZE_T itemLength;

        if ((item[0] & ACPI_RSRC_LARGE_ITEM) == 0)
        {
            if ((item[0] & ACPI_RSRC_SMALL_NAME_MASK) == ACPI_RSRC_SMALL_NAME_END)
            {
                break;
            }
            itemLength = (SIZE_T)(item[0] & ACPI_RSRC_SMALL_LEN_MASK) +
                         ACPI_RSRC_SMALL_HEADER_SIZE;
        }
        else
        {
            if (offset + ACPI_RSRC_LARGE_HEADER_SIZE > view.length)
            {
                break;
            }
            itemLength = (SIZE_T)item[1] | ((SIZE_T)item[2] << 8);
            itemLength += ACPI_RSRC_LARGE_HEADER_SIZE;
        }

        if (itemLength == 0 || offset + itemLength > view.length)
        {
            break;
        }

        if (item[0] == GPIO_INTERRUPT_IO_DESCRIPTOR ||
            item[0] == FUNCTION_CONFIG_DESCRIPTOR ||
            item[0] == SERIAL_BUS_DESCRIPTOR)
        {
            // Ask the hub on both passes so the count matches the fill.
            IO_RESOURCE_DESCRIPTOR Translated;

            RtlZeroMemory(&Translated, sizeof(Translated));
            if (NT_SUCCESS(UacpiTranslateConnectionDescriptor(
                    DeviceObject, (PVOID)item, (ULONG)itemLength, &Translated)))
            {
                if (IoOut != NULL)
                {
                    IoOut[count] = Translated;
                }

                if (CmOut != NULL)
                {
                    UacpiConnectionIoToCm(&Translated, &CmOut[count]);
                }

                count++;
            }
        }

        offset += itemLength;
    }

    uacpi_object_unref(obj);
    return count;
}

// Translation is idempotent; count and fill passes get the same ids.
ULONG
UacpiCrsConnectionCount(uacpi_namespace_node *node, PDEVICE_OBJECT DeviceObject)
{
    return UacpiForEachCrsConnection(node, DeviceObject, NULL, NULL);
}

ULONG
UacpiCrsEmitConnections(uacpi_namespace_node *node, PDEVICE_OBJECT DeviceObject,
                        PIO_RESOURCE_DESCRIPTOR out)
{
    return UacpiForEachCrsConnection(node, DeviceObject, out, NULL);
}

ULONG
UacpiCrsEmitConnectionsCm(uacpi_namespace_node *node, PDEVICE_OBJECT DeviceObject,
                          PCM_PARTIAL_RESOURCE_DESCRIPTOR out)
{
    return UacpiForEachCrsConnection(node, DeviceObject, NULL, out);
}
