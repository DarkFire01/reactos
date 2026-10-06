/*
 * PROJECT:     ReactOS USB4 Device Router Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Router operation mailbox (ROUTER_CS_9 to ROUTER_CS_26) and DROM read
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "usb4dr.h"

#define NDEBUG
#include <debug.h>

NTSTATUS
Usb4DrRouterOps::Create(
    _In_ Usb4DrFdo* Fdo)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;

    m_Fdo = Fdo;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Fdo->Device();
    Status = WdfWaitLockCreate(&Attributes, &m_Lock);
    if (!NT_SUCCESS(Status))
        DPRINT1("Router operation lock creation failed 0x%lx\n", Status);

    return Status;
}

NTSTATUS
Usb4DrRouterOps::WaitForOperationDone(
    _Out_ PULONG OpcodeRegister)
{
    Usb4DrRouter* Router = m_Fdo->Router();
    ULONG Poll;
    NTSTATUS Status;

    for (Poll = 0; ; Poll++)
    {
        Status = Router->ReadDwords(USB4DR_ROUTER_CS_OPCODE, 1, OpcodeRegister);
        if (!NT_SUCCESS(Status))
            return Status;

        if (!(*OpcodeRegister & USB4DR_ROUTER_CS26_VALID))
            return STATUS_SUCCESS;

        if (Poll >= USB4DR_ROP_POLL_COUNT)
            return STATUS_IO_TIMEOUT;

        Usb4DrSleepMs(USB4DR_ROP_POLL_MS);
    }
}

NTSTATUS
Usb4DrRouterOps::Execute(
    _In_ USHORT Opcode,
    _In_opt_ const ULONG* MetadataIn,
    _In_reads_opt_(DataInDwords) const ULONG* DataIn,
    _In_ ULONG DataInDwords,
    _Out_opt_ PULONG MetadataOut,
    _Out_writes_opt_(DataOutDwords) PULONG DataOut,
    _In_ ULONG DataOutDwords)
{
    Usb4DrRouter* Router = m_Fdo->Router();
    ULONG Register;
    NTSTATUS Status;

    if (MetadataOut)
        *MetadataOut = 0;
    if (DataOut)
        RtlZeroMemory(DataOut, DataOutDwords * sizeof(*DataOut));

    if (DataInDwords > USB4DR_ROUTER_DATA_DWORDS || DataOutDwords > USB4DR_ROUTER_DATA_DWORDS)
        return STATUS_INVALID_PARAMETER;

    Usb4DrWaitLockGuard Guard(m_Lock);

    /* The mailbox is free only once the router finished whatever ran before */
    Status = WaitForOperationDone(&Register);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Router operation 0x%x: mailbox busy 0x%lx\n", Opcode, Status);
        return Status;
    }

    if (DataIn && DataInDwords != 0)
    {
        Status = Router->WriteDwords(USB4DR_ROUTER_CS_DATA, DataInDwords, DataIn);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    if (MetadataIn)
    {
        Status = Router->WriteDwords(USB4DR_ROUTER_CS_METADATA, 1, MetadataIn);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    Status = Router->ReadDwords(USB4DR_ROUTER_CS_OPCODE, 1, &Register);
    if (!NT_SUCCESS(Status))
        return Status;

    Register = (Register & ~USB4DR_ROUTER_CS26_OPCODE_MASK) | Opcode;
    Status = Router->WriteDwords(USB4DR_ROUTER_CS_OPCODE, 1, &Register);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = Router->ReadDwords(USB4DR_ROUTER_CS_OPCODE, 1, &Register);
    if (!NT_SUCCESS(Status))
        return Status;

    Register |= USB4DR_ROUTER_CS26_VALID;
    Status = Router->WriteDwords(USB4DR_ROUTER_CS_OPCODE, 1, &Register);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WaitForOperationDone(&Register);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Router operation 0x%x did not complete 0x%lx\n", Opcode, Status);
        return Status;
    }

    if (Register & USB4DR_ROUTER_CS26_NOT_SUPPORTED)
    {
        DPRINT1("Router operation 0x%x is not supported\n", Opcode);
        return STATUS_NOT_SUPPORTED;
    }

    if (Register & USB4DR_ROUTER_CS26_STATUS_MASK)
    {
        DPRINT1("Router operation 0x%x failed with status 0x%lx\n",
                Opcode, Usb4DrField(Register, USB4DR_ROUTER_CS26_STATUS_MASK));
        return STATUS_UNSUCCESSFUL;
    }

    if (MetadataOut)
    {
        Status = Router->ReadDwords(USB4DR_ROUTER_CS_METADATA, 1, MetadataOut);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    if (DataOut && DataOutDwords != 0)
        Status = Router->ReadDwords(USB4DR_ROUTER_CS_DATA, DataOutDwords, DataOut);

    return Status;
}

NTSTATUS
Usb4DrRouterOps::ReadDromChunk(
    _In_ ULONG DwordAddress,
    _In_ ULONG DwordCount,
    _Out_writes_(DwordCount) PULONG Buffer,
    _Out_ PULONG DwordsRead)
{
    ULONG Request;
    ULONG Reply;
    ULONG ReplyCount;
    NTSTATUS Status;

    *DwordsRead = 0;

    Request = ((DwordAddress << USB4DR_DROM_META_ADDRESS_SHIFT) & USB4DR_DROM_META_ADDRESS_MASK) |
              ((DwordCount << USB4DR_DROM_META_COUNT_SHIFT) & USB4DR_DROM_META_COUNT_MASK);

    Status = Execute(USB4DR_ROP_DROM_READ, &Request, NULL, 0, &Reply, Buffer, DwordCount);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The router answers with the address it read and how many dwords it returned */
    ReplyCount = Usb4DrField(Reply, USB4DR_DROM_META_COUNT_MASK);
    if (Usb4DrField(Reply, USB4DR_DROM_META_ADDRESS_MASK) != DwordAddress ||
        ReplyCount == 0 || ReplyCount > DwordCount)
    {
        DPRINT1("DROM read at dword %lu returned metadata 0x%lx\n", DwordAddress, Reply);
        return STATUS_UNSUCCESSFUL;
    }

    *DwordsRead = ReplyCount;
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrRouterOps::ReadDrom(
    _Outptr_result_bytebuffer_(*Size) PUCHAR* Drom,
    _Out_ PULONG Size)
{
    ULONG Header[USB4DR_DROM_HEADER_BYTES / sizeof(ULONG)];
    PUCHAR HeaderBytes = (PUCHAR)Header;
    PULONG Buffer;
    ULONG DataLength;
    ULONG TotalBytes;
    ULONG TotalDwords;
    ULONG Offset;
    ULONG Count;
    ULONG Read;
    NTSTATUS Status;

    *Drom = NULL;
    *Size = 0;

    Status = ReadDromChunk(0, RTL_NUMBER_OF(Header), Header, &Read);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("DROM header read failed 0x%lx\n", Status);
        return Status;
    }

    DataLength = (HeaderBytes[USB4DR_DROM_LENGTH_OFFSET] |
                  (HeaderBytes[USB4DR_DROM_LENGTH_OFFSET + 1] << 8)) & USB4DR_DROM_LENGTH_MASK;
    if (DataLength < USB4DR_DROM_LENGTH_MIN)
    {
        DPRINT1("DROM data length %lu is too small\n", DataLength);
        return STATUS_UNSUCCESSFUL;
    }

    TotalBytes = DataLength + USB4DR_DROM_SIZE_EXTRA;
    TotalDwords = (TotalBytes + sizeof(ULONG) - 1) / sizeof(ULONG);

    Buffer = (PULONG)ExAllocatePoolWithTag(NonPagedPool, TotalDwords * sizeof(*Buffer), USB4DR_TAG_DROM);
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Buffer, TotalDwords * sizeof(*Buffer));
    RtlCopyMemory(Buffer, Header, sizeof(Header));

    /* The header is always taken as 4 dwords; the body follows 16 dwords at a time */
    for (Offset = RTL_NUMBER_OF(Header); Offset < TotalDwords; Offset += Read)
    {
        Count = min(TotalDwords - Offset, (ULONG)USB4DR_ROUTER_DATA_DWORDS);

        Status = ReadDromChunk(Offset, Count, &Buffer[Offset], &Read);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("DROM body read at dword %lu failed 0x%lx\n", Offset, Status);
            ExFreePoolWithTag(Buffer, USB4DR_TAG_DROM);
            return Status;
        }
    }

    DPRINT("DROM read, %lu bytes\n", TotalBytes);
    *Drom = (PUCHAR)Buffer;
    *Size = TotalBytes;
    return STATUS_SUCCESS;
}

NTSTATUS
Usb4DrRouterOps::QueryBufferAllocation(
    _Out_writes_(USB4DR_BUFFER_MAX_USB3_GEN_T + 1) PUCHAR Values)
{
    ULONG Data[USB4DR_ROUTER_DATA_DWORDS];
    ULONG Metadata;
    ULONG Count;
    ULONG Index;
    ULONG Parameter;
    ULONG Value;
    NTSTATUS Status;

    RtlZeroMemory(Values, USB4DR_BUFFER_MAX_USB3_GEN_T + 1);

    Status = Execute(USB4DR_ROP_BUFFER_ALLOCATION, NULL, NULL, 0, &Metadata, Data, RTL_NUMBER_OF(Data));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Buffer allocation request failed 0x%lx\n", Status);
        return Status;
    }

    /* The low byte of the metadata is the number of parameters the router returned */
    Count = Metadata & 0xFF;
    if (Count > RTL_NUMBER_OF(Data))
    {
        DPRINT1("Buffer allocation request returned %lu parameters\n", Count);
        return STATUS_UNSUCCESSFUL;
    }

    for (Index = 0; Index < Count; Index++)
    {
        Parameter = Usb4DrField(Data[Index], USB4DR_BUFFER_PARAM_INDEX_MASK);
        Value = Usb4DrField(Data[Index], USB4DR_BUFFER_PARAM_VALUE_MASK);
        if (Value > MAXUCHAR)
        {
            DPRINT1("Buffer allocation parameter %lu value %lu out of range\n", Parameter, Value);
            return STATUS_INVALID_PARAMETER;
        }

        DPRINT("Buffer allocation parameter %lu = %lu\n", Parameter, Value);
        if (Parameter <= USB4DR_BUFFER_MAX_USB3_GEN_T)
            Values[Parameter] = (UCHAR)Value;
    }

    return STATUS_SUCCESS;
}
