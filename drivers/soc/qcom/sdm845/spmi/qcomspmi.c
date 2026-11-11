/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SPMI PMIC arbiter, observer channel reads
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <qcom/spmiif.h>

#define QCDBG_COMPONENT "qcomspmi"
#include <qcom/qcomdbg.h>

/*
 * SDM845 arbiter, version 5. The core window holds the peripheral map, the
 * observer window carries the read channels. One execution environment, the
 * applications processor, so EE 0 throughout.
 */
#define SPMI_CORE_BASE          0x0C440000
#define SPMI_CORE_LENGTH        0x1100
#define SPMI_OBS_BASE           0x0E600000
#define SPMI_OBS_LENGTH         0x100000
#define SPMI_EE                 0

/* Core window: number of peripherals and the per peripheral identity map */
#define SPMI_FEATURES           0x0004
#define SPMI_PERIPH_MASK        0x3FF
#define SPMI_APID_MAP(Apid)     (0x0900 + 0x4 * (Apid))
#define SPMI_PPID_SHIFT         8
#define SPMI_PPID_MASK          0xFFF
#define SPMI_PPID_COUNT         0x1000
#define SPMI_APID_NONE          0xFFFF

/* A read channel lives one stride into the observer window per peripheral */
#define SPMI_OBS_CHANNEL(Apid)  (0x80 * (Apid))
#define SPMI_CMD                0x00
#define SPMI_STATUS             0x08
#define SPMI_RDATA0             0x18

#define SPMI_STATUS_DONE        0x1
#define SPMI_STATUS_FAILURE     0x2
#define SPMI_STATUS_DENIED      0x4
#define SPMI_STATUS_DROPPED     0x8

/* Extended register read, long form, the opcode the arbiter wants for a read */
#define SPMI_OP_EXT_READL       1

#define SPMI_MAX_BYTES          4
#define SPMI_TIMEOUT_US         1000

static PUCHAR SpmiCore = NULL;
static PUCHAR SpmiObs = NULL;
static USHORT SpmiApidForPpid[SPMI_PPID_COUNT];
static BOOLEAN SpmiReady = FALSE;
static BOOLEAN SpmiFailed = FALSE;

/*
 * Reads the identity map once so a peripheral can be found by its PPID. A
 * mapping failure is reported, never fatal, so a consumer that cannot reach the
 * arbiter fails its own read rather than taking the machine down.
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
    Base.QuadPart = SPMI_OBS_BASE;
    SpmiObs = MmMapIoSpace(Base, SPMI_OBS_LENGTH, MmNonCached);
    if (!SpmiCore || !SpmiObs)
    {
        QC_LOG("cannot map the SPMI arbiter, core %p observer %p", SpmiCore, SpmiObs);
        SpmiFailed = TRUE;
        return FALSE;
    }

    for (Apid = 0; Apid < SPMI_PPID_COUNT; Apid++)
        SpmiApidForPpid[Apid] = SPMI_APID_NONE;

    PeripheralCount = READ_REGISTER_ULONG((PULONG)(SpmiCore + SPMI_FEATURES)) & SPMI_PERIPH_MASK;

    /* The identity map must stay inside the mapped core window, a bad read of
     * the count would otherwise walk off the page */
    if (PeripheralCount > (SPMI_CORE_LENGTH - 0x900) / sizeof(ULONG))
        PeripheralCount = (SPMI_CORE_LENGTH - 0x900) / sizeof(ULONG);

    for (Apid = 0; Apid < PeripheralCount; Apid++)
    {
        ULONG Entry = READ_REGISTER_ULONG((PULONG)(SpmiCore + SPMI_APID_MAP(Apid)));
        ULONG Ppid;

        if (!Entry)
            continue;

        Ppid = (Entry >> SPMI_PPID_SHIFT) & SPMI_PPID_MASK;
        if (SpmiApidForPpid[Ppid] == SPMI_APID_NONE)
            SpmiApidForPpid[Ppid] = (USHORT)Apid;
    }

    QC_LOG("mapped the SPMI arbiter, %lu peripherals", PeripheralCount);
    SpmiReady = TRUE;
    return TRUE;
}

NTSTATUS
NTAPI
QcSpmiRead(
    _In_ UCHAR Sid,
    _In_ USHORT Address,
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ ULONG Length)
{
    PUCHAR Channel;
    ULONG Ppid;
    USHORT Apid;
    ULONG Command;
    ULONG Data;
    ULONG Status;
    ULONG Spin;
    ULONG Index;

    if (Length == 0 || Length > SPMI_MAX_BYTES)
        return STATUS_INVALID_PARAMETER;

    if (!SpmiEnsureMapped())
        return STATUS_DEVICE_NOT_READY;

    Ppid = ((ULONG)Sid << 8) | (Address >> 8);
    if (Ppid >= SPMI_PPID_COUNT || SpmiApidForPpid[Ppid] == SPMI_APID_NONE)
        return STATUS_NO_SUCH_DEVICE;

    Apid = SpmiApidForPpid[Ppid];
    Channel = SpmiObs + SPMI_EE * 0x10000 + SPMI_OBS_CHANNEL(Apid);

    Command = (SPMI_OP_EXT_READL << 27) | (((ULONG)Address & 0xFF) << 4) | ((Length - 1) & 0x7);
    WRITE_REGISTER_ULONG((PULONG)(Channel + SPMI_CMD), Command);

    for (Spin = SPMI_TIMEOUT_US; Spin; Spin--)
    {
        Status = READ_REGISTER_ULONG((PULONG)(Channel + SPMI_STATUS));
        if (Status & SPMI_STATUS_DONE)
            break;
        KeStallExecutionProcessor(1);
    }

    if (!Spin)
        return STATUS_IO_TIMEOUT;
    if (Status & (SPMI_STATUS_FAILURE | SPMI_STATUS_DENIED | SPMI_STATUS_DROPPED))
    {
        QC_LOG("read sid %u addr %04x failed, status %08lx", Sid, Address, Status);
        return STATUS_IO_DEVICE_ERROR;
    }

    Data = READ_REGISTER_ULONG((PULONG)(Channel + SPMI_RDATA0));
    for (Index = 0; Index < Length; Index++)
        Buffer[Index] = (UCHAR)(Data >> (Index * 8));

    return STATUS_SUCCESS;
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
    if (SpmiCore)
        MmUnmapIoSpace(SpmiCore, SPMI_CORE_LENGTH);
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
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    /* Export driver only, it never runs as a service */
    return STATUS_SUCCESS;
}
