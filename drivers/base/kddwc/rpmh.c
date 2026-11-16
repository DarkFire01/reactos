/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Bus and supply votes for the controller, sent to RPMh through the apps RSC
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kddwc.h"

/*
 * When boot services exit, Qualcomm firmware moves the USB controller to low
 * power: its buses drop to no bandwidth and the PHY supplies to their low
 * power level. The controller registers still answer for a while, but its DMA
 * stalls the bus. The votes are put back here, the way the operating system's
 * own drivers place them, before anything touches the controller.
 */

/* Command DB: a header with up to eight slave tables, then the data they point into */
#define CMDDB_SLAVE_COUNT               8
#define CMDDB_SLAVE_TABLE(Index)        (8 + (Index) * 16)
#define CMDDB_DATA                      144
#define CMDDB_ENTRY_BYTES               24
#define CMDDB_ID_BYTES                  8

/* Auxiliary data of a bus clock manager */
#define BCM_AUX_UNIT                    0
#define BCM_AUX_WIDTH                   4
#define BCM_AUX_VCD                     6
#define BCM_AUX_BYTES                   8

/* Bus clock manager command */
#define BCM_COMMIT                      0x40000000
#define BCM_VALID                       0x20000000
#define BCM_VOTE_X_SHIFT                14
#define BCM_VOTE_MAX                    0x3FFF

/* Bandwidths are kB/s, managers count bytes per second */
#define BCM_SCALE                       1000

/* Regulator registers and the high power mode every LDO generation shares */
#define VRM_VOLTAGE                     0x0
#define VRM_ENABLE                      0x4
#define VRM_MODE                        0x8
#define VRM_LDO_HIGH_POWER              7

/* RSC registers common to both layouts */
#define RSC_ID                          0x00
#define RSC_ID_MAJOR(Id)                (((Id) >> 16) & 0xFF)
#define RSC_CONFIG                      0x0C
#define RSC_CONFIG_COMMANDS(Config)     (((Config) >> 27) & 0x1F)
#define RSC_CONFIG_TCS(Config, Drv)     (((Config) >> ((Drv) * 6)) & 0x3F)
#define RSC_TCS_STRIDE                  672
#define RSC_MAX_TCS                     0x3F
#define RSC_APPS_DRV                    2

/* Relative to where the TCSs start */
#define TCS_IRQ_ENABLE                  0x00
#define TCS_IRQ_STATUS                  0x04
#define TCS_IRQ_CLEAR                   0x08

#define TCS_AMC_ENABLE                  0x00010000
#define TCS_AMC_TRIGGER                 0x01000000

#define TCS_MSG_LENGTH                  0x00000008
#define TCS_MSG_RESPONSE                0x00000100
#define TCS_MSG_WRITE                   0x00010000

#define KDDWC_RPMH_TIMEOUT_US           100000
#define KDDWC_RPMH_MAX_COMMANDS         32

/* Where the per TCS registers sit, which moved in the third RSC generation */
typedef struct _KDDWC_TCS_LAYOUT
{
    ULONG CommandStride;
    ULONG Control;
    ULONG Status;
    ULONG CommandEnable;
    ULONG MessageId;
    ULONG Address;
    ULONG Data;
} KDDWC_TCS_LAYOUT;

static const KDDWC_TCS_LAYOUT KdDwcTcsLayout2 = { 20, 0x14, 0x18, 0x1C, 0x30, 0x34, 0x38 };
static const KDDWC_TCS_LAYOUT KdDwcTcsLayout3 = { 24, 0x24, 0x28, 0x2C, 0x34, 0x38, 0x3C };

typedef struct _KDDWC_RPMH_COMMAND
{
    ULONG Address;
    ULONG Data;
    BOOLEAN Wait;
} KDDWC_RPMH_COMMAND;

typedef struct _KDDWC_BCM_VOTE
{
    ULONG Address;
    ULONG X;
    ULONG Y;
    UCHAR Vcd;
    BOOLEAN Mask;
} KDDWC_BCM_VOTE;

static volatile UCHAR* KdDwcCmdDb;
static ULONG KdDwcCmdDbSize;
static PUCHAR KdDwcTcs;
static ULONG KdDwcTcsId;
static ULONG KdDwcTcsCommands;
static ULONG KdDwcTcsCount;
static const KDDWC_TCS_LAYOUT* KdDwcLayout;

/* COMMAND DB *****************************************************************/

/* The DB is mapped as device memory, so every field is put together from single bytes */
static
ULONG
KdDwcCmdDbRead(
    _In_ ULONG Offset,
    _In_ ULONG Size)
{
    ULONG Value = 0;
    ULONG i;

    if (Offset + Size > KdDwcCmdDbSize)
        return 0;

    for (i = 0; i < Size; i++)
        Value |= (ULONG)READ_REGISTER_UCHAR((PUCHAR)&KdDwcCmdDb[Offset + i]) << (i * 8);

    return Value;
}

/**
 * @brief Looks a resource up by name and returns its RPMh address and where its auxiliary data starts.
 */
static
BOOLEAN
KdDwcCmdDbFind(
    _In_ PCSTR Name,
    _Out_ PULONG Address,
    _Out_ PULONG AuxOffset,
    _Out_ PULONG AuxLength)
{
    UCHAR Id[CMDDB_ID_BYTES] = { 0 };
    ULONG Slave, Entry, Count, Table, Data, Offset, i;

    for (i = 0; i < sizeof(Id) && Name[i] != ANSI_NULL; i++)
        Id[i] = (UCHAR)Name[i];

    for (Slave = 0; Slave < CMDDB_SLAVE_COUNT; Slave++)
    {
        if (KdDwcCmdDbRead(CMDDB_SLAVE_TABLE(Slave), 2) == 0)
            break;

        Table = CMDDB_DATA + KdDwcCmdDbRead(CMDDB_SLAVE_TABLE(Slave) + 2, 2);
        Data = CMDDB_DATA + KdDwcCmdDbRead(CMDDB_SLAVE_TABLE(Slave) + 4, 2);
        Count = KdDwcCmdDbRead(CMDDB_SLAVE_TABLE(Slave) + 6, 2);

        for (Entry = 0; Entry < Count; Entry++)
        {
            Offset = Table + Entry * CMDDB_ENTRY_BYTES;
            for (i = 0; i < CMDDB_ID_BYTES; i++)
            {
                if (KdDwcCmdDbRead(Offset + i, 1) != Id[i])
                    break;
            }

            if (i != CMDDB_ID_BYTES)
                continue;

            *Address = KdDwcCmdDbRead(Offset + 16, 4);
            *AuxLength = KdDwcCmdDbRead(Offset + 20, 2);
            *AuxOffset = Data + KdDwcCmdDbRead(Offset + 22, 2);
            return TRUE;
        }
    }

    KdDwcTrace("%s is not in the command DB\n", Name);
    return FALSE;
}

/* RSC ************************************************************************/

static
PUCHAR
KdDwcTcsRegister(
    _In_ ULONG Offset)
{
    return KdDwcTcs + KdDwcTcsId * RSC_TCS_STRIDE + Offset;
}

static
PUCHAR
KdDwcTcsCommand(
    _In_ ULONG Command,
    _In_ ULONG Offset)
{
    return KdDwcTcsRegister(Offset + Command * KdDwcLayout->CommandStride);
}

/* Clears the trigger before the mode, as the RSC wants, and arms both again when asked */
static
VOID
KdDwcTcsTrigger(
    _In_ BOOLEAN Start)
{
    PUCHAR Control = KdDwcTcsRegister(KdDwcLayout->Control);
    ULONG Value = KdDwcRead(Control, 0);

    Value &= ~TCS_AMC_TRIGGER;
    KdDwcWrite(Control, 0, Value);
    Value &= ~TCS_AMC_ENABLE;
    KdDwcWrite(Control, 0, Value);

    if (!Start)
        return;

    KdDwcWrite(Control, 0, TCS_AMC_ENABLE);
    KdDwcWrite(Control, 0, TCS_AMC_ENABLE | TCS_AMC_TRIGGER);
}

/**
 * @brief Sends one batch through the active TCS and polls for RPMh to finish it.
 */
static
BOOLEAN
KdDwcTcsSend(
    _In_reads_(Count) const KDDWC_RPMH_COMMAND* Commands,
    _In_ ULONG Count)
{
    ULONG Bit = 1UL << KdDwcTcsId;
    ULONG Enable = 0;
    ULONG MessageId;
    ULONG i;
    BOOLEAN Done;

    if (!KdDwcWaitFor(KdDwcTcsRegister(KdDwcLayout->Status), 0, 1, 1, KDDWC_RPMH_TIMEOUT_US))
    {
        KdDwcTrace("TCS %lu stays busy\n", KdDwcTcsId);
        return FALSE;
    }

    KdDwcWrite(KdDwcTcsRegister(KdDwcLayout->CommandEnable), 0, 0);
    KdDwcModify(KdDwcTcs, TCS_IRQ_ENABLE, 0, Bit);

    for (i = 0; i < Count; i++)
    {
        MessageId = TCS_MSG_LENGTH | TCS_MSG_WRITE;
        if (Commands[i].Wait)
            MessageId |= TCS_MSG_RESPONSE;

        KdDwcWrite(KdDwcTcsCommand(i, KdDwcLayout->MessageId), 0, MessageId);
        KdDwcWrite(KdDwcTcsCommand(i, KdDwcLayout->Address), 0, Commands[i].Address);
        KdDwcWrite(KdDwcTcsCommand(i, KdDwcLayout->Data), 0, Commands[i].Data);
        Enable |= 1UL << i;
    }

    KdDwcWrite(KdDwcTcsRegister(KdDwcLayout->CommandEnable), 0, Enable);
    KdDwcTcsTrigger(TRUE);

    Done = KdDwcWaitFor(KdDwcTcs, TCS_IRQ_STATUS, Bit, Bit, KDDWC_RPMH_TIMEOUT_US);
    if (!Done)
        KdDwcTrace("RPMh did not finish %lu commands\n", Count);

    /* Hand the TCS back the way it was found */
    KdDwcTcsTrigger(FALSE);
    KdDwcWrite(KdDwcTcsRegister(KdDwcLayout->CommandEnable), 0, 0);
    KdDwcWrite(KdDwcTcs, TCS_IRQ_CLEAR, Bit);
    KdDwcModify(KdDwcTcs, TCS_IRQ_ENABLE, Bit, 0);
    return Done;
}

/* Splits a list into batches the TCS can hold, ending each one on a command that waits */
static
BOOLEAN
KdDwcRpmhSend(
    _In_reads_(Count) const KDDWC_RPMH_COMMAND* Commands,
    _In_ ULONG Count)
{
    ULONG Start = 0;
    ULONG Length;

    while (Start < Count)
    {
        Length = min(Count - Start, KdDwcTcsCommands);
        while (Length > 1 && Start + Length < Count && !Commands[Start + Length - 1].Wait)
            Length--;

        if (!KdDwcTcsSend(&Commands[Start], Length))
            return FALSE;

        Start += Length;
    }

    return TRUE;
}

/* VOTES **********************************************************************/

static
ULONG64
KdDwcBcmDivide(
    _In_ ULONG64 Value,
    _In_ ULONG64 Base)
{
    if (Base == 0)
        return 0;

    /* Anything asked for gets at least one step */
    if (Value != 0 && Value < Base)
        return 1;

    return Value / Base;
}

/* Turns the nodes into one vote per bus clock manager */
static
ULONG
KdDwcBuildBcmVotes(
    _In_ const KDDWC_RPMH* Rpmh,
    _Out_writes_(KDDWC_RPMH_MAX_COMMANDS) KDDWC_BCM_VOTE* Votes)
{
    const KDDWC_BUS_NODE* Node;
    ULONG Count = 0;
    ULONG Address, AuxOffset, AuxLength, Unit, Width;
    ULONG64 Average, Peak;
    ULONG i, j;

    for (i = 0; i < Rpmh->NodeCount; i++)
    {
        Node = &Rpmh->Nodes[i];
        if (!KdDwcCmdDbFind(Node->Bcm, &Address, &AuxOffset, &AuxLength) || AuxLength < BCM_AUX_BYTES)
            continue;

        for (j = 0; j < Count && Votes[j].Address != Address; j++);
        if (j == Count)
        {
            if (Count == KDDWC_RPMH_MAX_COMMANDS)
                break;

            RtlZeroMemory(&Votes[j], sizeof(Votes[j]));
            Votes[j].Address = Address;
            Votes[j].Vcd = (UCHAR)KdDwcCmdDbRead(AuxOffset + BCM_AUX_VCD, 1);
            Count++;
        }

        if (Node->EnableMask != 0)
        {
            Votes[j].Y |= Node->EnableMask;
            Votes[j].Mask = TRUE;
            continue;
        }

        Unit = KdDwcCmdDbRead(AuxOffset + BCM_AUX_UNIT, 4);
        Width = KdDwcCmdDbRead(AuxOffset + BCM_AUX_WIDTH, 2);

        Average = KdDwcBcmDivide((ULONG64)Node->AverageKBps * Width, (ULONG64)Node->BusWidth * Node->Channels);
        Peak = KdDwcBcmDivide((ULONG64)Node->PeakKBps * Width, Node->BusWidth);
        Average = KdDwcBcmDivide(Average * BCM_SCALE, Unit);
        Peak = KdDwcBcmDivide(Peak * BCM_SCALE, Unit);

        Votes[j].X = max(Votes[j].X, (ULONG)min(Average, (ULONG64)BCM_VOTE_MAX));
        Votes[j].Y = max(Votes[j].Y, (ULONG)min(Peak, (ULONG64)BCM_VOTE_MAX));
    }

    return Count;
}

/*
 * A vote replaces everything the application processor had on that manager,
 * the processor's own path to memory included. Firmware's votes are still in
 * the TCSs it used, so no vote goes below them.
 */
static
VOID
KdDwcKeepFirmwareVotes(
    _Inout_updates_(Count) KDDWC_BCM_VOTE* Votes,
    _In_ ULONG Count)
{
    ULONG Active = KdDwcTcsId;
    ULONG Command, Address, Data, i;

    for (KdDwcTcsId = 0; KdDwcTcsId < KdDwcTcsCount; KdDwcTcsId++)
    {
        for (Command = 0; Command < KdDwcTcsCommands; Command++)
        {
            Address = KdDwcRead(KdDwcTcsCommand(Command, KdDwcLayout->Address), 0);
            for (i = 0; i < Count && Votes[i].Address != Address; i++);
            if (i == Count)
                continue;

            Data = KdDwcRead(KdDwcTcsCommand(Command, KdDwcLayout->Data), 0);
            if (!(Data & BCM_VALID))
                continue;

            KdDwcTrace("firmware TCS %lu voted 0x%lx = 0x%lx\n", KdDwcTcsId, Address, Data);
            if (Votes[i].Mask)
            {
                Votes[i].Y |= Data & BCM_VOTE_MAX;
                continue;
            }

            Votes[i].X = max(Votes[i].X, (Data >> BCM_VOTE_X_SHIFT) & BCM_VOTE_MAX);
            Votes[i].Y = max(Votes[i].Y, Data & BCM_VOTE_MAX);
        }
    }

    KdDwcTcsId = Active;
}

/**
 * @brief Votes the controller's buses, in clock domain order with each domain committed at its last manager.
 */
static
BOOLEAN
KdDwcVoteBuses(
    _In_ const KDDWC_RPMH* Rpmh)
{
    KDDWC_BCM_VOTE Votes[KDDWC_RPMH_MAX_COMMANDS];
    KDDWC_RPMH_COMMAND Commands[KDDWC_RPMH_MAX_COMMANDS];
    KDDWC_BCM_VOTE Swap;
    ULONG Count, i, j;
    BOOLEAN Last;

    Count = KdDwcBuildBcmVotes(Rpmh, Votes);
    if (Count == 0)
        return FALSE;

    KdDwcKeepFirmwareVotes(Votes, Count);

    for (i = 1; i < Count; i++)
    {
        for (j = i; j > 0 && Votes[j - 1].Vcd > Votes[j].Vcd; j--)
        {
            Swap = Votes[j];
            Votes[j] = Votes[j - 1];
            Votes[j - 1] = Swap;
        }
    }

    for (i = 0; i < Count; i++)
    {
        Last = (i + 1 == Count) || (Votes[i + 1].Vcd != Votes[i].Vcd);

        Commands[i].Address = Votes[i].Address;
        Commands[i].Data = (Votes[i].X << BCM_VOTE_X_SHIFT) | Votes[i].Y;
        if (Votes[i].X != 0 || Votes[i].Y != 0)
            Commands[i].Data |= BCM_VALID;
        if (Last)
            Commands[i].Data |= BCM_COMMIT;
        Commands[i].Wait = Last;

        KdDwcTrace("bus vote 0x%lx = 0x%lx\n", Commands[i].Address, Commands[i].Data);
    }

    return KdDwcRpmhSend(Commands, Count);
}

/*
 * A rail is voted by the index of a level, the levels being the 16 bit words
 * of the rail's auxiliary data. The first level at or above the one asked for
 * is the vote.
 */
static
BOOLEAN
KdDwcVoteRail(
    _In_ const KDDWC_RPMH* Rpmh)
{
    KDDWC_RPMH_COMMAND Command;
    ULONG Address, AuxOffset, AuxLength;
    ULONG Index, Count;

    if (Rpmh->Rail == NULL)
        return TRUE;

    if (!KdDwcCmdDbFind(Rpmh->Rail, &Address, &AuxOffset, &AuxLength))
        return FALSE;

    Count = AuxLength / sizeof(USHORT);
    for (Index = 0; Index < Count; Index++)
    {
        if (KdDwcCmdDbRead(AuxOffset + Index * sizeof(USHORT), sizeof(USHORT)) >= Rpmh->RailLevel)
            break;
    }

    if (Index == Count)
    {
        KdDwcTrace("%s has no level %u\n", Rpmh->Rail, Rpmh->RailLevel);
        return FALSE;
    }

    Command.Address = Address;
    Command.Data = Index;
    Command.Wait = TRUE;
    KdDwcTrace("rail %s at 0x%lx, level %u is index %lu\n", Rpmh->Rail, Address, Rpmh->RailLevel, Index);
    return KdDwcRpmhSend(&Command, 1);
}

/* Sets each PHY supply's voltage, then its high power mode, then turns it on */
static
BOOLEAN
KdDwcVoteSupplies(
    _In_ const KDDWC_RPMH* Rpmh)
{
    KDDWC_RPMH_COMMAND Commands[3];
    ULONG Address, AuxOffset, AuxLength;
    BOOLEAN Ok = TRUE;
    ULONG i;

    for (i = 0; i < Rpmh->SupplyCount; i++)
    {
        if (!KdDwcCmdDbFind(Rpmh->Supplies[i].Name, &Address, &AuxOffset, &AuxLength))
        {
            Ok = FALSE;
            continue;
        }

        Commands[0].Address = Address + VRM_VOLTAGE;
        Commands[0].Data = Rpmh->Supplies[i].Millivolts;
        Commands[1].Address = Address + VRM_MODE;
        Commands[1].Data = VRM_LDO_HIGH_POWER;
        Commands[2].Address = Address + VRM_ENABLE;
        Commands[2].Data = 1;
        Commands[0].Wait = Commands[1].Wait = Commands[2].Wait = TRUE;

        KdDwcTrace("supply %s at 0x%lx, %lu mV\n", Rpmh->Supplies[i].Name, Address, Rpmh->Supplies[i].Millivolts);
        if (!KdDwcRpmhSend(Commands, RTL_NUMBER_OF(Commands)))
            Ok = FALSE;
    }

    return Ok;
}

/**
 * @brief Puts back the bus and supply votes the controller needs, which firmware dropped at exit.
 *
 * @param[in] Rpmh
 * Where the apps RSC and the command DB are, and what to vote.
 */
BOOLEAN
NTAPI
KdDwcVoteRpmh(
    _In_ const KDDWC_RPMH* Rpmh)
{
    PHYSICAL_ADDRESS Address;
    UCHAR Magic[4];
    ULONG Id, Pages, i;
    PUCHAR Rsc;
    BOOLEAN Ok;

    Address.QuadPart = Rpmh->CmdDbBase;
    KdDwcCmdDbSize = Rpmh->CmdDbSize;
    KdDwcCmdDb = KdMapPhysicalMemory64(Address, BYTES_TO_PAGES(Rpmh->CmdDbSize));
    if (KdDwcCmdDb == NULL)
        return FALSE;

    for (i = 0; i < sizeof(Magic); i++)
        Magic[i] = (UCHAR)KdDwcCmdDbRead(4 + i, 1);
    if (Magic[0] != 0xDB || Magic[1] != 0x30 || Magic[2] != 0x03 || Magic[3] != 0x0C)
    {
        KdDwcTrace("no command DB at 0x%I64x\n", Rpmh->CmdDbBase);
        KdUnmapVirtualAddress((PVOID)KdDwcCmdDb, BYTES_TO_PAGES(Rpmh->CmdDbSize));
        KdDwcCmdDb = NULL;
        return FALSE;
    }

    Pages = BYTES_TO_PAGES(Rpmh->TcsOffset + RSC_MAX_TCS * RSC_TCS_STRIDE);
    Address.QuadPart = Rpmh->RscBase;
    Rsc = KdMapPhysicalMemory64(Address, Pages);
    if (Rsc == NULL)
        return FALSE;

    Id = KdDwcRead(Rsc, RSC_ID);
    KdDwcLayout = (RSC_ID_MAJOR(Id) >= 3) ? &KdDwcTcsLayout3 : &KdDwcTcsLayout2;
    KdDwcTcsCommands = RSC_CONFIG_COMMANDS(KdDwcRead(Rsc, RSC_CONFIG));
    KdDwcTcsCount = max(RSC_CONFIG_TCS(KdDwcRead(Rsc, RSC_CONFIG), RSC_APPS_DRV), Rpmh->ActiveTcs + 1);
    KdDwcTcs = Rsc + Rpmh->TcsOffset;
    KdDwcTcsId = Rpmh->ActiveTcs;
    KdDwcTrace("RSC 0x%lx, %lu TCSs of %lu commands, voting through TCS %lu\n",
               Id, KdDwcTcsCount, KdDwcTcsCommands, KdDwcTcsId);

    if (KdDwcTcsCommands == 0)
        return FALSE;

    /* Buses first, before the active TCS loses the commands firmware left in it */
    Ok = KdDwcVoteBuses(Rpmh);
    Ok = KdDwcVoteRail(Rpmh) && Ok;
    Ok = KdDwcVoteSupplies(Rpmh) && Ok;

    KdUnmapVirtualAddress(Rsc, Pages);
    KdUnmapVirtualAddress((PVOID)KdDwcCmdDb, BYTES_TO_PAGES(Rpmh->CmdDbSize));
    KdDwcCmdDb = NULL;
    return Ok;
}
