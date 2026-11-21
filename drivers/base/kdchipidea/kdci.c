/*
 * PROJECT:     ReactOS ChipIdea Kernel Debugger Transport
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KD transport entry points over a USB serial port on a ChipIdea device controller
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "kdci.h"
#include <stdio.h>

/* Generic timer registers, the counter runs at the rate firmware left in CNTFRQ */
#define KDCI_CNTFRQ_EL0                 ARM64_SYSREG(3, 3, 14, 0, 0)
#define KDCI_CNTVCT_EL0                 ARM64_SYSREG(3, 3, 14, 0, 2)

/* How long to wait for the host at boot, unless CIWAIT says otherwise */
#define KDCI_DEFAULT_WAIT_SECONDS       5

/* How long a byte the debugger protocol waits for may take to arrive */
#define KDCI_RECEIVE_TIMEOUT_US         50000

/* How long output may wait for a listening host before it is dropped */
#define KDCI_SEND_TIMEOUT_US            200000

/* How long the host has to stay quiet after configuring the port before the boot wait ends */
#define KDCI_QUIET_US                   1000000

ULONG64 KdCiLastEvent;

static BOOLEAN KdCiRunning;
static UCHAR KdCiRing[KDCI_RING_BYTES];
static ULONG KdCiRingHead;
static ULONG KdCiRingTail;

/* TIME ***********************************************************************/

ULONG64
NTAPI
KdCiMicroseconds(VOID)
{
    ULONG64 Frequency = _ReadStatusReg(KDCI_CNTFRQ_EL0) & 0xFFFFFFFF;
    ULONG64 Count;

    if (Frequency == 0)
        return 0;

    __isb(_ARM64_BARRIER_SY);
    Count = _ReadStatusReg(KDCI_CNTVCT_EL0);
    return (Count / Frequency) * 1000000 + ((Count % Frequency) * 1000000) / Frequency;
}

/**
 * @brief Busy-waits on the generic timer, which phase 0 can use before the HAL calibrates its stall.
 */
VOID
NTAPI
KdCiStall(
    _In_ ULONG Microseconds)
{
    ULONG64 Start = KdCiMicroseconds();

    if (Start == 0 && _ReadStatusReg(KDCI_CNTFRQ_EL0) == 0)
    {
        KeStallExecutionProcessor(Microseconds);
        return;
    }

    while (KdCiMicroseconds() - Start < Microseconds)
        YieldProcessor();
}

/* OPTIONS ********************************************************************/

/* Finds NAME at the start of a word of the load options and returns what follows it */
static
PCSTR
KdCiFindOption(
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
KdCiParseNumber(
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
KdCiRingUsed(VOID)
{
    return (KdCiRingHead + KDCI_RING_BYTES - KdCiRingTail) % KDCI_RING_BYTES;
}

/* Moves queued output to the host while it is listening */
static
VOID
KdCiPump(VOID)
{
    ULONG Chunk;
    ULONG Sent;

    KdCiService();

    if (!KdCiIsHostListening() || KdCiIsSending() || KdCiRingUsed() == 0)
        return;

    Chunk = (KdCiRingHead >= KdCiRingTail) ? KdCiRingHead - KdCiRingTail : KDCI_RING_BYTES - KdCiRingTail;
    Sent = KdCiSend(&KdCiRing[KdCiRingTail], Chunk);
    KdCiRingTail = (KdCiRingTail + Sent) % KDCI_RING_BYTES;
}

/*
 * Queues one byte. Without a listener the oldest output makes room; with one
 * it waits for the host, but only so long that a stuck host cannot stall the kernel.
 */
static
VOID
KdCiPutByte(
    _In_ UCHAR Byte)
{
    ULONG Next = (KdCiRingHead + 1) % KDCI_RING_BYTES;
    ULONG64 Start;

    if (Next == KdCiRingTail)
    {
        if (KdCiIsHostListening())
        {
            Start = KdCiMicroseconds();
            while (Next == KdCiRingTail && KdCiMicroseconds() - Start < KDCI_SEND_TIMEOUT_US)
                KdCiPump();
        }

        if (Next == KdCiRingTail)
            KdCiRingTail = (KdCiRingTail + 1) % KDCI_RING_BYTES;
    }

    KdCiRing[KdCiRingHead] = Byte;
    KdCiRingHead = Next;
}

/* BYTE LAYER FOR THE DEBUGGER PROTOCOL ***************************************/

VOID
NTAPI
KdpSendByte(
    _In_ UCHAR Byte)
{
    KdCiPutByte(Byte);

    /* Full packets go out as they fill, the rest when the protocol waits for an answer */
    if (KdCiRingUsed() >= KDCI_BULK_PACKET_HIGH)
        KdCiPump();
}

KDP_STATUS
NTAPI
KdpPollByte(
    _Out_ PUCHAR OutByte)
{
    KdCiPump();
    return KdCiTakeInput(OutByte) ? KDP_PACKET_RECEIVED : KDP_PACKET_TIMEOUT;
}

KDP_STATUS
NTAPI
KdpReceiveByte(
    _Out_ PUCHAR OutByte)
{
    ULONG64 Start = KdCiMicroseconds();

    /* With no debugger holding the port every packet would sit out its retries for nothing */
    do
    {
        KdCiPump();
        if (KdCiTakeInput(OutByte))
            return KDP_PACKET_RECEIVED;
    } while (KdCiIsHostListening() && KdCiMicroseconds() - Start < KDCI_RECEIVE_TIMEOUT_US);

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

    if (!KdCiRunning)
        return;

    for (i = 0; i < Length && String[i] != ANSI_NULL; i++)
    {
        if (String[i] == '\n')
            KdCiPutByte('\r');
        KdCiPutByte((UCHAR)String[i]);
    }

    KdCiPump();
}

/**
 * @brief Services the controller, so the host keeps the port and queued text drains while nothing prints.
 */
VOID
NTAPI
KdTransportPoll(VOID)
{
    if (KdCiRunning)
        KdCiPump();
}

/* KD TRANSPORT EXPORTS *******************************************************/

NTSTATUS
NTAPI
KdD0Transition(VOID)
{
    if (!KdCiRunning)
        return STATUS_SUCCESS;

    return KdCiPowerUp();
}

NTSTATUS
NTAPI
KdD3Transition(VOID)
{
    if (KdCiRunning)
        KdCiPowerDown();

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
 * Load options: CISOC=name picks the SoC instead of the ACPI tables, CIBASE=address
 * moves the controller and CIWAIT=seconds bounds the wait for the host.
 *
 * @param[in] LoaderBlock
 * The loader block at boot, or NULL when the link is brought back.
 */
NTSTATUS
NTAPI
KdDebuggerInitialize0(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    const KDCI_PLATFORM* Platform;
    PCSTR Options, Value;
    ULONG64 CoreBase;
    ULONG64 Start;
    ULONG WaitSeconds = KDCI_DEFAULT_WAIT_SECONDS;
    ULONG64 DmaBase;
    ULONG DmaBytes;
    CHAR Line[80];
    NTSTATUS Status;

    if (LoaderBlock == NULL)
        return KdCiRunning ? KdCiPowerUp() : STATUS_DEVICE_NOT_READY;

    if (KdCiRunning)
        return STATUS_SUCCESS;

    Options = LoaderBlock->LoadOptions;
    Platform = KdCiFindPlatform(LoaderBlock, KdCiFindOption(Options, "CISOC="));
    if (Platform == NULL)
        return STATUS_NOT_FOUND;

    CoreBase = Platform->CoreBase;
    Value = KdCiFindOption(Options, "CIBASE=");
    if (Value != NULL)
        CoreBase = KdCiParseNumber(Value);

    Value = KdCiFindOption(Options, "CIWAIT=");
    if (Value != NULL)
        WaitSeconds = (ULONG)KdCiParseNumber(Value);

    Status = KdCiStartController(LoaderBlock, Platform, CoreBase);
    if (!NT_SUCCESS(Status))
        return Status;

    KdCiRunning = TRUE;

    /* Nothing else runs this early, so the host gets the whole wait to set the port up */
    Start = KdCiMicroseconds();
    KdCiLastEvent = Start;
    while (KdCiMicroseconds() - Start < (ULONG64)WaitSeconds * 1000000)
    {
        KdCiService();
        if (KdCiIsConfigured() && KdCiMicroseconds() - KdCiLastEvent >= KDCI_QUIET_US)
            break;
    }

#ifndef _WINKD_
    KdCiDmaRange(&DmaBase, &DmaBytes);
    _snprintf(Line, sizeof(Line), "kdchipidea: %s, DMA block 0x%I64x to 0x%I64x\n",
              Platform->Name, DmaBase, DmaBase + DmaBytes - 1);
    Line[sizeof(Line) - 1] = ANSI_NULL;
    KdTransportPrint(Line, (ULONG)strlen(Line));
#else
    /* WinDbg owns the stream, so nothing but protocol packets may go out on it */
    UNREFERENCED_PARAMETER(DmaBase);
    UNREFERENCED_PARAMETER(DmaBytes);
    UNREFERENCED_PARAMETER(Line);
#endif
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
KdDebuggerInitialize1(
    _In_opt_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    UNREFERENCED_PARAMETER(LoaderBlock);
    return KdCiRunning ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}
