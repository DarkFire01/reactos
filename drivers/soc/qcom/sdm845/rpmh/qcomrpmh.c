/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     RPMh regulator control through the applications RSC
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <qcom/rpmhif.h>

#define QCDBG_COMPONENT "qcomrpmh"
#include <qcom/qcomdbg.h>

/*
 * Applications RSC, DRV 2 on the SDM845. The trigger command sets begin one
 * fixed offset into the DRV window, each set is a stride apart and each command
 * slot another stride inside it.
 */
#define RPMH_RSC_BASE               0x179E0000
#define RPMH_RSC_LENGTH             0x10000
#define RPMH_TCS_OFFSET             0xD00
#define RPMH_TCS_STRIDE             672
#define RPMH_CMD_STRIDE             20
#define RPMH_ACTIVE_TCS             0

/* The interrupt status and clear registers sit ahead of the first command set */
#define RPMH_IRQ_STATUS             0x04
#define RPMH_IRQ_CLEAR              0x08

/* Per command set control, then one block of command slots */
#define RPMH_TCS_CONTROL            0x14
#define RPMH_TCS_CMD_ENABLE         0x1C
#define RPMH_CMD_MSGID              0x30
#define RPMH_CMD_ADDR               0x34
#define RPMH_CMD_DATA               0x38
#define RPMH_CMD_STATUS             0x3C

/* A command reports issued once it leaves for the resource */
#define RPMH_CMD_STATUS_ISSUED      0x00000100

#define RPMH_AMC_MODE_ENABLE        0x00010000
#define RPMH_AMC_MODE_TRIGGER       0x01000000

/* Message header: a four byte write */
#define RPMH_MSGID_WRITE            0x00010000
#define RPMH_MSGID_LEN              0x00000008

/* VRM resource registers, relative to the resource address */
#define RPMH_VRM_VOLTAGE            0x0
#define RPMH_VRM_ENABLE             0x4
#define RPMH_VRM_MODE              0x8
#define RPMH_VRM_ON                 0x1

/* Command database, a read only table the always-on processor leaves in memory */
#define RPMH_CMDDB_BASE             0x85FE0000
#define RPMH_CMDDB_LENGTH           0x20000
#define RPMH_CMDDB_HEADER_SLOTS     8
#define RPMH_CMDDB_ENTRY_SIZE       24
#define RPMH_CMDDB_RSC_SIZE         16
#define RPMH_CMDDB_DATA_OFFSET      144
#define RPMH_CMDDB_ID_LENGTH        8

static const UCHAR RpmhCmdDbMagic[4] = { 0xDB, 0x30, 0x03, 0x0C };

static PUCHAR RpmhRsc = NULL;

/* The RSC window is mapped on first use; DllInitialize is not called for a
 * driver that is only pulled in as an import of another one */
static
VOID
RpmhEnsureRsc(VOID)
{
    PHYSICAL_ADDRESS Base;

    if (RpmhRsc)
        return;

    Base.QuadPart = RPMH_RSC_BASE;
    RpmhRsc = MmMapIoSpace(Base, RPMH_RSC_LENGTH, MmNonCached);
    if (!RpmhRsc)
        QC_FATAL(STATUS_INSUFFICIENT_RESOURCES, "cannot map the applications RSC");

    QC_LOG("mapped the applications RSC at %08x", RPMH_RSC_BASE);
}

/* The command database is little endian and not always naturally aligned */
static
USHORT
RpmhReadU16(
    _In_reads_bytes_(2) const UCHAR *Buffer)
{
    return (USHORT)Buffer[0] | ((USHORT)Buffer[1] << 8);
}

static
ULONG
RpmhReadU32(
    _In_reads_bytes_(4) const UCHAR *Buffer)
{
    return (ULONG)Buffer[0] | ((ULONG)Buffer[1] << 8) |
           ((ULONG)Buffer[2] << 16) | ((ULONG)Buffer[3] << 24);
}

static
ULONG
RpmhRead(
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(RpmhRsc + RPMH_TCS_OFFSET + Offset));
}

static
VOID
RpmhWrite(
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(RpmhRsc + RPMH_TCS_OFFSET + Offset), Value);
}

/* The hardware wants control writes read back before the next one goes out */
static
VOID
RpmhWriteSync(
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    ULONG Spin = 1000;

    RpmhWrite(Offset, Value);
    while (RpmhRead(Offset) != Value && Spin)
    {
        KeStallExecutionProcessor(1);
        Spin -= 1;
    }
}

static
ULONG
RpmhCommandOffset(
    _In_ ULONG Register,
    _In_ ULONG Slot)
{
    return Register + RPMH_CMD_STRIDE * Slot;
}

typedef struct _RPMH_COMMAND
{
    ULONG Address;
    ULONG Data;
} RPMH_COMMAND;

/* Sends commands as one active set and waits for the set to report done */
static
NTSTATUS
RpmhSendActiveSet(
    _In_reads_(Count) const RPMH_COMMAND *Commands,
    _In_ ULONG Count)
{
    ULONG Control;
    ULONG Slot;
    ULONG Spin;

    RpmhEnsureRsc();

    /* Drop any completion the firmware left pending so our poll is clean */
    RpmhWrite(RPMH_IRQ_CLEAR, 1UL << RPMH_ACTIVE_TCS);
    RpmhWrite(RPMH_TCS_CONTROL, 0);
    RpmhWrite(RPMH_TCS_CMD_ENABLE, 0);

    for (Slot = 0; Slot < Count; Slot++)
    {
        RpmhWrite(RpmhCommandOffset(RPMH_CMD_MSGID, Slot), RPMH_MSGID_LEN | RPMH_MSGID_WRITE);
        RpmhWrite(RpmhCommandOffset(RPMH_CMD_ADDR, Slot), Commands[Slot].Address);
        RpmhWrite(RpmhCommandOffset(RPMH_CMD_DATA, Slot), Commands[Slot].Data);
    }

    RpmhWrite(RPMH_TCS_CMD_ENABLE, (1UL << Count) - 1);

    /* Clear the trigger then the enable before arming the set again */
    Control = RpmhRead(RPMH_TCS_CONTROL);
    Control &= ~RPMH_AMC_MODE_TRIGGER;
    RpmhWriteSync(RPMH_TCS_CONTROL, Control);
    Control &= ~RPMH_AMC_MODE_ENABLE;
    RpmhWriteSync(RPMH_TCS_CONTROL, Control);

    RpmhWriteSync(RPMH_TCS_CONTROL, RPMH_AMC_MODE_ENABLE);
    RpmhWrite(RPMH_TCS_CONTROL, RPMH_AMC_MODE_ENABLE | RPMH_AMC_MODE_TRIGGER);

    for (Spin = 100000; Spin; Spin--)
    {
        if (RpmhRead(RPMH_IRQ_STATUS) & (1UL << RPMH_ACTIVE_TCS))
            break;
        KeStallExecutionProcessor(1);
    }

    /* Some RSC revisions never raise the summary bit, so fall back to the
     * per command status and accept the set once every command was issued */
    if (!Spin)
    {
        BOOLEAN Issued = TRUE;

        for (Slot = 0; Slot < Count; Slot++)
        {
            if (!(RpmhRead(RpmhCommandOffset(RPMH_CMD_STATUS, Slot)) & RPMH_CMD_STATUS_ISSUED))
            {
                Issued = FALSE;
                break;
            }
        }

        if (Issued)
            Spin = 1;
    }

    RpmhWrite(RPMH_IRQ_CLEAR, 1UL << RPMH_ACTIVE_TCS);
    RpmhWrite(RPMH_TCS_CONTROL, 0);
    RpmhWrite(RPMH_TCS_CMD_ENABLE, 0);

    if (!Spin)
    {
        QC_LOG("active set of %lu commands never completed", Count);
        return STATUS_IO_TIMEOUT;
    }

    return STATUS_SUCCESS;
}

/* Walks the command database and returns the address of a named resource */
static
ULONG
RpmhLookupResource(
    _In_z_ PCSTR Resource)
{
    PUCHAR CmdDb;
    PHYSICAL_ADDRESS Base;
    UCHAR Query[RPMH_CMDDB_ID_LENGTH];
    ULONG Length;
    ULONG Slot;
    ULONG Address = 0;

    Base.QuadPart = RPMH_CMDDB_BASE;
    CmdDb = MmMapIoSpace(Base, RPMH_CMDDB_LENGTH, MmCached);
    if (!CmdDb)
        QC_FATAL(STATUS_INSUFFICIENT_RESOURCES, "cannot map the command database");

    if (RtlCompareMemory(CmdDb + 4, RpmhCmdDbMagic, sizeof(RpmhCmdDbMagic)) != sizeof(RpmhCmdDbMagic))
    {
        MmUnmapIoSpace(CmdDb, RPMH_CMDDB_LENGTH);
        QC_FATAL(STATUS_DEVICE_CONFIGURATION_ERROR, "command database magic is wrong");
    }

    RtlZeroMemory(Query, sizeof(Query));
    for (Length = 0; Length < RPMH_CMDDB_ID_LENGTH && Resource[Length]; Length++)
        Query[Length] = Resource[Length];

    for (Slot = 0; Slot < RPMH_CMDDB_HEADER_SLOTS && !Address; Slot++)
    {
        PUCHAR Header = CmdDb + 8 + Slot * RPMH_CMDDB_RSC_SIZE;
        USHORT SlaveId = RpmhReadU16(Header + 0);
        USHORT HeaderOffset = RpmhReadU16(Header + 2);
        USHORT Count = RpmhReadU16(Header + 6);
        PUCHAR Entry;
        ULONG Index;

        if (SlaveId == 0)
            break;

        Entry = CmdDb + RPMH_CMDDB_DATA_OFFSET + HeaderOffset;
        for (Index = 0; Index < Count; Index++, Entry += RPMH_CMDDB_ENTRY_SIZE)
        {
            if (RtlCompareMemory(Entry, Query, RPMH_CMDDB_ID_LENGTH) == RPMH_CMDDB_ID_LENGTH)
            {
                Address = RpmhReadU32(Entry + 16);
                break;
            }
        }
    }

    MmUnmapIoSpace(CmdDb, RPMH_CMDDB_LENGTH);
    return Address;
}

NTSTATUS
NTAPI
QcRpmhEnableRegulator(
    _In_ PCQCOM_RPMH_VRM_REQUEST Request)
{
    RPMH_COMMAND Commands[3];
    ULONG Count = 0;
    ULONG Address;
    ULONG Selector;

    Address = RpmhLookupResource(Request->Resource);
    if (!Address)
    {
        QC_LOG("resource %s is not in the command database", Request->Resource);
        return STATUS_NO_SUCH_DEVICE;
    }

    if (Request->StepMicrovolts == 0 || Request->TargetMicrovolts < Request->MinMicrovolts)
        return STATUS_INVALID_PARAMETER;

    Selector = (Request->TargetMicrovolts - Request->MinMicrovolts) / Request->StepMicrovolts;

    Commands[Count].Address = Address + RPMH_VRM_VOLTAGE;
    Commands[Count].Data = Selector;
    Count++;

    if (Request->Mode)
    {
        Commands[Count].Address = Address + RPMH_VRM_MODE;
        Commands[Count].Data = Request->Mode;
        Count++;
    }

    Commands[Count].Address = Address + RPMH_VRM_ENABLE;
    Commands[Count].Data = RPMH_VRM_ON;
    Count++;

    QC_LOG("%s at %05lx, %lu uV selector %lu mode %lu",
           Request->Resource, Address, Request->TargetMicrovolts, Selector, Request->Mode);

    return RpmhSendActiveSet(Commands, Count);
}

NTSTATUS
NTAPI
DllInitialize(
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    /* The RSC is mapped lazily on first use, see RpmhEnsureRsc */
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DllUnload(VOID)
{
    if (RpmhRsc)
        MmUnmapIoSpace(RpmhRsc, RPMH_RSC_LENGTH);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    /* Export driver only, it never runs as a service */
    return STATUS_SUCCESS;
}
