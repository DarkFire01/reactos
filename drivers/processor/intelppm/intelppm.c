/*
 * PROJECT:     ReactOS Intel Processor Driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Intel MSR based P-states, throttling, MWAIT idle and boost control
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ppmlib.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

#define INTEL_MSR_PLATFORM_ID           0x017
#define INTEL_MSR_MODEL11_FUSES         0x020
#define INTEL_MSR_EBC_FREQUENCY_ID      0x02C
#define INTEL_MSR_BBL_CR_CTL3           0x119
#define INTEL_MSR_PERF_CTL              0x199
#define INTEL_MSR_CLOCK_MODULATION      0x19A
#define INTEL_MSR_MISC_ENABLE           0x1A0
#define INTEL_MSR_ENERGY_PERF_BIAS      0x1B0
#define INTEL_MSR_HWP_REQUEST_PKG       0x772
#define INTEL_MSR_HWP_INTERRUPT         0x773
#define INTEL_MSR_HWP_REQUEST           0x774
#define INTEL_MSR_HWP_STATUS            0x777

/* IA32_PERF_CTL: the requested ratio, and the turbo disengage bit */
#define INTEL_PERF_CTL_TARGET           0x000000000000FFFFULL
#define INTEL_PERF_CTL_TURBO_OFF        0x0000000100000000ULL

#define INTEL_MISC_ENABLE_EIST          0x0000000000010000ULL

/* _PDC/_OSC capabilities, bit 12 announces the HWP interrupt */
#define INTEL_PDC_CAPABILITIES          0x00002FBF
#define INTEL_PDC_HWP_INTERRUPT         0x00001000

/* CPUID bits */
#define INTEL_CPUID1_ECX_EIST           0x00000080
#define INTEL_CPUID1_ECX_MONITOR        0x00000008
#define INTEL_CPUID1_EDX_ACPI           0x00400000
#define INTEL_CPUID6_EAX_TURBO          0x00000002
#define INTEL_CPUID6_EAX_HWP            0x00000080
#define INTEL_CPUID6_EAX_HWP_NOTIFY     0x00000100
#define INTEL_CPUID6_ECX_COUNTERS       0x00000001
#define INTEL_CPUID6_ECX_EPB            0x00000008
#define INTEL_CPUID5_ECX_EXTENSIONS     0x00000001
#define INTEL_CPUID5_ECX_BREAK_ON_INT   0x00000002

/* {4077A616-290C-47BE-9EBD-D87058713953}, Intel processor _OSC */
static const GUID IntelOscUuid =
    { 0x4077a616, 0x290c, 0x47be, { 0x9e, 0xbd, 0xd8, 0x70, 0x58, 0x71, 0x39, 0x53 } };

static PPMLIB_PDC_REQUEST IntelPdc;
static PPMLIB_OSC_REQUEST IntelOsc;

/* CPUID **********************************************************************/

static
VOID
NTAPI
IntelCpuid(
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

/* Power management leaf 6, zeros when the processor stops short of it */
static
VOID
NTAPI
IntelPowerLeaf(
    _Out_writes_(4) PULONG Registers)
{
    ULONG Basic[4];

    RtlZeroMemory(Registers, 4 * sizeof(ULONG));
    IntelCpuid(0, Basic);
    if (Basic[0] >= 6)
        IntelCpuid(6, Registers);
}

static
BOOLEAN
NTAPI
IntelHasPowerFeature(
    _In_ ULONG Register,
    _In_ ULONG Mask)
{
    ULONG Leaf[4];

    IntelPowerLeaf(Leaf);
    return (Leaf[Register] & Mask) == Mask;
}

static
BOOLEAN
NTAPI
IntelHasMwait(VOID)
{
    ULONG Leaf[4];

    IntelCpuid(1, Leaf);
    if (!(Leaf[2] & INTEL_CPUID1_ECX_MONITOR))
        return FALSE;

    IntelCpuid(0, Leaf);
    return (Leaf[0] >= 5);
}

/* Counters stop wrapping from Sandy Bridge on, and were never trusted on NetBurst */
static
BOOLEAN
NTAPI
IntelCountersDoNotWrap(VOID)
{
    ULONG Leaf[4];
    ULONG Family;
    ULONG Model;

    IntelCpuid(1, Leaf);
    Family = (Leaf[0] >> 8) & 0xF;
    Model = ((Leaf[0] >> 4) & 0xF) | ((Leaf[0] >> 12) & 0xF0);

    if (Family == 0xF)
        return FALSE;

    return (Family != 6 || Model >= 0x2A);
}

/**
 * @brief Legacy SpeedStep detection for parts that predate EIST reporting.
 */
static
BOOLEAN
NTAPI
IntelHasLegacySpeedStep(VOID)
{
    ULONG Leaf[4];
    ULONG64 Msr;
    ULONG Family;
    ULONG Model;
    ULONG Stepping;
    ULONG Fuse;

    IntelCpuid(1, Leaf);
    Family = (Leaf[0] >> 8) & 0xF;
    Model = (Leaf[0] >> 4) & 0xF;
    Stepping = Leaf[0] & 0xF;

    if (Family == 0xF && Model <= 2)
    {
        Msr = __readmsr(INTEL_MSR_EBC_FREQUENCY_ID);
        return ((Msr & 0xFF00) != 0) && ((Msr & (1ULL << 21)) != 0);
    }

    if (Family != 6 || (Model != 8 && Model != 11) || (Leaf[0] & 0xF0000) != 0)
        return FALSE;

    Msr = __readmsr(INTEL_MSR_PLATFORM_ID);
    if (!(Msr & (1ULL << 50)))
        return FALSE;

    if (Msr & (3ULL << 56))
        return TRUE;

    if (Model == 8 && Stepping == 6)
    {
        Fuse = (ULONG)(Msr >> 51) & 3;
        return (Fuse == 0 || Fuse == 3);
    }

    if (Model == 8 && Stepping == 10)
        return (__readmsr(INTEL_MSR_BBL_CR_CTL3) & 1) != 0;

    if (Model == 11)
        return (__readmsr(INTEL_MSR_MODEL11_FUSES) & (1ULL << 44)) != 0;

    return FALSE;
}

/* PERFORMANCE ****************************************************************/

/* Only the target ratio changes, the turbo disengage bit stays as set */
static
NTSTATUS
NTAPI
IntelSetPerfCtl(
    _In_ PVOID Context,
    _In_ ULONG64 Control,
    _In_ ULONG64 Status)
{
    ULONG64 Value;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Status);

    Value = __readmsr(INTEL_MSR_PERF_CTL);
    Value = (Value & ~INTEL_PERF_CTL_TARGET) | (Control & INTEL_PERF_CTL_TARGET);
    __writemsr(INTEL_MSR_PERF_CTL, Value);
    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
IntelSetClockModulation(
    _In_ PVOID Context,
    _In_ ULONG64 Control,
    _In_ ULONG64 Status)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Status);

    __writemsr(INTEL_MSR_CLOCK_MODULATION, Control);
    return STATUS_SUCCESS;
}

static
VOID
NTAPI
IntelGetClockModulation(
    _Out_ PULONG64 ThrottleState)
{
    *ThrottleState = __readmsr(INTEL_MSR_CLOCK_MODULATION);
}

/* Boost policy 0 to 100 maps onto bias 15 (power) down to 0 (performance) */
static
VOID
NTAPI
IntelSetEnergyBias(
    _In_ PVOID Context,
    _In_ ULONG Value)
{
    ULONG64 Msr;
    ULONG Bias;

    UNREFERENCED_PARAMETER(Context);

    Bias = (ULONG)(15 * (100 - Value)) / 100;
    Msr = __readmsr(INTEL_MSR_ENERGY_PERF_BIAS);
    Msr = (Msr & ~0xFULL) | (Bias & 0xF);
    __writemsr(INTEL_MSR_ENERGY_PERF_BIAS, Msr);
}

static
VOID
NTAPI
IntelWriteTurbo(
    _In_ BOOLEAN Allowed)
{
    ULONG64 Msr;

    Msr = __readmsr(INTEL_MSR_PERF_CTL);
    if (Allowed)
        Msr &= ~INTEL_PERF_CTL_TURBO_OFF;
    else
        Msr |= INTEL_PERF_CTL_TURBO_OFF;
    __writemsr(INTEL_MSR_PERF_CTL, Msr);
}

static
VOID
NTAPI
IntelSetTurbo(
    _In_ PVOID Context,
    _In_ ULONG Value)
{
    UNREFERENCED_PARAMETER(Context);

    IntelWriteTurbo(Value == PPM_BOOST_MODE_ENABLED || Value == PPM_BOOST_MODE_AGGRESSIVE);
}

/* With the bias available the efficient modes keep turbo too */
static
VOID
NTAPI
IntelSetTurboWithBias(
    _In_ PVOID Context,
    _In_ ULONG Value)
{
    UNREFERENCED_PARAMETER(Context);

    IntelWriteTurbo(Value >= PPM_BOOST_MODE_ENABLED && Value <= PPM_BOOST_MODE_EFFICIENT_AGGRESSIVE);
}

static
NTSTATUS
NTAPI
IntelValidatePStates(
    _In_ PPPMLIB_CONTROL_PAIR Pct,
    _In_ PPPMLIB_PSS Pss,
    _Inout_ PULONG Errors)
{
    ULONG Leaf[4];

    UNREFERENCED_PARAMETER(Pss);

    IntelCpuid(1, Leaf);
    if ((Leaf[2] & INTEL_CPUID1_ECX_EIST) &&
        (__readmsr(INTEL_MSR_MISC_ENABLE) & INTEL_MISC_ENABLE_EIST) &&
        (Pct->Control.SpaceId == PPMLIB_SPACE_FFH || Pct->Control.SpaceId == PPMLIB_SPACE_IO))
    {
        return STATUS_SUCCESS;
    }

    if (Pct->Control.SpaceId == PPMLIB_SPACE_IO && IntelHasLegacySpeedStep())
        return STATUS_SUCCESS;

    *Errors |= PPMLIB_ERROR_PCT;
    return STATUS_NOT_SUPPORTED;
}

/* HWP owns throttling when Desired is the HWP request ratio field */
static
BOOLEAN
NTAPI
IntelIsThrottleEfficient(
    _In_ PPPMLIB_CPC Cpc)
{
    if (Cpc->Desired.SpaceId != PPMLIB_SPACE_FFH)
        return FALSE;

    if (Cpc->Desired.Address != INTEL_MSR_HWP_REQUEST && Cpc->Desired.Address != INTEL_MSR_HWP_REQUEST_PKG)
        return FALSE;

    return (Cpc->Desired.BitWidth == 8 && Cpc->Desired.BitOffset == 16);
}

static
VOID
NTAPI
IntelEnableHwpInterrupt(VOID)
{
    ULONG64 Msr;

    Msr = __readmsr(INTEL_MSR_HWP_INTERRUPT);
    Msr = (Msr & ~2ULL) | 1;
    __writemsr(INTEL_MSR_HWP_INTERRUPT, Msr);
}

/**
 * @brief Prepares the HWP guaranteed change interrupt.
 *
 * @remarks The HAL has no thermal interrupt to connect, so this always ends
 * with STATUS_NOT_SUPPORTED after quiescing the interrupt.
 */
static
NTSTATUS
NTAPI
IntelConnectHwpInterrupt(
    _In_ PPPMLIB_DEVICE Device)
{
    if (Device->OscOutput == NULL ||
        Device->OscOutputLength < 2 * sizeof(ULONG) ||
        !(Device->OscOutput[1] & INTEL_PDC_HWP_INTERRUPT))
    {
        return STATUS_NOT_SUPPORTED;
    }

    __writemsr(INTEL_MSR_HWP_INTERRUPT, __readmsr(INTEL_MSR_HWP_INTERRUPT) & ~3ULL);
    __writemsr(INTEL_MSR_HWP_STATUS, __readmsr(INTEL_MSR_HWP_STATUS) & ~5ULL);
    return STATUS_NOT_SUPPORTED;
}

/* IDLE ***********************************************************************/

static
VOID
NTAPI
IntelMwaitIdle(
    _In_ PPPMLIB_IDLE_CONTEXT Context)
{
#if defined(_MSC_VER)
    _mm_monitor(Context->MonitorAddress, 0, 0);
    _mm_mwait(Context->Extensions, Context->Hints);
#else
    __asm__ __volatile__("monitor" : : "a"(Context->MonitorAddress), "c"(0), "d"(0));
    __asm__ __volatile__("mwait" : : "a"(Context->Hints), "c"(Context->Extensions));
#endif
}

static
NTSTATUS
NTAPI
IntelMwaitCheck(
    _In_ PPPMLIB_IDLE_CONTEXT Context)
{
    if (!(Context->Flags & PPMLIB_MWAIT_BUS_MASTER_CHECK))
        return STATUS_SUCCESS;

    return PpmLibBusMasterCheck();
}

/* Allocates the line MONITOR watches and learns the MWAIT extensions */
static
NTSTATUS
NTAPI
IntelInitMwait(
    _Inout_ PPPMLIB_DEVICE Device)
{
    PHYSICAL_ADDRESS Lowest;
    PHYSICAL_ADDRESS Highest;
    PHYSICAL_ADDRESS Boundary;
    ULONG Leaf[4];
    ULONG LineSize;

    if (!IntelHasMwait())
        return STATUS_UNSUCCESSFUL;

    IntelCpuid(5, Leaf);
    LineSize = Leaf[0] & 0xFFFF;
    if (LineSize != 0)
    {
        Lowest.QuadPart = 0;
        Highest.QuadPart = -1;
        Boundary.QuadPart = 0;
        Device->MonitorLine = MmAllocateContiguousNodeMemory(LineSize,
                                                             Lowest,
                                                             Highest,
                                                             Boundary,
                                                             PAGE_READWRITE,
                                                             MM_ANY_NODE_OK);
        if (Device->MonitorLine == NULL)
            return STATUS_INSUFFICIENT_RESOURCES;
    }

    if ((Leaf[2] & (INTEL_CPUID5_ECX_EXTENSIONS | INTEL_CPUID5_ECX_BREAK_ON_INT)) ==
        (INTEL_CPUID5_ECX_EXTENSIONS | INTEL_CPUID5_ECX_BREAK_ON_INT))
    {
        Device->MwaitBreakOnInterrupt = TRUE;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
NTAPI
IntelInitializeCStates(
    _Inout_ PPPMLIB_DEVICE Device)
{
    if (!(Device->Enabled & PPMLIB_NATIVE_IDLE) || !NT_SUCCESS(IntelInitMwait(Device)))
    {
        Device->MonitorLine = NULL;
        Device->Enabled &= ~PPMLIB_NATIVE_IDLE;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief Decodes an FFH _CST entry of the native C-state instruction class.
 * The address holds the MWAIT hints and extensions.
 */
static
NTSTATUS
NTAPI
IntelDecodeCState(
    _In_ PPPMLIB_DEVICE Device,
    _In_ PPPMLIB_CSTATE CState,
    _Out_opt_ PBOOLEAN InterruptsEnabled,
    _Out_opt_ PPPMLIB_IDLE_STATE State)
{
    PPPMLIB_GAS Register = &CState->Register;
    ULONG Extensions;

    if (Register->SpaceId != PPMLIB_SPACE_FFH || Register->BitWidth != 1 || Register->BitOffset != 2)
        return STATUS_UNSUCCESSFUL;

    if (!(Device->Enabled & PPMLIB_NATIVE_IDLE))
        return STATUS_NOT_SUPPORTED;

    Extensions = (ULONG)(Register->Address >> 32);
    if (Device->MwaitBreakOnInterrupt)
        Extensions |= 1;

    if (State)
    {
        State->Native = TRUE;
        if ((Register->AccessBits & PPMLIB_MWAIT_BUS_MASTER_CHECK) && PpmLibIsValidRegister(&PpmLib.Fadt.Pm1aEvent))
            State->Check = IntelMwaitCheck;
        else
            State->Check = NULL;

        State->Handler = IntelMwaitIdle;
        State->Context.Hints = (ULONG)Register->Address;
        State->Context.Extensions = Extensions;
        State->Context.MonitorAddress = Device->MonitorLine;
        State->Context.Flags = Register->AccessBits;
    }

    if (InterruptsEnabled)
        *InterruptsEnabled = FALSE;

    return STATUS_SUCCESS;
}

/* INITIALIZATION *************************************************************/

static
NTSTATUS
NTAPI
IntelInitDriver(
    _Inout_ PPPMLIB_VENDOR Vendor,
    _Out_ PULONG64 Capabilities)
{
    BOOLEAN HwpInterrupt;
    BOOLEAN EnergyBias;
    ULONG64 Caps;
    ULONG Leaf[4];
    ULONG Supported;

    HwpInterrupt = IntelHasPowerFeature(0, INTEL_CPUID6_EAX_HWP | INTEL_CPUID6_EAX_HWP_NOTIFY);
    Supported = INTEL_PDC_CAPABILITIES;
    if (HwpInterrupt)
        Supported |= INTEL_PDC_HWP_INTERRUPT;

    IntelPdc.Revision = 1;
    IntelPdc.Count = 1;
    IntelPdc.Capabilities[0] = Supported;
    Vendor->Pdc = &IntelPdc;
    Vendor->PdcSize = sizeof(IntelPdc);

    /* The first dword is the query/status word, sent as 0 to commit */
    IntelOsc.Uuid = IntelOscUuid;
    IntelOsc.Revision = 1;
    IntelOsc.Count = 2;
    IntelOsc.Capabilities[0] = 0;
    IntelOsc.Capabilities[1] = Supported;
    Vendor->Osc = &IntelOsc;
    Vendor->OscSize = sizeof(IntelOsc);

    Vendor->ValidatePStates = IntelValidatePStates;
    Vendor->SetPStateFfh = IntelSetPerfCtl;
    Vendor->SetTStateFfh = IntelSetClockModulation;
    Caps = PPMLIB_PSS_FFH | PPMLIB_TSS_FFH | PPMLIB_ACPI1_THROTTLE_MP;

    if (IntelHasPowerFeature(2, INTEL_CPUID6_ECX_COUNTERS))
    {
        Vendor->PStateFeedback = PpmLibReadAperfMperf;
        Caps |= PPMLIB_FEEDBACK;
        if (IntelCountersDoNotWrap())
            Caps |= PPMLIB_NONWRAPPING_COUNTERS;
    }

    IntelCpuid(1, Leaf);
    if (Leaf[3] & INTEL_CPUID1_EDX_ACPI)
        Vendor->GetFfhThrottleState = IntelGetClockModulation;

    EnergyBias = IntelHasPowerFeature(2, INTEL_CPUID6_ECX_EPB);
    if (EnergyBias)
        Vendor->BoostPolicyHandler = IntelSetEnergyBias;

    if (IntelHasPowerFeature(0, INTEL_CPUID6_EAX_TURBO))
        Vendor->BoostModeHandler = EnergyBias ? IntelSetTurboWithBias : IntelSetTurbo;

    if (IntelHasMwait())
    {
        Vendor->InitializeCStates = IntelInitializeCStates;
        Vendor->DecodeCState = IntelDecodeCState;
        Caps |= PPMLIB_NATIVE_IDLE;
    }

    if (IntelHasPowerFeature(0, INTEL_CPUID6_EAX_HWP))
        Vendor->IsThrottleEfficient = IntelIsThrottleEfficient;

    if (HwpInterrupt)
    {
        Caps |= PPMLIB_NATIVE_INTERRUPT;
        Vendor->ConnectNativeInterrupt = IntelConnectHwpInterrupt;
        Vendor->EnableNativeInterrupt = IntelEnableHwpInterrupt;
    }

    *Capabilities = Caps;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    return PpmLibDriverEntry(DriverObject, RegistryPath, IntelInitDriver);
}

/* EOF */
