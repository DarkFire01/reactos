/*
 * PROJECT:     ReactOS Intel LPSS GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Controller lifetime and geometry
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The reference reaches these through a C++ chassis class with virtual
 * dispatch; there is one chassis for all four SoC variants Intel ships, so the
 * indirection buys nothing here and the calls are direct.
 */

#include "gpiopriv.h"

#include <debug.h>

/*
 * Skylake-LP's groups, decoded from the packed stores
 * _GPIO_CONTROLLER_SKL_LP::GpioLayout_Allocate makes into the bank table at
 * this+144 (:7015-7068). Each entry there is 32 bytes
 * carrying the pin count at +0, the community at +2, the HOSTSW_OWN index at
 * +4 and the pad offset at +6.
 *
 * The pad offsets corroborate the decode: they advance by exactly the previous
 * group's pin count times the eight-byte pad stride, and reset to zero at each
 * new community.
 *
 * The reference keeps one C++ layout class per part and picks between them on
 * the _HID the device enumerated with: a table of {id, index} pairs maps the
 * id to a layout index and a switch on that index constructs the matching
 * class. The tables below were decoded from those classes, and the parts from
 * Ice Lake on were taken from the newer iaLPSS2_GPIO2_WCL.sys, which carries
 * every layout through Meteor Lake.
 *
 * A part whose layout is not here must not be bound: community count, group
 * count and pad offsets all differ, so the wrong table writes the wrong pads.
 * Left out are the Broxton, Gemini Lake and Lakefield families, whose parts
 * expose one community per ACPI device rather than one per group.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksSkylakeLp[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          0,       0x000,      24    },   /* GPP_A */
    {     0,          1,       0x0C0,      24    },   /* GPP_B */
    {     1,          0,       0x000,      24    },   /* GPP_C */
    {     1,          1,       0x0C0,      24    },   /* GPP_D */
    {     1,          2,       0x180,      24    },   /* GPP_E */
    {     2,          0,       0x000,      24    },   /* GPP_F */
    {     2,          1,       0x0C0,       8    },   /* GPP_G */
};

/*
 * Skylake-H's groups (_GPIO_CONTROLLER_SKL_H::GpioLayout_Allocate, :7147).
 * Six groups share community 1, GPP_E is thirteen pads and GPD is eleven -
 * the same shape the public Sunrise Point-H pad map has, which is a decode
 * this table was checked against.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksSkylakeH[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          0,       0x000,      24    },   /* GPP_A */
    {     0,          1,       0x0C0,      24    },   /* GPP_B */
    {     1,          0,       0x000,      24    },   /* GPP_C */
    {     1,          1,       0x0C0,      24    },   /* GPP_D */
    {     1,          2,       0x180,      13    },   /* GPP_E */
    {     1,          3,       0x1E8,      24    },   /* GPP_F */
    {     1,          4,       0x2A8,      24    },   /* GPP_G */
    {     1,          5,       0x368,      24    },   /* GPP_H */
    {     2,          1,       0x000,      11    },   /* GPD   */
};

/*
 * Cannon Lake-LP's groups (_GPIO_CONTROLLER_CNL_LP::GpioLayout_Allocate,
 * :7345). This is the part every 10th-generation mobile machine enumerates as
 * (ACPI\INT34BB). Its first three groups are 25, 26 and 8 pads, which is the
 * public Cannon Lake-LP pad map exactly - the check this decode was held to.
 *
 * Note the pad stride: 25 pads are followed by a group at 0x190, so a pad here
 * is sixteen bytes, not the eight Skylake uses. Reading the offsets as a
 * Skylake table would address every group after the first at half its real
 * place.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksCannonLakeLp[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          0,       0x000,      25    },   /* GPP_A  */
    {     0,          1,       0x190,      26    },   /* GPP_B  */
    {     0,          2,       0x330,       8    },   /* GPP_G  */
    {     1,          0,       0x000,      25    },   /* GPP_D  */
    {     1,          1,       0x190,      24    },   /* GPP_F  */
    {     1,          2,       0x310,      24    },   /* GPP_H  */
    {     1,          3,       0x490,      32    },   /* vGPIO  */
    {     1,          4,       0x690,       8    },   /* vGPIO2 */
    {     2,          0,       0x000,      24    },   /* GPP_C  */
    {     2,          1,       0x180,      24    },   /* GPP_E  */
};

/*
 * Cannon Lake-H's groups (_GPIO_CONTROLLER_CNL_H::GpioLayout_Allocate, the
 * WCL binary's :9854 - the older CNL binary builds this one through statement
 * forms the decoder does not follow, so it is taken from the newer image that
 * carries the same class). Four communities, HOSTSW_OWN at 0xC0, and the same
 * sixteen-byte pad as its LP sibling.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksCannonLakeH[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          0,       0x000,      25    },
    {     0,          1,       0x190,      26    },
    {     1,          0,       0x000,      24    },
    {     1,          1,       0x180,      24    },
    {     1,          2,       0x300,       8    },
    {     1,          4,       0x400,      32    },
    {     2,          0,       0x000,      24    },
    {     2,          1,       0x180,      24    },
    {     2,          2,       0x300,      13    },
    {     2,          3,       0x3D0,      24    },
    {     3,          2,       0x140,      18    },
    {     3,          3,       0x260,      12    },
};

/*
 * Ice Lake-LP (ACPI\INT3455): 4 communities, HOSTSW_OWN at 0x0B0, 11 groups,
 * sixteen byte pads.
 *
 * Ice Lake-LP, the first part to split the pad map across four communities.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksIceLakeLp[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          0,       0x000,       8    },   /* GPP_G */
    {     0,          1,       0x080,      26    },   /* GPP_B */
    {     0,          2,       0x220,      25    },   /* GPP_A */
    {     1,          0,       0x000,      24    },   /* GPP_H */
    {     1,          1,       0x180,      21    },   /* GPP_D */
    {     1,          2,       0x2D0,      20    },   /* GPP_F */
    {     1,          3,       0x410,      29    },   /* GPP_V */
    {     2,          0,       0x000,      24    },   /* GPP_C */
    {     2,          2,       0x1E0,      24    },   /* GPP_E */
    {     3,          0,       0x000,       8    },   /* GPP_R */
    {     3,          1,       0x080,       8    },   /* GPP_S */
};

/*
 * Ice Lake-N (ACPI\INT34C3): 4 communities, HOSTSW_OWN at 0x0B0, 11 groups,
 * sixteen byte pads.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksIceLakeN[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          1,       0x090,      26    },   /* GPP_B */
    {     0,          2,       0x230,      21    },   /* GPP_A */
    {     0,          3,       0x380,       8    },   /* GPP_S */
    {     0,          4,       0x400,       8    },   /* GPP_R */
    {     1,          0,       0x000,      24    },   /* GPP_H */
    {     1,          1,       0x180,      26    },   /* GPP_D */
    {     1,          2,       0x320,      29    },   /* GPP_V0 */
    {     1,          3,       0x4F0,      24    },   /* GPP_C */
    {     2,          1,       0x060,      24    },   /* GPP_E */
    {     2,          3,       0x270,       4    },   /* GPP_V4 */
    {     3,          0,       0x000,       8    },   /* GPP_G */
};

/*
 * Ice Lake-H (ACPI\INT3456): 5 communities, HOSTSW_OWN at 0x0C0, 14 groups,
 * sixteen byte pads.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksIceLakeH[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          1,       0x090,      25    },   /* GPP_A */
    {     0,          2,       0x220,      26    },   /* GPP_B */
    {     1,          0,       0x000,      24    },   /* GPP_C */
    {     1,          1,       0x180,      18    },   /* GPP_D */
    {     1,          2,       0x2A0,       8    },   /* GPP_R */
    {     1,          3,       0x320,       8    },   /* GPP_S */
    {     1,          4,       0x3A0,      16    },   /* GPP_G */
    {     1,          5,       0x4A0,      29    },   /* GPP_V */
    {     2,          0,       0x000,      13    },   /* GPP_E */
    {     2,          1,       0x0D0,      24    },   /* GPP_F */
    {     3,          0,       0x000,      24    },   /* GPP_H */
    {     3,          1,       0x180,      12    },   /* GPP_K */
    {     3,          2,       0x240,      10    },   /* GPP_J */
    {     4,          0,       0x000,      18    },   /* GPP_I */
};

/*
 * Tiger Lake-LP (ACPI\INT34C5): 4 communities, HOSTSW_OWN at 0x0B0, 12 groups,
 * sixteen byte pads.
 *
 * Tiger Lake-LP. Alder Lake-P and Alder Lake-N keep this shape and change
 * only a pin count or a group, so the three tables read almost alike.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksTigerLakeLp[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          0,       0x000,      26    },   /* GPP_B */
    {     0,          1,       0x1A0,      16    },   /* GPP_T */
    {     0,          2,       0x2A0,      25    },   /* GPP_A */
    {     1,          0,       0x000,       8    },   /* GPP_S */
    {     1,          1,       0x080,      24    },   /* GPP_H */
    {     1,          2,       0x200,      21    },   /* GPP_D */
    {     1,          3,       0x350,      24    },   /* GPP_U */
    {     1,          4,       0x4D0,      27    },   /* GPP_V */
    {     2,          0,       0x000,      24    },   /* GPP_C */
    {     2,          1,       0x180,      25    },   /* GPP_F */
    {     2,          3,       0x370,      25    },   /* GPP_E */
    {     3,          0,       0x000,       8    },   /* GPP_R */
};

/*
 * Tiger Lake-H (ACPI\INT34C6): 5 communities, HOSTSW_OWN at 0x0C0, 15 groups,
 * sixteen byte pads.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksTigerLakeH[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          0,       0x080,      15    },   /* GPP_A */
    {     0,          1,       0x190,      20    },   /* GPP_R */
    {     0,          2,       0x2D0,      26    },   /* GPP_B */
    {     0,          3,       0x470,       8    },   /* GPP_v0 */
    {     1,          0,       0x000,      26    },   /* GPP_D */
    {     1,          1,       0x1A0,      24    },   /* GPP_C */
    {     1,          2,       0x320,       8    },   /* GPP_S */
    {     1,          3,       0x3A0,      17    },   /* GPP_G */
    {     1,          4,       0x4B0,      27    },   /* GPP_V */
    {     2,          0,       0x000,      13    },   /* GPP_E */
    {     2,          1,       0x0D0,      24    },   /* GPP_F */
    {     3,          0,       0x000,      24    },   /* GPP_H */
    {     3,          1,       0x180,      10    },   /* GPP_J */
    {     3,          2,       0x220,      15    },   /* GPP_k */
    {     4,          0,       0x000,      15    },   /* GPP_I */
};

/*
 * Alder Lake-P (ACPI\INTC1055): 4 communities, HOSTSW_OWN at 0x0B0, 12 groups,
 * sixteen byte pads.
 *
 * 12th generation mobile, and the id 13th generation Raptor Lake-P and
 * Raptor Lake-H enumerate as well - they are the same silicon here.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksAlderLakeP[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          0,       0x000,      26    },   /* GPP_B */
    {     0,          1,       0x1A0,      16    },   /* GPP_T */
    {     0,          2,       0x2A0,      25    },   /* GPP_A */
    {     1,          0,       0x000,       8    },   /* GPP_S */
    {     1,          1,       0x080,      24    },   /* GPP_H */
    {     1,          2,       0x200,      21    },   /* GPP_D */
    {     1,          3,       0x350,      24    },   /* GPP_U */
    {     1,          4,       0x4D0,      29    },   /* GPP_V */
    {     2,          0,       0x000,      24    },   /* GPP_C */
    {     2,          1,       0x180,      25    },   /* GPP_F */
    {     2,          3,       0x370,      25    },   /* GPP_E */
    {     3,          0,       0x000,       8    },   /* GPP_R */
};

/*
 * Alder Lake-N (ACPI\INTC1057): 4 communities, HOSTSW_OWN at 0x0B0, 12 groups,
 * sixteen byte pads.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksAlderLakeN[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          0,       0x000,      26    },   /* GPP_B */
    {     0,          1,       0x1A0,      16    },   /* GPP_T */
    {     0,          2,       0x2A0,      25    },   /* GPP_A */
    {     1,          0,       0x000,       8    },   /* GPP_S */
    {     1,          1,       0x080,      20    },   /* GPP_I */
    {     1,          2,       0x1C0,      24    },   /* GPP_H */
    {     1,          3,       0x340,      21    },   /* GPP_D */
    {     1,          4,       0x490,      29    },   /* GPP_V */
    {     2,          0,       0x000,      24    },   /* GPP_C */
    {     2,          1,       0x180,      25    },   /* GPP_F */
    {     2,          3,       0x370,      25    },   /* GPP_E */
    {     3,          0,       0x000,       8    },   /* GPP_R */
};

/*
 * Alder Lake-S (ACPI\INTC1085): 5 communities, HOSTSW_OWN at 0x150, 15 groups,
 * sixteen byte pads.
 *
 * 12th generation desktop, shared with 13th generation Raptor Lake-S
 * (ACPI\INTC1056), which binds to the same layout in the reference.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksAlderLakeS[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          0,       0x000,      25    },   /* GPP_I */
    {     0,          1,       0x190,      23    },   /* GPP_R */
    {     0,          2,       0x300,      12    },   /* GPP_J */
    {     0,          3,       0x3C0,      27    },   /* GPP_V */
    {     0,          4,       0x570,       8    },   /* GPP_V_USB */
    {     1,          0,       0x000,      24    },   /* GPP_B */
    {     1,          1,       0x180,       8    },   /* GPP_G */
    {     1,          2,       0x200,      24    },   /* GPP_H */
    {     2,          1,       0x090,      16    },   /* GPP_A */
    {     2,          2,       0x190,      24    },   /* GPP_C */
    {     3,          0,       0x000,       8    },   /* GPP_S */
    {     3,          1,       0x080,      23    },   /* GPP_E */
    {     3,          2,       0x1F0,      15    },   /* GPP_K */
    {     3,          3,       0x2E0,      24    },   /* GPP_F */
    {     4,          0,       0x000,      25    },   /* GPP_D */
};

/*
 * Meteor Lake-M (ACPI\INTC1082): 3 communities, HOSTSW_OWN at 0x110, 8 groups,
 * sixteen byte pads.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksMeteorLakeM[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          0,       0x000,      28    },   /* GPP_A */
    {     0,          1,       0x1C0,      19    },   /* vGPIO */
    {     0,          2,       0x2F0,      27    },   /* GPP_C */
    {     1,          0,       0x000,      20    },   /* GPP_B */
    {     1,          1,       0x140,       2    },   /* vGPIO3 */
    {     1,          2,       0x160,      24    },   /* GPP_D */
    {     2,          0,       0x000,      16    },   /* JTAG */
    {     2,          1,       0x100,      12    },   /* vGPIO4 */
};

/*
 * Meteor Lake-P (ACPI\INTC1083): 5 communities, HOSTSW_OWN at 0x140, 15 groups,
 * sixteen byte pads.
 *
 * Meteor Lake-P. Its community 0 opens with a five pad group the reference
 * names CPU, which is why the first pad offset here is not zero.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksMeteorLakeP[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          0,       0x000,       5    },   /* CPU */
    {     0,          1,       0x050,      24    },   /* GPP_V */
    {     0,          2,       0x1D0,      24    },   /* GPP_C */
    {     1,          0,       0x000,      25    },   /* GPP_A */
    {     1,          1,       0x190,      25    },   /* GPP_E */
    {     2,          0,       0x000,      26    },   /* GPP_H */
    {     2,          1,       0x1A0,      26    },   /* GPP_F */
    {     2,          2,       0x340,      15    },   /* SPI0 */
    {     2,          3,       0x430,      18    },   /* GPP_V3 */
    {     3,          0,       0x000,       8    },   /* GPP_S */
    {     3,          1,       0x080,      12    },   /* JTAG */
    {     4,          0,       0x000,      25    },   /* GPP_B */
    {     4,          1,       0x190,      25    },   /* GPP_D */
    {     4,          2,       0x320,      32    },   /* vGPIO0 */
    {     4,          3,       0x520,       3    },   /* vGPIO1 */
};

/*
 * Meteor Lake-S (ACPI\INTC1084): 5 communities, HOSTSW_OWN at 0x150, 19 groups,
 * sixteen byte pads.
 */
static const GPIO_BANK_DESCRIPTOR GpioBanksMeteorLakeS[] =
{
    /* Community  HostSwOwn  PadOffset  PinCount */
    {     0,          0,       0x000,      25    },   /* GPP_D */
    {     0,          1,       0x190,      14    },   /* GPP_R */
    {     0,          2,       0x270,      18    },   /* GPP_J */
    {     0,          3,       0x390,      31    },   /* vGPIO */
    {     1,          0,       0x000,      15    },   /* GPP_A */
    {     1,          1,       0x0F0,      12    },   /* ESPI */
    {     1,          2,       0x1B0,      22    },   /* GPP_B */
    {     2,          0,       0x000,       9    },   /* SPI0 */
    {     2,          1,       0x090,      24    },   /* GPP_C */
    {     2,          2,       0x210,      20    },   /* GPP_H */
    {     2,          3,       0x350,       4    },   /* vGPIO3 */
    {     2,          4,       0x390,       8    },   /* vGPIO0 */
    {     2,          5,       0x410,      31    },   /* vGPIO4 */
    {     3,          0,       0x000,       8    },   /* GPP_S */
    {     3,          1,       0x080,      23    },   /* GPP_E */
    {     3,          2,       0x1F0,      14    },   /* GPP_K */
    {     3,          3,       0x2D0,      24    },   /* GPP_F */
    {     4,          0,       0x000,      21    },   /* GPP_I */
    {     4,          1,       0x150,      16    },   /* JTAG */
};

/*
 * The parts this driver has a geometry for, keyed by the id PnP hands it.
 * Adding one means adding its table above: the community count, the group
 * count and every pad offset differ per part, so a missing entry has to stay
 * missing rather than borrow a neighbour's numbers.
 */
static const GPIO_LAYOUT GpioLayouts[] =
{
    { L"ACPI\\INT344B",  "Skylake-LP",     GpioBanksSkylakeLp,
      RTL_NUMBER_OF(GpioBanksSkylakeLp),     3, GPIO_COMMUNITY_HOSTSW_OWN_SKL,
      GPIO_PAD_STRIDE_2REG },
    { L"ACPI\\INT345D",  "Skylake-H",      GpioBanksSkylakeH,
      RTL_NUMBER_OF(GpioBanksSkylakeH),      3, GPIO_COMMUNITY_HOSTSW_OWN_SKL,
      GPIO_PAD_STRIDE_2REG },
    { L"ACPI\\INT3451",  "Skylake-H",      GpioBanksSkylakeH,
      RTL_NUMBER_OF(GpioBanksSkylakeH),      3, GPIO_COMMUNITY_HOSTSW_OWN_SKL,
      GPIO_PAD_STRIDE_2REG },
    { L"ACPI\\INT34BB",  "Cannon Lake-LP", GpioBanksCannonLakeLp,
      RTL_NUMBER_OF(GpioBanksCannonLakeLp),  3, GPIO_COMMUNITY_HOSTSW_OWN_CNL,
      GPIO_PAD_STRIDE_4REG },
    { L"ACPI\\INT3450",  "Cannon Lake-H",  GpioBanksCannonLakeH,
      RTL_NUMBER_OF(GpioBanksCannonLakeH),   4, GPIO_COMMUNITY_HOSTSW_OWN_CNL_H,
      GPIO_PAD_STRIDE_4REG },
    { L"ACPI\\INT3455",  "Ice Lake-LP",    GpioBanksIceLakeLp,
      RTL_NUMBER_OF(GpioBanksIceLakeLp),     4, GPIO_COMMUNITY_HOSTSW_OWN_CNL,
      GPIO_PAD_STRIDE_4REG },
    { L"ACPI\\INT34C3",  "Ice Lake-N",     GpioBanksIceLakeN,
      RTL_NUMBER_OF(GpioBanksIceLakeN),      4, GPIO_COMMUNITY_HOSTSW_OWN_CNL,
      GPIO_PAD_STRIDE_4REG },
    { L"ACPI\\INT3456",  "Ice Lake-H",     GpioBanksIceLakeH,
      RTL_NUMBER_OF(GpioBanksIceLakeH),      5, GPIO_COMMUNITY_HOSTSW_OWN_CNL_H,
      GPIO_PAD_STRIDE_4REG },
    { L"ACPI\\INT34C5",  "Tiger Lake-LP",  GpioBanksTigerLakeLp,
      RTL_NUMBER_OF(GpioBanksTigerLakeLp),   4, GPIO_COMMUNITY_HOSTSW_OWN_CNL,
      GPIO_PAD_STRIDE_4REG },
    { L"ACPI\\INT34C6",  "Tiger Lake-H",   GpioBanksTigerLakeH,
      RTL_NUMBER_OF(GpioBanksTigerLakeH),    5, GPIO_COMMUNITY_HOSTSW_OWN_CNL_H,
      GPIO_PAD_STRIDE_4REG },
    { L"ACPI\\INTC1055", "Alder Lake-P",   GpioBanksAlderLakeP,
      RTL_NUMBER_OF(GpioBanksAlderLakeP),    4, GPIO_COMMUNITY_HOSTSW_OWN_CNL,
      GPIO_PAD_STRIDE_4REG },
    { L"ACPI\\INTC1057", "Alder Lake-N",   GpioBanksAlderLakeN,
      RTL_NUMBER_OF(GpioBanksAlderLakeN),    4, GPIO_COMMUNITY_HOSTSW_OWN_CNL,
      GPIO_PAD_STRIDE_4REG },
    { L"ACPI\\INTC1085", "Alder Lake-S",   GpioBanksAlderLakeS,
      RTL_NUMBER_OF(GpioBanksAlderLakeS),    5, GPIO_COMMUNITY_HOSTSW_OWN_ADL_S,
      GPIO_PAD_STRIDE_4REG },
    { L"ACPI\\INTC1056", "Raptor Lake-S",  GpioBanksAlderLakeS,
      RTL_NUMBER_OF(GpioBanksAlderLakeS),    5, GPIO_COMMUNITY_HOSTSW_OWN_ADL_S,
      GPIO_PAD_STRIDE_4REG },
    { L"ACPI\\INTC1082", "Meteor Lake-M",  GpioBanksMeteorLakeM,
      RTL_NUMBER_OF(GpioBanksMeteorLakeM),   3, GPIO_COMMUNITY_HOSTSW_OWN_MTL_M,
      GPIO_PAD_STRIDE_4REG },
    { L"ACPI\\INTC1083", "Meteor Lake-P",  GpioBanksMeteorLakeP,
      RTL_NUMBER_OF(GpioBanksMeteorLakeP),   5, GPIO_COMMUNITY_HOSTSW_OWN_MTL_P,
      GPIO_PAD_STRIDE_4REG },
    { L"ACPI\\INTC105E", "Meteor Lake-P",  GpioBanksMeteorLakeP,
      RTL_NUMBER_OF(GpioBanksMeteorLakeP),   5, GPIO_COMMUNITY_HOSTSW_OWN_MTL_P,
      GPIO_PAD_STRIDE_4REG },
    { L"ACPI\\INTC1084", "Meteor Lake-S",  GpioBanksMeteorLakeS,
      RTL_NUMBER_OF(GpioBanksMeteorLakeS),   5, GPIO_COMMUNITY_HOSTSW_OWN_ADL_S,
      GPIO_PAD_STRIDE_4REG },
};

/**
 * @brief
 * Finds the pad geometry for the part this device enumerated as.
 *
 * @param[in] Device
 * The controller device.
 *
 * @return
 * Its layout, or NULL when the id is one this driver has no table for.
 */
static const GPIO_LAYOUT *
GpioFindLayout(
    _In_ WDFDEVICE Device)
{
    const GPIO_LAYOUT *Layout = NULL;
    PDEVICE_OBJECT Pdo;
    PWSTR Ids = NULL;
    PWSTR Id;
    ULONG Length = 0;
    ULONG Index;
    NTSTATUS Status;

    PAGED_CODE();

    Pdo = WdfDeviceWdmGetPhysicalDevice(Device);
    Status = IoGetDeviceProperty(Pdo, DevicePropertyHardwareID, 0, NULL, &Length);
    if (Status != STATUS_BUFFER_TOO_SMALL || Length == 0)
    {
        DPRINT1("GPIO: no hardware id (0x%08lx)\n", Status);
        return NULL;
    }

    Ids = ExAllocatePoolWithTag(PagedPool, Length, GPIO_POOL_TAG);
    if (Ids == NULL)
    {
        return NULL;
    }

    Status = IoGetDeviceProperty(Pdo, DevicePropertyHardwareID, Length, Ids, &Length);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("GPIO: could not read the hardware id (0x%08lx)\n", Status);
        ExFreePoolWithTag(Ids, GPIO_POOL_TAG);
        return NULL;
    }

    /* A hardware id is a multi-string; the most specific one comes first */
    for (Id = Ids; *Id != UNICODE_NULL; Id += wcslen(Id) + 1)
    {
        for (Index = 0; Index < RTL_NUMBER_OF(GpioLayouts); Index++)
        {
            if (_wcsicmp(Id, GpioLayouts[Index].HardwareId) == 0)
            {
                Layout = &GpioLayouts[Index];
                DPRINT1("GPIO: %S is %s, %lu group(s) over %lu communities\n",
                        Id, Layout->Name, Layout->BankCount, Layout->CommunityCount);
                goto Done;
            }
        }
        DPRINT1("GPIO: no pad geometry for %S\n", Id);
    }

Done:
    ExFreePoolWithTag(Ids, GPIO_POOL_TAG);
    return Layout;
}


/**
 * @brief
 * Claims the controller's register windows.
 *
 * Each community is a memory resource of its own rather than an offset into
 * one window, so the translated list is walked and every memory descriptor
 * mapped in turn (:4890 does one MmMapIoSpaceEx per descriptor). Anything else
 * in the list belongs to the interrupt, which GpioClx connects.
 *
 * @param[in] Device
 * The controller device.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] ResourcesRaw
 * The raw resource list. Unused; the windows come from the translated one.
 *
 * @param[in] ResourcesTranslated
 * The translated resource list.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_DEVICE_CONFIGURATION_ERROR when the firmware
 * described fewer windows than this part has communities.
 */
NTSTATUS
NTAPI
GpioPrepareController(
    _In_ WDFDEVICE Device,
    _In_ PVOID Context,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Descriptor;
    const GPIO_LAYOUT *Layout;
    ULONG Index;
    ULONG Count;
    ULONG Length;
    ULONG Mapped = 0;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    RtlZeroMemory(Controller, sizeof(*Controller));
    Controller->Device = Device;

    Layout = GpioFindLayout(Device);
    if (Layout == NULL)
    {
        /*
         * Refusing here is the point. Driving a part with another part's pad
         * offsets writes real pads at the wrong addresses, so a geometry this
         * driver does not have has to fail the start rather than be guessed at.
         */
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Controller->Layout = Layout;
    Controller->Banks = Layout->Banks;
    Controller->BankCount = Layout->BankCount;

    Count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (Index = 0; Index < Count && Mapped < Layout->CommunityCount; Index++)
    {
        Descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, Index);
        if (Descriptor == NULL || Descriptor->Type != CmResourceTypeMemory)
        {
            continue;
        }

        Controller->CommunityBase[Mapped] =
            (PUCHAR)MmMapIoSpace(Descriptor->u.Memory.Start,
                                 Descriptor->u.Memory.Length,
                                 MmNonCached);
        if (Controller->CommunityBase[Mapped] == NULL)
        {
            GpioReleaseController(Device, Context);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        Controller->CommunityLength[Mapped] = Descriptor->u.Memory.Length;
        Mapped++;
    }

    Controller->CommunityCount = Mapped;

    if (Mapped < Layout->CommunityCount)
    {
        GpioReleaseController(Device, Context);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /*
     * The pin total counts whole banks plus whatever the last one is short by,
     * which is how the reference works it out at :7094.
     */
    Controller->PinsPerBank = GPIO_PINS_PER_BANK;
    Controller->TotalPins = (USHORT)(GPIO_PINS_PER_BANK * (Controller->BankCount - 1) +
                                     Controller->Banks[Controller->BankCount - 1].PinCount);

    /*
     * One pad table per group, sized to that group rather than to the widest
     * one, which is what the reference allocates at :7073.
     */
    for (Index = 0; Index < Controller->BankCount; Index++)
    {
        Length = Controller->Banks[Index].PinCount * sizeof(GPIO_PIN_STATE);

        Controller->BankState[Index].Pins =
            ExAllocatePoolWithTag(NonPagedPool, Length, GPIO_POOL_TAG);
        if (Controller->BankState[Index].Pins == NULL)
        {
            GpioReleaseController(Device, Context);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(Controller->BankState[Index].Pins, Length);
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Gives the register windows back.
 *
 * @param[in] Device
 * The controller device.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
GpioReleaseController(
    _In_ WDFDEVICE Device,
    _In_ PVOID Context)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;
    ULONG Index;

    UNREFERENCED_PARAMETER(Device);

    for (Index = 0; Index < RTL_NUMBER_OF(Controller->CommunityBase); Index++)
    {
        if (Controller->CommunityBase[Index] != NULL)
        {
            MmUnmapIoSpace(Controller->CommunityBase[Index],
                           Controller->CommunityLength[Index]);
            Controller->CommunityBase[Index] = NULL;
            Controller->CommunityLength[Index] = 0;
        }
    }

    Controller->CommunityCount = 0;

    for (Index = 0; Index < RTL_NUMBER_OF(Controller->BankState); Index++)
    {
        if (Controller->BankState[Index].Pins != NULL)
        {
            ExFreePoolWithTag(Controller->BankState[Index].Pins, GPIO_POOL_TAG);
            Controller->BankState[Index].Pins = NULL;
        }
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Puts one group back the way a cold start wants it.
 *
 * Nothing is armed and nothing has fired: GPI_IE goes to zero and every status
 * bit is acknowledged, which is what the reference does on the branch it takes
 * when there is no saved context to restore (:5813, :5930).
 *
 * @param[in] Controller
 * The controller.
 *
 * @param[in] BankId
 * The group.
 */
static
VOID
GpioResetBankInterrupts(
    _In_ PGPIO_CONTROLLER Controller,
    _In_ BANK_ID BankId)
{
    PULONG Register;

    Register = GpioGroupRegister(Controller, BankId, GPIO_COMMUNITY_GPI_ENABLE);
    if (Register != NULL)
    {
        WRITE_REGISTER_ULONG(Register, 0);
    }

    Register = GpioGroupRegister(Controller, BankId, GPIO_COMMUNITY_GPI_STATUS);
    if (Register != NULL)
    {
        WRITE_REGISTER_ULONG(Register, MAXULONG);
    }
}

/**
 * @brief
 * Remembers one group's configuration before the power goes away.
 *
 * Only pads that are in use and still in GPIO mode are worth saving: a pad the
 * firmware has since muxed to a native function is not this driver's to put
 * back, which is the test the reference makes on PADCFG0 at :5661.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] SaveParameters
 * The group to save.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER for a group that does not exist.
 */
NTSTATUS
NTAPI
GpioSaveBankHardwareContext(
    _In_ PVOID Context,
    _In_ PGPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS SaveParameters)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;
    PGPIO_BANK_STATE Bank;
    PGPIO_PIN_STATE Pin;
    PULONG Register;
    PIN_NUMBER PinNumber;
    ULONG Value;

    if (SaveParameters->BankId >= Controller->BankCount)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Bank = &Controller->BankState[SaveParameters->BankId];

    for (PinNumber = 0;
         PinNumber < Controller->Banks[SaveParameters->BankId].PinCount;
         PinNumber++)
    {
        Pin = &Bank->Pins[PinNumber];
        if (!Pin->InUse)
        {
            continue;
        }

        Register = GpioPadAddress(Controller, SaveParameters->BankId, PinNumber);
        if (Register == NULL)
        {
            continue;
        }

        Value = READ_REGISTER_ULONG(Register);
        if ((Value & PADCFG0_PMODE_MASK) != 0)
        {
            continue;
        }

        Pin->SavedConfig0 = Value;
        Pin->SavedConfig1 = READ_REGISTER_ULONG(Register + 1);
        Pin->ContextSaved = TRUE;
    }

    /*
     * One GPI_IE word covers the whole group, so what is armed is recorded
     * once rather than per pad.
     */
    Register = GpioGroupRegister(Controller, SaveParameters->BankId,
                                 GPIO_COMMUNITY_GPI_ENABLE);
    if (Register != NULL)
    {
        Bank->SavedInterruptEnable = READ_REGISTER_ULONG(Register);
    }

    Bank->ContextSaved = TRUE;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Puts back what the matching save recorded.
 *
 * PADCFG1 is written before PADCFG0 because the second word carries the pull
 * and the routing while the first carries the mode, so writing the mode last
 * means the pad never spends a moment in GPIO mode with the wrong termination.
 * The reference orders the two writes the same way at :5560.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] RestoreParameters
 * The group to restore.
 *
 * @return
 * STATUS_SUCCESS, or STATUS_INVALID_PARAMETER for a group that does not exist.
 */
NTSTATUS
NTAPI
GpioRestoreBankHardwareContext(
    _In_ PVOID Context,
    _In_ PGPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS RestoreParameters)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;
    PGPIO_BANK_STATE Bank;
    PGPIO_PIN_STATE Pin;
    PULONG Register;
    PIN_NUMBER PinNumber;

    if (RestoreParameters->BankId >= Controller->BankCount)
    {
        return STATUS_INVALID_PARAMETER;
    }

    Bank = &Controller->BankState[RestoreParameters->BankId];

    /* Nothing was saved, so there is nothing to put back */
    if (!Bank->ContextSaved)
    {
        return STATUS_SUCCESS;
    }

    for (PinNumber = 0;
         PinNumber < Controller->Banks[RestoreParameters->BankId].PinCount;
         PinNumber++)
    {
        Pin = &Bank->Pins[PinNumber];
        if (!Pin->ContextSaved)
        {
            continue;
        }

        Register = GpioPadAddress(Controller, RestoreParameters->BankId, PinNumber);
        if (Register == NULL)
        {
            continue;
        }

        WRITE_REGISTER_ULONG(Register + 1, Pin->SavedConfig1);
        WRITE_REGISTER_ULONG(Register, Pin->SavedConfig0);

        Pin->ContextSaved = FALSE;
    }

    /*
     * The armed mask goes back last, so no pad is armed before it has been
     * told again what counts as an event.
     */
    Register = GpioGroupRegister(Controller, RestoreParameters->BankId,
                                 GPIO_COMMUNITY_GPI_ENABLE);
    if (Register != NULL)
    {
        WRITE_REGISTER_ULONG(Register, Bank->SavedInterruptEnable);
    }

    Bank->ContextSaved = FALSE;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Brings the controller up as the device enters D0.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] RestoreContext
 * TRUE when the registers were saved on the way down and should go back.
 *
 * @param[in] PreviousPowerState
 * The state being left.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
GpioStartController(
    _In_ PVOID Context,
    _In_ BOOLEAN RestoreContext,
    _In_ WDF_POWER_DEVICE_STATE PreviousPowerState)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;
    GPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS Parameters;
    BANK_ID BankId;
    ULONG PinNumber;

    UNREFERENCED_PARAMETER(PreviousPowerState);

    /*
     * The reference drives the whole controller a group at a time from here
     * rather than leaving GpioClx to call the per-bank routines itself
     * (:5786), so both paths are the same loop with a different body.
     */
    for (BankId = 0; BankId < (BANK_ID)Controller->BankCount; BankId++)
    {
        if (RestoreContext)
        {
            RtlZeroMemory(&Parameters, sizeof(Parameters));
            Parameters.BankId = BankId;

            GpioRestoreBankHardwareContext(Context, &Parameters);
            continue;
        }

        GpioResetBankInterrupts(Controller, BankId);

        /* Nothing is connected yet, so no pad state survives a cold start */
        for (PinNumber = 0;
             PinNumber < Controller->Banks[BankId].PinCount;
             PinNumber++)
        {
            RtlZeroMemory(&Controller->BankState[BankId].Pins[PinNumber],
                          sizeof(GPIO_PIN_STATE));
        }

        Controller->BankState[BankId].ContextSaved = FALSE;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Takes the controller down as the device leaves D0.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[in] SaveContext
 * TRUE when the registers have to survive the transition.
 *
 * @param[in] TargetPowerState
 * The state being entered.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
GpioStopController(
    _In_ PVOID Context,
    _In_ BOOLEAN SaveContext,
    _In_ WDF_POWER_DEVICE_STATE TargetPowerState)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;
    GPIO_SAVE_RESTORE_BANK_HARDWARE_CONTEXT_PARAMETERS Parameters;
    BANK_ID BankId;

    UNREFERENCED_PARAMETER(TargetPowerState);

    for (BankId = 0; BankId < (BANK_ID)Controller->BankCount; BankId++)
    {
        if (SaveContext)
        {
            RtlZeroMemory(&Parameters, sizeof(Parameters));
            Parameters.BankId = BankId;

            GpioSaveBankHardwareContext(Context, &Parameters);
        }
        else
        {
            /*
             * Nothing is coming back, so the group is left quiet rather than
             * recorded: disarming it stops a pad asserting the shared line
             * after the driver has stopped listening.
             */
            GpioResetBankInterrupts(Controller, BankId);
        }
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Reports the controller's geometry to GpioClx.
 *
 * @param[in] Context
 * This driver's controller storage.
 *
 * @param[out] ControllerInformation
 * Receives what GpioClx needs to address the hardware.
 *
 * @return
 * STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
GpioQueryControllerBasicInformation(
    _In_ PVOID Context,
    _Out_ PCLIENT_CONTROLLER_BASIC_INFORMATION ControllerInformation)
{
    PGPIO_CONTROLLER Controller = (PGPIO_CONTROLLER)Context;

    ControllerInformation->Version = 1;
    ControllerInformation->Size = sizeof(CLIENT_CONTROLLER_BASIC_INFORMATION);
    ControllerInformation->TotalPins = Controller->TotalPins;
    ControllerInformation->NumberOfPinsPerBank = Controller->PinsPerBank;

    /*
     * The reference sets exactly bits 0, 5 and 6 and clears 1 through 4
     * (:5105, "& 0xFFFFFFE1 | 0x61"): the registers are memory mapped, the
     * controller has an interrupt line of its own, and debouncing is emulated
     * because the pad hardware does not do it.
     */
    ControllerInformation->Flags.AsULONG = 0;
    ControllerInformation->Flags.MemoryMappedController = 1;
    ControllerInformation->Flags.DeviceInterruptSupported = 1;
    ControllerInformation->Flags.EmulateDebouncing = 1;

    return STATUS_SUCCESS;
}
