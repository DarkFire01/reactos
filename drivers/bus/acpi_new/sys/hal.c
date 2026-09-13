/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     HAL power-management handshake and interrupt model
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "acpipriv.h"
#include <uacpi/tables.h>   // uacpi_table_fadt
#include <uacpi/acpi.h>     // struct acpi_fadt

// 0 = PIC (8259), 1 = APIC. Set by UacpiHalPmHandshake.
ULONG g_AcpiInterruptModel = 0;

// Callback table handed to the HAL ('IPCA'). The Vista HAL reads a version-1
// table with three callbacks; Win8+ added MarkHiberPhase as a version-2 table.
#if (NTDDI_VERSION >= NTDDI_WIN8)
#define UACPI_DRIVER_HAL_VERSION 2
#else
#define UACPI_DRIVER_HAL_VERSION 1
#endif

typedef struct _ACPI_DRIVER_HAL_DISPATCH
{
    ULONG Signature;                 // 'IPCA' 0x41435049
    ULONG Version;                   // 1 (Vista/Win7) or 2 (Win8+)
    PVOID EnableDisableGpeEvents;
    PVOID InitEnableAcpi;
    PVOID GpeEnableWakeEvents;
#if (NTDDI_VERSION >= NTDDI_WIN8)
    PVOID MarkHiberPhase;            // Win8+ only
#endif
} UACPI_DRIVER_HAL_DISPATCH;

// PM dispatch table returned by HalInitPowerManagement ('HAL ' on every HAL
// checked: Vista SP1, Win8 DP, Win10 RS1). Win8 drops a timer slot and appends
// the PM register accessors.
#define UACPI_HAL_PM_SIGNATURE 0x48414C20u   // 'HAL '
#if (NTDDI_VERSION >= NTDDI_WIN8)
#define UACPI_HAL_PM_VERSION 4
#else
#define UACPI_HAL_PM_VERSION 3      // Win7 sits here until verified
#endif

typedef VOID (NTAPI *PACPI_HAL_MACHINE_STATE_INIT)(PVOID StateData, PULONG OutInterruptModel);
typedef ULONG (NTAPI *PACPI_HAL_PCI_CONFIG)(PVOID Context, ULONG Bus, ULONG Slot,
                                             PVOID Buffer, ULONG Offset, ULONG Length);

typedef struct _ACPI_HAL_PM_DISPATCH
{
    ULONG Signature;                 // 'HAL ' 0x48414C20 on every verified HAL
    ULONG Version;
#if (NTDDI_VERSION >= NTDDI_WIN8)
    PVOID Reserved0;                 // empty stub
#else
    PVOID TimerInit;
    PVOID TimerCarry;
#endif
    PACPI_HAL_MACHINE_STATE_INIT MachineStateInit;
    PVOID QueryFlags;
    PVOID PicStateIntact;
    PVOID RestorePicState;
    PACPI_HAL_PCI_CONFIG PciReadConfig;
    PACPI_HAL_PCI_CONFIG PciWriteConfig;
    PVOID GetApicVersion;
    PVOID SetMaxLegacyPciBus;
    PVOID IsVectorValid;
    PVOID GetTable;
    PVOID GetRsdp;
    PVOID GetFacsMapping;
    PVOID GetAllTables;
#if (NTDDI_VERSION >= NTDDI_WIN8)
    PVOID PmRegisterAvailable;
    PVOID PmRegisterRead;
    PVOID PmRegisterWrite;
#endif
} UACPI_HAL_PM_DISPATCH, *PUACPI_HAL_PM_DISPATCH;

// Per-state {Supported, SlpTypA, SlpTypB} for S1..S5, as the HAL expects.
typedef struct _ACPI_HAL_STATE_DATA
{
    UCHAR Supported;
    UCHAR SlpTypA;
    UCHAR SlpTypB;
} UACPI_HAL_STATE_DATA;

static UACPI_DRIVER_HAL_DISPATCH UacpiDriverHalTable;

// Kept after the handshake for PCI config access (UacpiHalPciReadConfig).
static PACPI_HAL_PCI_CONFIG g_AcpiHalPciRead;
static PACPI_HAL_PCI_CONFIG g_AcpiHalPciWrite;

// HalInitPowerManagement: slot 14 in HalDispatchTable v3 (Vista, Win7); v4
// (Win8+) drops HalIoAssignDriveLetters, moving it to 13.
#if (NTDDI_VERSION >= NTDDI_WIN8)
#define UACPI_HALDISPATCH_VERSION      4
#define UACPI_HALDISPATCH_SLOT_INITPM  13
#else
#define UACPI_HALDISPATCH_VERSION      3
#define UACPI_HALDISPATCH_SLOT_INITPM  14
#endif

typedef NTSTATUS (NTAPI *PACPI_HAL_INIT_PM)(PVOID DriverTable, PVOID *HalTable);

static PACPI_HAL_INIT_PM
UacpiHalResolveInitPowerManagement(VOID)
{
    UNICODE_STRING name;
    PULONG_PTR table;

    RtlInitUnicodeString(&name, L"HalDispatchTable");
    table = (PULONG_PTR)MmGetSystemRoutineAddress(&name);
    if (table == NULL)
    {
        UacpiTrace("[acpi] hal: HalDispatchTable not exported\n");
        return NULL;
    }
    if (*(PULONG)table != UACPI_HALDISPATCH_VERSION)
    {
        UacpiTrace("[acpi] hal: HalDispatchTable version %u, expected %u\n",
                  *(PULONG)table, UACPI_HALDISPATCH_VERSION);
        return NULL;
    }
    return (PACPI_HAL_INIT_PM)table[UACPI_HALDISPATCH_SLOT_INITPM];
}

// Fixed ACPI registers for the HAL callbacks.
//
// The HAL calls these inside its sleep path at HIGH_LEVEL with interrupts off,
// so they cannot take a uACPI lock or run AML. The PM1/GPE/SMI registers are
// decoded from the FADT at handshake and accessed with raw port/MMIO; the armed
// wake set is snapshotted into a flat cache the callbacks read lock-free.

#define UACPI_PM_MAX_REG_BYTES 32

typedef struct _UACPI_PM_REG
{
    BOOLEAN Present;
    BOOLEAN Mmio;
    PUCHAR  Mapped;     // MMIO virtual address (Mmio)
    USHORT  Port;       // I/O port (!Mmio)
    UCHAR   Length;     // width in bytes
} UACPI_PM_REG;

static UACPI_PM_REG UacpiPmReg[8];     // indexed by the enum below
static UACPI_PM_REG UacpiSmiCmdReg;
static UCHAR        UacpiAcpiEnableVal;
static BOOLEAN      UacpiPmRegsValid;

enum {
    UACPI_REG_PM1A_STS = 0, UACPI_REG_PM1B_STS,
    UACPI_REG_PM1A_EN,      UACPI_REG_PM1B_EN,
    UACPI_REG_PM1A_CNT,     UACPI_REG_PM1B_CNT,
    UACPI_REG_GPE0_EN,      UACPI_REG_GPE1_EN
};

// Armed wake set, snapshotted at PASSIVE by UacpiHalRefreshWakeCache.
static UCHAR   UacpiSavedWakeGpe0[UACPI_PM_MAX_REG_BYTES];
static UCHAR   UacpiSavedWakeGpe1[UACPI_PM_MAX_REG_BYTES];
static USHORT  UacpiSavedPm1Enable;
static UACPI_PM_REG UacpiGpe0StsReg;   // status halves, for the resume snapshot
static UACPI_PM_REG UacpiGpe1StsReg;

// Wake source latched by the resume callback (EnableDisableGpe(1)).
static UCHAR   UacpiSavedWakeStatusGpe0[UACPI_PM_MAX_REG_BYTES];
static UCHAR   UacpiSavedWakeStatusGpe1[UACPI_PM_MAX_REG_BYTES];
static USHORT  UacpiSavedWakePm1Status;
static BOOLEAN UacpiSavedWakeValid;

static VOID
UacpipPmRegDescribe(UACPI_PM_REG *Reg, UINT64 Address, UCHAR SpaceId,
                   ULONG Offset, ULONG Length)
{
    PHYSICAL_ADDRESS pa;

    RtlZeroMemory(Reg, sizeof(*Reg));
    if (Address == 0 || Length == 0)
    {
        return;
    }
    if (Length > UACPI_PM_MAX_REG_BYTES)
    {
        Length = UACPI_PM_MAX_REG_BYTES;
    }
    Reg->Length = (UCHAR)Length;

    if (SpaceId == 0)   // SystemMemory
    {
        pa.QuadPart = (LONGLONG)(Address + Offset);
        Reg->Mapped = (PUCHAR)MmMapIoSpace(pa, Length, MmNonCached);
        if (Reg->Mapped == NULL)
        {
            return;
        }
        Reg->Mmio = TRUE;
    }
    else                // SystemIO
    {
        Reg->Port = (USHORT)(Address + Offset);
    }
    Reg->Present = TRUE;
}

static VOID
UacpipPmRegReadBytes(const UACPI_PM_REG *Reg, PUCHAR Out)
{
    ULONG i;

    for (i = 0; i < Reg->Length; i++)
    {
        Out[i] = Reg->Mmio ? READ_REGISTER_UCHAR(Reg->Mapped + i)
                           : READ_PORT_UCHAR((PUCHAR)(ULONG_PTR)(Reg->Port + i));
    }
}

static VOID
UacpipPmRegWriteBytes(const UACPI_PM_REG *Reg, const UCHAR *In)
{
    ULONG i;

    for (i = 0; i < Reg->Length; i++)
    {
        if (Reg->Mmio)
        {
            WRITE_REGISTER_UCHAR(Reg->Mapped + i, In[i]);
        }
        else
        {
            WRITE_PORT_UCHAR((PUCHAR)(ULONG_PTR)(Reg->Port + i), In[i]);
        }
    }
}

// Up to 4 bytes as a little-endian ULONG; absent register reads as 0.
static ULONG
UacpipPmRegRead(const UACPI_PM_REG *Reg)
{
    UCHAR buf[UACPI_PM_MAX_REG_BYTES] = { 0 };
    ULONG v = 0, i, n;

    if (!Reg->Present)
    {
        return 0;
    }
    UacpipPmRegReadBytes(Reg, buf);
    n = Reg->Length < 4 ? Reg->Length : 4;
    for (i = 0; i < n; i++)
    {
        v |= (ULONG)buf[i] << (8 * i);
    }
    return v;
}

static VOID
UacpipPmRegWrite(const UACPI_PM_REG *Reg, ULONG Value)
{
    UCHAR buf[UACPI_PM_MAX_REG_BYTES] = { 0 };
    ULONG i;

    if (!Reg->Present)
    {
        return;
    }
    for (i = 0; i < Reg->Length; i++)
    {
        buf[i] = (UCHAR)(Value >> (8 * i));
    }
    UacpipPmRegWriteBytes(Reg, buf);
}

static ULONG UacpipReadPm1Status(VOID)
{
    return UacpipPmRegRead(&UacpiPmReg[UACPI_REG_PM1A_STS]) |
           UacpipPmRegRead(&UacpiPmReg[UACPI_REG_PM1B_STS]);
}
static VOID UacpipWritePm1Status(ULONG V)   // write-1-to-clear, both blocks
{
    UacpipPmRegWrite(&UacpiPmReg[UACPI_REG_PM1A_STS], V);
    UacpipPmRegWrite(&UacpiPmReg[UACPI_REG_PM1B_STS], V);
}
static VOID UacpipWritePm1Enable(ULONG V)
{
    UacpipPmRegWrite(&UacpiPmReg[UACPI_REG_PM1A_EN], V);
    UacpipPmRegWrite(&UacpiPmReg[UACPI_REG_PM1B_EN], V);
}
static ULONG UacpipReadPm1Control(VOID)
{
    return UacpipPmRegRead(&UacpiPmReg[UACPI_REG_PM1A_CNT]) |
           UacpipPmRegRead(&UacpiPmReg[UACPI_REG_PM1B_CNT]);
}
static VOID UacpipWritePm1Control(ULONG V)
{
    UacpipPmRegWrite(&UacpiPmReg[UACPI_REG_PM1A_CNT], V);
    UacpipPmRegWrite(&UacpiPmReg[UACPI_REG_PM1B_CNT], V);
}

// Decode PM1/GPE/SMI from the FADT. PASSIVE_LEVEL; called once at handshake.
static VOID
UacpiHalResolveSleepRegisters(VOID)
{
    struct acpi_fadt *fadt = NULL;
    ULONG evtHalf, cntLen, gpe0Half, gpe1Half;

    if (UacpiPmRegsValid)
    {
        return;
    }
    if (uacpi_unlikely_error(uacpi_table_fadt(&fadt)) || fadt == NULL)
    {
        UacpiTrace("[acpi] hal: no FADT - sleep register callbacks disabled\n");
        return;
    }

    evtHalf = fadt->pm1_evt_len / 2;
    cntLen  = fadt->pm1_cnt_len;
    gpe0Half = fadt->gpe0_blk_len / 2;
    gpe1Half = fadt->gpe1_blk_len / 2;

    // PM1a/b event block: status is the low half, enable the high half.
    if (fadt->x_pm1a_evt_blk.address != 0)
    {
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_PM1A_STS],
                           fadt->x_pm1a_evt_blk.address,
                           fadt->x_pm1a_evt_blk.address_space_id, 0, evtHalf);
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_PM1A_EN],
                           fadt->x_pm1a_evt_blk.address,
                           fadt->x_pm1a_evt_blk.address_space_id, evtHalf, evtHalf);
    }
    else
    {
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_PM1A_STS], fadt->pm1a_evt_blk, 1, 0, evtHalf);
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_PM1A_EN], fadt->pm1a_evt_blk, 1, evtHalf, evtHalf);
    }
    if (fadt->x_pm1b_evt_blk.address != 0)
    {
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_PM1B_STS],
                           fadt->x_pm1b_evt_blk.address,
                           fadt->x_pm1b_evt_blk.address_space_id, 0, evtHalf);
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_PM1B_EN],
                           fadt->x_pm1b_evt_blk.address,
                           fadt->x_pm1b_evt_blk.address_space_id, evtHalf, evtHalf);
    }
    else if (fadt->pm1b_evt_blk != 0)
    {
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_PM1B_STS], fadt->pm1b_evt_blk, 1, 0, evtHalf);
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_PM1B_EN], fadt->pm1b_evt_blk, 1, evtHalf, evtHalf);
    }

    // PM1a/b control block.
    if (fadt->x_pm1a_cnt_blk.address != 0)
    {
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_PM1A_CNT],
                           fadt->x_pm1a_cnt_blk.address,
                           fadt->x_pm1a_cnt_blk.address_space_id, 0, cntLen);
    }
    else
    {
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_PM1A_CNT], fadt->pm1a_cnt_blk, 1, 0, cntLen);
    }
    if (fadt->x_pm1b_cnt_blk.address != 0)
    {
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_PM1B_CNT],
                           fadt->x_pm1b_cnt_blk.address,
                           fadt->x_pm1b_cnt_blk.address_space_id, 0, cntLen);
    }
    else if (fadt->pm1b_cnt_blk != 0)
    {
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_PM1B_CNT], fadt->pm1b_cnt_blk, 1, 0, cntLen);
    }

    // GPE0/1 enable = high half of each block; status = low half.
    if (fadt->x_gpe0_blk.address != 0)
    {
        UacpipPmRegDescribe(&UacpiGpe0StsReg, fadt->x_gpe0_blk.address,
                           fadt->x_gpe0_blk.address_space_id, 0, gpe0Half);
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_GPE0_EN], fadt->x_gpe0_blk.address,
                           fadt->x_gpe0_blk.address_space_id, gpe0Half, gpe0Half);
    }
    else
    {
        UacpipPmRegDescribe(&UacpiGpe0StsReg, fadt->gpe0_blk, 1, 0, gpe0Half);
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_GPE0_EN], fadt->gpe0_blk, 1, gpe0Half, gpe0Half);
    }
    if (fadt->x_gpe1_blk.address != 0)
    {
        UacpipPmRegDescribe(&UacpiGpe1StsReg, fadt->x_gpe1_blk.address,
                           fadt->x_gpe1_blk.address_space_id, 0, gpe1Half);
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_GPE1_EN], fadt->x_gpe1_blk.address,
                           fadt->x_gpe1_blk.address_space_id, gpe1Half, gpe1Half);
    }
    else if (fadt->gpe1_blk != 0)
    {
        UacpipPmRegDescribe(&UacpiGpe1StsReg, fadt->gpe1_blk, 1, 0, gpe1Half);
        UacpipPmRegDescribe(&UacpiPmReg[UACPI_REG_GPE1_EN], fadt->gpe1_blk, 1, gpe1Half, gpe1Half);
    }

    UacpipPmRegDescribe(&UacpiSmiCmdReg, fadt->smi_cmd, 1, 0, 1);
    UacpiAcpiEnableVal = fadt->acpi_enable;

    UacpiPmRegsValid = TRUE;
}

// Snapshot the armed wake set into the cache the HIGH_LEVEL callbacks consume.
// PASSIVE_LEVEL; sleep.c calls this after arming the wake GPEs.
VOID
UacpiHalRefreshWakeCache(VOID)
{
    if (!UacpiPmRegsValid)
    {
        return;
    }
    RtlZeroMemory(UacpiSavedWakeGpe0, sizeof(UacpiSavedWakeGpe0));
    RtlZeroMemory(UacpiSavedWakeGpe1, sizeof(UacpiSavedWakeGpe1));
    UacpipPmRegReadBytes(&UacpiPmReg[UACPI_REG_GPE0_EN], UacpiSavedWakeGpe0);
    UacpipPmRegReadBytes(&UacpiPmReg[UACPI_REG_GPE1_EN], UacpiSavedWakeGpe1);
    UacpiSavedPm1Enable = (USHORT)(UacpipPmRegRead(&UacpiPmReg[UACPI_REG_PM1A_EN]) |
                                   UacpipPmRegRead(&UacpiPmReg[UACPI_REG_PM1B_EN]));
}

// The HAL calls these from HaliAcpiSleep at HIGH_LEVEL, interrupts off.

// EnableDisableGpeEvents: 0 = pre-sleep with no wake (S5); 1 = resume.
static NTSTATUS NTAPI UacpiHalCbEnableDisableGpe(ULONG Enable)
{
    if (!UacpiPmRegsValid)
    {
        return STATUS_SUCCESS;
    }
    if (Enable == 0)
    {
        // Drop every GPE enable so nothing fires through soft-off.
        UacpipPmRegWrite(&UacpiPmReg[UACPI_REG_GPE0_EN], 0);
        UacpipPmRegWrite(&UacpiPmReg[UACPI_REG_GPE1_EN], 0);
        return STATUS_SUCCESS;
    }

    // Resume: latch the GPE/PM1 status so the wake source can be identified,
    // then re-assert the armed wake set (uACPI's S0 IRP restores the runtime
    // set afterwards).
    RtlZeroMemory(UacpiSavedWakeStatusGpe0, sizeof(UacpiSavedWakeStatusGpe0));
    RtlZeroMemory(UacpiSavedWakeStatusGpe1, sizeof(UacpiSavedWakeStatusGpe1));
    UacpipPmRegReadBytes(&UacpiGpe0StsReg, UacpiSavedWakeStatusGpe0);
    UacpipPmRegReadBytes(&UacpiGpe1StsReg, UacpiSavedWakeStatusGpe1);
    UacpiSavedWakePm1Status = (USHORT)UacpipReadPm1Status();
    UacpiSavedWakeValid = TRUE;

    UacpipPmRegWriteBytes(&UacpiPmReg[UACPI_REG_GPE0_EN], UacpiSavedWakeGpe0);
    UacpipPmRegWriteBytes(&UacpiPmReg[UACPI_REG_GPE1_EN], UacpiSavedWakeGpe1);
    return STATUS_SUCCESS;
}

// InitEnableAcpi: re-enter ACPI mode (used on hibernate resume / aborted sleep).
static NTSTATUS NTAPI UacpiHalCbInitEnableAcpi(ULONG Flags)
{
    ULONG cnt;

    if (!UacpiPmRegsValid)
    {
        return STATUS_SUCCESS;
    }

    // If firmware handed control back in legacy mode, re-arm SCI_EN via SMI_CMD.
    cnt = UacpipReadPm1Control();
    if ((cnt & 0x1) == 0 && UacpiSmiCmdReg.Present)
    {
        ULONG spins = 0;

        UacpipPmRegWrite(&UacpiSmiCmdReg, UacpiAcpiEnableVal);
        // Poll SCI_EN for ~2s; a firmware that never sets it is fatal.
        while ((UacpipReadPm1Control() & 0x1) == 0)
        {
            if (++spins > 200000)
            {
                KeBugCheckEx(0xA5 /* ACPI_BIOS_ERROR */, 0x11, 6, 0, 0);
            }
            KeStallExecutionProcessor(10);
        }
    }

    // Clear stale PM1 status (write-1-to-clear) and restore the PM1 enables.
    UacpipWritePm1Status(UacpipReadPm1Status());
    UacpipWritePm1Enable(UacpiSavedPm1Enable);

    if (Flags != 0)
    {
        // Clear GPE status and re-assert the armed wake set.
        UCHAR sts[UACPI_PM_MAX_REG_BYTES];

        RtlZeroMemory(sts, sizeof(sts));
        UacpipPmRegReadBytes(&UacpiGpe0StsReg, sts);
        UacpipPmRegWriteBytes(&UacpiGpe0StsReg, sts);
        RtlZeroMemory(sts, sizeof(sts));
        UacpipPmRegReadBytes(&UacpiGpe1StsReg, sts);
        UacpipPmRegWriteBytes(&UacpiGpe1StsReg, sts);
        UacpipPmRegWriteBytes(&UacpiPmReg[UACPI_REG_GPE0_EN], UacpiSavedWakeGpe0);
        UacpipPmRegWriteBytes(&UacpiPmReg[UACPI_REG_GPE1_EN], UacpiSavedWakeGpe1);
    }

    // Clear SLP_EN (0x2000) and BM_RLD (0x2), leaving SCI_EN and the rest.
    UacpipWritePm1Control(UacpipReadPm1Control() & ~0x2002u);
    return STATUS_SUCCESS;
}

// GpeEnableWakeEvents: pre-sleep arm for S1-S4. The wake set is already in
// hardware (sleep.c armed it at PASSIVE); re-assert it and keep it snapshotted.
static NTSTATUS NTAPI UacpiHalCbGpeEnableWake(VOID)
{
    if (!UacpiPmRegsValid)
    {
        return STATUS_SUCCESS;
    }
    UacpipPmRegWriteBytes(&UacpiPmReg[UACPI_REG_GPE0_EN], UacpiSavedWakeGpe0);
    UacpipPmRegWriteBytes(&UacpiPmReg[UACPI_REG_GPE1_EN], UacpiSavedWakeGpe1);
    return STATUS_SUCCESS;
}

// MarkHiberPhase (Win8+): driver globals are already in the hibernate image.
#if (NTDDI_VERSION >= NTDDI_WIN8)
static NTSTATUS NTAPI UacpiHalCbMarkHiberPhase(VOID)
{
    return STATUS_SUCCESS;
}
#endif

// Harvest \_S1..\_S5 SLP_TYP triples into the HAL state-data array.
static VOID
UacpiHalHarvestSleepStates(UACPI_HAL_STATE_DATA StateData[5])
{
    ULONG sx;

    RtlZeroMemory(StateData, 5 * sizeof(UACPI_HAL_STATE_DATA));
    for (sx = 1; sx <= 5; sx++)
    {
        char name[5] = { '_', 'S', (char)('0' + sx), '_', 0 };
        uacpi_object *ret = NULL;

        if (uacpi_likely_success(
                uacpi_eval(uacpi_namespace_root(), name, NULL, &ret)) && ret != NULL)
                {
            uacpi_object_array pkg;
            if (uacpi_object_get_type(ret) == UACPI_OBJECT_PACKAGE &&
                uacpi_likely_success(uacpi_object_get_package(ret, &pkg)))
                {
                uacpi_u64 v = 0;
                StateData[sx - 1].Supported = 1;
                if (pkg.count >= 1 &&
                    uacpi_likely_success(uacpi_object_get_integer(pkg.objects[0], &v)))
                    {
                    StateData[sx - 1].SlpTypA = (UCHAR)v;
                }
                if (pkg.count >= 2 &&
                    uacpi_likely_success(uacpi_object_get_integer(pkg.objects[1], &v)))
                    {
                    StateData[sx - 1].SlpTypB = (UCHAR)v;
                }
            }
            uacpi_object_unref(ret);
        }
    }
}

// Pass \_Sx to the HAL, store its interrupt model, evaluate \_PIC. FDO start.
NTSTATUS
UacpiHalPmHandshake(VOID)
{
    UACPI_HAL_STATE_DATA stateData[5];
    PACPI_HAL_INIT_PM initPm;
    PUACPI_HAL_PM_DISPATCH halTable = NULL;
    ULONG model = (ULONG)-1;   // sentinel: the HAL MUST overwrite this
    NTSTATUS status;

    PAGED_CODE();

    // Decode the fixed ACPI registers the sleep callbacks use (before any sleep).
    UacpiHalResolveSleepRegisters();

    UacpiHalHarvestSleepStates(stateData);

    // Build the driver->HAL callback table.
    RtlZeroMemory(&UacpiDriverHalTable, sizeof(UacpiDriverHalTable));
    UacpiDriverHalTable.Signature            = 0x41435049u;   // 'IPCA'
    UacpiDriverHalTable.Version              = UACPI_DRIVER_HAL_VERSION;
    UacpiDriverHalTable.EnableDisableGpeEvents = UacpiHalCbEnableDisableGpe;
    UacpiDriverHalTable.InitEnableAcpi         = UacpiHalCbInitEnableAcpi;
    UacpiDriverHalTable.GpeEnableWakeEvents    = UacpiHalCbGpeEnableWake;
#if (NTDDI_VERSION >= NTDDI_WIN8)
    UacpiDriverHalTable.MarkHiberPhase         = UacpiHalCbMarkHiberPhase;
#endif

    // Handshake failure is fatal (0xA5/0x11); there is no PIC fallback.
    initPm = UacpiHalResolveInitPowerManagement();
    if (initPm == NULL)
    {
        UacpiTrace("[acpi] hal: FATAL - HalInitPowerManagement not found\n");
        KeBugCheckEx(0xA5 /* ACPI_BIOS_ERROR */, 0x11, 0, 0, 0);
    }

    status = initPm(&UacpiDriverHalTable, (PVOID *)&halTable);
    if (!NT_SUCCESS(status) || halTable == NULL)
    {
        UacpiTrace("[acpi] hal: FATAL - HalInitPowerManagement failed 0x%X\n", status);
        KeBugCheckEx(0xA5 /* ACPI_BIOS_ERROR */, 0x11, 1, (ULONG_PTR)status, 0);
    }

    // A Version mismatch means this build's NTDDI gate is wrong for the running HAL.
    if (halTable->Signature != UACPI_HAL_PM_SIGNATURE ||
        halTable->Version != UACPI_HAL_PM_VERSION ||
        halTable->MachineStateInit == NULL ||
        halTable->PciReadConfig == NULL || halTable->PciWriteConfig == NULL)
    {
        UacpiTrace("[acpi] hal: FATAL - unexpected PM dispatch table (sig 0x%08X ver %u, "
                  "expected ver %u)\n",
                  halTable->Signature, halTable->Version, UACPI_HAL_PM_VERSION);
        KeBugCheckEx(0xA5 /* ACPI_BIOS_ERROR */, 0x11, 2,
                     (ULONG_PTR)halTable->Signature, halTable->Version);
    }
    g_AcpiHalPciRead  = halTable->PciReadConfig;    // PCI config access from here on (pci.sys)
    g_AcpiHalPciWrite = halTable->PciWriteConfig;

    // The HAL returns the interrupt model and registers the sleep handlers.
    halTable->MachineStateInit(stateData, &model);

    UacpiTrace("[acpi] hal: PM handshake OK (table ver %u); model %u, "
              "S1..S5 {%u,%u,%u,%u,%u}\n",
              halTable->Version, model,
              stateData[0].Supported, stateData[1].Supported, stateData[2].Supported,
              stateData[3].Supported, stateData[4].Supported);

    if (model == (ULONG)-1)
    {
        // The HAL never wrote the out-param: slot/arity contract is wrong.
        UacpiTrace("[acpi] hal: FATAL - MachineStateInit did not report a model "
                  "(slot/arity mismatch)\n");
        KeBugCheckEx(0xA3 /* ACPI_DRIVER_INTERNAL */, 0x12,
                     (ULONG_PTR)halTable->MachineStateInit, halTable->Version, 0);
    }

    // Only PIC (0) and APIC (1) are implemented; anything else is fatal.
    if (model != 0 && model != 1)
    {
        UacpiTrace("[acpi] hal: FATAL - interrupt model %u (SAPIC/other) not "
                  "implemented\n", model);
        KeBugCheckEx(0xA3 /* ACPI_DRIVER_INTERNAL */, 0x13, model, 0, 0);
    }
    g_AcpiInterruptModel = model;
    UacpiTrace("[acpi] hal: interrupt model = %u (%s)\n", g_AcpiInterruptModel,
              model == 1 ? "APIC" : "PIC");

    // Tell firmware which routing we will use.  Absent \_PIC is success (spec).
    {
        uacpi_interrupt_model um =
            (model == 1) ? UACPI_INTERRUPT_MODEL_IOAPIC : UACPI_INTERRUPT_MODEL_PIC;
        uacpi_status st = uacpi_set_interrupt_model(um);
        if (uacpi_unlikely_error(st))
        {
            UacpiTrace("[acpi] hal: \\_PIC(%u) failed: %s\n",
                      model, uacpi_status_to_string(st));
        }
    }

    return STATUS_SUCCESS;
}

// PCI config pass-through to the HAL; returns bytes moved. DISPATCH_LEVEL safe.
ULONG
UacpiHalPciReadConfig(PVOID Context, ULONG Bus, ULONG Slot, PVOID Buffer,
                     ULONG Offset, ULONG Length)
{
    if (g_AcpiHalPciRead == NULL)
    {
        return 0;   // handshake not done yet; caller treats short read as failure
    }
    return g_AcpiHalPciRead(Context, Bus, Slot, Buffer, Offset, Length);
}

ULONG
UacpiHalPciWriteConfig(PVOID Context, ULONG Bus, ULONG Slot, PVOID Buffer,
                      ULONG Offset, ULONG Length)
{
    if (g_AcpiHalPciWrite == NULL)
    {
        return 0;
    }
    return g_AcpiHalPciWrite(Context, Bus, Slot, Buffer, Offset, Length);
}
