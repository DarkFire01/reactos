/*
 * PROJECT:     ReactOS Qualcomm xHCI lower filter
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Controller and PHY bring up, suspend and resume
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "xhcif.h"

#define NDEBUG
#include <debug.h>

#define SSPHY_TUNE_ANY          0x00
#define SSPHY_TUNE_MSM8992_ONLY 0x01

typedef struct _SSPHY_TUNING
{
    USHORT Offset;
    UCHAR Value;
    UCHAR Msm8992Value;
    UCHAR Flags;
} SSPHY_TUNING;

/* SuperSpeed PHY bring up, applied in order */
static const SSPHY_TUNING SsPhyTuning[] =
{
    { 0x6010, 0x11, 0x11, SSPHY_TUNE_ANY },
    { 0x0604, 0x01, 0x01, SSPHY_TUNE_ANY },
    { 0x0048, 0x08, 0x08, SSPHY_TUNE_ANY },
    { 0x00AC, 0x82, 0x82, SSPHY_TUNE_ANY },
    { 0x010C, 0x03, 0x03, SSPHY_TUNE_ANY },
    { 0x0100, 0xD5, 0xD5, SSPHY_TUNE_ANY },
    { 0x0104, 0xAA, 0xAA, SSPHY_TUNE_ANY },
    { 0x0108, 0x4D, 0x4D, SSPHY_TUNE_ANY },
    { 0x009C, 0x11, 0x11, SSPHY_TUNE_ANY },
    { 0x0090, 0x2B, 0x2B, SSPHY_TUNE_ANY },
    { 0x0094, 0x68, 0x68, SSPHY_TUNE_ANY },
    { 0x0114, 0x7C, 0x7C, SSPHY_TUNE_ANY },
    { 0x0034, 0x07, 0x07, SSPHY_TUNE_ANY },
    { 0x0038, 0x1F, 0x1F, SSPHY_TUNE_ANY },
    { 0x003C, 0x0F, 0x0F, SSPHY_TUNE_ANY },
    { 0x0024, 0x01, 0x01, SSPHY_TUNE_ANY },
    { 0x000C, 0x0F, 0x03, SSPHY_TUNE_ANY },
    { 0x0010, 0x0F, 0x03, SSPHY_TUNE_ANY },
    { 0x0014, 0x46, 0x46, SSPHY_TUNE_ANY },
    { 0x00A0, 0x00, 0xFF, SSPHY_TUNE_MSM8992_ONLY },
    { 0x041C, 0x75, 0x75, SSPHY_TUNE_ANY },
    { 0x004C, 0x90, 0x90, SSPHY_TUNE_ANY },
    { 0x0050, 0x07, 0x07, SSPHY_TUNE_ANY },
    { 0x0004, 0xE1, 0xE1, SSPHY_TUNE_ANY },
    { 0x00E0, 0x20, 0x20, SSPHY_TUNE_ANY },
    { 0x00E8, 0x77, 0x77, SSPHY_TUNE_ANY },
    { 0x00F0, 0x15, 0x15, SSPHY_TUNE_ANY },
    { 0x0268, 0x02, 0x02, SSPHY_TUNE_ANY },
    { 0x04F0, 0x67, 0x67, SSPHY_TUNE_ANY },
    { 0x04BC, 0x06, 0x06, SSPHY_TUNE_ANY },
    { 0x04C0, 0x6C, 0x6C, SSPHY_TUNE_ANY },
    { 0x04C4, 0xA7, 0xA7, SSPHY_TUNE_ANY },
    { 0x04F8, 0x40, 0x40, SSPHY_TUNE_ANY },
    { 0x0500, 0x73, 0x73, SSPHY_TUNE_ANY },
    { 0x0504, 0x06, 0x06, SSPHY_TUNE_ANY },
    { 0x064C, 0x48, 0x48, SSPHY_TUNE_ANY },
    { 0x00B4, 0x01, 0x01, SSPHY_TUNE_ANY },
    { 0x00B8, 0x02, 0x02, SSPHY_TUNE_ANY },
    { 0x00C0, 0x31, 0x31, SSPHY_TUNE_ANY },
    { 0x00C4, 0x01, 0x01, SSPHY_TUNE_ANY },
    { 0x00C8, 0x19, 0x19, SSPHY_TUNE_ANY },
    { 0x00CC, 0x19, 0x19, SSPHY_TUNE_ANY },
    { 0x0654, 0x08, 0x08, SSPHY_TUNE_ANY },
    { 0x065C, 0xE5, 0xE5, SSPHY_TUNE_ANY },
    { 0x0660, 0x03, 0x03, SSPHY_TUNE_ANY },
    { 0x06A0, 0x13, 0x13, SSPHY_TUNE_ANY },
    { 0x066C, 0xFF, 0xFF, SSPHY_TUNE_ANY },
    { 0x0674, 0x17, 0x17, SSPHY_TUNE_ANY },
    { 0x06AC, 0x05, 0x05, SSPHY_TUNE_ANY },
};

typedef struct _XHCIF_REG_WRITE
{
    USHORT Offset;
    UCHAR Value;
} XHCIF_REG_WRITE;

/* SDM845 high speed PHY setup, applied in order before the board tuning */
static const XHCIF_REG_WRITE Qusb2Setup[] =
{
    { 0x0004, 0x03 },
    { 0x018C, 0x7C },
    { 0x002C, 0x80 },
    { 0x0184, 0x0A },
    { 0x00B4, 0x19 },
    { 0x0194, 0x40 },
    { 0x0198, 0x20 },
    { 0x0214, 0x21 },
    { 0x0220, 0x00 },
    { 0x0224, 0x58 },
    { 0x0240, 0x30 },
    { 0x0244, 0x29 },
    { 0x0248, 0xCA },
    { 0x024C, 0x04 },
    { 0x0250, 0x03 },
    { 0x023C, 0x00 },
};

/* SDM845 clock branches the controller and its PHY need, in enable order */
static const ULONG Sdm845UsbBranches[] =
{
    SDM845_GCC_CFG_NOC_USB3_CBCR,
    SDM845_GCC_USB30_MASTER_CBCR,
    SDM845_GCC_AGGRE_USB3_CBCR,
    SDM845_GCC_USB30_SLEEP_CBCR,
    SDM845_GCC_USB30_MOCK_UTMI_CBCR,
    SDM845_GCC_AHB2PHY_CBCR,
};

static
ULONG
XhcifRead(
    _In_ PUCHAR Base,
    _In_ ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)(Base + Offset));
}

static
VOID
XhcifWrite(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)(Base + Offset), Value);
}

static
VOID
XhcifModify(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Clear,
    _In_ ULONG Set)
{
    XhcifWrite(Base, Offset, (XhcifRead(Base, Offset) & ~Clear) | Set);
}

/* Write the pending bits of a write one to clear register back to it */
static
VOID
XhcifAcknowledge(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Mask)
{
    XhcifWrite(Base, Offset, XhcifRead(Base, Offset) & Mask);
}

static
VOID
XhcifStall(VOID)
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -10000;
    KeDelayExecutionThread(KernelMode, FALSE, &Interval);
}

/* Waits for bits to read as Value, giving up after a few milliseconds */
static
BOOLEAN
XhcifPoll(
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Mask,
    _In_ ULONG Value,
    _Inout_ PULONG Tries)
{
    while ((XhcifRead(Base, Offset) & Mask) != Value)
    {
        if (*Tries >= XHCIF_POLL_TRIES)
            return FALSE;

        (*Tries)++;
        XhcifStall();
    }

    return TRUE;
}

static
VOID
XhcifResetBlocks(
    _In_ PXHCIF_DEVICE Device)
{
    XhcifModify(Device->Gcc, GCC_USB30_BCR, 0, 1);
    XhcifStall();
    XhcifModify(Device->Gcc, GCC_USB30_BCR, 1, 0);

    XhcifModify(Device->Gcc, GCC_USB2_PHY_BCR, 0, 1);
    XhcifModify(Device->Gcc, GCC_USB3_PHY_BCR, 0, 1);
    XhcifModify(Device->Gcc, GCC_USB3_PHY_PHY_BCR, 0, 1);
    XhcifStall();
    XhcifModify(Device->Gcc, GCC_USB3_PHY_PHY_BCR, 1, 0);
    XhcifModify(Device->Gcc, GCC_USB3_PHY_BCR, 1, 0);
    XhcifModify(Device->Gcc, GCC_USB2_PHY_BCR, 1, 0);
    XhcifStall();
}

static
VOID
XhcifStartSsPhy(
    _In_ PXHCIF_DEVICE Device)
{
    ULONG Tries = 0;
    ULONG i;

    XhcifModify(Device->Gcc, GCC_USB3_PHY_BCR, 0, 1);
    XhcifModify(Device->Gcc, GCC_USB3_PHY_PHY_BCR, 0, 1);
    XhcifStall();
    XhcifModify(Device->Gcc, GCC_USB3_PHY_BCR, 1, 0);
    XhcifStall();

    for (i = 0; i < RTL_NUMBER_OF(SsPhyTuning); i++)
    {
        if ((SsPhyTuning[i].Flags & SSPHY_TUNE_MSM8992_ONLY) && !Device->Msm8992)
            continue;

        XhcifWrite(Device->SsPhy,
                   SsPhyTuning[i].Offset,
                   Device->Msm8992 ? SsPhyTuning[i].Msm8992Value : SsPhyTuning[i].Value);
    }

    XhcifModify(Device->Gcc, GCC_USB3_PHY_PHY_BCR, 1, 0);
    XhcifStall();

    XhcifWrite(Device->SsPhy, SSPHY_SW_RESET, 0);
    XhcifWrite(Device->SsPhy, SSPHY_START_CONTROL, 3);

    if (!XhcifPoll(Device->SsPhy, SSPHY_PCS_STATUS, SSPHY_PCS_STATUS_PHYSTATUS, 0, &Tries))
        DPRINT1("SuperSpeed PHY did not come out of reset\n");
}

/**
 * @brief
 * Reads the high speed PHY tuning the board firmware hands out and applies it.
 *
 * @remarks
 * Each entry is a package of kind, register and value. Entries are applied up to the first malformed one.
 */
static
NTSTATUS
XhcifApplyHsPhyTuning(
    _In_ PXHCIF_DEVICE Device)
{
    ACPI_EVAL_INPUT_BUFFER Input;
    PACPI_EVAL_OUTPUT_BUFFER Output = NULL;
    WDF_MEMORY_DESCRIPTOR InputDescriptor, OutputDescriptor;
    PACPI_METHOD_ARGUMENT Package, Field;
    PUCHAR OutputEnd, PackageEnd;
    ULONG Fields[XHCIF_PHY_ENTRY_FIELDS];
    ULONG Attempt, Entry, Count;
    ULONG OutputSize = 0;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(Input.MethodName, XHCIF_PHY_METHOD, sizeof(Input.MethodName));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InputDescriptor, &Input, sizeof(Input));

    /* Grow the reply buffer while the method says it needs more room */
    for (Attempt = 1; Attempt <= XHCIF_PHY_EVAL_ATTEMPTS; Attempt++)
    {
        OutputSize = XHCIF_PHY_EVAL_STEP * Attempt;
        Output = ExAllocatePoolWithTag(PagedPool, OutputSize, XHCIF_POOL_TAG);
        if (!Output)
            return STATUS_INSUFFICIENT_RESOURCES;

        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&OutputDescriptor, Output, OutputSize);
        Status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device->WdfDevice),
                                                   NULL,
                                                   IOCTL_ACPI_EVAL_METHOD,
                                                   &InputDescriptor,
                                                   &OutputDescriptor,
                                                   NULL,
                                                   NULL);
        if (NT_SUCCESS(Status))
            break;

        ExFreePoolWithTag(Output, XHCIF_POOL_TAG);
        Output = NULL;

        if (Status != STATUS_BUFFER_TOO_SMALL)
            break;
    }

    if (!Output)
        return Status;

    if (Output->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE)
    {
        ExFreePoolWithTag(Output, XHCIF_POOL_TAG);
        return STATUS_ACPI_INVALID_DATA;
    }

    OutputEnd = (PUCHAR)Output + OutputSize;
    Package = Output->Argument;

    for (Entry = 0; Entry < Output->Count; Entry++)
    {
        if ((PUCHAR)Package + FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) > OutputEnd ||
            (PUCHAR)ACPI_METHOD_NEXT_ARGUMENT(Package) > OutputEnd ||
            Package->Type != ACPI_METHOD_ARGUMENT_PACKAGE)
        {
            Status = STATUS_ACPI_INVALID_DATA;
            break;
        }

        /* An entry is exactly three integers */
        PackageEnd = Package->Data + Package->DataLength;
        Field = (PACPI_METHOD_ARGUMENT)Package->Data;
        for (Count = 0; (PUCHAR)Field < PackageEnd; Count++)
        {
            if (Count >= XHCIF_PHY_ENTRY_FIELDS ||
                Field->Type != ACPI_METHOD_ARGUMENT_INTEGER ||
                (PUCHAR)ACPI_METHOD_NEXT_ARGUMENT(Field) > PackageEnd)
            {
                break;
            }

            Fields[Count] = Field->Argument;
            Field = ACPI_METHOD_NEXT_ARGUMENT(Field);
        }

        if (Count != XHCIF_PHY_ENTRY_FIELDS || (PUCHAR)Field < PackageEnd)
        {
            Status = STATUS_ACPI_INVALID_DATA;
            break;
        }

        /* Only writes into this SoC's high speed PHY are taken */
        if (Fields[0] == XHCIF_PHY_ENTRY_WRITE)
        {
            if (Device->Soc == XhcifSocSdm845)
            {
                if (Fields[1] >= XHCIF_SDM845_QUSB2_BASE &&
                    Fields[1] < XHCIF_SDM845_QUSB2_BASE + XHCIF_SDM845_QUSB2_SIZE &&
                    !(Fields[1] & 3))
                {
                    XhcifWrite(Device->Qusb2, Fields[1] - XHCIF_SDM845_QUSB2_BASE, Fields[2]);
                }
            }
            else if (Fields[1] == XHCIF_CONTROLLER_BASE + QSCR_HS_PHY_PARAMS)
            {
                XhcifWrite(Device->Controller, QSCR_HS_PHY_PARAMS, Fields[2]);
            }
        }

        Package = ACPI_METHOD_NEXT_ARGUMENT(Package);
    }

    ExFreePoolWithTag(Output, XHCIF_POOL_TAG);
    return Status;
}

/* Powers the controller domain, resets the core and turns its clock branches on, rates stay as firmware set them */
static
VOID
XhcifSdm845EnableClocks(
    _In_ PXHCIF_DEVICE Device)
{
    ULONG Tries = 0;
    ULONG i;

    XhcifModify(Device->Gcc, SDM845_GCC_USB30_PRIM_GDSCR, GCC_GDSC_SW_COLLAPSE, 0);
    if (!XhcifPoll(Device->Gcc, SDM845_GCC_USB30_PRIM_GDSCR, GCC_GDSC_PWR_ON, GCC_GDSC_PWR_ON, &Tries))
        DPRINT1("USB power domain did not come up\n");

    XhcifModify(Device->Gcc, SDM845_GCC_USB30_PRIM_BCR, 0, 1);
    XhcifStall();
    XhcifModify(Device->Gcc, SDM845_GCC_USB30_PRIM_BCR, 1, 0);

    for (i = 0; i < RTL_NUMBER_OF(Sdm845UsbBranches); i++)
    {
        Tries = 0;
        XhcifModify(Device->Gcc, Sdm845UsbBranches[i], 0, GCC_CBCR_ENABLE);
        if (!XhcifPoll(Device->Gcc, Sdm845UsbBranches[i], GCC_CBCR_CLK_OFF, 0, &Tries))
            DPRINT1("USB clock branch 0x%x stayed off\n", Sdm845UsbBranches[i]);
    }
}

static
VOID
XhcifSdm845StartHsPhy(
    _In_ PXHCIF_DEVICE Device)
{
    NTSTATUS Status;
    ULONG i;

    XhcifModify(Device->Gcc, SDM845_GCC_QUSB2PHY_PRIM_BCR, 0, 1);
    XhcifStall();
    XhcifModify(Device->Gcc, SDM845_GCC_QUSB2PHY_PRIM_BCR, 1, 0);

    /* Program the PHY while it is held powered down */
    XhcifModify(Device->Qusb2,
                QUSB2_POWERDOWN,
                0,
                QUSB2_POWERDOWN_POWER_DOWN | QUSB2_POWERDOWN_CLAMP_N_EN | QUSB2_POWERDOWN_VREF_TRIM);

    for (i = 0; i < RTL_NUMBER_OF(Qusb2Setup); i++)
        XhcifWrite(Device->Qusb2, Qusb2Setup[i].Offset, Qusb2Setup[i].Value);

    Status = XhcifApplyHsPhyTuning(Device);
    if (!NT_SUCCESS(Status))
        DPRINT1("No high speed PHY tuning from firmware: 0x%lx\n", Status);

    XhcifModify(Device->Qusb2, QUSB2_POWERDOWN, QUSB2_POWERDOWN_POWER_DOWN, 0);
    XhcifStall();

    if (!(XhcifRead(Device->Qusb2, QUSB2_PLL_STATUS) & QUSB2_PLL_STATUS_CORE_READY))
        DPRINT1("High speed PHY PLL did not lock: 0x%lx\n", XhcifRead(Device->Qusb2, QUSB2_PLL_STATUS));
}

/**
 * @brief
 * Brings up the SDM845 primary controller as a USB 2 host.
 *
 * @remarks
 * The SuperSpeed PHY is never started, so the core takes its pipe clock from the UTMI side.
 */
static
VOID
XhcifSdm845InitializeHardware(
    _In_ PXHCIF_DEVICE Device)
{
    XhcifSdm845EnableClocks(Device);
    XhcifSdm845StartHsPhy(Device);

    XhcifModify(Device->Controller, DWC3_GCTL, 0, DWC3_GCTL_CORESOFTRESET);
    XhcifModify(Device->Controller, DWC3_GUSB2PHYCFG, 0, DWC3_GUSB2PHYCFG_PHYSOFTRST);
    XhcifStall();
    XhcifModify(Device->Controller, DWC3_GUSB2PHYCFG, DWC3_GUSB2PHYCFG_PHYSOFTRST, 0);

    XhcifModify(Device->Controller, QSCR_GENERAL_CFG, 0, QSCR_GENERAL_CFG_PIPE_CLK_GATE);
    XhcifStall();
    XhcifModify(Device->Controller,
                QSCR_GENERAL_CFG,
                0,
                QSCR_GENERAL_CFG_UTMI_AS_PIPE | QSCR_GENERAL_CFG_PHYSTATUS_SW);
    XhcifStall();
    XhcifModify(Device->Controller, QSCR_GENERAL_CFG, QSCR_GENERAL_CFG_PIPE_CLK_GATE, 0);

    XhcifModify(Device->Controller, DWC3_GCTL, DWC3_GCTL_CORESOFTRESET, 0);
    XhcifStall();

    /* This PHY misbehaves when the core suspends it or lets it sleep between transfers */
    XhcifModify(Device->Controller, DWC3_GUSB2PHYCFG, DWC3_GUSB2PHYCFG_SUSPEND, 0);
    XhcifModify(Device->Controller, DWC3_GUCTL1, 0, DWC3_GUCTL1_PARKMODE_DISABLE_SS);

    XhcifModify(Device->Controller, DWC3_GCTL, DWC3_GCTL_PRTCAPDIR_MASK, DWC3_GCTL_PRTCAPDIR_HOST);
}

/**
 * @brief
 * Resets the controller and both PHYs and brings the core up as a host.
 */
VOID
NTAPI
XhcifInitializeHardware(
    _In_ PXHCIF_DEVICE Device)
{
    NTSTATUS Status;

    if (Device->Soc == XhcifSocSdm845)
    {
        XhcifSdm845InitializeHardware(Device);
        Device->PhySuspended = FALSE;
        return;
    }

    XhcifModify(Device->TcsrUsb, 0, 0, 1);
    Device->PhySuspended = TRUE;

    XhcifResetBlocks(Device);
    XhcifStartSsPhy(Device);

    XhcifModify(Device->Controller, DWC3_GCTL, 0, DWC3_GCTL_CORESOFTRESET);
    XhcifWrite(Device->Controller, QSCR_HS_PHY_PARAMS, QSCR_HS_PHY_PARAMS_DEFAULT);
    XhcifModify(Device->Controller, QSCR_SS_PHY_CTRL, 0, QSCR_SS_PHY_CTRL_REF_ENABLE);
    XhcifModify(Device->Controller, QSCR_HS_PHY_COMMON, 0, 0x8000);
    XhcifModify(Device->Controller, QSCR_HS_PHY_COMMON, 0x70, 0);

    Status = XhcifApplyHsPhyTuning(Device);
    if (!NT_SUCCESS(Status))
        DPRINT1("No high speed PHY tuning from firmware: 0x%lx\n", Status);

    XhcifModify(Device->Controller, QSCR_HS_PHY_COMMON, 0, 0x4000);

    XhcifModify(Device->Controller, DWC3_GUSB2PHYCFG, 0, DWC3_GUSB2PHYCFG_PHYSOFTRST);
    XhcifModify(Device->Controller, DWC3_GUSB3PIPECTL, 0, DWC3_GUSB3PIPECTL_PHYSOFTRST);
    XhcifStall();
    XhcifModify(Device->Controller, DWC3_GUSB2PHYCFG, DWC3_GUSB2PHYCFG_PHYSOFTRST, 0);
    XhcifModify(Device->Controller, DWC3_GUSB3PIPECTL, DWC3_GUSB3PIPECTL_PHYSOFTRST, 0);
    XhcifStall();

    XhcifModify(Device->Controller, DWC3_GUSB3PIPECTL, DWC3_GUSB3PIPECTL_DELAYP1TRANS, 0);
    XhcifModify(Device->Controller, DWC3_GCTL, DWC3_GCTL_CORESOFTRESET, 0);
    XhcifStall();

    XhcifModify(Device->Controller, QSCR_SS_PHY_PARAMS, 0x7, 0x5);
    XhcifModify(Device->Controller, DWC3_GUSB3PIPECTL, 0, DWC3_GUSB3PIPECTL_ELASTIC_MODE);
    XhcifModify(Device->Controller, DWC3_GCTL, 0, DWC3_GCTL_DSBLCLKGTNG);

    XhcifModify(Device->Controller, DWC3_GCTL, DWC3_GCTL_PRTCAPDIR_MASK, DWC3_GCTL_PRTCAPDIR_HOST);
    XhcifModify(Device->Controller, QSCR_HS_PHY_PARAMS, QSCR_HS_PHY_PARAMS_BUS_MASK, 0);
}

static
VOID
XhcifSuspendHsPhy(
    _In_ PXHCIF_DEVICE Device,
    _In_ BOOLEAN Attached)
{
    ULONG Tries = 0;

    XhcifModify(Device->Controller, DWC3_GUSB2PHYCFG, DWC3_GUSB2PHYCFG_SUSPEND, 0);
    XhcifStall();
    XhcifModify(Device->Controller, DWC3_GUSB2PHYCFG, 0, DWC3_GUSB2PHYCFG_SUSPEND);
    XhcifStall();

    /* Both waits share one budget */
    XhcifPoll(Device->Controller, QSCR_PWR_EVNT_IRQ_STAT, QSCR_PWR_EVNT_LPM_IN_L2, QSCR_PWR_EVNT_LPM_IN_L2, &Tries);
    XhcifAcknowledge(Device->Controller, QSCR_PWR_EVNT_IRQ_STAT, MAXULONG);
    XhcifStall();
    XhcifPoll(Device->Controller,
              DWC3_GDBGLTSSM,
              DWC3_GDBGLTSSM_STATE_MASK,
              Attached ? DWC3_GDBGLTSSM_STATE_U3 : DWC3_GDBGLTSSM_STATE_DISABLED,
              &Tries);

    XhcifModify(Device->Controller, QSCR_HS_PHY_COMMON, 0, 0x800);
    XhcifModify(Device->Controller, QSCR_HS_PHY_COMMON, 0x2000, 0);
    XhcifModify(Device->Controller, QSCR_CHARGING_DET_CTRL, 0x36, 0);
    if (!Attached)
        XhcifModify(Device->Controller, QSCR_CTRL, 0, QSCR_CTRL_HS_SUSPEND);

    XhcifModify(Device->Controller, QSCR_HS_PHY_CTRL, QSCR_HS_PHY_CTRL_RETENTION, 0);
    XhcifModify(Device->Controller, QSCR_HS_PHY_CTRL, 0, QSCR_HS_PHY_CTRL_SUSPEND);
    XhcifStall();

    if (Attached)
    {
        XhcifModify(Device->Controller, QSCR_HS_PHY_CTRL, 0, QSCR_HS_PHY_CTRL_SLEEP_CLAMP);
        XhcifStall();
        XhcifModify(Device->Controller, QSCR_HS_PHY_CTRL, QSCR_HS_PHY_CTRL_SLEEP_CLAMP, 0);
    }

    XhcifModify(Device->Controller, QSCR_HS_PHY_COMMON, 0x8, 0);

    XhcifModify(Device->SsPhy, SSPHY_LOW_POWER_C, 0, 0x01);
    XhcifModify(Device->SsPhy, SSPHY_LOW_POWER_B, 0x40, 0);
    XhcifModify(Device->SsPhy, SSPHY_LOW_POWER_B, 0, 0x80);
    XhcifModify(Device->SsPhy, SSPHY_LOW_POWER_A, 0x88, 0);
    XhcifModify(Device->SsPhy, SSPHY_LOW_POWER_D, 0x01, 0);
    XhcifModify(Device->SsPhy, SSPHY_LOW_POWER_E, 0x01, 0);
}

static
VOID
XhcifSuspendSsLink(
    _In_ PXHCIF_DEVICE Device,
    _In_ BOOLEAN Active)
{
    ULONG LinkState = DWC3_DSTS_LINK_STATE(XhcifRead(Device->Controller, DWC3_DSTS));
    ULONG Tries = 0;

    if (LinkState < 3 || LinkState > 5)
        return;

    XhcifModify(Device->Controller, DWC3_GUSB3PIPECTL, DWC3_GUSB3PIPECTL_SUSPHY, 0);
    XhcifStall();
    XhcifModify(Device->Controller, DWC3_GUSB3PIPECTL, 0, DWC3_GUSB3PIPECTL_SUSPHY);
    XhcifStall();

    XhcifPoll(Device->Controller, QSCR_PWR_EVNT_IRQ_STAT, QSCR_PWR_EVNT_P3_ENTERED, QSCR_PWR_EVNT_P3_ENTERED, &Tries);
    XhcifAcknowledge(Device->Controller, QSCR_PWR_EVNT_IRQ_STAT, MAXULONG);

    if (Active)
        XhcifModify(Device->SsPhy, SSPHY_AUTONOMOUS_MODE, 0, 0x2);
    XhcifModify(Device->SsPhy, SSPHY_AUTONOMOUS_MODE, 0, 0x1);
}

/**
 * @brief
 * Puts both PHYs into their low power setup when the controller leaves D0.
 *
 * @remarks
 * Nothing is done while both ports have a device that is not suspended.
 */
VOID
NTAPI
XhcifSuspendPhys(
    _In_ PXHCIF_DEVICE Device)
{
    ULONG HsPort, SsPort;
    BOOLEAN HsSuspended, SsSuspended;

    /* The SDM845 PHY is kept out of suspend, and nothing here gates its power */
    if (Device->Soc == XhcifSocSdm845)
        return;

    HsPort = XhcifRead(Device->Controller, XHCIF_PORTSC_HS);
    SsPort = XhcifRead(Device->Controller, XHCIF_PORTSC_SS);
    HsSuspended = (HsPort & XHCIF_PORTSC_CCS) && (HsPort & XHCIF_PORTSC_PLS_MASK) == XHCIF_PORTSC_PLS_U3;
    SsSuspended = (SsPort & XHCIF_PORTSC_CCS) && (SsPort & XHCIF_PORTSC_PLS_MASK) == XHCIF_PORTSC_PLS_U3;
    if (!HsSuspended && !SsSuspended && (HsPort & XHCIF_PORTSC_CCS) && (SsPort & XHCIF_PORTSC_CCS))
        return;

    XhcifSuspendHsPhy(Device, !!(HsPort & XHCIF_PORTSC_CCS));
    XhcifSuspendSsLink(Device, !!(SsPort & (XHCIF_PORTSC_CCS | XHCIF_PORTSC_PR)));

    XhcifAcknowledge(Device->Controller, QSCR_PWR_EVNT_IRQ_STAT, 0x7FFF);
    XhcifModify(Device->Controller, QSCR_PWR_EVNT_IRQ_MASK, 0, 0x1028);
    XhcifAcknowledge(Device->Controller, QSCR_HS_PHY_IRQ_STAT, MAXULONG);
    XhcifModify(Device->Controller, QSCR_ALT_INTERRUPT_EN, 0, 0x1000);

    Device->PhySuspended = TRUE;
}

/**
 * @brief
 * Undoes the low power setup when the controller returns to D0.
 */
VOID
NTAPI
XhcifResumePhys(
    _In_ PXHCIF_DEVICE Device)
{
    if (!Device->PhySuspended)
        return;

    XhcifAcknowledge(Device->Controller, QSCR_PWR_EVNT_IRQ_STAT, 0x7FFF);
    XhcifModify(Device->Controller, QSCR_PWR_EVNT_IRQ_MASK, 0x7FFF, 0);
    XhcifAcknowledge(Device->Controller, QSCR_HS_PHY_IRQ_STAT, MAXULONG);
    XhcifModify(Device->Controller, QSCR_ALT_INTERRUPT_EN, 0x301E, 0);

    XhcifModify(Device->SsPhy, SSPHY_LOW_POWER_E, 0, 0x01);
    XhcifModify(Device->SsPhy, SSPHY_LOW_POWER_D, 0, 0x01);
    XhcifModify(Device->SsPhy, SSPHY_LOW_POWER_A, 0, 0x88);
    XhcifModify(Device->SsPhy, SSPHY_LOW_POWER_B, 0xC0, 0);
    XhcifModify(Device->SsPhy, SSPHY_LOW_POWER_C, 0x01, 0);

    XhcifModify(Device->Controller, QSCR_CTRL, QSCR_CTRL_HS_SUSPEND, 0);
    XhcifModify(Device->Controller, QSCR_HS_PHY_COMMON, 0, 0x8);
    XhcifModify(Device->Controller, QSCR_HS_PHY_CTRL, QSCR_HS_PHY_CTRL_SUSPEND, 0);
    XhcifModify(Device->Controller, DWC3_GUSB2PHYCFG, DWC3_GUSB2PHYCFG_SUSPEND, 0);

    XhcifModify(Device->SsPhy, SSPHY_AUTONOMOUS_MODE, 0x3, 0);
    XhcifWrite(Device->SsPhy, SSPHY_LFPS_RXTERM_IRQ_CLEAR, 1);
    XhcifWrite(Device->SsPhy, SSPHY_LFPS_RXTERM_IRQ_CLEAR, 0);
    XhcifModify(Device->Controller, DWC3_GUSB3PIPECTL, DWC3_GUSB3PIPECTL_SUSPHY, 0);

    Device->PhySuspended = FALSE;
}
