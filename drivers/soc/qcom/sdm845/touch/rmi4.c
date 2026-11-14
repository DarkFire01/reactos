/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Synaptics RMI4 register protocol over an SPB I2C target
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "qcomtouch.h"

#include <debug.h>

#define RMI_WRITE_MAX           32
#define RMI_PRESENCE_MAX        35
#define RMI_STRUCT_MAX          128

static
USHORT
RmiLe16(
    _In_reads_bytes_(2) const UCHAR *Buffer)
{
    return (USHORT)Buffer[0] | ((USHORT)Buffer[1] << 8);
}

static
NTSTATUS
RmiSetPage(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ UCHAR Page)
{
    UCHAR Command[2];
    NTSTATUS Status;

    if (Page == Device->Page)
        return STATUS_SUCCESS;

    Command[0] = RMI_PAGE_SELECT;
    Command[1] = Page;
    Status = QcTouchI2cWrite(Device, Command, sizeof(Command));
    if (NT_SUCCESS(Status))
        Device->Page = Page;
    else
        Device->Page = 0xFF;

    return Status;
}

static
NTSTATUS
RmiReadBlock(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ USHORT Address,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length)
{
    UCHAR Register = (UCHAR)(Address & 0xFF);
    NTSTATUS Status;

    Status = RmiSetPage(Device, (UCHAR)(Address >> 8));
    if (!NT_SUCCESS(Status))
        return Status;

    return QcTouchI2cWriteRead(Device, &Register, sizeof(Register), Buffer, Length);
}

static
NTSTATUS
RmiWriteBlock(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ USHORT Address,
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ ULONG Length)
{
    UCHAR Command[1 + RMI_WRITE_MAX];
    NTSTATUS Status;

    if (Length > RMI_WRITE_MAX)
        return STATUS_INVALID_PARAMETER;

    Status = RmiSetPage(Device, (UCHAR)(Address >> 8));
    if (!NT_SUCCESS(Status))
        return Status;

    Command[0] = (UCHAR)(Address & 0xFF);
    RtlCopyMemory(&Command[1], Buffer, Length);

    return QcTouchI2cWrite(Device, Command, Length + 1);
}

static
NTSTATUS
RmiReadByte(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ USHORT Address,
    _Out_ PUCHAR Value)
{
    return RmiReadBlock(Device, Address, Value, 1);
}

static
NTSTATUS
RmiWriteByte(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ USHORT Address,
    _In_ UCHAR Value)
{
    return RmiWriteBlock(Device, Address, &Value, 1);
}

/* One packet register: a size then a run of subpacket presence bytes */
static
ULONG
RmiParseRegisterItem(
    _In_reads_bytes_(Size) const UCHAR *Buffer,
    _In_ ULONG Size,
    _Out_ PRMI_REG_ITEM Item)
{
    ULONG Offset = 0;
    ULONG MapOffset = 0;
    ULONG Bit;
    UCHAR Byte;

    if (Offset >= Size)
        return 0;

    Item->Size = Buffer[Offset++];
    if (Item->Size == 0 && Size - Offset >= 2)
    {
        Item->Size = RmiLe16(&Buffer[Offset]);
        Offset += 2;
    }
    if (Item->Size == 0 && Size - Offset >= 4)
    {
        Item->Size = Buffer[Offset] | ((ULONG)Buffer[Offset + 1] << 8) |
                     ((ULONG)Buffer[Offset + 2] << 16) | ((ULONG)Buffer[Offset + 3] << 24);
        Offset += 4;
    }

    Item->SubpacketCount = 0;
    Item->HasSubpacket0 = FALSE;
    do
    {
        if (Offset >= Size)
            return 0;

        Byte = Buffer[Offset];
        for (Bit = 0; Bit < 7; Bit++)
        {
            if (Byte & (1 << Bit))
            {
                if (MapOffset == 0)
                    Item->HasSubpacket0 = TRUE;
                Item->SubpacketCount++;
            }
            MapOffset++;
        }
    } while (Buffer[Offset++] & 0x80);

    return Offset;
}

/* F12 describes its query, control and data registers through three of these */
static
NTSTATUS
RmiReadRegisterDesc(
    _In_ PQCTOUCH_DEVICE Device,
    _In_ USHORT Address,
    _Out_ PRMI_REG_DESC Descriptor)
{
    UCHAR Presence[RMI_PRESENCE_MAX];
    UCHAR Structure[RMI_STRUCT_MAX];
    UCHAR SizePresence;
    ULONG StructSize;
    ULONG PresenceOffset;
    ULONG PresentRegisters[RMI_REG_DESC_MAX_REGS];
    ULONG PresentCount = 0;
    ULONG MapOffset = 0;
    ULONG Cursor = 0;
    ULONG Index;
    ULONG Bit;
    NTSTATUS Status;

    RtlZeroMemory(Descriptor, sizeof(*Descriptor));

    Status = RmiReadByte(Device, Address, &SizePresence);
    if (!NT_SUCCESS(Status))
        return Status;
    if (SizePresence < 1 || SizePresence > RMI_PRESENCE_MAX)
    {
        DPRINT1("F12 presence register size %u\n", SizePresence);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Status = RmiReadBlock(Device, Address + 1, Presence, SizePresence);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Presence[0] == 0)
    {
        StructSize = RmiLe16(&Presence[1]);
        PresenceOffset = 3;
    }
    else
    {
        StructSize = Presence[0];
        PresenceOffset = 1;
    }

    if (StructSize == 0 || StructSize > RMI_STRUCT_MAX)
    {
        DPRINT1("F12 structure size %lu\n", StructSize);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    for (Index = PresenceOffset; Index < SizePresence; Index++)
    {
        for (Bit = 0; Bit < 8; Bit++)
        {
            if ((Presence[Index] & (1 << Bit)) && PresentCount < RMI_REG_DESC_MAX_REGS)
                PresentRegisters[PresentCount++] = MapOffset;
            MapOffset++;
        }
    }

    Status = RmiReadBlock(Device, Address + 2, Structure, StructSize);
    if (!NT_SUCCESS(Status))
        return Status;

    for (Index = 0; Index < PresentCount; Index++)
    {
        RMI_REG_ITEM Item;
        ULONG Consumed;

        if (Cursor >= StructSize)
            break;

        Consumed = RmiParseRegisterItem(&Structure[Cursor], StructSize - Cursor, &Item);
        if (Consumed == 0)
            break;

        Item.Register = PresentRegisters[Index];
        Cursor += Consumed;

        if (Item.Size != 0)
            Descriptor->Items[Descriptor->Count++] = Item;
    }

    return STATUS_SUCCESS;
}

static
PRMI_REG_ITEM
RmiFindRegister(
    _In_ PRMI_REG_DESC Descriptor,
    _In_ ULONG Register)
{
    ULONG Index;

    for (Index = 0; Index < Descriptor->Count; Index++)
    {
        if (Descriptor->Items[Index].Register == Register)
            return &Descriptor->Items[Index];
    }

    return NULL;
}

/* Control and query registers are addressed by their position in the present set */
static
ULONG
RmiRegisterIndex(
    _In_ PRMI_REG_DESC Descriptor,
    _In_ ULONG Register)
{
    ULONG Index;

    for (Index = 0; Index < Descriptor->Count; Index++)
    {
        if (Descriptor->Items[Index].Register == Register)
            return Index;
    }

    return 0;
}

/* Data registers pack into one block, so a data register sits at a byte offset */
static
ULONG
RmiRegisterByteOffset(
    _In_ PRMI_REG_DESC Descriptor,
    _In_ ULONG Register)
{
    ULONG Offset = 0;
    ULONG Index;

    for (Index = 0; Index < Descriptor->Count; Index++)
    {
        if (Descriptor->Items[Index].Register == Register)
            break;
        Offset += Descriptor->Items[Index].Size;
    }

    return Offset;
}

NTSTATUS
NTAPI
RmiScanFunctions(
    _In_ PQCTOUCH_DEVICE Device)
{
    UCHAR Entry[RMI_PDT_ENTRY_SIZE];
    ULONG InterruptBits = 0;
    ULONG Page;
    ULONG EmptyPages = 0;
    ULONG Retry;
    NTSTATUS Status = STATUS_IO_DEVICE_ERROR;

    /* The controller NACKs every access while it finishes its own power-on,
     * so wait for it to acknowledge before trusting the descriptor table */
    for (Retry = 0; Retry < 15; Retry++)
    {
        LARGE_INTEGER Wait;
        UCHAR Value;

        Device->Page = 0xFF;
        Status = RmiReadBlock(Device, 0x0000, &Value, sizeof(Value));
        if (NT_SUCCESS(Status))
            break;

        Wait.QuadPart = -10000LL * 20;
        KeDelayExecutionThread(KernelMode, FALSE, &Wait);
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("touch controller never acknowledged, 0x%08lx\n", Status);
        return Status;
    }

    for (Page = 0; Page <= 0xFF; Page++)
    {
        USHORT PageStart = (USHORT)(Page * RMI_PAGE_SIZE);
        LONG Address;
        BOOLEAN FoundAny = FALSE;

        for (Address = PageStart + RMI_PDT_SCAN_START;
             Address >= (LONG)(PageStart + RMI_PDT_SCAN_END);
             Address -= RMI_PDT_ENTRY_SIZE)
        {
            PRMI_FUNCTION Function = NULL;
            UCHAR Number;

            Status = RmiReadBlock(Device, (USHORT)Address, Entry, sizeof(Entry));
            if (!NT_SUCCESS(Status))
                return Status;

            Number = Entry[5];
            if (Number == 0x00 || Number == 0xFF)
                break;

            FoundAny = TRUE;

            if (Number == RMI_FUNCTION_F01)
                Function = &Device->F01;
            else if (Number == RMI_FUNCTION_F12)
                Function = &Device->F12;

            if (Function)
            {
                Function->Found = TRUE;
                Function->QueryBase = (USHORT)(PageStart + Entry[0]);
                Function->CommandBase = (USHORT)(PageStart + Entry[1]);
                Function->ControlBase = (USHORT)(PageStart + Entry[2]);
                Function->DataBase = (USHORT)(PageStart + Entry[3]);
                Function->InterruptCount = Entry[4] & RMI_PDT_INT_SOURCE_MASK;
                Function->InterruptPosition = InterruptBits;
            }

            InterruptBits += Entry[4] & RMI_PDT_INT_SOURCE_MASK;
        }

        if (!FoundAny)
        {
            EmptyPages++;
            if (EmptyPages >= 2)
                break;
        }
        else
        {
            EmptyPages = 0;
        }
    }

    Device->InterruptRegisters = (InterruptBits + 7) / 8;
    if (Device->InterruptRegisters == 0)
        Device->InterruptRegisters = 1;
    if (Device->InterruptRegisters > 4)
        Device->InterruptRegisters = 4;

    if (!Device->F01.Found || !Device->F12.Found)
    {
        DPRINT1("descriptor table lacks F01 or F12\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    DPRINT("F01 ctrl %04x data %04x irq %lu, F12 query %04x data %04x irq %lu, %lu irq regs\n",
           Device->F01.ControlBase, Device->F01.DataBase, Device->F01.InterruptPosition,
           Device->F12.QueryBase, Device->F12.DataBase, Device->F12.InterruptPosition,
           Device->InterruptRegisters);

    return STATUS_SUCCESS;
}

static
NTSTATUS
RmiConfigureF12(
    _In_ PQCTOUCH_DEVICE Device)
{
    RMI_REG_DESC ControlDesc;
    RMI_REG_DESC DataDesc;
    RMI_REG_DESC QueryDesc;
    PRMI_REG_ITEM Item;
    UCHAR Tuning[RMI_READ_MAX];
    USHORT QueryAddress;
    UCHAR General;
    ULONG Index;
    NTSTATUS Status;

    Status = RmiReadByte(Device, Device->F12.QueryBase, &General);
    if (!NT_SUCCESS(Status))
        return Status;
    if (!(General & RMI_F12_QUERY_HAS_DESC))
        return STATUS_NOT_SUPPORTED;

    QueryAddress = Device->F12.QueryBase + 1;
    Status = RmiReadRegisterDesc(Device, QueryAddress, &QueryDesc);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = RmiReadRegisterDesc(Device, QueryAddress + 3, &ControlDesc);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = RmiReadRegisterDesc(Device, QueryAddress + 6, &DataDesc);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Control 8 carries the maximum X and Y the sensor reports */
    Item = RmiFindRegister(&ControlDesc, RMI_F12_CTRL_SENSOR_TUNING);
    if (!Item || Item->Size > sizeof(Tuning))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    Index = RmiRegisterIndex(&ControlDesc, RMI_F12_CTRL_SENSOR_TUNING);
    Status = RmiReadBlock(Device, Device->F12.ControlBase + (USHORT)Index, Tuning, Item->Size);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Item->HasSubpacket0 && Item->Size >= 4)
    {
        Device->MaxX = RmiLe16(&Tuning[0]);
        Device->MaxY = RmiLe16(&Tuning[2]);
    }
    if (Device->MaxX == 0 || Device->MaxY == 0)
    {
        DPRINT1("F12 reports max %lu x %lu\n", Device->MaxX, Device->MaxY);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Data 1 is the list of sensed objects, one eight byte slot each */
    Item = RmiFindRegister(&DataDesc, RMI_F12_DATA_SENSED_OBJECTS);
    if (!Item)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    Device->Data1Offset = RmiRegisterByteOffset(&DataDesc, RMI_F12_DATA_SENSED_OBJECTS);
    Device->FingerCount = min(Item->SubpacketCount, QCTOUCH_MAX_CONTACTS);

    /* The reader only pulls the object slots, so bound them to one short read */
    while (Device->Data1Offset + Device->FingerCount * RMI_F12_FINGER_BYTES > RMI_READ_MAX)
        Device->FingerCount--;
    if (Device->FingerCount == 0)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    Device->DataPacketSize = Device->Data1Offset + Device->FingerCount * RMI_F12_FINGER_BYTES;

    DPRINT1("F12 max %lu x %lu, %lu slots at data offset %lu\n",
           Device->MaxX, Device->MaxY, Device->FingerCount, Device->Data1Offset);

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RmiConfigure(
    _In_ PQCTOUCH_DEVICE Device)
{
    UCHAR Enable[4] = { 0 };
    UCHAR Control0;
    UCHAR Status8;
    ULONG Bit;
    LARGE_INTEGER Delay;
    NTSTATUS Status;

    /* A firmware reset brings the chip to a known state */
    Status = RmiWriteByte(Device, Device->F01.CommandBase, RMI_F01_CMD_RESET);
    if (!NT_SUCCESS(Status))
        return Status;

    Delay.QuadPart = -10000LL * RMI_RESET_DELAY_MS;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);

    Status = RmiConfigureF12(Device);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Keep the sensor awake and mark the configuration applied */
    Status = RmiReadByte(Device, Device->F01.ControlBase, &Control0);
    if (!NT_SUCCESS(Status))
        return Status;

    Control0 &= ~RMI_F01_CTRL0_SLEEP_MASK;
    Control0 |= RMI_F01_CTRL0_NOSLEEP | RMI_F01_CTRL0_CONFIGURED;
    Status = RmiWriteByte(Device, Device->F01.ControlBase, Control0);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Let the F12 interrupt sources through the F01 interrupt enable */
    for (Bit = Device->F12.InterruptPosition;
         Bit < Device->F12.InterruptPosition + Device->F12.InterruptCount;
         Bit++)
    {
        Enable[Bit / 8] |= (UCHAR)(1 << (Bit % 8));
    }

    Status = RmiWriteBlock(Device, Device->F01.ControlBase + 1, Enable, Device->InterruptRegisters);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = RmiReadByte(Device, Device->F01.DataBase, &Status8);
    if (NT_SUCCESS(Status) && (Status8 & RMI_F01_STATUS_UNCONFIGURED))
        DPRINT1("F01 still reports unconfigured, status %02x\n", Status8);

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
RmiSleep(
    _In_ PQCTOUCH_DEVICE Device)
{
    UCHAR Control0;
    NTSTATUS Status;

    Status = RmiReadByte(Device, Device->F01.ControlBase, &Control0);
    if (!NT_SUCCESS(Status))
        return Status;

    Control0 &= ~(RMI_F01_CTRL0_SLEEP_MASK | RMI_F01_CTRL0_NOSLEEP);
    Control0 |= RMI_F01_CTRL0_SENSOR_SLEEP;
    return RmiWriteByte(Device, Device->F01.ControlBase, Control0);
}

/**
 * @brief Reads and so acknowledges the interrupt status, then the F12 object
 * slots when F12 raised it.
 *
 * @return STATUS_NO_MORE_ENTRIES when no source is pending.
 */
NTSTATUS
NTAPI
RmiReadAttention(
    _In_ PQCTOUCH_DEVICE Device,
    _Out_ PBOOLEAN HasObjects,
    _Out_writes_bytes_(RMI_READ_MAX) PUCHAR Packet)
{
    UCHAR Pending[4] = { 0 };
    ULONG Bit;
    BOOLEAN Any = FALSE;
    NTSTATUS Status;

    *HasObjects = FALSE;

    Status = RmiReadBlock(Device, Device->F01.DataBase + 1, Pending, Device->InterruptRegisters);
    if (!NT_SUCCESS(Status))
        return Status;

    for (Bit = 0; Bit < Device->InterruptRegisters; Bit++)
    {
        if (Pending[Bit])
            Any = TRUE;
    }

    if (!Any)
        return STATUS_NO_MORE_ENTRIES;

    for (Bit = Device->F12.InterruptPosition;
         Bit < Device->F12.InterruptPosition + Device->F12.InterruptCount;
         Bit++)
    {
        if (Pending[Bit / 8] & (1 << (Bit % 8)))
            *HasObjects = TRUE;
    }

    if (!*HasObjects)
        return STATUS_SUCCESS;

    return RmiReadBlock(Device, Device->F12.DataBase, Packet, Device->DataPacketSize);
}
