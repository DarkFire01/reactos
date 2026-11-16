/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     SoC selection and the clock, reset and PHY steps the SoC files share
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kddwc.h"
#include <drivers/acpi/acpi.h>

/* Clock controller register bits */
#define GCC_GDSC_SW_COLLAPSE            0x00000001
#define GCC_GDSC_PWR_ON                 0x80000000
#define GCC_CBCR_ENABLE                 0x00000001
#define GCC_CBCR_CLK_OFF                0x80000000

/* QUSB2 PHY, the same offsets in its first and second generation */
#define QUSB2_PLL_STATUS                0x01A0
#define QUSB2_PLL_STATUS_CORE_READY     0x01
#define QUSB2_POWERDOWN                 0x0210
#define QUSB2_POWERDOWN_POWER_DOWN      0x01

/* Synopsys 7 nm and 5 nm high speed PHY */
#define SNPS_UTMI_CTRL0                 0x3C
#define SNPS_UTMI_CTRL0_SLEEPM          0x01
#define SNPS_UTMI_CTRL5                 0x50
#define SNPS_UTMI_CTRL5_POR             0x02
#define SNPS_COMMON0                    0x54
#define SNPS_COMMON0_SIDDQ              0x04
#define SNPS_COMMON0_FSEL_MASK          0x70
#define SNPS_COMMON1                    0x58
#define SNPS_COMMON1_VBUSVLDEXTSEL0     0x10
#define SNPS_COMMON1_PLLBTUNE           0x20
#define SNPS_COMMON2                    0x5C
#define SNPS_COMMON2_VREGBYPASS         0x01
#define SNPS_CTRL1                      0x60
#define SNPS_CTRL1_VBUSVLDEXT0          0x01
#define SNPS_CTRL2                      0x64
#define SNPS_CTRL2_SUSPEND_N            0x04
#define SNPS_CTRL2_SUSPEND_N_SEL        0x08
#define SNPS_CFG0                       0x94
#define SNPS_CFG0_CMN_CTRL_OVERRIDE     0x02
#define SNPS_REFCLK_CTRL                0xA0
#define SNPS_REFCLK_CTRL_SEL_HIGH       0x02

#define KDDWC_CLOCK_TIMEOUT_US          10000
#define KDDWC_DOMAIN_TIMEOUT_US         100000

static const KDDWC_PLATFORM* const KdDwcPlatforms[] =
{
    &KdDwcPlatformMsm8994,
    &KdDwcPlatformMsm8998,
    &KdDwcPlatformSdm845,
    &KdDwcPlatformSm8150,
    &KdDwcPlatformSc8180x,
    &KdDwcPlatformSc7180,
    &KdDwcPlatformSc7280,
    &KdDwcPlatformSm8350,
    &KdDwcPlatformSc8280xp,
    &KdDwcPlatformX1e80100
};

/* SELECTION ******************************************************************/

/* Compares a padded ACPI header field with a name, ignoring the trailing spaces */
static
BOOLEAN
KdDwcSameId(
    _In_reads_(8) const CHAR* Field,
    _In_ PCSTR Name)
{
    ULONG i;

    for (i = 0; i < 8 && Name[i] != ANSI_NULL; i++)
    {
        if (RtlUpperChar(Field[i]) != RtlUpperChar(Name[i]))
            return FALSE;
    }

    return (i == 8) || (Field[i] == ' ') || (Field[i] == ANSI_NULL);
}

static
const KDDWC_PLATFORM*
KdDwcMatchId(
    _In_ const DESCRIPTION_HEADER* Header)
{
    ULONG i, j;

    KdDwcTrace("ACPI %.4s OEM table ID '%.8s'\n", (PCSTR)&Header->Signature, Header->OEMTableID);

    for (i = 0; i < RTL_NUMBER_OF(KdDwcPlatforms); i++)
    {
        for (j = 0; j < RTL_NUMBER_OF(KdDwcPlatforms[i]->AcpiIds); j++)
        {
            if (KdDwcPlatforms[i]->AcpiIds[j] != NULL &&
                KdDwcSameId((const CHAR*)Header->OEMTableID, KdDwcPlatforms[i]->AcpiIds[j]))
            {
                return KdDwcPlatforms[i];
            }
        }
    }

    return NULL;
}

/*
 * The DSDT is not in the root table, the FADT points at it. Only its header is
 * read, a byte at a time, since the debugger maps it as device memory.
 */
static
BOOLEAN
KdDwcReadDsdtHeader(
    _In_ const FADT* Fadt,
    _Out_ PDESCRIPTION_HEADER Header)
{
    PHYSICAL_ADDRESS Address;
    volatile UCHAR* Mapped;
    PUCHAR Copy = (PUCHAR)Header;
    ULONG i;

    Address.QuadPart = 0;
    if (Fadt->Header.Length >= FIELD_OFFSET(FADT, x_dsdt) + sizeof(Fadt->x_dsdt))
        Address = Fadt->x_dsdt;
    if (Address.QuadPart == 0)
        Address.QuadPart = Fadt->dsdt;
    if (Address.QuadPart == 0)
        return FALSE;

    Mapped = KdMapPhysicalMemory64(Address, 1);
    if (Mapped == NULL)
        return FALSE;

    for (i = 0; i < sizeof(*Header); i++)
        Copy[i] = Mapped[i];

    KdUnmapVirtualAddress((PVOID)Mapped, 1);
    return Header->Signature == DSDT_SIGNATURE;
}

/**
 * @brief Picks the SoC by name, or by what its firmware's ACPI tables call it.
 *
 * @param[in] LoaderBlock
 * The loader block, for the ACPI tables.
 *
 * @param[in] Name
 * A SoC name from the command line, or NULL to look at the ACPI tables.
 */
const KDDWC_PLATFORM*
NTAPI
KdDwcFindPlatform(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock,
    _In_opt_ PCSTR Name)
{
    const KDDWC_PLATFORM* Platform = NULL;
    DESCRIPTION_HEADER Dsdt;
    PFADT Fadt;
    ULONG i, j;

    if (Name != NULL)
    {
        for (i = 0; i < RTL_NUMBER_OF(KdDwcPlatforms); i++)
        {
            for (j = 0; KdDwcPlatforms[i]->Name[j] != ANSI_NULL; j++)
            {
                if (RtlUpperChar(Name[j]) != KdDwcPlatforms[i]->Name[j])
                    break;
            }

            if (KdDwcPlatforms[i]->Name[j] == ANSI_NULL &&
                (Name[j] == ANSI_NULL || Name[j] == ' ' || Name[j] == '/'))
            {
                return KdDwcPlatforms[i];
            }
        }

        KdDwcTrace("unknown SoC name\n");
        return NULL;
    }

    if (LoaderBlock == NULL)
        return NULL;

    /* Qualcomm firmware gives its static tables a generic ID, only the DSDT names the SoC */
    Fadt = KdGetAcpiTablePhase0(LoaderBlock, FADT_SIGNATURE);
    if (Fadt == NULL)
    {
        KdDwcTrace("no FADT\n");
        return NULL;
    }

    if (KdDwcReadDsdtHeader(Fadt, &Dsdt))
        Platform = KdDwcMatchId(&Dsdt);
    else
        KdDwcTrace("the FADT has no usable DSDT\n");

    if (Platform == NULL)
        Platform = KdDwcMatchId(&Fadt->Header);

    KdUnmapVirtualAddress(Fadt, BYTES_TO_PAGES(Fadt->Header.Length));
    return Platform;
}

/* CLOCKS AND RESETS **********************************************************/

/**
 * @brief Makes sure a power domain is on, turning it on only when firmware left it collapsed.
 */
BOOLEAN
NTAPI
KdDwcPowerDomainOn(
    _In_ ULONG64 Gdscr)
{
    PUCHAR Register = KdDwcMapRegister(Gdscr);

    if (Register == NULL)
        return FALSE;

    KdDwcTrace("power domain 0x%I64x is 0x%lx\n", Gdscr, KdDwcRead(Register, 0));
    if (KdDwcRead(Register, 0) & GCC_GDSC_PWR_ON)
        return TRUE;

    KdDwcModify(Register, 0, GCC_GDSC_SW_COLLAPSE, 0);
    if (!KdDwcWaitFor(Register, 0, GCC_GDSC_PWR_ON, GCC_GDSC_PWR_ON, KDDWC_DOMAIN_TIMEOUT_US))
    {
        KdDwcTrace("power domain 0x%I64x stayed off: 0x%lx\n", Gdscr, KdDwcRead(Register, 0));
        return FALSE;
    }

    KdDwcTrace("power domain 0x%I64x now 0x%lx\n", Gdscr, KdDwcRead(Register, 0));
    return TRUE;
}

/**
 * @brief Makes sure a clock branch runs, enabling it only when it is off.
 */
BOOLEAN
NTAPI
KdDwcClockOn(
    _In_ ULONG64 Cbcr)
{
    PUCHAR Register = KdDwcMapRegister(Cbcr);

    if (Register == NULL)
        return FALSE;

    KdDwcTrace("clock 0x%I64x is 0x%lx\n", Cbcr, KdDwcRead(Register, 0));
    if (!(KdDwcRead(Register, 0) & GCC_CBCR_CLK_OFF))
        return TRUE;

    KdDwcModify(Register, 0, 0, GCC_CBCR_ENABLE);
    if (!KdDwcWaitFor(Register, 0, GCC_CBCR_CLK_OFF, 0, KDDWC_CLOCK_TIMEOUT_US))
    {
        KdDwcTrace("clock 0x%I64x stayed off\n", Cbcr);
        return FALSE;
    }

    KdDwcTrace("clock 0x%I64x now 0x%lx\n", Cbcr, KdDwcRead(Register, 0));
    return TRUE;
}

/**
 * @brief Reports whether a voted clock branch runs. Its enable bit is in a vote register, so it is not written.
 */
BOOLEAN
NTAPI
KdDwcVotedClockIsOn(
    _In_ ULONG64 Cbcr)
{
    PUCHAR Register = KdDwcMapRegister(Cbcr);

    if (Register == NULL)
        return FALSE;

    KdDwcTrace("voted clock 0x%I64x is 0x%lx\n", Cbcr, KdDwcRead(Register, 0));
    if (KdDwcRead(Register, 0) & GCC_CBCR_CLK_OFF)
    {
        KdDwcTrace("voted clock 0x%I64x is off\n", Cbcr);
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief Tells whether a power domain is on and its clock branches run, from the clock controller alone.
 *
 * @param[in] Gcc
 * Physical base of the clock controller.
 *
 * @param[in] PowerDomain
 * Offset of the domain's GDSC.
 *
 * @param[in] Branches
 * Offsets of the branches the controller needs.
 *
 * @param[in] Count
 * Number of branches.
 */
BOOLEAN
NTAPI
KdDwcClocksRunning(
    _In_ ULONG64 Gcc,
    _In_ ULONG PowerDomain,
    _In_reads_(Count) const ULONG* Branches,
    _In_ ULONG Count)
{
    PUCHAR Register = KdDwcMapRegister(Gcc + PowerDomain);
    ULONG i;

    if (Register == NULL || !(KdDwcRead(Register, 0) & GCC_GDSC_PWR_ON))
        return FALSE;

    for (i = 0; i < Count; i++)
    {
        Register = KdDwcMapRegister(Gcc + Branches[i]);
        if (Register == NULL || (KdDwcRead(Register, 0) & GCC_CBCR_CLK_OFF))
            return FALSE;
    }

    return TRUE;
}

VOID
NTAPI
KdDwcPulseReset(
    _In_ ULONG64 Bcr)
{
    PUCHAR Register = KdDwcMapRegister(Bcr);

    if (Register == NULL)
        return;

    KdDwcTrace("pulsing reset 0x%I64x\n", Bcr);
    KdDwcModify(Register, 0, 0, 1);
    KdDwcStall(150);
    KdDwcModify(Register, 0, 1, 0);
    KdDwcStall(150);
    KdDwcTrace("reset 0x%I64x released\n", Bcr);
}

/* QUSB2 PHY ******************************************************************/

BOOLEAN
NTAPI
KdDwcQusb2IsRunning(
    _In_ ULONG64 Phy)
{
    PUCHAR Base = KdDwcMapRegister(Phy);

    if (Base == NULL)
        return FALSE;

    return !(KdDwcRead(Base, QUSB2_POWERDOWN) & QUSB2_POWERDOWN_POWER_DOWN) &&
           (KdDwcRead(Base, QUSB2_PLL_STATUS) & QUSB2_PLL_STATUS_CORE_READY);
}

/**
 * @brief Resets and programs a QUSB2 PHY, then powers it up and checks its PLL.
 *
 * The PHY's supplies come from the PMIC and are left as firmware set them.
 *
 * @param[in] Phy
 * Physical base of the PHY.
 *
 * @param[in] PhyBcr
 * Its block reset in the clock controller.
 *
 * @param[in] PowerDownBits
 * The bits that hold it powered down and clamped while it is programmed.
 *
 * @param[in] Table
 * The setup writes, in order.
 *
 * @param[in] Count
 * Number of writes in the table.
 */
BOOLEAN
NTAPI
KdDwcStartQusb2(
    _In_ ULONG64 Phy,
    _In_ ULONG64 PhyBcr,
    _In_ UCHAR PowerDownBits,
    _In_reads_(Count) const KDDWC_REG_WRITE* Table,
    _In_ ULONG Count)
{
    PUCHAR Base = KdDwcMapRegister(Phy);
    ULONG i;

    if (Base == NULL)
        return FALSE;

    KdDwcPulseReset(PhyBcr);

    KdDwcTrace("reading QUSB2 0x%I64x at %p\n", Phy, Base);
    KdDwcTrace("QUSB2 powerdown 0x%lx\n", KdDwcRead(Base, QUSB2_POWERDOWN));
    KdDwcModify(Base, QUSB2_POWERDOWN, 0, PowerDownBits);
    for (i = 0; i < Count; i++)
        KdDwcWrite(Base, Table[i].Offset, Table[i].Value);

    KdDwcTrace("QUSB2 programmed, powering up\n");
    KdDwcModify(Base, QUSB2_POWERDOWN, QUSB2_POWERDOWN_POWER_DOWN, 0);
    KdDwcStall(260);
    KdDwcTrace("QUSB2 PLL status 0x%lx\n", KdDwcRead(Base, QUSB2_PLL_STATUS));

    if (!(KdDwcRead(Base, QUSB2_PLL_STATUS) & QUSB2_PLL_STATUS_CORE_READY))
    {
        KdDwcTrace("QUSB2 PLL did not lock: 0x%lx\n", KdDwcRead(Base, QUSB2_PLL_STATUS));
        return FALSE;
    }

    return TRUE;
}

/* SYNOPSYS HIGH SPEED PHY ****************************************************/

BOOLEAN
NTAPI
KdDwcSnpsHsIsRunning(
    _In_ ULONG64 Phy)
{
    PUCHAR Base = KdDwcMapRegister(Phy);

    if (Base == NULL)
        return FALSE;

    return !(KdDwcRead(Base, SNPS_COMMON0) & SNPS_COMMON0_SIDDQ) &&
           !(KdDwcRead(Base, SNPS_UTMI_CTRL5) & SNPS_UTMI_CTRL5_POR);
}

/**
 * @brief Resets the Synopsys high speed PHY and brings it out of power down.
 *
 * VBUS valid comes from the external select, which is what device mode needs
 * with nothing else reporting it. The PHY has no lock status to check.
 */
BOOLEAN
NTAPI
KdDwcStartSnpsHs(
    _In_ ULONG64 Phy,
    _In_ ULONG64 PhyBcr)
{
    PUCHAR Base = KdDwcMapRegister(Phy);

    if (Base == NULL)
        return FALSE;

    KdDwcPulseReset(PhyBcr);

    /* Hold the common block in power on reset while it is set up */
    KdDwcModify(Base, SNPS_CFG0, 0, SNPS_CFG0_CMN_CTRL_OVERRIDE);
    KdDwcModify(Base, SNPS_UTMI_CTRL5, 0, SNPS_UTMI_CTRL5_POR);
    KdDwcModify(Base, SNPS_COMMON0, SNPS_COMMON0_FSEL_MASK, 0);
    KdDwcModify(Base, SNPS_COMMON1, 0, SNPS_COMMON1_PLLBTUNE);
    KdDwcModify(Base, SNPS_REFCLK_CTRL, 0, SNPS_REFCLK_CTRL_SEL_HIGH);
    KdDwcModify(Base, SNPS_COMMON1, 0, SNPS_COMMON1_VBUSVLDEXTSEL0);
    KdDwcModify(Base, SNPS_CTRL1, 0, SNPS_CTRL1_VBUSVLDEXT0);
    KdDwcModify(Base, SNPS_COMMON2, 0, SNPS_COMMON2_VREGBYPASS);
    KdDwcModify(Base, SNPS_CTRL2, 0, SNPS_CTRL2_SUSPEND_N_SEL | SNPS_CTRL2_SUSPEND_N);
    KdDwcModify(Base, SNPS_UTMI_CTRL0, 0, SNPS_UTMI_CTRL0_SLEEPM);

    /* Power up, then let the UTMI side run it again */
    KdDwcModify(Base, SNPS_COMMON0, SNPS_COMMON0_SIDDQ, 0);
    KdDwcModify(Base, SNPS_UTMI_CTRL5, SNPS_UTMI_CTRL5_POR, 0);
    KdDwcModify(Base, SNPS_CTRL2, SNPS_CTRL2_SUSPEND_N_SEL, 0);
    KdDwcModify(Base, SNPS_CFG0, SNPS_CFG0_CMN_CTRL_OVERRIDE, 0);
    KdDwcStall(100);

    return TRUE;
}
