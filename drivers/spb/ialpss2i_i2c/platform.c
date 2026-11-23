/*
 * PROJECT:     ReactOS Simple Peripheral Bus
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Intel LPSS I2C per platform bus timing
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include "ialpss2i_i2c.h"
#include <acpiioct.h>

#define NDEBUG
#include <debug.h>

/* Families that share one set of default counts */
typedef enum _LI2C_PROFILE
{
    Li2cProfileGeneric,
    Li2cProfileAtom,
    Li2cProfileCannonLakeLp,
    Li2cProfileCannonLakeH,
    Li2cProfileIceLake,
    Li2cProfileTigerLake,
    Li2cProfileCount
} LI2C_PROFILE;

/* Standard, fast, fast plus and high speed counts per profile */
static const LI2C_TIMING Li2cProfileTiming[Li2cProfileCount][Li2cSpeedCount] =
{
    /* Generic */
    { { 528, 640, 28, 28, 1 }, { 128, 160, 28, 28, 6 }, { 30, 80, 40, 40, 6 }, { 15, 40, 20, 20, 2 } },
    /* Atom */
    { { 580, 720, 42, 42, 1 }, { 100, 210, 42, 42, 6 }, { 35, 71, 16, 16, 6 }, { 6, 28, 6, 6, 2 } },
    /* Cannon Lake and Comet Lake LP */
    { { 1000, 1100, 50, 50, 10 }, { 250, 250, 50, 50, 10 }, { 70, 104, 10, 10, 6 }, { 28, 72, 10, 10, 2 } },
    /* Cannon Lake and Comet Lake H */
    { { 1020, 1120, 50, 50, 10 }, { 257, 257, 50, 50, 10 }, { 91, 104, 10, 10, 6 }, { 16, 40, 10, 10, 2 } },
    /* Ice Lake, Elkhart Lake, Jasper Lake */
    { { 532, 641, 28, 28, 4 }, { 132, 161, 28, 28, 6 }, { 34, 81, 16, 16, 2 }, { 20, 25, 8, 8, 3 } },
    /* Tiger Lake and later */
    { { 409, 576, 16, 32, 4 }, { 63, 179, 16, 32, 6 }, { 28, 65, 32, 0, 1 }, { 16, 34, 16, 0, 3 } },
};

/* Active LTR per speed, then the idle LTR */
static const ULONG Li2cDefaultActiveLtr[Li2cSpeedCount] = { 2560, 640, 256, 76 };
#define LI2C_DEFAULT_IDLE_LTR LPSS_LTR_UNUSED

/** A run of evenly spaced device ids, one controller each. */
typedef struct _LI2C_ID_RUN
{
    USHORT FirstId;
    UCHAR Count;
    UCHAR Step;
    UCHAR FirstController;
    UCHAR Profile;
} LI2C_ID_RUN;

static const LI2C_ID_RUN Li2cIdRuns[] =
{
    { 0x9D60, 6, 1, 0, Li2cProfileGeneric },       /* Sunrise Point LP */
    { 0xA160, 4, 1, 0, Li2cProfileGeneric },       /* Sunrise Point H */
    { 0xA2E0, 4, 1, 0, Li2cProfileGeneric },       /* Union Point H */
    { 0x0AAC, 8, 2, 0, Li2cProfileAtom },          /* Broxton */
    { 0x1AAC, 8, 2, 0, Li2cProfileAtom },
    { 0x5AAC, 8, 2, 0, Li2cProfileAtom },
    { 0x31AC, 8, 2, 0, Li2cProfileAtom },          /* Gemini Lake */
    { 0x9DE8, 4, 1, 0, Li2cProfileCannonLakeLp },  /* Cannon Lake LP */
    { 0x9DC5, 2, 1, 4, Li2cProfileCannonLakeLp },
    { 0x02E8, 4, 1, 0, Li2cProfileCannonLakeLp },  /* Comet Lake LP */
    { 0x02C5, 2, 1, 4, Li2cProfileCannonLakeLp },
    { 0xA368, 4, 1, 0, Li2cProfileCannonLakeH },   /* Cannon Lake H */
    { 0x06E8, 4, 1, 0, Li2cProfileCannonLakeH },   /* Comet Lake H */
    { 0x34E8, 4, 1, 0, Li2cProfileIceLake },       /* Ice Lake LP */
    { 0x34C5, 2, 1, 4, Li2cProfileIceLake },
    { 0x38E8, 4, 1, 0, Li2cProfileIceLake },
    { 0x38C5, 2, 1, 4, Li2cProfileIceLake },
    { 0x4B78, 4, 1, 0, Li2cProfileIceLake },       /* Elkhart Lake */
    { 0x4B4B, 2, 1, 4, Li2cProfileIceLake },
    { 0x4B44, 2, 1, 6, Li2cProfileIceLake },
    { 0x4DE8, 4, 1, 0, Li2cProfileIceLake },       /* Jasper Lake */
    { 0x4DC5, 2, 1, 4, Li2cProfileIceLake },
    { 0x98E8, 4, 1, 0, Li2cProfileTigerLake },
    { 0x98C5, 2, 1, 4, Li2cProfileTigerLake },
    { 0xA0E8, 4, 1, 0, Li2cProfileTigerLake },     /* Tiger Lake LP */
    { 0xA0C5, 2, 1, 4, Li2cProfileTigerLake },
    { 0xA0D8, 2, 1, 6, Li2cProfileTigerLake },
    { 0x43E8, 4, 1, 0, Li2cProfileTigerLake },     /* Tiger Lake H */
    { 0x43AD, 2, 1, 4, Li2cProfileTigerLake },
    { 0x43D8, 2, 1, 6, Li2cProfileTigerLake },
    { 0x7ACC, 4, 1, 0, Li2cProfileTigerLake },     /* Alder Lake S */
    { 0x7AFC, 2, 1, 4, Li2cProfileTigerLake },
    { 0x51E8, 4, 1, 0, Li2cProfileTigerLake },     /* Alder Lake P */
    { 0x51C5, 2, 1, 4, Li2cProfileTigerLake },
    { 0x51D8, 2, 1, 6, Li2cProfileTigerLake },
    { 0x54E8, 4, 1, 0, Li2cProfileTigerLake },     /* Alder Lake N */
    { 0x54C5, 2, 1, 4, Li2cProfileTigerLake },
    { 0x54D8, 2, 1, 6, Li2cProfileTigerLake },
    { 0x7A4C, 4, 1, 0, Li2cProfileTigerLake },     /* Raptor Lake S */
    { 0x7A7C, 2, 1, 4, Li2cProfileTigerLake },
    { 0x7E78, 4, 1, 0, Li2cProfileTigerLake },     /* Meteor Lake */
    { 0x7E50, 2, 1, 4, Li2cProfileTigerLake },
    { 0xA878, 4, 1, 0, Li2cProfileTigerLake },
    { 0xA850, 2, 1, 4, Li2cProfileTigerLake },
    { 0x7F4C, 4, 1, 0, Li2cProfileTigerLake },
    { 0x7F7A, 2, 1, 4, Li2cProfileTigerLake },
    { 0xE478, 4, 1, 0, Li2cProfileTigerLake },
    { 0xE450, 2, 1, 4, Li2cProfileTigerLake },
    { 0x7778, 4, 1, 0, Li2cProfileTigerLake },
    { 0x7750, 2, 1, 4, Li2cProfileTigerLake },
    { 0xD378, 4, 1, 0, Li2cProfileTigerLake },
    { 0xD350, 2, 1, 4, Li2cProfileTigerLake },
    { 0x4D78, 4, 1, 0, Li2cProfileTigerLake },
    { 0x4D50, 2, 1, 4, Li2cProfileTigerLake },
    { 0x6E4C, 4, 1, 0, Li2cProfileTigerLake },
    { 0x6E7A, 2, 1, 4, Li2cProfileTigerLake },
    { 0xD278, 4, 1, 0, Li2cProfileTigerLake },
    { 0xD250, 2, 1, 4, Li2cProfileTigerLake },
    { 0xE378, 4, 1, 0, Li2cProfileTigerLake },
    { 0xE350, 2, 1, 4, Li2cProfileTigerLake },
};

/* Revisions from here up are pre-silicon parts with their own clocks */
#define LI2C_FIRST_EMULATION_REVISION 0xF0

/**
 * @brief
 * Installs the counts and LTR values the controller's family starts with.
 */
_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
Li2cLoadPlatformDefaults(
    _Inout_ PLI2C_DEVICE Device)
{
    LI2C_PROFILE Profile = Li2cProfileGeneric;
    const LI2C_ID_RUN *Run = NULL;
    ULONG Controller = 0;
    ULONG Offset;
    ULONG Index;

    PAGED_CODE();

    for (Index = 0; Index < RTL_NUMBER_OF(Li2cIdRuns); Index++)
    {
        Offset = Device->PciDeviceId - Li2cIdRuns[Index].FirstId;
        if (Device->PciDeviceId < Li2cIdRuns[Index].FirstId ||
            (Offset % Li2cIdRuns[Index].Step) != 0 ||
            Offset / Li2cIdRuns[Index].Step >= Li2cIdRuns[Index].Count)
        {
            continue;
        }

        Run = &Li2cIdRuns[Index];
        Profile = (LI2C_PROFILE)Run->Profile;
        Controller = Run->FirstController + Offset / Run->Step;
        break;
    }

    /* Broxton emulation parts run the generic counts, Gemini Lake has no such parts */
    if (Profile == Li2cProfileAtom &&
        Run->FirstId != 0x31AC &&
        Device->Revision >= LI2C_FIRST_EMULATION_REVISION)
    {
        Profile = Li2cProfileGeneric;
    }

    RtlCopyMemory(Device->Timing, Li2cProfileTiming[Profile], sizeof(Device->Timing));
    RtlCopyMemory(Device->ActiveLtr, Li2cDefaultActiveLtr, sizeof(Device->ActiveLtr));
    Device->IdleLtr = LI2C_DEFAULT_IDLE_LTR;

    /* The last two Cannon Lake LP style controllers need their clocks forced on */
    Device->DynamicClockGating = !(Profile == Li2cProfileCannonLakeLp && Controller > 3);

    DPRINT("iaLPSS2 I2C: DEV_%04X rev %02X controller %lu profile %d\n",
           Device->PciDeviceId,
           Device->Revision,
           Controller,
           Profile);
}

/**
 * @brief
 * Takes one speed's counts from an ACPI xxCN method, when the firmware has one.
 *
 * The package is high count, low count, then optionally the hold time, the
 * receive hold time and the spike length. Zero counts leave the defaults.
 */
static
VOID
NTAPI
Li2cEvaluateTimingMethod(
    _In_ PLI2C_DEVICE Device,
    _In_ ULONG MethodName,
    _Inout_ PLI2C_TIMING Timing)
{
    WDF_MEMORY_DESCRIPTOR InputMemory;
    WDF_MEMORY_DESCRIPTOR OutputMemory;
    ACPI_EVAL_INPUT_BUFFER Input;
    PACPI_EVAL_OUTPUT_BUFFER Output;
    PACPI_METHOD_ARGUMENT Argument;
    ULONG_PTR Returned = 0;
    ULONG HighCount = 0;
    ULONG Index;
    NTSTATUS Status;

    Output = ExAllocatePoolZero(NonPagedPoolNx, 512, LI2C_POOL_TAG);
    if (Output == NULL)
        return;

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    Input.MethodNameAsUlong = MethodName;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&InputMemory, &Input, sizeof(Input));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&OutputMemory, Output, 512);

    Status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Device->Device),
                                               NULL,
                                               IOCTL_ACPI_EVAL_METHOD,
                                               &InputMemory,
                                               &OutputMemory,
                                               NULL,
                                               &Returned);
    if (!NT_SUCCESS(Status) || Returned == 0)
        goto Exit;

    if (Output->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || Output->Count < 2 || Output->Count > 5)
        goto Exit;

    Argument = Output->Argument;
    for (Index = 0; Index < Output->Count; Index++)
    {
        if (Argument->Type != ACPI_METHOD_ARGUMENT_INTEGER)
            break;

        switch (Index)
        {
            case 0:
                HighCount = Argument->Argument;
                break;

            case 1:
                if (HighCount == 0 || Argument->Argument == 0)
                    goto Exit;
                Timing->HighCount = HighCount;
                Timing->LowCount = Argument->Argument;
                break;

            case 2:
                Timing->TxHold = Argument->Argument;
                Timing->RxHold = Argument->Argument;
                break;

            case 3:
                Timing->RxHold = Argument->Argument;
                break;

            default:
                Timing->Spike = Argument->Argument;
                break;
        }

        if (Index == 0 && HighCount == 0)
            break;

        Argument = ACPI_METHOD_NEXT_ARGUMENT(Argument);
    }

Exit:
    ExFreePoolWithTag(Output, LI2C_POOL_TAG);
}

/**
 * @brief
 * Applies the firmware's SSCN, FMCN, FPCN and HSCN overrides.
 */
_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
Li2cLoadAcpiTiming(
    _Inout_ PLI2C_DEVICE Device)
{
    PAGED_CODE();

    Li2cEvaluateTimingMethod(Device, (ULONG)'NCSS', &Device->Timing[Li2cStandard]);
    Li2cEvaluateTimingMethod(Device, (ULONG)'NCMF', &Device->Timing[Li2cFast]);
    Li2cEvaluateTimingMethod(Device, (ULONG)'NCPF', &Device->Timing[Li2cFastPlus]);
    Li2cEvaluateTimingMethod(Device, (ULONG)'NCSH', &Device->Timing[Li2cHigh]);
}
