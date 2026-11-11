/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SPMI PMIC arbiter, observer reads and owned channel writes
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <qcom/spmiif.h>

#define QCDBG_COMPONENT "qcomspmi"
#include <qcom/qcomdbg.h>
#include <qcom/qcompnp.h>

/*
 * SDM845 arbiter, version 5. The core window holds the peripheral map, the
 * config window says which execution environment owns each peripheral, the
 * observer window carries reads and the channel window carries writes. This
 * driver runs as the applications processor, EE 0.
 */
#define SPMI_CORE_BASE          0x0C440000
#define SPMI_CORE_LENGTH        0x1100
#define SPMI_CFG_BASE           0x0C40A000
#define SPMI_CFG_LENGTH         0x1000
#define SPMI_OBS_BASE           0x0E600000
#define SPMI_OBS_LENGTH         0x100000
#define SPMI_CHNL_BASE          0x0C600000
#define SPMI_CHNL_STRIDE        0x10000
#define SPMI_CHNL_LENGTH        0x1000
#define SPMI_EE                 0

/* Core window: number of peripherals and the per peripheral identity map */
#define SPMI_FEATURES           0x0004
#define SPMI_PERIPH_MASK        0x3FF
#define SPMI_APID_MAP(Apid)     (0x0900 + 0x4 * (Apid))
#define SPMI_PPID_SHIFT         8
#define SPMI_PPID_MASK          0xFFF
#define SPMI_PPID_COUNT         0x1000
#define SPMI_APID_NONE          0xFFFF
#define SPMI_APID_COUNT         ((SPMI_CORE_LENGTH - 0x900) / sizeof(ULONG))

/* Config window: owning EE of each peripheral */
#define SPMI_OWNER(Apid)        (0x0700 + 0x4 * (Apid))
#define SPMI_OWNER_MASK         0xF

/* Observer reads sit one stride per peripheral, writes get a whole page each */
#define SPMI_OBS_CHANNEL(Apid)  (0x80 * (Apid))
#define SPMI_CMD                0x00
#define SPMI_STATUS             0x08
#define SPMI_WDATA0             0x10
#define SPMI_RDATA0             0x18

#define SPMI_STATUS_DONE        0x1
#define SPMI_STATUS_FAILURE     0x2
#define SPMI_STATUS_DENIED      0x4
#define SPMI_STATUS_DROPPED     0x8

/* Extended register access, long form */
#define SPMI_OP_EXT_WRITEL      0
#define SPMI_OP_EXT_READL       1
#define SPMI_OP_SHIFT           27
#define SPMI_CMD_OFFSET_SHIFT   4

#define SPMI_MAX_BYTES          4
#define SPMI_TIMEOUT_US         1000

static PUCHAR SpmiCore = NULL;
static PUCHAR SpmiCfg = NULL;
static PUCHAR SpmiObs = NULL;
static PUCHAR SpmiChannel[SPMI_APID_COUNT];
static USHORT SpmiApidForPpid[SPMI_PPID_COUNT];
static UCHAR SpmiOwner[SPMI_APID_COUNT];
static BOOLEAN SpmiReady = FALSE;
static BOOLEAN SpmiFailed = FALSE;

/* One transaction on the bus at a time, a zeroed lock is a released one */
static KSPIN_LOCK SpmiLock;

/*
 * Reads the identity map once so a peripheral can be found by its PPID. A
 * PPID listed more than once resolves to the entry this EE owns. A mapping
 * failure is reported, never fatal, so a consumer that cannot reach the
 * arbiter fails its own access rather than taking the machine down.
 */
static
BOOLEAN
SpmiEnsureMapped(VOID)
{
    PHYSICAL_ADDRESS Base;
    ULONG PeripheralCount;
    ULONG Apid;

    if (SpmiReady)
        return TRUE;
    if (SpmiFailed)
        return FALSE;

    Base.QuadPart = SPMI_CORE_BASE;
    SpmiCore = MmMapIoSpace(Base, SPMI_CORE_LENGTH, MmNonCached);
    Base.QuadPart = SPMI_CFG_BASE;
    SpmiCfg = MmMapIoSpace(Base, SPMI_CFG_LENGTH, MmNonCached);
    Base.QuadPart = SPMI_OBS_BASE;
    SpmiObs = MmMapIoSpace(Base, SPMI_OBS_LENGTH, MmNonCached);
    if (!SpmiCore || !SpmiCfg || !SpmiObs)
    {
        QC_LOG("cannot map the SPMI arbiter, core %p config %p observer %p", SpmiCore, SpmiCfg, SpmiObs);
        SpmiFailed = TRUE;
        return FALSE;
    }

    for (Apid = 0; Apid < SPMI_PPID_COUNT; Apid++)
        SpmiApidForPpid[Apid] = SPMI_APID_NONE;

    PeripheralCount = READ_REGISTER_ULONG((PULONG)(SpmiCore + SPMI_FEATURES)) & SPMI_PERIPH_MASK;

    /* The identity map must stay inside the mapped core window */
    if (PeripheralCount > SPMI_APID_COUNT)
        PeripheralCount = SPMI_APID_COUNT;

    for (Apid = 0; Apid < PeripheralCount; Apid++)
    {
        ULONG Entry = READ_REGISTER_ULONG((PULONG)(SpmiCore + SPMI_APID_MAP(Apid)));
        USHORT Known;
        ULONG Ppid;

        SpmiOwner[Apid] = (UCHAR)(READ_REGISTER_ULONG((PULONG)(SpmiCfg + SPMI_OWNER(Apid))) & SPMI_OWNER_MASK);
        if (!Entry)
            continue;

        Ppid = (Entry >> SPMI_PPID_SHIFT) & SPMI_PPID_MASK;
        Known = SpmiApidForPpid[Ppid];
        if (Known == SPMI_APID_NONE || (SpmiOwner[Known] != SPMI_EE && SpmiOwner[Apid] == SPMI_EE))
            SpmiApidForPpid[Ppid] = (USHORT)Apid;
    }

    QC_LOG("mapped the SPMI arbiter, %lu peripherals", PeripheralCount);
    SpmiReady = TRUE;
    return TRUE;
}

static
NTSTATUS
SpmiLookup(
    _In_ UCHAR Sid,
    _In_ USHORT Address,
    _Out_ PUSHORT Apid)
{
    ULONG Ppid;

    if (!SpmiEnsureMapped())
        return STATUS_DEVICE_NOT_READY;

    Ppid = ((ULONG)Sid << 8) | (Address >> 8);
    if (Ppid >= SPMI_PPID_COUNT || SpmiApidForPpid[Ppid] == SPMI_APID_NONE)
        return STATUS_NO_SUCH_DEVICE;

    *Apid = SpmiApidForPpid[Ppid];
    return STATUS_SUCCESS;
}

/* Issues one command on a channel and waits for the arbiter to finish it */
static
NTSTATUS
SpmiExecute(
    _In_ PUCHAR Channel,
    _In_ ULONG Opcode,
    _In_ UCHAR Sid,
    _In_ USHORT Address,
    _In_ ULONG Length,
    _In_ BOOLEAN Write,
    _In_ ULONG WriteData,
    _Out_opt_ PULONG ReadData)
{
    KIRQL OldIrql;
    ULONG Status = 0;
    ULONG Spin;

    KeAcquireSpinLock(&SpmiLock, &OldIrql);

    if (Write)
        WRITE_REGISTER_ULONG((PULONG)(Channel + SPMI_WDATA0), WriteData);

    WRITE_REGISTER_ULONG((PULONG)(Channel + SPMI_CMD),
                         (Opcode << SPMI_OP_SHIFT) |
                         (((ULONG)Address & 0xFF) << SPMI_CMD_OFFSET_SHIFT) |
                         ((Length - 1) & 0x7));

    for (Spin = SPMI_TIMEOUT_US; Spin; Spin--)
    {
        Status = READ_REGISTER_ULONG((PULONG)(Channel + SPMI_STATUS));
        if (Status & SPMI_STATUS_DONE)
            break;
        KeStallExecutionProcessor(1);
    }

    if (Spin && ReadData && !(Status & (SPMI_STATUS_FAILURE | SPMI_STATUS_DENIED | SPMI_STATUS_DROPPED)))
        *ReadData = READ_REGISTER_ULONG((PULONG)(Channel + SPMI_RDATA0));

    KeReleaseSpinLock(&SpmiLock, OldIrql);

    if (!Spin)
    {
        QC_LOG("%s sid %u addr %04x timed out", Write ? "write" : "read", Sid, Address);
        return STATUS_IO_TIMEOUT;
    }
    if (Status & SPMI_STATUS_DENIED)
    {
        QC_LOG("%s sid %u addr %04x denied", Write ? "write" : "read", Sid, Address);
        return STATUS_ACCESS_DENIED;
    }
    if (Status & (SPMI_STATUS_FAILURE | SPMI_STATUS_DROPPED))
    {
        QC_LOG("%s sid %u addr %04x failed, status %08lx", Write ? "write" : "read", Sid, Address, Status);
        return STATUS_IO_DEVICE_ERROR;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
QcSpmiRead(
    _In_ UCHAR Sid,
    _In_ USHORT Address,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length)
{
    USHORT Apid;
    ULONG Data = 0;
    ULONG Index;
    NTSTATUS Status;

    if (Length == 0 || Length > SPMI_MAX_BYTES)
        return STATUS_INVALID_PARAMETER;

    Status = SpmiLookup(Sid, Address, &Apid);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = SpmiExecute(SpmiObs + SPMI_EE * 0x10000 + SPMI_OBS_CHANNEL(Apid),
                         SPMI_OP_EXT_READL, Sid, Address, Length, FALSE, 0, &Data);
    if (!NT_SUCCESS(Status))
        return Status;

    for (Index = 0; Index < Length; Index++)
        Buffer[Index] = (UCHAR)(Data >> (Index * 8));

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
QcSpmiWrite(
    _In_ UCHAR Sid,
    _In_ USHORT Address,
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ ULONG Length)
{
    PHYSICAL_ADDRESS Base;
    PUCHAR Channel;
    PVOID Previous;
    USHORT Apid;
    ULONG Data = 0;
    ULONG Index;
    NTSTATUS Status;

    PAGED_CODE();

    if (Length == 0 || Length > SPMI_MAX_BYTES)
        return STATUS_INVALID_PARAMETER;

    Status = SpmiLookup(Sid, Address, &Apid);
    if (!NT_SUCCESS(Status))
        return Status;

    /* Only the owning EE may write, anything else is denied by the arbiter anyway */
    if (SpmiOwner[Apid] != SPMI_EE)
    {
        QC_LOG("write sid %u addr %04x refused, peripheral belongs to EE %u", Sid, Address, SpmiOwner[Apid]);
        return STATUS_ACCESS_DENIED;
    }

    Channel = SpmiChannel[Apid];
    if (!Channel)
    {
        Base.QuadPart = SPMI_CHNL_BASE + (ULONGLONG)SPMI_CHNL_STRIDE * Apid;
        Channel = MmMapIoSpace(Base, SPMI_CHNL_LENGTH, MmNonCached);
        if (!Channel)
            return STATUS_INSUFFICIENT_RESOURCES;

        Previous = InterlockedCompareExchangePointer((PVOID *)&SpmiChannel[Apid], Channel, NULL);
        if (Previous)
        {
            MmUnmapIoSpace(Channel, SPMI_CHNL_LENGTH);
            Channel = Previous;
        }
    }

    for (Index = 0; Index < Length; Index++)
        Data |= (ULONG)Buffer[Index] << (Index * 8);

    return SpmiExecute(Channel, SPMI_OP_EXT_WRITEL, Sid, Address, Length, TRUE, Data, NULL);
}

NTSTATUS
NTAPI
DllInitialize(
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    /* The arbiter is mapped lazily on first use, see SpmiEnsureMapped */
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DllUnload(VOID)
{
    ULONG Apid;

    for (Apid = 0; Apid < SPMI_APID_COUNT; Apid++)
    {
        if (SpmiChannel[Apid])
            MmUnmapIoSpace(SpmiChannel[Apid], SPMI_CHNL_LENGTH);
    }
    if (SpmiCore)
        MmUnmapIoSpace(SpmiCore, SPMI_CORE_LENGTH);
    if (SpmiCfg)
        MmUnmapIoSpace(SpmiCfg, SPMI_CFG_LENGTH);
    if (SpmiObs)
        MmUnmapIoSpace(SpmiObs, SPMI_OBS_LENGTH);
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
