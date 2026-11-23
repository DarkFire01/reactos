/*
 * PROJECT:     ReactOS Intel LPSS GPIO controller driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Pad group geometry of the supported SoCs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "ialpss2i_gpio.h"

#define NDEBUG
#include <debug.h>

/*
 * Groups are listed in GpioClx bank order. Firmware numbers GpioInt pins as
 * bank * PinsPerBank + pad, so the order is part of the ACPI contract.
 */

/* Community, slot in the per group arrays, pad offset, pad count */
static const IGPIO_GROUP IGpioSunrisePointLp[] =
{
    { 0, 0, 0x000, 24 }, { 0, 1, 0x0C0, 24 },
    { 1, 0, 0x000, 24 }, { 1, 1, 0x0C0, 24 }, { 1, 2, 0x180, 24 },
    { 2, 0, 0x000, 24 }, { 2, 1, 0x0C0,  8 },
};

static const IGPIO_GROUP IGpioSunrisePointH[] =
{
    { 0, 0, 0x000, 24 }, { 0, 1, 0x0C0, 24 },
    { 1, 0, 0x000, 24 }, { 1, 1, 0x0C0, 24 }, { 1, 2, 0x180, 13 },
    { 1, 3, 0x1E8, 24 }, { 1, 4, 0x2A8, 24 }, { 1, 5, 0x368, 24 },
    { 2, 1, 0x000, 11 },
};

static const IGPIO_GROUP IGpioCannonLakeLp[] =
{
    { 0, 0, 0x000, 25 }, { 0, 1, 0x190, 26 }, { 0, 2, 0x330,  8 },
    { 1, 0, 0x000, 25 }, { 1, 1, 0x190, 24 }, { 1, 2, 0x310, 24 },
    { 1, 3, 0x490, 32 }, { 1, 4, 0x690,  8 },
    { 2, 0, 0x000, 24 }, { 2, 1, 0x180, 24 },
};

static const IGPIO_GROUP IGpioCannonLakeH[] =
{
    { 0, 0, 0x000, 25 }, { 0, 1, 0x190, 26 },
    { 1, 0, 0x000, 24 }, { 1, 1, 0x180, 24 }, { 1, 2, 0x300,  8 }, { 1, 4, 0x400, 32 },
    { 2, 0, 0x000, 24 }, { 2, 1, 0x180, 24 }, { 2, 2, 0x300, 13 }, { 2, 3, 0x3D0, 24 },
    { 3, 2, 0x140, 18 }, { 3, 3, 0x260, 12 },
};

static const IGPIO_GROUP IGpioIceLakeLp[] =
{
    { 0, 0, 0x000,  8 }, { 0, 1, 0x080, 26 }, { 0, 2, 0x220, 25 },
    { 1, 0, 0x000, 24 }, { 1, 1, 0x180, 21 }, { 1, 2, 0x2D0, 20 }, { 1, 3, 0x410, 29 },
    { 2, 0, 0x000, 24 }, { 2, 2, 0x1E0, 24 },
    { 3, 0, 0x000,  8 }, { 3, 1, 0x080,  8 },
};

static const IGPIO_GROUP IGpioIceLakeN[] =
{
    { 0, 1, 0x090, 26 }, { 0, 2, 0x230, 21 }, { 0, 3, 0x380,  8 }, { 0, 4, 0x400,  8 },
    { 1, 0, 0x000, 24 }, { 1, 1, 0x180, 26 }, { 1, 2, 0x320, 29 }, { 1, 3, 0x4F0, 24 },
    { 2, 1, 0x060, 24 }, { 2, 3, 0x270,  4 },
    { 3, 0, 0x000,  8 },
};

/* Alder Lake-P has the same groups, only GPP_V is two pads longer */
static const IGPIO_GROUP IGpioTigerLakeLp[] =
{
    { 0, 0, 0x000, 26 }, { 0, 1, 0x1A0, 16 }, { 0, 2, 0x2A0, 25 },
    { 1, 0, 0x000,  8 }, { 1, 1, 0x080, 24 }, { 1, 2, 0x200, 21 }, { 1, 3, 0x350, 24 },
    { 1, 4, 0x4D0, 27 },
    { 2, 0, 0x000, 24 }, { 2, 1, 0x180, 25 }, { 2, 3, 0x370, 25 },
    { 3, 0, 0x000,  8 },
};

static const IGPIO_GROUP IGpioAlderLakeP[] =
{
    { 0, 0, 0x000, 26 }, { 0, 1, 0x1A0, 16 }, { 0, 2, 0x2A0, 25 },
    { 1, 0, 0x000,  8 }, { 1, 1, 0x080, 24 }, { 1, 2, 0x200, 21 }, { 1, 3, 0x350, 24 },
    { 1, 4, 0x4D0, 29 },
    { 2, 0, 0x000, 24 }, { 2, 1, 0x180, 25 }, { 2, 3, 0x370, 25 },
    { 3, 0, 0x000,  8 },
};

static const IGPIO_GROUP IGpioTigerLakeH[] =
{
    { 0, 0, 0x080, 15 }, { 0, 1, 0x190, 20 }, { 0, 2, 0x2D0, 26 }, { 0, 3, 0x470,  8 },
    { 1, 0, 0x000, 26 }, { 1, 1, 0x1A0, 24 }, { 1, 2, 0x320,  8 }, { 1, 3, 0x3A0, 17 },
    { 1, 4, 0x4B0, 27 },
    { 2, 0, 0x000, 13 }, { 2, 1, 0x0D0, 24 },
    { 3, 0, 0x000, 24 }, { 3, 1, 0x180, 10 }, { 3, 2, 0x220, 15 },
    { 4, 0, 0x000, 15 },
};

static const IGPIO_GROUP IGpioAlderLakeN[] =
{
    { 0, 0, 0x000, 26 }, { 0, 1, 0x1A0, 16 }, { 0, 2, 0x2A0, 25 },
    { 1, 0, 0x000,  8 }, { 1, 1, 0x080, 20 }, { 1, 2, 0x1C0, 24 }, { 1, 3, 0x340, 21 },
    { 1, 4, 0x490, 29 },
    { 2, 0, 0x000, 24 }, { 2, 1, 0x180, 25 }, { 2, 3, 0x370, 25 },
    { 3, 0, 0x000,  8 },
};

static const IGPIO_GROUP IGpioAlderLakeS[] =
{
    { 0, 0, 0x000, 25 }, { 0, 1, 0x190, 23 }, { 0, 2, 0x300, 12 }, { 0, 3, 0x3C0, 27 },
    { 0, 4, 0x570,  8 },
    { 1, 0, 0x000, 24 }, { 1, 1, 0x180,  8 }, { 1, 2, 0x200, 24 },
    { 2, 1, 0x090, 16 }, { 2, 2, 0x190, 24 },
    { 3, 0, 0x000,  8 }, { 3, 1, 0x080, 23 }, { 3, 2, 0x1F0, 15 }, { 3, 3, 0x2E0, 24 },
    { 4, 0, 0x000, 25 },
};

static const IGPIO_GROUP IGpioMeteorLakeM[] =
{
    { 0, 0, 0x000, 28 }, { 0, 1, 0x1C0, 19 }, { 0, 2, 0x2F0, 27 },
    { 1, 0, 0x000, 20 }, { 1, 1, 0x140,  2 }, { 1, 2, 0x160, 24 },
    { 2, 0, 0x000, 16 }, { 2, 1, 0x100, 12 },
};

static const IGPIO_GROUP IGpioMeteorLakeP[] =
{
    { 0, 0, 0x000,  5 }, { 0, 1, 0x050, 24 }, { 0, 2, 0x1D0, 24 },
    { 1, 0, 0x000, 25 }, { 1, 1, 0x190, 25 },
    { 2, 0, 0x000, 26 }, { 2, 1, 0x1A0, 26 }, { 2, 2, 0x340, 15 }, { 2, 3, 0x430, 18 },
    { 3, 0, 0x000,  8 }, { 3, 1, 0x080, 12 },
    { 4, 0, 0x000, 25 }, { 4, 1, 0x190, 25 }, { 4, 2, 0x320, 32 }, { 4, 3, 0x520,  3 },
};

static const IGPIO_GROUP IGpioMeteorLakeS[] =
{
    { 0, 0, 0x000, 25 }, { 0, 1, 0x190, 14 }, { 0, 2, 0x270, 18 }, { 0, 3, 0x390, 31 },
    { 1, 0, 0x000, 15 }, { 1, 1, 0x0F0, 12 }, { 1, 2, 0x1B0, 22 },
    { 2, 0, 0x000,  9 }, { 2, 1, 0x090, 24 }, { 2, 2, 0x210, 20 }, { 2, 3, 0x350,  4 },
    { 2, 4, 0x390,  8 }, { 2, 5, 0x410, 31 },
    { 3, 0, 0x000,  8 }, { 3, 1, 0x080, 23 }, { 3, 2, 0x1F0, 14 }, { 3, 3, 0x2D0, 24 },
    { 4, 0, 0x000, 21 }, { 4, 1, 0x150, 16 },
};

/* Ownership, status and enable arrays, then the first pad */
#define IGPIO_MAP_SPT   { 0x0D0, 0x100, 0x120, 0x400 }
#define IGPIO_MAP_CNL   { 0x0B0, 0x100, 0x120, 0x600 }
#define IGPIO_MAP_CNLH  { 0x0C0, 0x100, 0x120, 0x600 }
#define IGPIO_MAP_ICL   { 0x0B0, 0x100, 0x110, 0x600 }
#define IGPIO_MAP_TGL   { 0x0B0, 0x100, 0x120, 0x700 }
#define IGPIO_MAP_ADLS  { 0x150, 0x200, 0x220, 0x700 }
#define IGPIO_MAP_MTLM  { 0x110, 0x200, 0x210, 0x600 }
#define IGPIO_MAP_MTLP  { 0x140, 0x200, 0x210, 0x600 }
#define IGPIO_MAP_MTLS  { 0x150, 0x200, 0x220, 0x600 }

#define IGPIO_SOC_ENTRY(Id, Groups, Communities, Stride, Width, Map) \
    { Id, Groups, RTL_NUMBER_OF(Groups), Communities, Stride, Width, Map }

static const IGPIO_SOC IGpioSocs[] =
{
    IGPIO_SOC_ENTRY(L"ACPI\\INT344B", IGpioSunrisePointLp, 3, 8, 24, IGPIO_MAP_SPT),
    IGPIO_SOC_ENTRY(L"ACPI\\INT345D", IGpioSunrisePointH, 3, 8, 24, IGPIO_MAP_SPT),
    IGPIO_SOC_ENTRY(L"ACPI\\INT34BB", IGpioCannonLakeLp, 3, 16, 32, IGPIO_MAP_CNL),
    IGPIO_SOC_ENTRY(L"ACPI\\INT3451", IGpioSunrisePointH, 3, 8, 24, IGPIO_MAP_SPT),
    IGPIO_SOC_ENTRY(L"ACPI\\INT3455", IGpioIceLakeLp, 4, 16, 32, IGPIO_MAP_ICL),
    IGPIO_SOC_ENTRY(L"ACPI\\INT3450", IGpioCannonLakeH, 4, 16, 32, IGPIO_MAP_CNLH),
    IGPIO_SOC_ENTRY(L"ACPI\\INT34C3", IGpioIceLakeN, 4, 16, 32, IGPIO_MAP_CNL),
    IGPIO_SOC_ENTRY(L"ACPI\\INT34C5", IGpioTigerLakeLp, 4, 16, 32, IGPIO_MAP_TGL),
    IGPIO_SOC_ENTRY(L"ACPI\\INT34C6", IGpioTigerLakeH, 5, 16, 32, IGPIO_MAP_CNLH),
    IGPIO_SOC_ENTRY(L"ACPI\\INTC1055", IGpioAlderLakeP, 4, 16, 32, IGPIO_MAP_TGL),
    IGPIO_SOC_ENTRY(L"ACPI\\INTC1056", IGpioAlderLakeS, 5, 16, 32, IGPIO_MAP_ADLS),
    IGPIO_SOC_ENTRY(L"ACPI\\INTC1057", IGpioAlderLakeN, 4, 16, 32, IGPIO_MAP_TGL),
    IGPIO_SOC_ENTRY(L"ACPI\\INTC1085", IGpioAlderLakeS, 5, 16, 32, IGPIO_MAP_ADLS),
    IGPIO_SOC_ENTRY(L"ACPI\\INTC1083", IGpioMeteorLakeP, 5, 16, 32, IGPIO_MAP_MTLP),
    IGPIO_SOC_ENTRY(L"ACPI\\INTC1084", IGpioMeteorLakeS, 5, 16, 32, IGPIO_MAP_MTLS),
    IGPIO_SOC_ENTRY(L"ACPI\\INTC1082", IGpioMeteorLakeM, 3, 16, 32, IGPIO_MAP_MTLM),
    IGPIO_SOC_ENTRY(L"ACPI\\INTC105E", IGpioMeteorLakeP, 5, 16, 32, IGPIO_MAP_MTLP),
};

C_ASSERT(RTL_NUMBER_OF(IGpioMeteorLakeS) <= IGPIO_MAX_GROUPS);

/**
 * @brief
 * Whether any id of a hardware id multi string starts with the given one.
 */
static
BOOLEAN
NTAPI
IGpioIdListMatches(
    _In_reads_(Characters) PCWSTR IdList,
    _In_ SIZE_T Characters,
    _In_ PCWSTR Wanted)
{
    SIZE_T WantedLength = wcslen(Wanted);
    SIZE_T Length;
    SIZE_T Offset = 0;

    while (Offset < Characters)
    {
        Length = wcsnlen(&IdList[Offset], Characters - Offset);
        if (Length == 0)
            break;

        if (Length >= WantedLength && wcsncmp(&IdList[Offset], Wanted, WantedLength) == 0)
            return TRUE;

        Offset += Length + 1;
    }

    return FALSE;
}

/**
 * @brief
 * Picks the pad geometry by hardware id, in table order.
 *
 * @return
 * The SoC description, or NULL when the part is not known.
 */
_IRQL_requires_(PASSIVE_LEVEL)
const IGPIO_SOC *
NTAPI
IGpioFindSoc(
    _In_ WDFDEVICE Device)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    const IGPIO_SOC *Found = NULL;
    WDFMEMORY Memory;
    size_t Size;
    PCWSTR Ids;
    ULONG Index;
    NTSTATUS Status;

    PAGED_CODE();

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    Attributes.ParentObject = Device;

    Status = WdfDeviceAllocAndQueryProperty(Device,
                                            DevicePropertyHardwareID,
                                            NonPagedPoolNx,
                                            &Attributes,
                                            &Memory);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("iaLPSS2 GPIO: no hardware id 0x%08lx\n", Status);
        return NULL;
    }

    Ids = WdfMemoryGetBuffer(Memory, &Size);

    for (Index = 0; Index < RTL_NUMBER_OF(IGpioSocs); Index++)
    {
        if (IGpioIdListMatches(Ids, Size / sizeof(WCHAR), IGpioSocs[Index].HardwareId))
        {
            Found = &IGpioSocs[Index];
            break;
        }
    }

    if (Found == NULL)
        DPRINT1("iaLPSS2 GPIO: %S is not a known part\n", Ids);

    WdfObjectDelete(Memory);
    return Found;
}
