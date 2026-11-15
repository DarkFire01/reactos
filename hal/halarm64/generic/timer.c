/*
 * PROJECT:     ReactOS ARM64 HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Generic timer, clock interrupt and stall support
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* CNTV_CTL_EL0 bits */
#define CNTV_CTL_ENABLE     0x1
#define CNTV_CTL_IMASK      0x2

/* GLOBALS *******************************************************************/

ULONG64 HalpTimerFrequency;
static ULONG64 HalpClockPeriod;
static ULONG HalpCurrentTimeIncrement = HALP_CLOCK_INCREMENT;
/* Every processor has its own generic timer, the boot processor keeps the system time */
static KINTERRUPT HalpClockInterrupt[MAXIMUM_PROCESSORS];

/* FUNCTIONS *****************************************************************/

FORCEINLINE
ULONG64
HalpReadCounter(VOID)
{
    __isb(_ARM64_BARRIER_SY);
    return (ULONG64)_ReadStatusReg(ARM64_CNTVCT);
}

/* Converts a duration in 100 ns units into counter ticks */
static
ULONG64
HalpTimeToTicks(
    _In_ ULONG64 Time)
{
    return (Time * HalpTimerFrequency) / 10000000ULL;
}

VOID
NTAPI
HalpInitializeClock(VOID)
{
    HalpTimerFrequency = KeArm64ReadRegister(ARM64_CNTFRQ_EL0) & 0xFFFFFFFF;
    if (!HalpTimerFrequency)
        KeBugCheckEx(HAL_INITIALIZATION_FAILED, 0x7173, 0, 0, 0);

    HalpClockPeriod = HalpTimeToTicks(HalpCurrentTimeIncrement);

    /* Keep the timer quiet until the clock interrupt is connected */
    KeArm64WriteRegister(ARM64_CNTV_CTL_EL0, CNTV_CTL_IMASK);

    KeSetTimeIncrement(HALP_CLOCK_INCREMENT, HALP_CLOCK_INCREMENT);
}

static
BOOLEAN
NTAPI
HalpClockInterruptHandler(
    _In_ PKINTERRUPT Interrupt,
    _In_ PVOID ServiceContext)
{
    PKTRAP_FRAME TrapFrame;
    ULONG64 Compare;

    UNREFERENCED_PARAMETER(ServiceContext);

    /* Stay on the grid from the previous deadline so the clock does not drift */
    Compare = KeArm64ReadRegister(ARM64_CNTV_CVAL_EL0) + HalpClockPeriod;
    if ((LONG64)(Compare - HalpReadCounter()) <= 0)
        Compare = HalpReadCounter() + HalpClockPeriod;
    KeArm64WriteRegister(ARM64_CNTV_CVAL_EL0, Compare);

    TrapFrame = Interrupt->TrapFrame;
    if (KeGetCurrentProcessorNumber() == 0)
        KeUpdateSystemTime(TrapFrame, HalpCurrentTimeIncrement, TrapFrame->PreviousIrql);
    else
        KeUpdateRunTime(TrapFrame, TrapFrame->PreviousIrql);
    return TRUE;
}

/**
 * @brief Arms the generic timer of one processor and connects its clock interrupt there.
 */
VOID
NTAPI
HalpStartProcessorClock(
    _In_ ULONG ProcessorNumber)
{
    PKINTERRUPT Interrupt;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    ASSERT(ProcessorNumber < (ULONG)KeNumberProcessors);

    Interrupt = &HalpClockInterrupt[ProcessorNumber];
    KeInitializeInterrupt(Interrupt,
                          HalpClockInterruptHandler,
                          NULL,
                          NULL,
                          HalpPlatform.TimerIntId,
                          CLOCK_LEVEL,
                          CLOCK_LEVEL,
                          LevelSensitive,
                          FALSE,
                          (CHAR)ProcessorNumber,
                          FALSE);

    /* The timer registers are banked, so this runs on the processor being set up */
    KeSetSystemAffinityThread(AFFINITY_MASK(ProcessorNumber));
    ASSERT(KeGetCurrentProcessorNumber() == ProcessorNumber);
    KeArm64WriteRegister(ARM64_CNTV_CVAL_EL0, HalpReadCounter() + HalpClockPeriod);
    KeArm64WriteRegister(ARM64_CNTV_CTL_EL0, CNTV_CTL_ENABLE);
    __isb(_ARM64_BARRIER_SY);
    KeRevertToUserAffinityThread();

    /* The line stays off until this, connecting moves to the processor on its own */
    if (!KeConnectInterrupt(Interrupt))
        KeBugCheckEx(HAL_INITIALIZATION_FAILED, 0x7173, 1, HalpPlatform.TimerIntId, ProcessorNumber);
}

VOID
NTAPI
HalpStartClockInterrupt(VOID)
{
    HalpStartProcessorClock(0);
}

VOID
NTAPI
KeStallExecutionProcessor(
    _In_ ULONG MicroSeconds)
{
    ULONG64 Start, Ticks;

    Ticks = ((ULONG64)MicroSeconds * HalpTimerFrequency + 999999) / 1000000;
    Start = HalpReadCounter();

    while ((HalpReadCounter() - Start) < Ticks)
        YieldProcessor();
}

LARGE_INTEGER
NTAPI
KeQueryPerformanceCounter(
    _Out_opt_ PLARGE_INTEGER PerformanceFrequency)
{
    LARGE_INTEGER Value;

    if (PerformanceFrequency)
        PerformanceFrequency->QuadPart = HalpTimerFrequency;

    Value.QuadPart = HalpReadCounter();
    return Value;
}

VOID
NTAPI
HalCalibratePerformanceCounter(
    _In_ volatile PLONG Count,
    _In_ ULONGLONG NewCount)
{
    UNREFERENCED_PARAMETER(NewCount);

    /* The architected counter is shared by every processor */
    InterlockedDecrement(Count);
    while (*Count)
        YieldProcessor();
}

ULONG
NTAPI
HalSetTimeIncrement(
    _In_ ULONG Increment)
{
    /* The clock runs at one fixed rate */
    UNREFERENCED_PARAMETER(Increment);
    return HalpCurrentTimeIncrement;
}

ULONG_PTR
NTAPI
HalSetProfileInterval(
    _In_ ULONG_PTR Interval)
{
    return Interval;
}

VOID
NTAPI
HalStartProfileInterrupt(
    _In_ KPROFILE_SOURCE ProfileSource)
{
    UNREFERENCED_PARAMETER(ProfileSource);
}

VOID
NTAPI
HalStopProfileInterrupt(
    _In_ KPROFILE_SOURCE ProfileSource)
{
    UNREFERENCED_PARAMETER(ProfileSource);
}
