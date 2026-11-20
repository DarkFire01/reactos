/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Power Manager Modern Standby support (Always On & Always Connected)
 * COPYRIGHT:   Copyright 2023 George Bișoc <george.bisoc@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#include <drivers/acpi/acpi.h>
#define NDEBUG
#include <debug.h>

/* The FADT flag a platform sets when it idles in S0 instead of sleeping, revision 5 and later */
#define POP_FADT_LOW_POWER_S0_IDLE_CAPABLE  0x00200000
#define POP_FADT_REVISION_LOW_POWER_S0      5

/* GLOBALS ********************************************************************/

BOOLEAN PopAoAcPresent = FALSE;

/* The console display is off and the platform idles in connected standby */
BOOLEAN PopAoAcInStandby = FALSE;

#define TAG_PO_AOAC_THROTTLE    'tAoP'

/* A desktop process moved to idle priority for the standby, and the class it had */
typedef struct _POP_AOAC_THROTTLED
{
    LIST_ENTRY Link;
    PEPROCESS Process;
    UCHAR PriorityClass;
} POP_AOAC_THROTTLED, *PPOP_AOAC_THROTTLED;

static LIST_ENTRY PopAoAcThrottledList = { &PopAoAcThrottledList, &PopAoAcThrottledList };

/* The shell and the session plumbing keep their priority, as on Windows they are never moderated */
static const CHAR *PopAoAcExemptImages[] =
{
    "smss.exe",
    "csrss.exe",
    "wininit.exe",
    "winlogon.exe",
    "services.exe",
    "lsass.exe",
    "dwm.exe",
    "explorer.exe",
    "mobileshell.exe",
};

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief
 * Determines whether the platform is a Modern Standby one, by checking the
 * low power S0 idle capability the firmware declares in its FADT.
 *
 * @param[in] LoaderBlock
 * The loader parameter block the firmware tables came with.
 */
CODE_SEG("INIT")
VOID
NTAPI
PopInitializeAoAc(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PFADT Fadt;

    /* Only a HAL that knows its firmware tables can tell us */
    if (!KdGetAcpiTablePhase0)
        return;

    Fadt = KdGetAcpiTablePhase0(LoaderBlock, FADT_SIGNATURE);
    if (!Fadt ||
        Fadt->Header.Revision < POP_FADT_REVISION_LOW_POWER_S0 ||
        Fadt->Header.Length < RTL_SIZEOF_THROUGH_FIELD(FADT, flags))
    {
        return;
    }

    if (Fadt->flags & POP_FADT_LOW_POWER_S0_IDLE_CAPABLE)
    {
        PopAoAcPresent = TRUE;
        DPRINT1("The platform supports Modern Standby\n");
    }
}

static
BOOLEAN
PopAoAcIsImage(
    _In_ PEPROCESS Process,
    _In_z_ const CHAR *Name)
{
    return _strnicmp((const CHAR *)Process->ImageFileName, Name, sizeof(Process->ImageFileName)) == 0;
}

/**
 * @brief
 * Tells whether a process is a desktop app the standby slows down. System
 * processes, critical ones, services and the shell keep running as they are.
 */
static
BOOLEAN
PopAoAcIsModerated(
    _In_ PEPROCESS Process,
    _In_opt_ HANDLE ServicesId)
{
    ULONG i;

    if (Process == PsInitialSystemProcess || Process == PsIdleProcess)
        return FALSE;
    if (Process->BreakOnTermination || Process->ProcessDelete)
        return FALSE;

    /* Children of the service control manager are services */
    if (ServicesId && Process->InheritedFromUniqueProcessId == ServicesId)
        return FALSE;

    for (i = 0; i < RTL_NUMBER_OF(PopAoAcExemptImages); i++)
    {
        if (PopAoAcIsImage(Process, PopAoAcExemptImages[i]))
            return FALSE;
    }

    return Process->PriorityClass != PROCESS_PRIORITY_CLASS_IDLE &&
           Process->PriorityClass != PROCESS_PRIORITY_CLASS_REALTIME;
}

/**
 * @brief
 * Drops every desktop app to idle priority for the time the display is off,
 * remembering the class each had so the wake gives it back.
 */
static
VOID
PopAoAcThrottleProcesses(VOID)
{
    PPOP_AOAC_THROTTLED Entry;
    PEPROCESS Process;
    HANDLE ServicesId = NULL;
    ULONG Count = 0;

    PAGED_CODE();

    for (Process = PsGetNextProcess(NULL); Process; Process = PsGetNextProcess(Process))
    {
        if (PopAoAcIsImage(Process, "services.exe"))
            ServicesId = Process->UniqueProcessId;
    }

    for (Process = PsGetNextProcess(NULL); Process; Process = PsGetNextProcess(Process))
    {
        if (!PopAoAcIsModerated(Process, ServicesId))
            continue;

        Entry = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Entry), TAG_PO_AOAC_THROTTLE);
        if (!Entry)
            continue;

        ObReferenceObject(Process);
        Entry->Process = Process;
        Entry->PriorityClass = Process->PriorityClass;
        InsertTailList(&PopAoAcThrottledList, &Entry->Link);

        Process->PriorityClass = PROCESS_PRIORITY_CLASS_IDLE;
        PsSetProcessPriorityByClass(Process, PsProcessPriorityBackground);
        Count++;
    }

    DPRINT1("Connected standby moved %lu processes to idle priority\n", Count);
}

/**
 * @brief
 * Gives every throttled process its old class back. A process that changed
 * its class itself meanwhile keeps what it chose.
 */
static
VOID
PopAoAcRestoreProcesses(VOID)
{
    PPOP_AOAC_THROTTLED Entry;
    PLIST_ENTRY Link;

    PAGED_CODE();

    while (!IsListEmpty(&PopAoAcThrottledList))
    {
        Link = RemoveHeadList(&PopAoAcThrottledList);
        Entry = CONTAINING_RECORD(Link, POP_AOAC_THROTTLED, Link);

        if (!Entry->Process->ProcessDelete &&
            Entry->Process->PriorityClass == PROCESS_PRIORITY_CLASS_IDLE)
        {
            Entry->Process->PriorityClass = Entry->PriorityClass;
            PsSetProcessPriorityByClass(Entry->Process, PsProcessPriorityBackground);
        }

        ObDereferenceObject(Entry->Process);
        ExFreePoolWithTag(Entry, TAG_PO_AOAC_THROTTLE);
    }
}

/* Drivers watching the display find out through the console and session display settings */
static
VOID
PopAoAcNotifyDisplayState(VOID)
{
    PopNotifyPowerSettingChange(&GUID_CONSOLE_DISPLAY_STATE);
    PopNotifyPowerSettingChange(&GUID_SESSION_DISPLAY_STATE);
    PopNotifyPowerSettingChange(&GUID_MONITOR_POWER_ON);
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief
 * Handles a power button sleep request on a Modern Standby platform. Instead
 * of entering a sleep state the console display goes off and the platform
 * stays in S0, the next press brings the display back.
 */
VOID
NTAPI
PopAoAcHandleSleepButton(VOID)
{
    WIN32_POWEREVENT_PARAMETERS Params;

    PAGED_CODE();

    PopAoAcInStandby = !PopAoAcInStandby;
    if (PopAoAcInStandby)
    {
        DPRINT1("Entering connected standby, turning the display off\n");
        Params.EventNumber = PsW32DisplayState;
        Params.Code = 0;
        PopDispatchPowerEvent(&Params);

        PopAoAcNotifyDisplayState();
        PopAoAcThrottleProcesses();
        return;
    }

    DPRINT1("Leaving connected standby, turning the display on\n");
    PopAoAcRestoreProcesses();

    Params.EventNumber = PsW32DisplayState;
    Params.Code = 1;
    PopDispatchPowerEvent(&Params);

    Params.EventNumber = PsW32FullWake;
    Params.Code = 0;
    PopDispatchPowerEvent(&Params);

    PopAoAcNotifyDisplayState();
}

/* EOF */
