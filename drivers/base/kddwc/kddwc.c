/*
 * PROJECT:     ReactOS DWC3 Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KD transport entry points over a USB serial port on a Qualcomm DWC3 controller
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kddwc.h"
#include <stdio.h>

/* Generic timer registers, the counter runs from reset at the rate firmware left in CNTFRQ */
#define KDDWC_CNTFRQ_EL0                ARM64_SYSREG(3, 3, 14, 0, 0)
#define KDDWC_CNTVCT_EL0                ARM64_SYSREG(3, 3, 14, 0, 2)

/* How long to wait for the host to set the port up at boot, unless DWCWAIT says otherwise */
#define KDDWC_DEFAULT_WAIT_SECONDS      5

/* How long a byte the debugger protocol waits for may take to arrive */
#define KDDWC_RECEIVE_TIMEOUT_US        50000

/* How often the boot wait shows the controller state */
#define KDDWC_HEARTBEAT_US              250000

/* Iterations of the speed probe loop, about 100 us at full clock */
#define KDDWC_SPEED_LOOPS               50000

/* How long output may wait for a listening host before it is dropped */
#define KDDWC_SEND_TIMEOUT_US           200000

/* How long the host has to stay quiet after configuring the port before the boot wait ends */
#define KDDWC_QUIET_US                  1000000

/* Trace lines held while the host is busy */
#define KDDWC_LOG_BYTES                 0x8000
#define KDDWC_LOG_LINE                  240

PKDDWC_BOOT_PRINT KdDwcBootPrint;
BOOLEAN KdDwcTraceTraffic;
BOOLEAN KdDwcSkipRpmh;
BOOLEAN KdDwcHoldLog;
ULONG64 KdDwcLastEvent;

static CHAR KdDwcLogText[KDDWC_LOG_BYTES];
static ULONG KdDwcLogUsed;
static ULONG KdDwcLogDropped;

static BOOLEAN KdDwcRunning;
static UCHAR KdDwcRing[KDDWC_RING_BYTES];
static ULONG KdDwcRingHead;
static ULONG KdDwcRingTail;
static ULONG KdDwcDropped;

/* TIME ***********************************************************************/

ULONG64
NTAPI
KdDwcMicroseconds(VOID)
{
    ULONG64 Frequency = _ReadStatusReg(KDDWC_CNTFRQ_EL0) & 0xFFFFFFFF;
    ULONG64 Count;

    if (Frequency == 0)
        return 0;

    __isb(_ARM64_BARRIER_SY);
    Count = _ReadStatusReg(KDDWC_CNTVCT_EL0);
    return (Count / Frequency) * 1000000 + ((Count % Frequency) * 1000000) / Frequency;
}

/**
 * @brief Busy-waits on the generic timer, which phase 0 can use before the HAL calibrates its stall.
 */
VOID
NTAPI
KdDwcStall(
    _In_ ULONG Microseconds)
{
    ULONG64 Start = KdDwcMicroseconds();

    if (Start == 0 && _ReadStatusReg(KDDWC_CNTFRQ_EL0) == 0)
    {
        KeStallExecutionProcessor(Microseconds);
        return;
    }

    while (KdDwcMicroseconds() - Start < Microseconds)
        YieldProcessor();
}

/**
 * @brief Times a fixed loop several times and prints the fastest and slowest run.
 *
 * A processor clock that dropped slows every run, time taken away by a higher
 * exception level leaves some runs at full speed.
 */
VOID
NTAPI
KdDwcTraceSpeed(
    _In_ PCSTR Where)
{
    volatile ULONG Counter;
    ULONG64 Start, Elapsed;
    ULONG64 Fastest = ~0ULL;
    ULONG64 Slowest = 0;
    ULONG Run;

    if (!KdDwcTraceTraffic)
        return;

    for (Run = 0; Run < 8; Run++)
    {
        Start = KdDwcMicroseconds();
        for (Counter = 0; Counter < KDDWC_SPEED_LOOPS; Counter++);
        Elapsed = KdDwcMicroseconds() - Start;

        Fastest = min(Fastest, Elapsed);
        Slowest = max(Slowest, Elapsed);
    }

    KdDwcTrace("speed %s: %I64u to %I64u us\n", Where, Fastest, Slowest);
}

/* TRACE **********************************************************************/

/**
 * @brief Prints one trace line on the loader screen, or keeps it for later while the log is held.
 */
VOID
__cdecl
KdDwcLog(
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    CHAR Line[KDDWC_LOG_LINE];
    va_list Arguments;
    INT Prefix, Length;

    if (KdDwcBootPrint == NULL)
        return;

    Prefix = _snprintf(Line, sizeof(Line), "kddwc %I64u: ", KdDwcMicroseconds() / 1000);
    if (Prefix < 0)
        Prefix = 0;

    va_start(Arguments, Format);
    Length = _vsnprintf(Line + Prefix, sizeof(Line) - Prefix - 1, Format, Arguments);
    va_end(Arguments);

    if (Length < 0 || Length >= (INT)(sizeof(Line) - Prefix - 1))
        Length = sizeof(Line) - Prefix - 2;
    Line[Prefix + Length] = ANSI_NULL;
    Length += Prefix + 1;

    if (!KdDwcHoldLog)
    {
        KdDwcBootPrint("%s", Line);
        return;
    }

    if (KdDwcLogUsed + Length > sizeof(KdDwcLogText))
    {
        KdDwcLogDropped++;
        return;
    }

    RtlCopyMemory(&KdDwcLogText[KdDwcLogUsed], Line, Length);
    KdDwcLogUsed += Length;
}

/**
 * @brief Draws every held trace line.
 */
VOID
NTAPI
KdDwcFlushLog(VOID)
{
    ULONG Offset;

    if (KdDwcBootPrint != NULL)
    {
        for (Offset = 0; Offset < KdDwcLogUsed; Offset += (ULONG)strlen(&KdDwcLogText[Offset]) + 1)
            KdDwcBootPrint("%s", &KdDwcLogText[Offset]);

        if (KdDwcLogDropped != 0)
            KdDwcBootPrint("kddwc: %lu trace lines did not fit\n", KdDwcLogDropped);
    }

    KdDwcLogUsed = 0;
    KdDwcLogDropped = 0;
}

/* OPTIONS ********************************************************************/

/* Finds NAME at the start of a word of the load options and returns what follows it */
static
PCSTR
KdDwcFindOption(
    _In_opt_ PCSTR Options,
    _In_ PCSTR Name)
{
    SIZE_T Length = strlen(Name);
    PCSTR Current;

    if (Options == NULL)
        return NULL;

    for (Current = Options; *Current != ANSI_NULL; Current++)
    {
        if (Current != Options && Current[-1] != ' ' && Current[-1] != '/')
            continue;

        if (_strnicmp(Current, Name, Length) == 0)
            return Current + Length;
    }

    return NULL;
}

/* Reads a decimal or 0x prefixed hexadecimal number, stopping at the first other character */
static
ULONG64
KdDwcParseNumber(
    _In_ PCSTR String)
{
    ULONG64 Value = 0;
    ULONG Base = 10;
    ULONG Digit;
    CHAR Character;

    if (String[0] == '0' && (String[1] == 'x' || String[1] == 'X'))
    {
        Base = 16;
        String += 2;
    }

    for (;; String++)
    {
        Character = *String;
        if (Character >= 'a' && Character <= 'f')
            Character -= 'a' - 'A';
        if (Character >= '0' && Character <= '9')
            Digit = Character - '0';
        else if (Base == 16 && Character >= 'A' && Character <= 'F')
            Digit = Character - 'A' + 10;
        else
            break;

        Value = Value * Base + Digit;
    }

    return Value;
}

/* OUTPUT RING ****************************************************************/

static
ULONG
KdDwcRingUsed(VOID)
{
    return (KdDwcRingHead + KDDWC_RING_BYTES - KdDwcRingTail) % KDDWC_RING_BYTES;
}

/* Moves queued output to the host while it is listening */
static
VOID
KdDwcPump(VOID)
{
    ULONG Chunk;
    ULONG Sent;

    KdDwcService();

    if (!KdDwcIsHostListening() || KdDwcIsSending() || KdDwcRingUsed() == 0)
        return;

    /* One contiguous run of the ring per transfer */
    Chunk = (KdDwcRingHead >= KdDwcRingTail) ? KdDwcRingHead - KdDwcRingTail : KDDWC_RING_BYTES - KdDwcRingTail;
    Sent = KdDwcSend(&KdDwcRing[KdDwcRingTail], Chunk);
    KdDwcRingTail = (KdDwcRingTail + Sent) % KDDWC_RING_BYTES;
}

/*
 * Queues one byte. With nobody listening the oldest output makes room; with a
 * listener it waits for the host, up to a limit so a stuck host cannot stall
 * the kernel.
 */
static
VOID
KdDwcPutByte(
    _In_ UCHAR Byte)
{
    ULONG Next = (KdDwcRingHead + 1) % KDDWC_RING_BYTES;
    ULONG64 Start;

    if (Next == KdDwcRingTail)
    {
        if (KdDwcIsHostListening())
        {
            Start = KdDwcMicroseconds();
            while (Next == KdDwcRingTail && KdDwcMicroseconds() - Start < KDDWC_SEND_TIMEOUT_US)
                KdDwcPump();
        }

        if (Next == KdDwcRingTail)
        {
            KdDwcRingTail = (KdDwcRingTail + 1) % KDDWC_RING_BYTES;
            KdDwcDropped++;
        }
    }

    KdDwcRing[KdDwcRingHead] = Byte;
    KdDwcRingHead = Next;
}

/* BYTE LAYER FOR THE DEBUGGER PROTOCOL ***************************************/

VOID
NTAPI
KdpSendByte(
    _In_ UCHAR Byte)
{
    KdDwcPutByte(Byte);

    /* Full packets go out as they fill, the rest when the protocol waits for an answer */
    if (KdDwcRingUsed() >= KDDWC_BULK_PACKET_HIGH)
        KdDwcPump();
}

KDP_STATUS
NTAPI
KdpPollByte(
    _Out_ PUCHAR OutByte)
{
    KdDwcPump();
    return KdDwcTakeInput(OutByte) ? KDP_PACKET_RECEIVED : KDP_PACKET_TIMEOUT;
}

KDP_STATUS
NTAPI
KdpReceiveByte(
    _Out_ PUCHAR OutByte)
{
    ULONG64 Start = KdDwcMicroseconds();

    do
    {
        KdDwcPump();
        if (KdDwcTakeInput(OutByte))
            return KDP_PACKET_RECEIVED;
    } while (KdDwcMicroseconds() - Start < KDDWC_RECEIVE_TIMEOUT_US);

    return KDP_PACKET_TIMEOUT;
}

KDP_STATUS
NTAPI
KdpPollBreakIn(VOID)
{
    UCHAR Byte;

    if (KdpPollByte(&Byte) == KDP_PACKET_RECEIVED && Byte == BREAKIN_PACKET_BYTE)
        return KDP_PACKET_RECEIVED;

    return KDP_PACKET_TIMEOUT;
}

/* TEXT OUTPUT FOR THE REACTOS DEBUG PRINT PROVIDER ***************************/

/**
 * @brief Queues text for a terminal on the host, line ends as a terminal wants them.
 */
VOID
NTAPI
KdTransportPrint(
    _In_reads_bytes_(Length) PCCH String,
    _In_ ULONG Length)
{
    ULONG i;

    if (!KdDwcRunning)
        return;

    for (i = 0; i < Length && String[i] != ANSI_NULL; i++)
    {
        if (String[i] == '\n')
            KdDwcPutByte('\r');
        KdDwcPutByte((UCHAR)String[i]);
    }

    KdDwcPump();
}

/**
 * @brief Services the controller, so the host keeps the port and queued text drains while nothing prints.
 */
VOID
NTAPI
KdTransportPoll(VOID)
{
    if (KdDwcRunning)
        KdDwcPump();
}

/* KD TRANSPORT EXPORTS *******************************************************/

/**
 * @brief The debug device has power again, so the controller is brought back now.
 */
NTSTATUS
NTAPI
KdD0Transition(VOID)
{
    if (!KdDwcRunning)
        return STATUS_SUCCESS;

    return KdDwcPowerUp();
}

/**
 * @brief The debug device is losing power, so the controller is left alone until it returns.
 */
NTSTATUS
NTAPI
KdD3Transition(VOID)
{
    if (KdDwcRunning)
        KdDwcPowerDown();

    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KdSave(
    _In_ BOOLEAN SleepTransition)
{
    UNREFERENCED_PARAMETER(SleepTransition);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KdRestore(
    _In_ BOOLEAN SleepTransition)
{
    UNREFERENCED_PARAMETER(SleepTransition);
    return STATUS_SUCCESS;
}

VOID
NTAPI
KdSetHiberRange(VOID)
{
}

/**
 * @brief Finds the SoC, takes the controller from firmware and waits a while for the host.
 *
 * Load options: DWCSOC=name picks the SoC instead of the ACPI tables, DWCBASE=address
 * moves the controller, DWCWAIT=seconds bounds the wait for the host and
 * FRLDRSCREENLOG traces the bring-up on the loader's screen.
 *
 * @param[in] LoaderBlock
 * The loader block at boot, or NULL when the link is brought back.
 */
NTSTATUS
NTAPI
KdDebuggerInitialize0(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    const KDDWC_PLATFORM* Platform;
    PCSTR Options, Value;
    ULONG64 CoreBase;
    ULONG64 Start;
    ULONG64 Beat;
    ULONG WaitSeconds = KDDWC_DEFAULT_WAIT_SECONDS;
    NTSTATUS Status;

    /* Without a loader block the link is being brought back, after a power loss for one */
    if (LoaderBlock == NULL)
        return KdDwcRunning ? KdDwcPowerUp() : STATUS_DEVICE_NOT_READY;

    if (KdDwcRunning)
        return STATUS_SUCCESS;

    Options = LoaderBlock->LoadOptions;
    if (KdDwcFindOption(Options, "FRLDRSCREENLOG") != NULL)
        KdDwcBootPrint = (PKDDWC_BOOT_PRINT)LoaderBlock->u.I386.CommonDataArea;
    KdDwcTraceTraffic = (KdDwcBootPrint != NULL) && (KdDwcFindOption(Options, "DWCTRACE") != NULL);
    KdDwcSkipRpmh = (KdDwcFindOption(Options, "DWCNORPMH") != NULL);
    KdDwcTrace("starting, options '%s'\n", Options ? Options : "");

    Value = KdDwcFindOption(Options, "DWCSOC=");
    KdDwcTrace("looking for the SoC %s\n", Value ? "by name" : "in the ACPI tables");
    Platform = KdDwcFindPlatform(LoaderBlock, Value);
    if (Platform == NULL)
    {
        KdDwcTrace("no supported SoC found\n");
        KdDwcBootPrint = NULL;
        return STATUS_NOT_FOUND;
    }
    KdDwcTrace("SoC %s\n", Platform->Name);

    CoreBase = Platform->CoreBase;
    Value = KdDwcFindOption(Options, "DWCBASE=");
    if (Value != NULL)
        CoreBase = KdDwcParseNumber(Value);

    Value = KdDwcFindOption(Options, "DWCWAIT=");
    if (Value != NULL)
        WaitSeconds = (ULONG)KdDwcParseNumber(Value);

    KdDwcTrace("starting the controller at 0x%I64x\n", CoreBase);
    Status = KdDwcStartController(LoaderBlock, Platform, CoreBase);
    if (!NT_SUCCESS(Status))
    {
        KdDwcTrace("controller did not start: 0x%lx\n", Status);
        KdDwcBootPrint = NULL;
        return Status;
    }

    KdDwcRunning = TRUE;
    KdDwcTrace("waiting %lu s for the host\n", WaitSeconds);

    /*
     * Nothing else runs this early, so the host gets the whole wait to set the
     * port up, and the requests that follow the configuration are answered here
     * too. The trace is only drawn once the host is done.
     */
    KdDwcHoldLog = TRUE;
    Start = KdDwcMicroseconds();
    Beat = Start;
    KdDwcLastEvent = Start;
    while (KdDwcMicroseconds() - Start < (ULONG64)WaitSeconds * 1000000)
    {
        KdDwcService();
        if (KdDwcIsConfigured() && KdDwcMicroseconds() - KdDwcLastEvent >= KDDWC_QUIET_US)
            break;

        if (KdDwcTraceTraffic && KdDwcMicroseconds() - Beat >= KDDWC_HEARTBEAT_US)
        {
            Beat = KdDwcMicroseconds();
            KdDwcTrace("%I64u ms\n", (Beat - Start) / 1000);
            KdDwcTraceState();
        }
    }

    KdDwcTrace("host %s\n", KdDwcIsConfigured() ? "configured the port" : "has not configured the port yet");
    KdDwcHoldLog = FALSE;
    KdDwcFlushLog();
    KdDwcTraceSpeed("after the wait");
    KdDwcTraceTraffic = FALSE;
    KdDwcBootPrint = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KdDebuggerInitialize1(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    UNREFERENCED_PARAMETER(LoaderBlock);
    return KdDwcRunning ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}
