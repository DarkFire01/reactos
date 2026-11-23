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
POWER_PLATFORM_ROLE PopPlatformRole = PlatformRoleUnspecified;

/* The console display is off and the platform idles in connected standby */
BOOLEAN PopAoAcInStandby = FALSE;

#define TAG_PO_AOAC_THROTTLE    'tAoP'

/* Desktop apps get this long with the display off to finish what they were doing */
#define POP_AOAC_FREEZE_DELAY_MS    30000

/* Devices are directed to low power once the display has been off this long */
#define POP_AOAC_DIRECTED_DELAY_MS  5000

/* A desktop process moderated for the standby, the class it had and whether it is frozen */
typedef struct _POP_AOAC_THROTTLED
{
    LIST_ENTRY Link;
    PEPROCESS Process;
    UCHAR PriorityClass;
    BOOLEAN Frozen;

    /* An activator let the app run until this interrupt time */
    ULONGLONG ActiveUntil;
} POP_AOAC_THROTTLED, *PPOP_AOAC_THROTTLED;

/* Bounds of the time an activator may let a frozen app run */
#define POP_AOAC_ACTIVATE_MIN_MS    1000
#define POP_AOAC_ACTIVATE_MAX_MS    300000

static LIST_ENTRY PopAoAcThrottledList = { &PopAoAcThrottledList, &PopAoAcThrottledList };

/* Guards the standby state and the list against the delayed freeze */
static FAST_MUTEX PopAoAcLock;
static KTIMER PopAoAcFreezeTimer;
static KDPC PopAoAcFreezeDpc;
static WORK_QUEUE_ITEM PopAoAcFreezeItem;
static volatile LONG PopAoAcFreezeQueued;

/* When the freeze timer is due, 0 while it is not set */
static ULONGLONG PopAoAcFreezeDue;

static KDEFERRED_ROUTINE PopAoAcFreezeDpcRoutine;
static WORKER_THREAD_ROUTINE PopAoAcFreezeWorker;

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

    ExInitializeFastMutex(&PopAoAcLock);
    KeInitializeTimer(&PopAoAcFreezeTimer);
    KeInitializeDpc(&PopAoAcFreezeDpc, PopAoAcFreezeDpcRoutine, NULL);
    ExInitializeWorkItem(&PopAoAcFreezeItem, PopAoAcFreezeWorker, NULL);

    /* Only a HAL that knows its firmware tables can tell us */
    if (!KdGetAcpiTablePhase0)
        return;

    Fadt = KdGetAcpiTablePhase0(LoaderBlock, FADT_SIGNATURE);

    /* The preferred PM profile is the platform role, from FADT revision 3 on */
    if (Fadt &&
        Fadt->Header.Revision >= 3 &&
        Fadt->Header.Length >= RTL_SIZEOF_THROUGH_FIELD(FADT, pm_profile) &&
        Fadt->pm_profile < PlatformRoleMaximum)
    {
        PopPlatformRole = (POWER_PLATFORM_ROLE)Fadt->pm_profile;
    }

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
        Entry->Frozen = FALSE;
        Entry->ActiveUntil = 0;
        InsertTailList(&PopAoAcThrottledList, &Entry->Link);

        Process->PriorityClass = PROCESS_PRIORITY_CLASS_IDLE;
        PsSetProcessPriorityByClass(Process, PsProcessPriorityBackground);
        Count++;
    }

    DPRINT1("Connected standby moved %lu processes to idle priority\n", Count);
}

/**
 * @brief
 * The grace period is over, the moderated apps stop until the display comes
 * back. Runs as a work item, the standby may have ended meanwhile.
 */
/* Moves the freeze timer earlier, never later. Called with the lock held. */
static
VOID
PopAoAcScheduleFreeze(
    _In_ ULONGLONG Due)
{
    ULONGLONG Now = KeQueryInterruptTime();
    LARGE_INTEGER DueTime;

    if (PopAoAcFreezeDue && PopAoAcFreezeDue <= Due)
        return;

    PopAoAcFreezeDue = Due;
    DueTime.QuadPart = -(LONGLONG)max(Due > Now ? Due - Now : 0, 10000);
    KeSetTimer(&PopAoAcFreezeTimer, DueTime, &PopAoAcFreezeDpc);
}

static
VOID
NTAPI
PopAoAcFreezeWorker(
    _In_ PVOID Parameter)
{
    PPOP_AOAC_THROTTLED Entry;
    PLIST_ENTRY Link;
    ULONGLONG Now, Next = 0;
    ULONG Count = 0;

    UNREFERENCED_PARAMETER(Parameter);

    InterlockedExchange(&PopAoAcFreezeQueued, 0);

    ExAcquireFastMutex(&PopAoAcLock);
    PopAoAcFreezeDue = 0;
    if (PopAoAcInStandby)
    {
        Now = KeQueryInterruptTime();
        for (Link = PopAoAcThrottledList.Flink; Link != &PopAoAcThrottledList; Link = Link->Flink)
        {
            Entry = CONTAINING_RECORD(Link, POP_AOAC_THROTTLED, Link);
            if (Entry->Frozen || Entry->Process->ProcessDelete)
                continue;

            /* Still inside its activation, it is frozen once the time is up */
            if (Entry->ActiveUntil > Now)
            {
                if (!Next || Entry->ActiveUntil < Next)
                    Next = Entry->ActiveUntil;
                continue;
            }

            /* An app that asked to keep working, a download or playback, only stays slowed */
            if (PopProcessKeepsSystemAwake(Entry->Process->UniqueProcessId))
            {
                DPRINT1("Connected standby leaves %.16s running, it holds a power request\n",
                        Entry->Process->ImageFileName);
                continue;
            }

            Entry->Frozen = KeFreezeProcess(&Entry->Process->Pcb);
            if (Entry->Frozen)
                Count++;
        }

        if (Count)
            DPRINT1("Connected standby froze %lu desktop processes\n", Count);
        if (Next)
            PopAoAcScheduleFreeze(Next);
    }
    ExReleaseFastMutex(&PopAoAcLock);
}

static
VOID
NTAPI
PopAoAcFreezeDpcRoutine(
    _In_ PKDPC Dpc,
    _In_opt_ PVOID DeferredContext,
    _In_opt_ PVOID SystemArgument1,
    _In_opt_ PVOID SystemArgument2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(DeferredContext);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    /* One work item, a second timer expiry before it ran finds it queued */
    if (!InterlockedExchange(&PopAoAcFreezeQueued, 1))
        ExQueueWorkItem(&PopAoAcFreezeItem, DelayedWorkQueue);
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

        if (Entry->Frozen)
            KeThawProcess(&Entry->Process->Pcb);

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
    PopNotifyPowerSettingChange(&PopIdleResiliencySettingGuid);
    PopNotifyPowerSettingChange(&PopLowPowerPhaseSettingGuid);
}

/* The display events call into win32k, so they go out with the lock dropped */
static
VOID
PopAoAcEnterStandby(VOID)
{
    WIN32_POWEREVENT_PARAMETERS Params;

    ExAcquireFastMutex(&PopAoAcLock);
    if (PopAoAcInStandby)
    {
        ExReleaseFastMutex(&PopAoAcLock);
        return;
    }
    PopAoAcInStandby = TRUE;
    ExReleaseFastMutex(&PopAoAcLock);

    DPRINT1("Entering connected standby, turning the display off\n");
    Params.EventNumber = PsW32DisplayState;
    Params.Code = 0;
    PopDispatchPowerEvent(&Params);
    PopAoAcNotifyDisplayState();

    ExAcquireFastMutex(&PopAoAcLock);
    if (PopAoAcInStandby)
    {
        PopAoAcThrottleProcesses();
        PopAoAcScheduleFreeze(KeQueryInterruptTime() + 10000ULL * POP_AOAC_FREEZE_DELAY_MS);
    }
    ExReleaseFastMutex(&PopAoAcLock);

    /* The processors drop to their lowest performance while the display is off */
    PpmConnectedStandby(TRUE);

    PopFxScheduleDirectedPowerDown(POP_AOAC_DIRECTED_DELAY_MS);
}

/* Apps run again before win32k sends them the display on messages */
static
BOOLEAN
PopAoAcLeaveStandby(VOID)
{
    WIN32_POWEREVENT_PARAMETERS Params;

    ExAcquireFastMutex(&PopAoAcLock);
    if (!PopAoAcInStandby)
    {
        ExReleaseFastMutex(&PopAoAcLock);
        return FALSE;
    }

    DPRINT1("Leaving connected standby, turning the display on\n");
    PopAoAcInStandby = FALSE;
    KeCancelTimer(&PopAoAcFreezeTimer);
    PopAoAcFreezeDue = 0;
    PopAoAcRestoreProcesses();
    ExReleaseFastMutex(&PopAoAcLock);

    /* Devices come back before the display does, the panel and touch may be among them */
    PopFxDirectedPowerUp();
    PpmConnectedStandby(FALSE);

    Params.EventNumber = PsW32DisplayState;
    Params.Code = 1;
    PopDispatchPowerEvent(&Params);

    Params.EventNumber = PsW32FullWake;
    Params.Code = 0;
    PopDispatchPowerEvent(&Params);

    PopAoAcNotifyDisplayState();
    return TRUE;
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
    BOOLEAN InStandby;

    PAGED_CODE();

    ExAcquireFastMutex(&PopAoAcLock);
    InStandby = PopAoAcInStandby;
    ExReleaseFastMutex(&PopAoAcLock);

    if (InStandby)
        PopAoAcLeaveStandby();
    else
        PopAoAcEnterStandby();
}

/**
 * @brief
 * A sleep request on a Modern Standby platform enters connected standby, the
 * platform stays in S0 with the display off.
 *
 * @return
 * STATUS_SUCCESS, also when the platform was in connected standby already.
 */
NTSTATUS
NTAPI
PopAoAcSleep(VOID)
{
    PAGED_CODE();

    PopAoAcEnterStandby();
    return STATUS_SUCCESS;
}

/**
 * @brief
 * A wake trigger fired, an alarm or an incoming call. The display comes back
 * on as if the power button had been pressed.
 *
 * @return
 * STATUS_SUCCESS when the platform left connected standby, STATUS_ALREADY_COMPLETE
 * when the display was on already.
 */
NTSTATUS
NTAPI
PopAoAcWake(VOID)
{
    PAGED_CODE();

    return PopAoAcLeaveStandby() ? STATUS_SUCCESS : STATUS_ALREADY_COMPLETE;
}

/**
 * @brief
 * Lets one moderated app run for a while with the display still off, the
 * way a background trigger does. It is frozen again once the time is up.
 *
 * @param[in] ProcessId
 * The app to run.
 *
 * @param[in] BudgetMs
 * How long it may run, clamped to the activation bounds.
 *
 * @return
 * STATUS_SUCCESS also when the app is not moderated, it runs already.
 */
NTSTATUS
NTAPI
PopAoAcActivateProcess(
    _In_ HANDLE ProcessId,
    _In_ ULONG BudgetMs)
{
    PPOP_AOAC_THROTTLED Entry;
    PLIST_ENTRY Link;
    ULONGLONG Until;

    PAGED_CODE();

    BudgetMs = min(max(BudgetMs, POP_AOAC_ACTIVATE_MIN_MS), POP_AOAC_ACTIVATE_MAX_MS);

    ExAcquireFastMutex(&PopAoAcLock);
    if (!PopAoAcInStandby)
    {
        ExReleaseFastMutex(&PopAoAcLock);
        return STATUS_SUCCESS;
    }

    for (Link = PopAoAcThrottledList.Flink; Link != &PopAoAcThrottledList; Link = Link->Flink)
    {
        Entry = CONTAINING_RECORD(Link, POP_AOAC_THROTTLED, Link);
        if (Entry->Process->UniqueProcessId != ProcessId)
            continue;

        Until = KeQueryInterruptTime() + 10000ULL * BudgetMs;
        if (Until > Entry->ActiveUntil)
            Entry->ActiveUntil = Until;

        if (Entry->Frozen)
        {
            KeThawProcess(&Entry->Process->Pcb);
            Entry->Frozen = FALSE;
        }

        PopAoAcScheduleFreeze(Entry->ActiveUntil);
        DPRINT1("Connected standby lets %.16s run for %lu ms\n", Entry->Process->ImageFileName, BudgetMs);
        break;
    }
    ExReleaseFastMutex(&PopAoAcLock);

    return STATUS_SUCCESS;
}

/* EOF */
