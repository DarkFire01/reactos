/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     QUPv3 GENI serial engine I2C master, FIFO mode, polled
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <qcom/geniif.h>

#define QCDBG_COMPONENT "qcomgeni"
#include <qcom/qcomdbg.h>
#include <qcom/qcompnp.h>

#define GENI_POOL_TAG               'neGQ'

#define GENI_ENGINE_LENGTH          0x4000
#define GENI_CLOCK_WINDOW           0x60000

/* Engine registers */
#define GENI_FORCE_DEFAULT          0x020
#define GENI_OUTPUT_CTRL            0x024
#define GENI_CGC_CTRL               0x028
#define GENI_STATUS                 0x040
#define GENI_SER_M_CLK_CFG          0x048
#define GENI_IF_DISABLE             0x064
#define GENI_FW_REVISION            0x068
#define GENI_CLK_SEL                0x07C
#define GENI_BYTE_GRANULARITY       0x254
#define GENI_DMA_MODE_EN            0x258
#define GENI_TX_PACKING_CFG0        0x260
#define GENI_TX_PACKING_CFG1        0x264
#define GENI_I2C_TX_LENGTH          0x26C
#define GENI_I2C_RX_LENGTH          0x270
#define GENI_I2C_SCL_COUNTERS       0x278
#define GENI_RX_PACKING_CFG0        0x284
#define GENI_RX_PACKING_CFG1        0x288
#define GENI_M_CMD0                 0x600
#define GENI_M_CMD_CTRL             0x604
#define GENI_M_IRQ_STATUS           0x610
#define GENI_M_IRQ_ENABLE           0x614
#define GENI_M_IRQ_CLEAR            0x618
#define GENI_S_IRQ_CLEAR            0x648
#define GENI_TX_FIFO                0x700
#define GENI_RX_FIFO                0x780
#define GENI_TX_FIFO_STATUS         0x800
#define GENI_RX_FIFO_STATUS         0x804
#define GENI_TX_WATERMARK           0x80C
#define GENI_RX_WATERMARK           0x810
#define GENI_RX_RFR_WATERMARK       0x814
#define GENI_IOS                    0x908
#define GENI_M_GP_LENGTH            0x910
#define GENI_DMA_TX_IRQ_CLEAR       0xC44
#define GENI_DMA_RX_IRQ_CLEAR       0xD44
#define GENI_GSI_EVENT_EN           0xE18
#define GENI_SE_IRQ_ENABLE          0xE1C
#define GENI_HW_PARAM_0             0xE24
#define GENI_DMA_GENERAL_CFG        0xE30

#define GENI_STATUS_M_ACTIVE        0x00000001

#define GENI_FW_PROTOCOL(Revision)  (((Revision) >> 8) & 0xFF)
#define GENI_PROTOCOL_I2C           3
#define GENI_PROTOCOL_NONE          0xFF

#define GENI_TX_DEPTH(Param)        (((Param) >> 16) & 0x3F)
#define GENI_TX_WORD_COUNT(Status)  ((Status) & 0x0FFFFFFF)
#define GENI_RX_WORD_COUNT(Status)  ((Status) & 0x01FFFFFF)

/* Primary sequencer interrupt bits */
#define GENI_M_DONE                 0x00000001
#define GENI_M_OVERRUN              0x00000002
#define GENI_M_ILLEGAL              0x00000004
#define GENI_M_FAILURE              0x00000008
#define GENI_M_CANCEL               0x00000010
#define GENI_M_ABORT                0x00000020
#define GENI_M_GP_IRQ(Index)        (0x00000200 << (Index))
#define GENI_M_FIFO_ERRORS          0x33C00000
#define GENI_M_RX_WATERMARK         0x04000000
#define GENI_M_RX_LAST              0x08000000
#define GENI_M_TX_WATERMARK         0x40000000

#define GENI_M_ERRORS               (GENI_M_OVERRUN | GENI_M_ILLEGAL | GENI_M_FAILURE | GENI_M_ABORT)

/* Command control */
#define GENI_CMD_ABORT              0x2
#define GENI_CMD_CANCEL             0x4

/* I2C opcodes live in the top five bits, the slave address in bits 15:9 */
#define GENI_OPCODE_SHIFT           27
#define GENI_I2C_WRITE              1
#define GENI_I2C_READ               2
#define GENI_I2C_ADDRESS_SHIFT      9
#define GENI_I2C_STOP_STRETCH       0x4

/* Clock controller fields */
#define CLOCK_BRANCH_OFF            0x80000000
#define RCG_CMD                     0x0
#define RCG_CFG                     0x4
#define RCG_M                       0x8
#define RCG_N                       0xC
#define RCG_D                       0x10
#define RCG_CMD_UPDATE              0x1
#define RCG_CFG_DIVIDER_MASK        0x1F
#define RCG_CFG_SOURCE_MASK         0x700
#define RCG_CFG_MODE_MASK           0x3000

/* Divider field holds twice the divider minus one, so 1 is a straight crystal feed */
#define RCG_CFG_CRYSTAL_DIRECT      0x1

typedef struct _GENI_BUS_TIMING
{
    ULONG SpeedHz;
    ULONG Divider;
    ULONG HighCount;
    ULONG LowCount;
    ULONG CycleCount;
} GENI_BUS_TIMING;

/* Counts for a 19.2 MHz engine clock */
static const GENI_BUS_TIMING GeniTimings[] =
{
    { 100000, 7, 10, 12, 26 },
    { 400000, 2, 5, 11, 22 },
    { 1000000, 1, 2, 8, 18 },
};

typedef struct _QCOM_GENI_I2C
{
    PUCHAR Engine;
    PUCHAR Clocks;
    QCOM_GENI_I2C_CONFIG Config;
    ULONG TxDepth;
    FAST_MUTEX Lock;
} QCOM_GENI_I2C;

static
ULONG
GeniRead(
    _In_ PQCOM_GENI_I2C I2c,
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(I2c->Engine + Offset));
}

static
VOID
GeniWrite(
    _In_ PQCOM_GENI_I2C I2c,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(I2c->Engine + Offset), Value);
}

static
ULONG
ClockRead(
    _In_ PQCOM_GENI_I2C I2c,
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(I2c->Clocks + Offset));
}

static
VOID
ClockWrite(
    _In_ PQCOM_GENI_I2C I2c,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(I2c->Clocks + Offset), Value);
}

static
BOOLEAN
GeniWaitClear(
    _In_ PUCHAR Register,
    _In_ ULONG Mask,
    _In_ ULONG Microseconds)
{
    while (READ_REGISTER_ULONG((PULONG)Register) & Mask)
    {
        if (!Microseconds)
            return FALSE;

        KeStallExecutionProcessor(1);
        Microseconds -= 1;
    }

    return TRUE;
}

static
VOID
GeniEnableClocks(
    _In_ PQCOM_GENI_I2C I2c)
{
    ULONG Vote;
    ULONG Cfg;
    PUCHAR Rcg;

    Vote = ClockRead(I2c, I2c->Config.ClockVoteOffset);
    QC_LOG("clock vote %05lx = %08lx, branch %05lx = %08lx",
           I2c->Config.ClockVoteOffset,
           Vote,
           I2c->Config.EngineBranchOffset,
           ClockRead(I2c, I2c->Config.EngineBranchOffset));

    ClockWrite(I2c, I2c->Config.ClockVoteOffset, Vote | I2c->Config.ClockVoteMask);

    if (!GeniWaitClear(I2c->Clocks + I2c->Config.EngineBranchOffset, CLOCK_BRANCH_OFF, 1000))
    {
        QC_FATAL(STATUS_DEVICE_NOT_READY,
                 "engine clock branch stays off, cbcr %08lx",
                 ClockRead(I2c, I2c->Config.EngineBranchOffset));
    }

    /* The bus timings below assume the engine runs straight from the 19.2 MHz crystal */
    Rcg = I2c->Clocks + I2c->Config.EngineRcgOffset;
    Cfg = ClockRead(I2c, I2c->Config.EngineRcgOffset + RCG_CFG);
    QC_LOG("engine rcg cmd %08lx cfg %08lx",
           ClockRead(I2c, I2c->Config.EngineRcgOffset + RCG_CMD),
           Cfg);

    if ((Cfg & (RCG_CFG_SOURCE_MASK | RCG_CFG_MODE_MASK)) ||
        (Cfg & RCG_CFG_DIVIDER_MASK) > RCG_CFG_CRYSTAL_DIRECT)
    {
        ClockWrite(I2c, I2c->Config.EngineRcgOffset + RCG_M, 0);
        ClockWrite(I2c, I2c->Config.EngineRcgOffset + RCG_N, 0);
        ClockWrite(I2c, I2c->Config.EngineRcgOffset + RCG_D, 0);

        Cfg &= ~(RCG_CFG_SOURCE_MASK | RCG_CFG_MODE_MASK | RCG_CFG_DIVIDER_MASK);
        Cfg |= RCG_CFG_CRYSTAL_DIRECT;
        ClockWrite(I2c, I2c->Config.EngineRcgOffset + RCG_CFG, Cfg);
        ClockWrite(I2c,
                   I2c->Config.EngineRcgOffset + RCG_CMD,
                   ClockRead(I2c, I2c->Config.EngineRcgOffset + RCG_CMD) | RCG_CMD_UPDATE);

        if (!GeniWaitClear(Rcg + RCG_CMD, RCG_CMD_UPDATE, 500))
        {
            QC_FATAL(STATUS_DEVICE_NOT_READY,
                     "engine rcg update stuck, cmd %08lx",
                     ClockRead(I2c, I2c->Config.EngineRcgOffset + RCG_CMD));
        }

        QC_LOG("engine rcg moved to crystal, cfg %08lx",
               ClockRead(I2c, I2c->Config.EngineRcgOffset + RCG_CFG));
    }
}

/* One packing vector per byte: start bit, MSB first, 8 bits, stop on the last */
static
ULONG
GeniPackingVector(
    _In_ ULONG ByteIndex)
{
    ULONG Vector;

    Vector = ((ByteIndex * 8 + 7) << 5) | (1 << 4) | (7 << 1);
    if (ByteIndex == 3)
        Vector |= 1;

    return Vector;
}

static
VOID
GeniStopCommand(
    _In_ PQCOM_GENI_I2C I2c)
{
    GeniWrite(I2c, GENI_TX_WATERMARK, 0);
    GeniWrite(I2c, GENI_M_CMD_CTRL, GENI_CMD_CANCEL);
    if (GeniWaitClear(I2c->Engine + GENI_STATUS, GENI_STATUS_M_ACTIVE, 2000))
        return;

    GeniWrite(I2c, GENI_M_CMD_CTRL, GENI_CMD_ABORT);
    if (GeniWaitClear(I2c->Engine + GENI_STATUS, GENI_STATUS_M_ACTIVE, 2000))
        return;

    QcGeniI2cDump(I2c, "stuck");
    QC_FATAL(STATUS_IO_TIMEOUT, "engine ignores cancel and abort, status %08lx", GeniRead(I2c, GENI_STATUS));
}

static
VOID
GeniInitializeEngine(
    _In_ PQCOM_GENI_I2C I2c)
{
    const GENI_BUS_TIMING *Timing = NULL;
    ULONG Revision;
    ULONG Protocol;
    ULONG Index;
    ULONG Value;

    Revision = GeniRead(I2c, GENI_FW_REVISION);
    Protocol = GENI_FW_PROTOCOL(Revision);
    QC_LOG("engine fw revision %08lx protocol %lu, status %08lx",
           Revision, Protocol, GeniRead(I2c, GENI_STATUS));

    if (Protocol == GENI_PROTOCOL_NONE || Protocol == 0)
        QC_FATAL(STATUS_DEVICE_CONFIGURATION_ERROR, "engine has no protocol firmware loaded");
    if (Protocol != GENI_PROTOCOL_I2C)
        QC_FATAL(STATUS_DEVICE_CONFIGURATION_ERROR, "engine firmware is protocol %lu, not I2C", Protocol);

    if (GeniRead(I2c, GENI_IF_DISABLE) & 1)
        QC_FATAL(STATUS_NOT_SUPPORTED, "engine FIFO is disabled, it needs GPI DMA");

    I2c->TxDepth = GENI_TX_DEPTH(GeniRead(I2c, GENI_HW_PARAM_0));
    if (I2c->TxDepth < 2)
        QC_FATAL(STATUS_DEVICE_CONFIGURATION_ERROR, "engine reports TX FIFO depth %lu", I2c->TxDepth);

    for (Index = 0; Index < RTL_NUMBER_OF(GeniTimings); Index++)
    {
        if (GeniTimings[Index].SpeedHz == I2c->Config.BusSpeedHz)
            Timing = &GeniTimings[Index];
    }

    if (!Timing)
        QC_FATAL(STATUS_INVALID_PARAMETER, "no timing for %lu Hz", I2c->Config.BusSpeedHz);

    if (GeniRead(I2c, GENI_STATUS) & GENI_STATUS_M_ACTIVE)
    {
        QC_LOG("firmware left a command running, stopping it");
        GeniStopCommand(I2c);
    }

    /* Quiet every event source before touching the mode */
    GeniWrite(I2c, GENI_GSI_EVENT_EN, 0);
    GeniWrite(I2c, GENI_M_IRQ_CLEAR, MAXULONG);
    GeniWrite(I2c, GENI_S_IRQ_CLEAR, MAXULONG);
    GeniWrite(I2c, GENI_DMA_TX_IRQ_CLEAR, MAXULONG);
    GeniWrite(I2c, GENI_DMA_RX_IRQ_CLEAR, MAXULONG);

    /* Ungate the engine and DMA clocks and park the outputs at their defaults */
    GeniWrite(I2c, GENI_CGC_CTRL, GeniRead(I2c, GENI_CGC_CTRL) | 0x7F);
    GeniWrite(I2c, GENI_DMA_GENERAL_CFG, GeniRead(I2c, GENI_DMA_GENERAL_CFG) | 0xF);
    GeniWrite(I2c, GENI_OUTPUT_CTRL, 0x7F);
    GeniWrite(I2c, GENI_FORCE_DEFAULT, 1);

    /* FIFO mode, nothing is routed to GSI */
    GeniWrite(I2c, GENI_SE_IRQ_ENABLE, GeniRead(I2c, GENI_SE_IRQ_ENABLE) | 0xF);
    GeniWrite(I2c, GENI_DMA_MODE_EN, GeniRead(I2c, GENI_DMA_MODE_EN) & ~1UL);
    GeniWrite(I2c, GENI_GSI_EVENT_EN, 0);

    GeniWrite(I2c, GENI_RX_WATERMARK, I2c->TxDepth - 1);
    GeniWrite(I2c, GENI_RX_RFR_WATERMARK, I2c->TxDepth);

    /* Status bits only latch while enabled, the driver polls them */
    Value = GeniRead(I2c, GENI_M_IRQ_ENABLE);
    Value |= GENI_M_DONE | GENI_M_ERRORS | GENI_M_CANCEL | GENI_M_FIFO_ERRORS;
    Value |= GENI_M_RX_WATERMARK | GENI_M_RX_LAST | GENI_M_TX_WATERMARK;
    for (Index = 0; Index < 6; Index++)
        Value |= GENI_M_GP_IRQ(Index);
    GeniWrite(I2c, GENI_M_IRQ_ENABLE, Value);

    /* Byte 0 of every FIFO word is the first byte on the wire */
    Value = GeniPackingVector(0) | (GeniPackingVector(1) << 10);
    GeniWrite(I2c, GENI_TX_PACKING_CFG0, Value);
    GeniWrite(I2c, GENI_RX_PACKING_CFG0, Value);
    Value = GeniPackingVector(2) | (GeniPackingVector(3) << 10);
    GeniWrite(I2c, GENI_TX_PACKING_CFG1, Value);
    GeniWrite(I2c, GENI_RX_PACKING_CFG1, Value);
    GeniWrite(I2c, GENI_BYTE_GRANULARITY, 0);

    GeniWrite(I2c, GENI_CLK_SEL, 0);
    GeniWrite(I2c, GENI_SER_M_CLK_CFG, (Timing->Divider << 4) | 1);
    GeniWrite(I2c,
              GENI_I2C_SCL_COUNTERS,
              (Timing->HighCount << 20) | (Timing->LowCount << 10) | Timing->CycleCount);

    QC_LOG("engine ready, %lu Hz, fifo depth %lu, ios %08lx",
           Timing->SpeedHz, I2c->TxDepth, GeniRead(I2c, GENI_IOS));
}

NTSTATUS
NTAPI
QcGeniI2cOpen(
    _In_ PCQCOM_GENI_I2C_CONFIG Config,
    _Out_ PQCOM_GENI_I2C *I2c)
{
    PQCOM_GENI_I2C Context;

    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Context), GENI_POOL_TAG);
    if (!Context)
        QC_FATAL(STATUS_INSUFFICIENT_RESOURCES, "no pool for the engine context");

    RtlZeroMemory(Context, sizeof(*Context));
    ExInitializeFastMutex(&Context->Lock);
    Context->Config = *Config;

    if (Config->ClockVoteOffset + sizeof(ULONG) > GENI_CLOCK_WINDOW ||
        Config->EngineBranchOffset + sizeof(ULONG) > GENI_CLOCK_WINDOW ||
        Config->EngineRcgOffset + RCG_D + sizeof(ULONG) > GENI_CLOCK_WINDOW)
    {
        QC_FATAL(STATUS_INVALID_PARAMETER, "clock offsets do not fit the mapped window");
    }

    Context->Clocks = MmMapIoSpace(Config->ClockControllerBase, GENI_CLOCK_WINDOW, MmNonCached);
    if (!Context->Clocks)
        QC_FATAL(STATUS_INSUFFICIENT_RESOURCES, "cannot map the clock controller %I64x", Config->ClockControllerBase.QuadPart);

    Context->Engine = MmMapIoSpace(Config->EngineBase, GENI_ENGINE_LENGTH, MmNonCached);
    if (!Context->Engine)
        QC_FATAL(STATUS_INSUFFICIENT_RESOURCES, "cannot map the engine %I64x", Config->EngineBase.QuadPart);

    QC_LOG("engine %I64x, clocks %I64x", Config->EngineBase.QuadPart, Config->ClockControllerBase.QuadPart);

    GeniEnableClocks(Context);
    GeniInitializeEngine(Context);

    *I2c = Context;
    return STATUS_SUCCESS;
}

VOID
NTAPI
QcGeniI2cClose(
    _In_ PQCOM_GENI_I2C I2c)
{
    MmUnmapIoSpace(I2c->Engine, GENI_ENGINE_LENGTH);
    MmUnmapIoSpace(I2c->Clocks, GENI_CLOCK_WINDOW);
    ExFreePoolWithTag(I2c, GENI_POOL_TAG);
}

static
NTSTATUS
GeniDecodeFailure(
    _In_ PQCOM_GENI_I2C I2c,
    _In_ ULONG Status,
    _In_ BOOLEAN Read)
{
    /* Address NACK, nothing was clocked out when the address went unanswered.
     * This is expected while probing a controller, so it stays quiet. */
    if (Status & GENI_M_GP_IRQ(1))
    {
        if (!GeniRead(I2c, GENI_M_GP_LENGTH))
            return STATUS_NO_SUCH_DEVICE;
    }

    if (Status & GENI_M_GP_IRQ(2))
        QC_LOG("data NACK, irq %08lx gp %lu", Status, GeniRead(I2c, GENI_M_GP_LENGTH));
    if (Status & GENI_M_GP_IRQ(4))
        QC_LOG("bus protocol error, irq %08lx ios %08lx", Status, GeniRead(I2c, GENI_IOS));
    if (Status & GENI_M_GP_IRQ(5))
        QC_LOG("arbitration lost, irq %08lx ios %08lx", Status, GeniRead(I2c, GENI_IOS));
    if (Status & (GENI_M_OVERRUN | GENI_M_ILLEGAL))
        QC_LOG("command rejected, irq %08lx cmd %08lx", Status, GeniRead(I2c, GENI_M_CMD0));

    return STATUS_IO_DEVICE_ERROR;
}

static
NTSTATUS
GeniRunCommand(
    _In_ PQCOM_GENI_I2C I2c,
    _In_ ULONG Opcode,
    _In_ USHORT Address,
    _In_ ULONG Parameters,
    _In_reads_bytes_opt_(Length) const UCHAR *WriteBuffer,
    _Out_writes_bytes_opt_(Length) PUCHAR ReadBuffer,
    _In_ ULONG Length)
{
    BOOLEAN Read = (Opcode == GENI_I2C_READ);
    ULONGLONG Deadline;
    ULONG Done = 0;
    ULONG Status;
    ULONG Words;
    ULONG Word;
    ULONG Byte;
    NTSTATUS Result;

    if (GeniRead(I2c, GENI_STATUS) & GENI_STATUS_M_ACTIVE)
    {
        QC_LOG("previous command still active, stopping it");
        GeniStopCommand(I2c);
    }

    GeniWrite(I2c, GENI_M_IRQ_CLEAR, MAXULONG);
    GeniWrite(I2c, Read ? GENI_I2C_RX_LENGTH : GENI_I2C_TX_LENGTH, Length);
    GeniWrite(I2c,
              GENI_M_CMD0,
              (Opcode << GENI_OPCODE_SHIFT) | (((ULONG)Address & 0x7F) << GENI_I2C_ADDRESS_SHIFT) | Parameters);
    if (!Read)
        GeniWrite(I2c, GENI_TX_WATERMARK, 1);

    /* Ten bit times per byte at the bus rate, times ten, plus 20 ms of slack */
    Deadline = KeQueryInterruptTime() +
               ((ULONGLONG)Length * 100 * 10000000ULL) / I2c->Config.BusSpeedHz +
               200000;

    for (;;)
    {
        Status = GeniRead(I2c, GENI_M_IRQ_STATUS);

        if (Status & GENI_M_ERRORS)
        {
            Result = GeniDecodeFailure(I2c, Status, Read);
            GeniWrite(I2c, GENI_TX_WATERMARK, 0);
            GeniWrite(I2c, GENI_M_IRQ_CLEAR, Status);
            if (!GeniWaitClear(I2c->Engine + GENI_STATUS, GENI_STATUS_M_ACTIVE, 1000))
                GeniStopCommand(I2c);
            return Result;
        }

        if (Read && (Status & (GENI_M_RX_WATERMARK | GENI_M_RX_LAST | GENI_M_DONE)))
        {
            Words = GENI_RX_WORD_COUNT(GeniRead(I2c, GENI_RX_FIFO_STATUS));
            while (Words)
            {
                Word = GeniRead(I2c, GENI_RX_FIFO);
                for (Byte = 0; Byte < 4 && Done < Length; Byte++)
                {
                    ReadBuffer[Done] = (UCHAR)(Word >> (Byte * 8));
                    Done += 1;
                }
                Words -= 1;
            }
        }

        if (!Read && (Status & GENI_M_TX_WATERMARK))
        {
            Words = GENI_TX_WORD_COUNT(GeniRead(I2c, GENI_TX_FIFO_STATUS));
            Words = (Words < I2c->TxDepth) ? (I2c->TxDepth - Words) : 0;
            while (Words && Done < Length)
            {
                Word = 0;
                for (Byte = 0; Byte < 4 && Done < Length; Byte++)
                {
                    Word |= (ULONG)WriteBuffer[Done] << (Byte * 8);
                    Done += 1;
                }
                GeniWrite(I2c, GENI_TX_FIFO, Word);
                Words -= 1;
            }

            if (Done == Length)
                GeniWrite(I2c, GENI_TX_WATERMARK, 0);
        }

        if (Status)
            GeniWrite(I2c, GENI_M_IRQ_CLEAR, Status);

        if (Status & GENI_M_DONE)
            break;

        if (KeQueryInterruptTime() > Deadline)
        {
            QC_LOG("%s of %lu bytes at %02x timed out after %lu, irq %08lx status %08lx",
                   Read ? "read" : "write", Length, Address, Done, Status, GeniRead(I2c, GENI_STATUS));
            GeniStopCommand(I2c);
            return STATUS_IO_TIMEOUT;
        }

        KeStallExecutionProcessor(2);
    }

    if (Done != Length)
    {
        QC_LOG("%s at %02x moved %lu of %lu bytes", Read ? "read" : "write", Address, Done, Length);
        return STATUS_DATA_ERROR;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
QcGeniI2cTransfer(
    _In_ PQCOM_GENI_I2C I2c,
    _In_ USHORT Address,
    _In_reads_bytes_opt_(WriteLength) const UCHAR *WriteBuffer,
    _In_ ULONG WriteLength,
    _Out_writes_bytes_opt_(ReadLength) PUCHAR ReadBuffer,
    _In_ ULONG ReadLength)
{
    NTSTATUS Status = STATUS_SUCCESS;

    if ((WriteLength && !WriteBuffer) || (ReadLength && !ReadBuffer) || (!WriteLength && !ReadLength))
        return STATUS_INVALID_PARAMETER;

    ExAcquireFastMutex(&I2c->Lock);

    /* Holding the clock low after the write turns the read into a repeated start */
    if (WriteLength)
    {
        Status = GeniRunCommand(I2c,
                                GENI_I2C_WRITE,
                                Address,
                                ReadLength ? GENI_I2C_STOP_STRETCH : 0,
                                WriteBuffer,
                                NULL,
                                WriteLength);
    }

    if (NT_SUCCESS(Status) && ReadLength)
        Status = GeniRunCommand(I2c, GENI_I2C_READ, Address, 0, NULL, ReadBuffer, ReadLength);

    ExReleaseFastMutex(&I2c->Lock);
    return Status;
}

VOID
NTAPI
QcGeniI2cDump(
    _In_ PQCOM_GENI_I2C I2c,
    _In_z_ PCSTR Label)
{
    QC_LOG("%s status %08lx irq %08lx cmd %08lx ios %08lx",
           Label,
           GeniRead(I2c, GENI_STATUS),
           GeniRead(I2c, GENI_M_IRQ_STATUS),
           GeniRead(I2c, GENI_M_CMD0),
           GeniRead(I2c, GENI_IOS));
    QC_LOG("%s txfifo %08lx rxfifo %08lx clk %08lx gp %08lx",
           Label,
           GeniRead(I2c, GENI_TX_FIFO_STATUS),
           GeniRead(I2c, GENI_RX_FIFO_STATUS),
           GeniRead(I2c, GENI_SER_M_CLK_CFG),
           GeniRead(I2c, GENI_M_GP_LENGTH));
    QC_LOG("%s cbcr %08lx rcg %08lx",
           Label,
           ClockRead(I2c, I2c->Config.EngineBranchOffset),
           ClockRead(I2c, I2c->Config.EngineRcgOffset + RCG_CFG));
}

NTSTATUS
NTAPI
DllInitialize(
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DllUnload(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    /* The exports are the real work, the ACPI node is only claimed */
    QcPnpInitializeClaimDriver(DriverObject, QCDBG_COMPONENT);
    return STATUS_SUCCESS;
}
