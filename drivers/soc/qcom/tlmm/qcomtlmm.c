/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Top level mode multiplexer (TLMM) pin control
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntddk.h>
#include <qcom/tlmmif.h>

#define QCDBG_COMPONENT "qcomtlmm"
#include <qcom/qcomdbg.h>

#define TLMM_POOL_TAG           'mlTQ'

/* Every pin owns a 4 KB page of registers inside its tile */
#define TLMM_PIN_STRIDE         0x1000
#define TLMM_PIN_CONFIG         0x0
#define TLMM_PIN_IO             0x4
#define TLMM_PIN_INTR_CONFIG    0x8
#define TLMM_PIN_INTR_STATUS    0xC

#define TLMM_CFG_PULL_MASK      0x003
#define TLMM_CFG_FUNC_SHIFT     2
#define TLMM_CFG_FUNC_MASK      0x03C
#define TLMM_CFG_DRIVE_SHIFT    6
#define TLMM_CFG_DRIVE_MASK     0x1C0
#define TLMM_CFG_OUTPUT_ENABLE  0x200

#define TLMM_IO_INPUT           0x1
#define TLMM_IO_OUTPUT          0x2

#define TLMM_MAX_FUNCTION       15

typedef struct _QCOM_TLMM
{
    PUCHAR Registers;
    SIZE_T Length;
    PHYSICAL_ADDRESS Base;
    KSPIN_LOCK Lock;
} QCOM_TLMM;

static
PUCHAR
TlmmPinRegisters(
    _In_ PQCOM_TLMM Tlmm,
    _In_ PCQCOM_TLMM_PIN Pin)
{
    ULONG_PTR Offset;

    Offset = (ULONG_PTR)Pin->TileOffset + (ULONG_PTR)Pin->Number * TLMM_PIN_STRIDE;
    if (Offset + TLMM_PIN_STRIDE > Tlmm->Length)
    {
        QC_FATAL(STATUS_INVALID_PARAMETER,
                 "pin %lu tile 0x%lx is outside the 0x%Ix byte window",
                 Pin->Number, Pin->TileOffset, Tlmm->Length);
    }

    return Tlmm->Registers + Offset;
}

NTSTATUS
NTAPI
QcTlmmConnect(
    _In_ PHYSICAL_ADDRESS Base,
    _In_ SIZE_T Length,
    _Out_ PQCOM_TLMM *Tlmm)
{
    PQCOM_TLMM Context;

    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Context), TLMM_POOL_TAG);
    if (!Context)
        QC_FATAL(STATUS_INSUFFICIENT_RESOURCES, "no pool for the TLMM context");

    RtlZeroMemory(Context, sizeof(*Context));
    KeInitializeSpinLock(&Context->Lock);
    Context->Base = Base;
    Context->Length = Length;
    Context->Registers = MmMapIoSpace(Base, Length, MmNonCached);
    if (!Context->Registers)
    {
        QC_FATAL(STATUS_INSUFFICIENT_RESOURCES,
                 "cannot map TLMM at %I64x length 0x%Ix",
                 Base.QuadPart, Length);
    }

    QC_LOG("mapped TLMM %I64x length 0x%Ix", Base.QuadPart, Length);
    *Tlmm = Context;
    return STATUS_SUCCESS;
}

VOID
NTAPI
QcTlmmDisconnect(
    _In_ PQCOM_TLMM Tlmm)
{
    MmUnmapIoSpace(Tlmm->Registers, Tlmm->Length);
    ExFreePoolWithTag(Tlmm, TLMM_POOL_TAG);
}

NTSTATUS
NTAPI
QcTlmmConfigurePin(
    _In_ PQCOM_TLMM Tlmm,
    _In_ PCQCOM_TLMM_PIN Pin)
{
    PUCHAR Registers;
    ULONG Config;
    ULONG Io;
    KIRQL OldIrql;

    if (Pin->Function > TLMM_MAX_FUNCTION)
        QC_FATAL(STATUS_INVALID_PARAMETER, "pin %lu function %lu out of range", Pin->Number, Pin->Function);

    /* The drive field counts 2 mA steps starting at 2 mA */
    if (Pin->DriveMilliamps < 2 || Pin->DriveMilliamps > 16 || (Pin->DriveMilliamps & 1))
        QC_FATAL(STATUS_INVALID_PARAMETER, "pin %lu drive %lu mA invalid", Pin->Number, Pin->DriveMilliamps);

    Registers = TlmmPinRegisters(Tlmm, Pin);

    KeAcquireSpinLock(&Tlmm->Lock, &OldIrql);

    /* Latch the level before the driver turns on so the line never glitches */
    if (Pin->Direction != TlmmInput)
    {
        Io = READ_REGISTER_ULONG((PULONG)(Registers + TLMM_PIN_IO));
        if (Pin->Direction == TlmmOutputHigh)
            Io |= TLMM_IO_OUTPUT;
        else
            Io &= ~TLMM_IO_OUTPUT;
        WRITE_REGISTER_ULONG((PULONG)(Registers + TLMM_PIN_IO), Io);
    }

    Config = READ_REGISTER_ULONG((PULONG)(Registers + TLMM_PIN_CONFIG));
    Config &= ~(TLMM_CFG_PULL_MASK | TLMM_CFG_FUNC_MASK | TLMM_CFG_DRIVE_MASK | TLMM_CFG_OUTPUT_ENABLE);
    Config |= (ULONG)Pin->Pull & TLMM_CFG_PULL_MASK;
    Config |= (Pin->Function << TLMM_CFG_FUNC_SHIFT) & TLMM_CFG_FUNC_MASK;
    Config |= (((Pin->DriveMilliamps / 2) - 1) << TLMM_CFG_DRIVE_SHIFT) & TLMM_CFG_DRIVE_MASK;
    if (Pin->Direction != TlmmInput)
        Config |= TLMM_CFG_OUTPUT_ENABLE;
    WRITE_REGISTER_ULONG((PULONG)(Registers + TLMM_PIN_CONFIG), Config);

    KeReleaseSpinLock(&Tlmm->Lock, OldIrql);

    QC_LOG("pin %lu func %lu pull %d %lumA %s cfg %03lx",
           Pin->Number,
           Pin->Function,
           Pin->Pull,
           Pin->DriveMilliamps,
           Pin->Direction == TlmmInput ? "in" : (Pin->Direction == TlmmOutputHigh ? "out1" : "out0"),
           Config);

    return STATUS_SUCCESS;
}

VOID
NTAPI
QcTlmmSetLevel(
    _In_ PQCOM_TLMM Tlmm,
    _In_ PCQCOM_TLMM_PIN Pin,
    _In_ BOOLEAN High)
{
    PUCHAR Registers;
    ULONG Io;
    KIRQL OldIrql;

    Registers = TlmmPinRegisters(Tlmm, Pin);

    KeAcquireSpinLock(&Tlmm->Lock, &OldIrql);
    Io = READ_REGISTER_ULONG((PULONG)(Registers + TLMM_PIN_IO));
    if (High)
        Io |= TLMM_IO_OUTPUT;
    else
        Io &= ~TLMM_IO_OUTPUT;
    WRITE_REGISTER_ULONG((PULONG)(Registers + TLMM_PIN_IO), Io);
    KeReleaseSpinLock(&Tlmm->Lock, OldIrql);
}

BOOLEAN
NTAPI
QcTlmmGetLevel(
    _In_ PQCOM_TLMM Tlmm,
    _In_ PCQCOM_TLMM_PIN Pin)
{
    PUCHAR Registers;

    Registers = TlmmPinRegisters(Tlmm, Pin);
    return (READ_REGISTER_ULONG((PULONG)(Registers + TLMM_PIN_IO)) & TLMM_IO_INPUT) != 0;
}

VOID
NTAPI
QcTlmmDumpPin(
    _In_ PQCOM_TLMM Tlmm,
    _In_ PCQCOM_TLMM_PIN Pin,
    _In_z_ PCSTR Label)
{
    PUCHAR Registers;
    ULONG Config;
    ULONG Io;
    ULONG IntrConfig;
    ULONG IntrStatus;

    Registers = TlmmPinRegisters(Tlmm, Pin);
    Config = READ_REGISTER_ULONG((PULONG)(Registers + TLMM_PIN_CONFIG));
    Io = READ_REGISTER_ULONG((PULONG)(Registers + TLMM_PIN_IO));
    IntrConfig = READ_REGISTER_ULONG((PULONG)(Registers + TLMM_PIN_INTR_CONFIG));
    IntrStatus = READ_REGISTER_ULONG((PULONG)(Registers + TLMM_PIN_INTR_STATUS));

    QC_LOG("%s gpio%lu cfg %03lx(f%lu p%lu %lumA oe%lu) io %lx icfg %lx ist %lx",
           Label,
           Pin->Number,
           Config,
           (Config & TLMM_CFG_FUNC_MASK) >> TLMM_CFG_FUNC_SHIFT,
           Config & TLMM_CFG_PULL_MASK,
           ((Config & TLMM_CFG_DRIVE_MASK) >> TLMM_CFG_DRIVE_SHIFT) * 2 + 2,
           (Config & TLMM_CFG_OUTPUT_ENABLE) ? 1UL : 0UL,
           Io,
           IntrConfig,
           IntrStatus);
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
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    /* Export driver only, it never runs as a service */
    return STATUS_SUCCESS;
}
