/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared bring-up log ring and fatal freeze
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <ntifs.h>
#include <ndk/inbvfuncs.h>
#include <ntstrsafe.h>
#include <drivers/bootvid/display.h>
#include <qcom/qcomdbg.h>

/* Lines kept for the fatal screen, a 640x480 boot screen holds about 36 */
#define QCDBG_RING_LINES        64
#define QCDBG_SCREEN_LINES      33
#define QCDBG_FORMAT_LENGTH     256

static CHAR QcDbgRing[QCDBG_RING_LINES][QCDBG_LINE_LENGTH + 1];
static ULONG QcDbgNextLine;
static ULONG QcDbgLineCount;
static KSPIN_LOCK QcDbgLock;
static volatile LONG QcDbgFreezing;
static volatile LONG QcDbgScreenOwner;

static
VOID
QcDbgStoreLine(
    _In_z_ PCSTR Line)
{
    KIRQL OldIrql = PASSIVE_LEVEL;
    BOOLEAN Locked = FALSE;

    /* Above dispatch only the fatal path runs and the other processors are stopping */
    if (KeGetCurrentIrql() <= DISPATCH_LEVEL)
    {
        KeAcquireSpinLock(&QcDbgLock, &OldIrql);
        Locked = TRUE;
    }

    RtlStringCbCopyA(QcDbgRing[QcDbgNextLine], sizeof(QcDbgRing[0]), Line);
    QcDbgNextLine = (QcDbgNextLine + 1) % QCDBG_RING_LINES;
    if (QcDbgLineCount < QCDBG_RING_LINES)
        QcDbgLineCount++;

    if (Locked)
        KeReleaseSpinLock(&QcDbgLock, OldIrql);
}

static
VOID
QcDbgFormatAndStore(
    _In_z_ PCSTR Component,
    _In_z_ PCSTR Format,
    _In_ va_list Arguments)
{
    CHAR Message[QCDBG_FORMAT_LENGTH];
    CHAR Line[QCDBG_FORMAT_LENGTH + 32];
    SIZE_T Length;

    if (!NT_SUCCESS(RtlStringCbVPrintfA(Message, sizeof(Message), Format, Arguments)))
        Message[sizeof(Message) - 1] = ANSI_NULL;

    /* Callers may end lines with a newline, the ring stores bare lines */
    Length = strlen(Message);
    while (Length && (Message[Length - 1] == '\n' || Message[Length - 1] == '\r'))
    {
        Length -= 1;
        Message[Length] = ANSI_NULL;
    }

    RtlStringCbPrintfA(Line, sizeof(Line), "[%s] %s", Component, Message);
    DbgPrint("%s\n", Line);
    QcDbgStoreLine(Line);
}

VOID
__cdecl
QcDbgLog(
    _In_z_ PCSTR Component,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    va_list Arguments;

    va_start(Arguments, Format);
    QcDbgFormatAndStore(Component, Format, Arguments);
    va_end(Arguments);
}

static
VOID
QcDbgShowRing(VOID)
{
    ULONG Shown;
    ULONG Index;
    ULONG Line;

    if (!InbvIsBootDriverInstalled())
        return;

    /* Win32k may own the display by now, take it back like the screen debugger does */
    InbvAcquireDisplayOwnership();
    InbvResetDisplay();
    InbvSolidColorFill(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, BV_COLOR_BLUE);
    InbvSetTextColor(BV_COLOR_WHITE);
    InbvInstallDisplayStringFilter(NULL);
    InbvEnableDisplayString(TRUE);
    InbvSetScrollRegion(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);

    InbvDisplayString((PCHAR)"QCOM BRING-UP FAILURE, SYSTEM FROZEN\r\n");

    Shown = min(QcDbgLineCount, QCDBG_SCREEN_LINES);
    Index = (QcDbgNextLine + QCDBG_RING_LINES - Shown) % QCDBG_RING_LINES;
    for (Line = 0; Line < Shown; Line++)
    {
        InbvDisplayString(QcDbgRing[Index]);
        InbvDisplayString((PCHAR)"\r\n");
        Index = (Index + 1) % QCDBG_RING_LINES;
    }
}

static
DECLSPEC_NORETURN
VOID
QcDbgSpinForever(VOID)
{
    for (;;)
        YieldProcessor();
}

static
ULONG_PTR
NTAPI
QcDbgFreezeWorker(
    _In_ ULONG_PTR Argument)
{
    UNREFERENCED_PARAMETER(Argument);

    /* Redraw once everyone is parked so nothing paints over the log */
    if (InterlockedCompareExchange(&QcDbgScreenOwner, 1, 0) == 0)
        QcDbgShowRing();

    QcDbgSpinForever();
}

DECLSPEC_NORETURN
VOID
__cdecl
QcDbgFatal(
    _In_z_ PCSTR Component,
    _In_ NTSTATUS Status,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    CHAR Reason[QCDBG_FORMAT_LENGTH];
    va_list Arguments;
    KIRQL OldIrql;

    va_start(Arguments, Format);
    if (!NT_SUCCESS(RtlStringCbVPrintfA(Reason, sizeof(Reason), Format, Arguments)))
        Reason[sizeof(Reason) - 1] = ANSI_NULL;
    va_end(Arguments);

    /* A second failure while freezing just parks this processor */
    if (InterlockedExchange(&QcDbgFreezing, 1) != 0)
    {
        if (KeGetCurrentIrql() < HIGH_LEVEL)
            KeRaiseIrql(HIGH_LEVEL, &OldIrql);
        QcDbgSpinForever();
    }

    QcDbgLog(Component, "FATAL 0x%08lx: %s", Status, Reason);

    /* Show the log first in case the other processors never answer the freeze */
    QcDbgShowRing();

    if (KeGetCurrentIrql() <= DISPATCH_LEVEL)
        KeIpiGenericCall(QcDbgFreezeWorker, 0);

    if (KeGetCurrentIrql() < HIGH_LEVEL)
        KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    QcDbgSpinForever();
}

NTSTATUS
NTAPI
DllInitialize(
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);

    KeInitializeSpinLock(&QcDbgLock);
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DllUnload(VOID)
{
    return STATUS_SUCCESS;
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    /* Export driver only, it never runs as a service */
    return STATUS_SUCCESS;
}
