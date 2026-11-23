/*
 * PROJECT:     ReactOS AMD Processor Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     AMD hardware P-state control and core performance boost
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ppmlib.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

#define AMD_MSR_HWCR                    0xC0010015
#define AMD_MSR_PSTATE_CONTROL          0xC0010062

/* HWCR CpbDis, set to keep the core out of boost states */
#define AMD_HWCR_BOOST_DISABLE          (1ULL << 25)

/* The _PSS control value is the hardware P-state number */
#define AMD_PSTATE_COMMAND_MASK         0xF

#define AMD_CPUID6_ECX_COUNTERS         0x00000001
#define AMD_CPUID_POWER_HW_PSTATE       0x00000080
#define AMD_CPUID_POWER_BOOST           0x00000200

/* FUNCTIONS ******************************************************************/

static
VOID
NTAPI
AmdCpuid(
    _In_ ULONG Leaf,
    _Out_writes_(4) PULONG Registers)
{
    INT Values[4];

    __cpuid(Values, (INT)Leaf);
    Registers[0] = (ULONG)Values[0];
    Registers[1] = (ULONG)Values[1];
    Registers[2] = (ULONG)Values[2];
    Registers[3] = (ULONG)Values[3];
}

/*
 * EDX of the advanced power management leaf. The leaf limit is compared
 * against 7 rather than 0x80000007, which every processor passes.
 */
static
BOOLEAN
NTAPI
AmdQueryPowerFeatures(
    _Out_ PULONG Features)
{
    ULONG Leaf[4];

    *Features = 0;
    AmdCpuid(0x80000000, Leaf);
    if (Leaf[0] < 7)
        return FALSE;

    AmdCpuid(0x80000007, Leaf);
    *Features = Leaf[3];
    return TRUE;
}

static
NTSTATUS
NTAPI
AmdDeviceStart(
    _In_ PPPMLIB_DEVICE Device)
{
    UNREFERENCED_PARAMETER(Device);
    return STATUS_SUCCESS;
}

/* FFH _PCT needs hardware P-states, an I/O _PCT is always taken */
static
NTSTATUS
NTAPI
AmdValidatePStates(
    _In_ PPPMLIB_CONTROL_PAIR Pct,
    _In_ PPPMLIB_PSS Pss,
    _Inout_ PULONG Errors)
{
    ULONG Features;

    UNREFERENCED_PARAMETER(Pss);

    if (AmdQueryPowerFeatures(&Features) &&
        (Features & AMD_CPUID_POWER_HW_PSTATE) &&
        (Pct->Control.SpaceId == PPMLIB_SPACE_IO || Pct->Control.SpaceId == PPMLIB_SPACE_FFH))
    {
        return STATUS_SUCCESS;
    }

    if (Pct->Control.SpaceId == PPMLIB_SPACE_IO)
        return STATUS_SUCCESS;

    *Errors |= PPMLIB_ERROR_PCT;
    return STATUS_NOT_SUPPORTED;
}

static
NTSTATUS
NTAPI
AmdSetPState(
    _In_ PVOID Context,
    _In_ ULONG64 Control,
    _In_ ULONG64 Status)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Status);

    __writemsr(AMD_MSR_PSTATE_CONTROL, Control & AMD_PSTATE_COMMAND_MASK);
    return STATUS_SUCCESS;
}

/* Only the plain enabled and aggressive modes allow boost */
static
VOID
NTAPI
AmdSetBoostMode(
    _In_ PVOID Context,
    _In_ ULONG Value)
{
    ULONG64 Hwcr;

    UNREFERENCED_PARAMETER(Context);

    Hwcr = __readmsr(AMD_MSR_HWCR);
    if (Value == PPM_BOOST_MODE_ENABLED || Value == PPM_BOOST_MODE_AGGRESSIVE)
        Hwcr &= ~AMD_HWCR_BOOST_DISABLE;
    else
        Hwcr |= AMD_HWCR_BOOST_DISABLE;
    __writemsr(AMD_MSR_HWCR, Hwcr);
}

static
NTSTATUS
NTAPI
AmdInitDriver(
    _Inout_ PPPMLIB_VENDOR Vendor,
    _Out_ PULONG64 Capabilities)
{
    ULONG64 Caps = PPMLIB_PSS_FFH;
    ULONG Features;
    ULONG Leaf[4];

    Vendor->ValidatePStates = AmdValidatePStates;
    Vendor->SetPStateFfh = AmdSetPState;
    Vendor->DeviceStart = AmdDeviceStart;

    AmdCpuid(0, Leaf);
    if (Leaf[0] >= 6)
    {
        AmdCpuid(6, Leaf);
        if (Leaf[2] & AMD_CPUID6_ECX_COUNTERS)
        {
            Vendor->PStateFeedback = PpmLibReadAperfMperf;
            Caps |= PPMLIB_FEEDBACK;
        }
    }

    if (AmdQueryPowerFeatures(&Features) && (Features & AMD_CPUID_POWER_BOOST))
        Vendor->BoostModeHandler = AmdSetBoostMode;

    *Capabilities = Caps;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    return PpmLibDriverEntry(DriverObject, RegistryPath, AmdInitDriver);
}

/* EOF */
