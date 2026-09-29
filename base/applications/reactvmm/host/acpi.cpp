/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The tables a machine describes itself with
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Every one of these is a header, a body, and a byte chosen so that the whole
 * thing adds up to nothing. What finds them starts from a pointer sitting at a
 * fixed place, reads a table of addresses out of it, and looks through those
 * for the four letters it wants. A machine that offers none of it is not a
 * machine an operating system of this age will start on: it looks for the one
 * describing power and stops where it should have found it.
 *
 * These are written into the machine's memory rather than built by the
 * firmware, because what is in them is decided by how the machine was put
 * together and the firmware is the one part that is the same every time.
 */

#include "acpi.h"

#include <string.h>

namespace rtvm
{

/* What each of them is called, in the four letters anything looks for */
#define ACPI_POINTER_NAME   "RSD PTR "
#define ACPI_ROOT_NAME      "RSDT"
#define ACPI_WIDE_ROOT_NAME "XSDT"
#define ACPI_POWER_NAME     "FACP"
#define ACPI_SHARED_NAME    "FACS"
#define ACPI_DEFINITION     "DSDT"
#define ACPI_LINES_NAME     "APIC"

/* Whose machine this is, which is only ever read back by people */
#define ACPI_MAKER          "RTVMM "
#define ACPI_MAKER_TABLE    "RTVMMPC "
#define ACPI_MAKER_REVISION 1

/* Where each table is put, far enough apart that none of them reaches another */
#define ACPI_POINTER_AT     0x0000
#define ACPI_ROOT_AT        0x0040
#define ACPI_WIDE_ROOT_AT   0x0080
#define ACPI_POWER_AT       0x0100
#define ACPI_SHARED_AT      0x0200
#define ACPI_LINES_AT       0x0280
#define ACPI_DEFINITION_AT  0x0300

/* What the one describing power says the machine can and cannot do */
#define ACPI_CAN_FLUSH      0x00000001
#define ACPI_HALTS_IDLE     0x00000004
#define ACPI_NO_SLEEP_KEY   0x00000020
#define ACPI_WIDE_TIMER     0x00000100

/* And what it says is still wired the old way */
#define ACPI_HAS_LEGACY     0x0001
#define ACPI_HAS_KEYBOARD   0x0002

/* The one register of the clock that says which hundred years it is */
#define ACPI_CENTURY_AT     0x32

/* The number that is written to put the machine away */
#define ACPI_PUT_AWAY       5

/*
 * What a bus of this kind is called, in the four letters and four numbers
 * everything has been named by since long before any of this. Written the way
 * it is packed rather than as the letters, because that is what is read back.
 */
#define ACPI_BUS_IS         0x030AD041

/* The first of the pair of ports the bus itself is asked through */
#define ACPI_BUS_PORT       0x0CF8

/* And the last port the board answers on before the bus has the rest */
#define ACPI_BOARD_LAST_PORT 0x0CFF

/*
 * The memory the bus has to give out, which is above anything the machine has
 * as memory and below where the two controllers a processor is reached
 * through answer.
 */
#define ACPI_BUS_MEMORY_FIRST 0xE0000000
#define ACPI_BUS_MEMORY_LAST  0xFEBFFFFF

/* Where the two controllers a processor is reached through answer */
#define ACPI_LOCAL_LINES    0xFEE00000
#define ACPI_SHARED_LINES   0xFEC00000

/* The kinds of row the table of lines holds */
#define ACPI_ROW_PROCESSOR  0
#define ACPI_ROW_ROUTER     1
#define ACPI_ROW_REPLACED   2

/* And that the old pair of controllers is there as well */
#define ACPI_OLD_PAIR_TOO   0x00000001

#pragma pack(push, 1)

/* What every one of them but the pointer begins with */
typedef struct _ACPI_HEADER
{
    CHAR Name[4];
    ULONG Length;
    UCHAR Revision;
    UCHAR Sum;
    CHAR Maker[6];
    CHAR MakerTable[8];
    ULONG MakerRevision;
    CHAR Builder[4];
    ULONG BuilderRevision;
} ACPI_HEADER;

C_ASSERT(sizeof(ACPI_HEADER) == 36);

/* The one at a fixed place that everything else is found through */
typedef struct _ACPI_POINTER
{
    CHAR Name[8];
    UCHAR Sum;
    CHAR Maker[6];
    UCHAR Revision;
    ULONG Root;
    ULONG Length;
    ULONG64 WideRoot;
    UCHAR WideSum;
    UCHAR Spare[3];
} ACPI_POINTER;

C_ASSERT(sizeof(ACPI_POINTER) == 36);

/* How one register is described, wherever it happens to live */
typedef struct _ACPI_REGISTER
{
    UCHAR Where;
    UCHAR Width;
    UCHAR Offset;
    UCHAR Size;
    ULONG64 Address;
} ACPI_REGISTER;

C_ASSERT(sizeof(ACPI_REGISTER) == 12);

/* The one that is looked for first and whose absence stops everything */
typedef struct _ACPI_POWER
{
    ACPI_HEADER Header;
    ULONG Shared;
    ULONG Definition;
    UCHAR Spare;
    UCHAR Profile;
    USHORT Line;
    ULONG Command;
    UCHAR TurnOn;
    UCHAR TurnOff;
    UCHAR Hibernate;
    UCHAR StateCount;
    ULONG EventPort;
    ULONG SecondEventPort;
    ULONG ControlPort;
    ULONG SecondControlPort;
    ULONG OtherControlPort;
    ULONG TimerPort;
    ULONG GeneralPort;
    ULONG SecondGeneralPort;
    UCHAR EventLength;
    UCHAR ControlLength;
    UCHAR OtherControlLength;
    UCHAR TimerLength;
    UCHAR GeneralLength;
    UCHAR SecondGeneralLength;
    UCHAR SecondGeneralFirst;
    UCHAR StateChangeCount;
    USHORT ShallowLatency;
    USHORT DeepLatency;
    USHORT FlushSize;
    USHORT FlushStride;
    UCHAR DutyOffset;
    UCHAR DutyWidth;
    UCHAR DayAlarm;
    UCHAR MonthAlarm;
    UCHAR Century;
    USHORT StillWired;
    UCHAR Reserved;
    ULONG Flags;
    ACPI_REGISTER Restart;
    UCHAR RestartValue;
    UCHAR Spare2[3];
    ULONG64 WideShared;
    ULONG64 WideDefinition;
    ACPI_REGISTER WideEvent;
    ACPI_REGISTER WideSecondEvent;
    ACPI_REGISTER WideControl;
    ACPI_REGISTER WideSecondControl;
    ACPI_REGISTER WideOtherControl;
    ACPI_REGISTER WideTimer;
    ACPI_REGISTER WideGeneral;
    ACPI_REGISTER WideSecondGeneral;
} ACPI_POWER;

C_ASSERT(sizeof(ACPI_POWER) == 244);

/* The one the firmware and the system both write, which is mostly empty */
typedef struct _ACPI_SHARED
{
    CHAR Name[4];
    ULONG Length;
    ULONG Signature;
    ULONG WakingVector;
    ULONG Lock;
    ULONG Flags;
    ULONG64 WideWakingVector;
    UCHAR Version;
    UCHAR Spare[31];
} ACPI_SHARED;

C_ASSERT(sizeof(ACPI_SHARED) == 64);

/* The table saying how interrupts get from a device to a processor */
typedef struct _ACPI_LINES
{
    ACPI_HEADER Header;
    ULONG LocalAddress;
    ULONG Flags;
} ACPI_LINES;

typedef struct _ACPI_ROW_PROCESSOR_ENTRY
{
    UCHAR Kind;
    UCHAR Length;
    UCHAR Processor;
    UCHAR Identifier;
    ULONG Flags;
} ACPI_ROW_PROCESSOR_ENTRY;

typedef struct _ACPI_ROW_ROUTER_ENTRY
{
    UCHAR Kind;
    UCHAR Length;
    UCHAR Identifier;
    UCHAR Spare;
    ULONG Address;
    ULONG FirstLine;
} ACPI_ROW_ROUTER_ENTRY;

typedef struct _ACPI_ROW_REPLACED_ENTRY
{
    UCHAR Kind;
    UCHAR Length;
    UCHAR Bus;
    UCHAR Was;
    ULONG Becomes;
    USHORT How;
} ACPI_ROW_REPLACED_ENTRY;

#pragma pack(pop)

/* WHAT IS IN THEM ************************************************************/

/* THE LANGUAGE THE LAST OF THEM IS WRITTEN IN ********************************/

/* What each thing that can be said begins with */
#define AML_NOTHING         0x00
#define AML_NAMED           0x08
#define AML_ONE_BYTE        0x0A
#define AML_FOUR_BYTES      0x0C
#define AML_A_RUN_OF_BYTES  0x11
#define AML_A_LIST          0x12
#define AML_UNDER           0x10
#define AML_MORE            0x5B
#define AML_A_DEVICE        0x82
#define AML_FROM_THE_TOP    0x5C

/* And how a run of addresses is written inside one of those runs of bytes */
#define AML_RANGE_OF_FOUR   0x87
#define AML_RANGE_OF_TWO    0x88
#define AML_NOTHING_MORE    0x79

/* What kind of thing a run of addresses is a run of */
#define AML_RANGE_MEMORY    0
#define AML_RANGE_PORTS     1
#define AML_RANGE_BUSES     2

/* That both ends of it are where they are said to be and will not move */
#define AML_RANGE_FIXED     0x0C

/* That the whole of it is there, and that it may be read and written */
#define AML_PORTS_WHOLE     0x03
#define AML_MEMORY_PLAIN    0x01

/*
 * What is written into the one table that is not plain numbers. Everything in
 * it is a thing being said, whatever it is being said about, and a length
 * covering both. The length comes first and is only known at the end, so every
 * one of them is put down three bytes wide and filled in once the end is
 * reached: three bytes is enough for far more than any of this will ever be.
 */
class AcpiText
{
public:
    AcpiText(UCHAR *At, ULONG Room) noexcept : m_At(At), m_Room(Room) {}

    void Put(UCHAR Value)
    {
        if (m_Used < m_Room)
            m_At[m_Used] = Value;
        else
            m_Overflowed = true;

        m_Used++;
    }

    void Put(const void *From, ULONG Length)
    {
        const auto *Bytes = static_cast<const UCHAR *>(From);

        for (ULONG Index = 0; Index < Length; Index++)
            Put(Bytes[Index]);
    }

    void PutTwo(USHORT Value)
    {
        Put((UCHAR)(Value & 0xFF));
        Put((UCHAR)(Value >> 8));
    }

    void PutFour(ULONG Value)
    {
        PutTwo((USHORT)(Value & 0xFFFF));
        PutTwo((USHORT)(Value >> 16));
    }

    /* A name is always four characters, padded with the one that stands in */
    void PutName(const char *Text)
    {
        for (ULONG Index = 0; Index < 4; Index++)
            Put((UCHAR)((Text[Index] != '\0') ? Text[Index] : '_'));
    }

    ULONG Open()
    {
        const ULONG Mark = m_Used;

        Put(0);
        Put(0);
        Put(0);
        return Mark;
    }

    void Close(ULONG Mark)
    {
        const ULONG Length = m_Used - Mark;

        if ((Mark + 3) > m_Room)
            return;

        /*
         * The top two bits say how many bytes follow the first, and with two
         * of them the first carries only the lowest four bits of the length.
         */
        m_At[Mark] = (UCHAR)(0x80 | (Length & 0x0F));
        m_At[Mark + 1] = (UCHAR)((Length >> 4) & 0xFF);
        m_At[Mark + 2] = (UCHAR)((Length >> 12) & 0xFF);
    }

    /* Said by whatever was building this when it found it could not finish */
    void Fail() noexcept { m_Overflowed = true; }

    bool Overflowed() const noexcept { return m_Overflowed; }
    ULONG Length() const noexcept { return m_Used; }

private:
    UCHAR *m_At;
    ULONG m_Room;
    ULONG m_Used = 0;
    bool m_Overflowed = false;
};

/* One run of addresses, of the kind written two bytes to a number */
static void AcpiShortRange(AcpiText &Text, UCHAR Kind, UCHAR What,
                           USHORT First, USHORT Last)
{
    Text.Put(AML_RANGE_OF_TWO);
    Text.PutTwo(13);
    Text.Put(Kind);
    Text.Put(AML_RANGE_FIXED);
    Text.Put(What);
    Text.PutTwo(0);
    Text.PutTwo(First);
    Text.PutTwo(Last);
    Text.PutTwo(0);
    Text.PutTwo((USHORT)(Last - First + 1));
}

/* And one of the kind written four bytes to a number */
static void AcpiLongRange(AcpiText &Text, UCHAR Kind, UCHAR What,
                          ULONG First, ULONG Last)
{
    Text.Put(AML_RANGE_OF_FOUR);
    Text.PutTwo(23);
    Text.Put(Kind);
    Text.Put(AML_RANGE_FIXED);
    Text.Put(What);
    Text.PutFour(0);
    Text.PutFour(First);
    Text.PutFour(Last);
    Text.PutFour(0);
    Text.PutFour(Last - First + 1);
}

/* Name (<Four>, <Value>), where the value is four bytes of number */
static void AcpiNamedNumber(AcpiText &Text, const char *Name, ULONG Value)
{
    Text.Put(AML_NAMED);
    Text.PutName(Name);

    if (Value == 0)
    {
        Text.Put(AML_NOTHING);
        return;
    }

    Text.Put(AML_FOUR_BYTES);
    Text.PutFour(Value);
}

/*
 * The one that says what the machine is made of, in the language the system
 * reads it in rather than in numbers.
 *
 * Only what is true. Almost everything this machine has is at a place a system
 * of this age already knows without being told; what is said here is the bus,
 * because nothing on a bus is anywhere a system can guess at, and what to
 * write to turn the machine off.
 *
 * A bus is described by the addresses it hands out rather than by what is on
 * it. What is on it is found by asking the bus, and that is the whole reason
 * for describing it at all.
 */
static void AcpiDescribeMachine(AcpiText &Text)
{
    /* Scope (\_SB) { ... }, which is where anything that is a device goes */
    Text.Put(AML_UNDER);

    const ULONG Devices = Text.Open();

    Text.Put(AML_FROM_THE_TOP);
    Text.PutName("_SB_");

    /* Device (PCI0) */
    Text.Put(AML_MORE);
    Text.Put(AML_A_DEVICE);

    const ULONG Bus = Text.Open();

    Text.PutName("PCI0");

    /* What it is, which is the one way onto a bus of this kind */
    AcpiNamedNumber(Text, "_HID", ACPI_BUS_IS);
    AcpiNamedNumber(Text, "_UID", 0);
    AcpiNamedNumber(Text, "_ADR", 0);
    AcpiNamedNumber(Text, "_BBN", 0);

    /*
     * And what it hands out to whatever is found on it. A run of bytes says
     * how many of them there are before it says what any of them is, so these
     * are put together first and only then written down.
     */
    UCHAR Ranges[128] = {};
    AcpiText Held(Ranges, sizeof(Ranges));

    /* The one bus there is, which is the one everything hangs off */
    AcpiShortRange(Held, AML_RANGE_BUSES, 0, 0, 0);

    /*
     * The ports, in the two runs every machine of this kind has: everything
     * below the pair the bus itself answers on, and everything above where the
     * board stops listening.
     */
    AcpiShortRange(Held, AML_RANGE_PORTS, AML_PORTS_WHOLE,
                   0x0000, (USHORT)(ACPI_BUS_PORT - 1));
    AcpiShortRange(Held, AML_RANGE_PORTS, AML_PORTS_WHOLE,
                   (USHORT)(ACPI_BOARD_LAST_PORT + 1), 0xFFFF);

    /* And the memory, which is above anything this machine has as memory */
    AcpiLongRange(Held, AML_RANGE_MEMORY, AML_MEMORY_PLAIN,
                  ACPI_BUS_MEMORY_FIRST, ACPI_BUS_MEMORY_LAST);

    Held.Put(AML_NOTHING_MORE);
    Held.Put(0);

    /* More runs than there was room for, which is a mistake in this file */
    const ULONG Written = Held.Overflowed() ? 0 : Held.Length();

    if (Held.Overflowed())
        Text.Fail();

    Text.Put(AML_NAMED);
    Text.PutName("_CRS");
    Text.Put(AML_A_RUN_OF_BYTES);

    const ULONG Bytes = Text.Open();

    Text.Put(AML_ONE_BYTE);
    Text.Put((UCHAR)Written);
    Text.Put(Ranges, Written);
    Text.Close(Bytes);

    Text.Close(Bus);
    Text.Close(Devices);

    /*
     * Name (\_S5, Package (4) { 5, 0, 0, 0 }), which is the number the machine
     * is written to be put away with.
     */
    Text.Put(AML_NAMED);
    Text.Put(AML_FROM_THE_TOP);
    Text.PutName("_S5_");
    Text.Put(AML_A_LIST);

    const ULONG Numbers = Text.Open();

    Text.Put(4);

    for (ULONG Index = 0; Index < 4; Index++)
    {
        Text.Put(AML_ONE_BYTE);
        Text.Put((UCHAR)((Index == 0) ? ACPI_PUT_AWAY : 0));
    }

    Text.Close(Numbers);
}

/* The byte that makes the whole of one add up to nothing */
static UCHAR AcpiSum(const void *Table, ULONG Length)
{
    const auto *Bytes = static_cast<const UCHAR *>(Table);
    UCHAR Total = 0;

    for (ULONG Index = 0; Index < Length; Index++)
        Total = (UCHAR)(Total + Bytes[Index]);

    return (UCHAR)(0 - Total);
}

/* Fills in the part every one of them starts with */
static void AcpiName(ACPI_HEADER &Header, const char *Name, ULONG Length,
                     UCHAR Revision)
{
    memset(&Header, 0, sizeof(Header));
    memcpy(Header.Name, Name, 4);

    Header.Length = Length;
    Header.Revision = Revision;

    memcpy(Header.Maker, ACPI_MAKER, sizeof(Header.Maker));
    memcpy(Header.MakerTable, ACPI_MAKER_TABLE, sizeof(Header.MakerTable));
    Header.MakerRevision = ACPI_MAKER_REVISION;

    memcpy(Header.Builder, "RTVM", sizeof(Header.Builder));
    Header.BuilderRevision = ACPI_MAKER_REVISION;
}

/* Says where one register lives, which for all of these is a port */
static void AcpiPort(ACPI_REGISTER &Register, ULONG Port, UCHAR Bits)
{
    memset(&Register, 0, sizeof(Register));

    /* One means a port rather than somewhere in memory */
    Register.Where = 1;
    Register.Width = Bits;
    Register.Size = (UCHAR)((Bits == 8) ? 1 : ((Bits == 16) ? 2 : 3));
    Register.Address = Port;
}

/* BUILDING THEM **************************************************************/

bool DescribeWithTables(Memory &Ram, ULONG ProcessorCount)
{
    UCHAR Tables[ACPI_TABLE_SIZE] = {};

    const ULONG64 Root = ACPI_TABLE_BASE + ACPI_ROOT_AT;
    const ULONG64 WideRoot = ACPI_TABLE_BASE + ACPI_WIDE_ROOT_AT;
    const ULONG64 Power = ACPI_TABLE_BASE + ACPI_POWER_AT;
    const ULONG64 Shared = ACPI_TABLE_BASE + ACPI_SHARED_AT;
    const ULONG64 Lines = ACPI_TABLE_BASE + ACPI_LINES_AT;
    const ULONG64 Definition = ACPI_TABLE_BASE + ACPI_DEFINITION_AT;

    /* The one that describes what the machine is made of */
    {
        auto *Block = reinterpret_cast<ACPI_HEADER *>(&Tables[ACPI_DEFINITION_AT]);
        const ULONG Room = ACPI_TABLE_SIZE - ACPI_DEFINITION_AT - sizeof(*Block);

        AcpiText Text(reinterpret_cast<UCHAR *>(Block + 1), Room);

        AcpiDescribeMachine(Text);

        if (Text.Overflowed())
            return false;

        const ULONG Length = sizeof(*Block) + Text.Length();

        AcpiName(*Block, ACPI_DEFINITION, Length, 2);
        Block->Sum = AcpiSum(Block, Length);
    }

    /* The one both sides write, which starts out saying nothing happened */
    {
        auto *Block = reinterpret_cast<ACPI_SHARED *>(&Tables[ACPI_SHARED_AT]);

        memset(Block, 0, sizeof(*Block));
        memcpy(Block->Name, ACPI_SHARED_NAME, sizeof(Block->Name));
        Block->Length = sizeof(*Block);
        Block->Version = 2;
    }

    /* The one that is looked for first */
    {
        auto *Block = reinterpret_cast<ACPI_POWER *>(&Tables[ACPI_POWER_AT]);

        memset(Block, 0, sizeof(*Block));
        AcpiName(Block->Header, ACPI_POWER_NAME, sizeof(*Block), 3);

        Block->Shared = (ULONG)Shared;
        Block->Definition = (ULONG)Definition;
        Block->WideShared = Shared;
        Block->WideDefinition = Definition;

        /* A desktop, which is what everything about this machine looks like */
        Block->Profile = 1;
        Block->Line = ACPI_EVENT_LINE;

        /*
         * No port to write to turn any of this on, which is how a machine says
         * it is on already and there is nothing to ask for.
         */
        Block->Command = 0;
        Block->TurnOn = 0;
        Block->TurnOff = 0;

        Block->EventPort = ACPI_EVENT_PORT;
        Block->EventLength = ACPI_EVENT_LENGTH;
        Block->ControlPort = ACPI_CONTROL_PORT;
        Block->ControlLength = ACPI_CONTROL_LENGTH;
        Block->TimerPort = ACPI_TIMER_PORT;
        Block->TimerLength = ACPI_TIMER_LENGTH;
        Block->GeneralPort = ACPI_GENERAL_PORT;
        Block->GeneralLength = ACPI_GENERAL_LENGTH;

        /* How long the two ways of resting take to come back from */
        Block->ShallowLatency = 101;
        Block->DeepLatency = 1001;

        Block->Century = ACPI_CENTURY_AT;
        Block->StillWired = ACPI_HAS_LEGACY | ACPI_HAS_KEYBOARD;
        Block->Flags = ACPI_CAN_FLUSH | ACPI_HALTS_IDLE | ACPI_NO_SLEEP_KEY |
                       ACPI_WIDE_TIMER;

        AcpiPort(Block->WideEvent, ACPI_EVENT_PORT, 32);
        AcpiPort(Block->WideControl, ACPI_CONTROL_PORT, 16);
        AcpiPort(Block->WideTimer, ACPI_TIMER_PORT, 32);
        AcpiPort(Block->WideGeneral, ACPI_GENERAL_PORT, 32);

        Block->Header.Sum = AcpiSum(Block, sizeof(*Block));
    }

    /* How an interrupt gets from a device to a processor */
    {
        auto *Block = reinterpret_cast<ACPI_LINES *>(&Tables[ACPI_LINES_AT]);
        UCHAR *At = reinterpret_cast<UCHAR *>(Block + 1);

        memset(Block, 0, sizeof(*Block));
        Block->LocalAddress = ACPI_LOCAL_LINES;

        /* The pair of controllers a machine of this kind has always had */
        Block->Flags = ACPI_OLD_PAIR_TOO;

        for (ULONG Index = 0; Index < ProcessorCount; Index++)
        {
            auto *Row = reinterpret_cast<ACPI_ROW_PROCESSOR_ENTRY *>(At);

            Row->Kind = ACPI_ROW_PROCESSOR;
            Row->Length = sizeof(*Row);
            Row->Processor = (UCHAR)Index;
            Row->Identifier = (UCHAR)Index;
            Row->Flags = 1;

            At += sizeof(*Row);
        }

        {
            auto *Row = reinterpret_cast<ACPI_ROW_ROUTER_ENTRY *>(At);

            Row->Kind = ACPI_ROW_ROUTER;
            Row->Length = sizeof(*Row);
            Row->Identifier = (UCHAR)ProcessorCount;
            Row->Address = ACPI_SHARED_LINES;
            Row->FirstLine = 0;

            At += sizeof(*Row);
        }

        /*
         * The clock is wired to the first line on the old pair and to the
         * third on the router, which is the one difference every machine of
         * this kind has and every system expects to be told about.
         */
        {
            auto *Row = reinterpret_cast<ACPI_ROW_REPLACED_ENTRY *>(At);

            Row->Kind = ACPI_ROW_REPLACED;
            Row->Length = sizeof(*Row);
            Row->Bus = 0;
            Row->Was = 0;
            Row->Becomes = 2;
            Row->How = 0;

            At += sizeof(*Row);
        }

        const ULONG Length = (ULONG)(At - reinterpret_cast<UCHAR *>(Block));

        AcpiName(Block->Header, ACPI_LINES_NAME, Length, 3);
        Block->LocalAddress = ACPI_LOCAL_LINES;
        Block->Flags = ACPI_OLD_PAIR_TOO;
        Block->Header.Sum = AcpiSum(Block, Length);
    }

    /* The two lists of where the rest of them are, one of each width */
    {
        auto *Block = reinterpret_cast<ACPI_HEADER *>(&Tables[ACPI_ROOT_AT]);
        auto *Entry = reinterpret_cast<ULONG *>(Block + 1);
        const ULONG Length = sizeof(*Block) + (2 * sizeof(*Entry));

        Entry[0] = (ULONG)Power;
        Entry[1] = (ULONG)Lines;

        AcpiName(*Block, ACPI_ROOT_NAME, Length, 1);
        Block->Sum = AcpiSum(Block, Length);
    }

    {
        auto *Block = reinterpret_cast<ACPI_HEADER *>(&Tables[ACPI_WIDE_ROOT_AT]);
        auto *Entry = reinterpret_cast<ULONG64 *>(Block + 1);
        const ULONG Length = sizeof(*Block) + (2 * sizeof(*Entry));

        Entry[0] = Power;
        Entry[1] = Lines;

        AcpiName(*Block, ACPI_WIDE_ROOT_NAME, Length, 1);
        Block->Sum = AcpiSum(Block, Length);
    }

    /* And the one at the fixed place that all of it is found through */
    {
        auto *Block = reinterpret_cast<ACPI_POINTER *>(&Tables[ACPI_POINTER_AT]);

        memset(Block, 0, sizeof(*Block));
        memcpy(Block->Name, ACPI_POINTER_NAME, sizeof(Block->Name));
        memcpy(Block->Maker, ACPI_MAKER, sizeof(Block->Maker));

        Block->Revision = 2;
        Block->Root = (ULONG)Root;
        Block->Length = sizeof(*Block);
        Block->WideRoot = WideRoot;

        /*
         * Two of them. The first covers only as much as the oldest of these
         * ever had, and anything written since checks the second as well.
         */
        Block->Sum = AcpiSum(Block, 20);
        Block->WideSum = AcpiSum(Block, sizeof(*Block));
    }

    return Ram.Write(ACPI_TABLE_BASE, Tables, sizeof(Tables));
}

} /* namespace rtvm */
