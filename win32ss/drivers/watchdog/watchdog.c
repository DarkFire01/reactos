
#include <ntifs.h>

#define NDEBUG
#include <debug.h>

NTSTATUS
NTAPI
DriverEntry (
    _In_ PDRIVER_OBJECT	DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNIMPLEMENTED;
    return STATUS_SUCCESS;
}

VOID
NTAPI
WdAllocateWatchdog(
    PVOID p1,
    PVOID p2,
    ULONG p3)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdAllocateDeferredWatchdog(
    PVOID p1,
    PVOID p2,
    ULONG p3)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdFreeWatchdog(
    PVOID p1)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdFreeDeferredWatchdog(
    PVOID p1)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdStartWatch(
    PVOID p1,
    LARGE_INTEGER p2,
    ULONG p3)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdStartDeferredWatch(
    PVOID p1,
    PVOID p2,
    ULONG p3)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdStopWatch(
    PVOID p1,
    ULONG p2)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdStopDeferredWatch(
    PVOID p1)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdSuspendWatch(
    PVOID p1)
{
    UNIMPLEMENTED;
}

VOID
FASTCALL
WdSuspendDeferredWatch(
    PVOID p1)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdResumeWatch(
    PVOID p1,
    PVOID p2)
{
    UNIMPLEMENTED;
}

VOID
FASTCALL
WdResumeDeferredWatch(
    PVOID p1,
    PVOID p2)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdResetWatch(
    PVOID p1)
{
    UNIMPLEMENTED;
}

VOID
FASTCALL
WdResetDeferredWatch(
    PVOID p1)
{
    UNIMPLEMENTED;
}

VOID
FASTCALL
WdEnterMonitoredSection(
    PVOID p1)
{
    UNIMPLEMENTED;
}

VOID
FASTCALL
WdExitMonitoredSection(
    PVOID p1)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdAttachContext(
    PVOID p1,
    PVOID p2)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdDetachContext(
    PVOID p1)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdGetDeviceObject(
    PVOID p1)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdGetLowestDeviceObject(
    PVOID p1)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdGetLastEvent(
    PVOID p1)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdCompleteEvent(
    PVOID p1,
    PVOID p2)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdReferenceObject(
    PVOID p1)
{
    UNIMPLEMENTED;
}

VOID
NTAPI
WdDereferenceObject(
    PVOID p1)
{
    UNIMPLEMENTED;
}

BOOLEAN
NTAPI
WdMadeAnyProgress(
    PVOID p1)
{
    UNIMPLEMENTED;
    return FALSE;
}

/**
 * @brief
 * Reports a session state change from win32k.
 *
 * @param[in] SessionState
 * 0 when the session opens, 1 when it closes, 2 when it runs down.
 *
 * @return
 * STATUS_INVALID_PARAMETER_1 for an unknown state, otherwise STATUS_SUCCESS.
 * Nothing registers for session changes here, so there is nobody to tell.
 */
NTSTATUS
NTAPI
SMgrNotifySessionChange(
    _In_ ULONG SessionState)
{
    if (SessionState > 2)
        return STATUS_INVALID_PARAMETER_1;

    return STATUS_SUCCESS;
}

static PVOID SMgrWin32kCallout;

/**
 * @brief
 * Records the routine win32k wants GDI callouts delivered to.
 *
 * @param[in] Callout
 * The win32k callout.
 *
 * @return
 * STATUS_INVALID_PARAMETER when a different callout is already registered.
 */
NTSTATUS
NTAPI
SMgrRegisterGdiCallout(
    _In_ PVOID Callout)
{
    PVOID Previous;

    Previous = InterlockedCompareExchangePointer(&SMgrWin32kCallout, Callout, NULL);
    if ((Previous != NULL) && (Previous != Callout))
        return STATUS_INVALID_PARAMETER;

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Delivers a GDI callout to the sessions. Only the Windows watchdog implements the delivery.
 *
 * @return
 * STATUS_NOT_IMPLEMENTED.
 */
NTSTATUS
NTAPI
SMgrGdiCallout(
    _In_ PVOID Params,
    _In_ BOOLEAN AllSessions,
    _In_ BOOLEAN Synchronous,
    _In_opt_ PVOID Filter,
    _In_opt_ PVOID FilterContext,
    _In_opt_ PVOID ScenarioContext)
{
    UNREFERENCED_PARAMETER(Params);
    UNREFERENCED_PARAMETER(AllSessions);
    UNREFERENCED_PARAMETER(Synchronous);
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(FilterContext);
    UNREFERENCED_PARAMETER(ScenarioContext);

    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/* \Device\VideoN numbers, shared by videoprt and dxgkrnl so they never pick the same one */
#define DMGR_MAX_GDI_VIEWS  256

static KSPIN_LOCK DMgrGdiViewLock;
static ULONG DMgrUsedGdiViews[DMGR_MAX_GDI_VIEWS];
static ULONG DMgrUsedGdiViewCount;
static ULONG DMgrNextGdiViewId;

/**
 * @brief
 * Hands out the next display device number.
 *
 * @param[out] GdiViewId
 * Receives the number, used as \Device\VideoN and DISPLAY(N+1).
 *
 * @return
 * STATUS_QUOTA_EXCEEDED when every number is taken, otherwise STATUS_SUCCESS.
 */
NTSTATUS
NTAPI
DMgrAcquireGdiViewId(
    _Out_ PULONG GdiViewId)
{
    NTSTATUS Status = STATUS_SUCCESS;
    ULONG Id = 0;
    KIRQL OldIrql;

    KeAcquireSpinLock(&DMgrGdiViewLock, &OldIrql);
    if ((DMgrUsedGdiViewCount >= RTL_NUMBER_OF(DMgrUsedGdiViews)) ||
        (DMgrNextGdiViewId == MAXULONG))
    {
        Status = STATUS_QUOTA_EXCEEDED;
    }
    else
    {
        Id = DMgrNextGdiViewId++;
        DMgrUsedGdiViews[DMgrUsedGdiViewCount++] = Id;
    }
    KeReleaseSpinLock(&DMgrGdiViewLock, OldIrql);

    if (NT_SUCCESS(Status))
        *GdiViewId = Id;

    return Status;
}

/**
 * @brief
 * Gives back a number from DMgrAcquireGdiViewId.
 *
 * @param[in] GdiViewId
 * The number to give back.
 *
 * @param[in] Rollback
 * TRUE to hand the number out again next time, which only works for the
 * newest one.
 */
VOID
NTAPI
DMgrReleaseGdiViewId(
    _In_ ULONG GdiViewId,
    _In_ BOOLEAN Rollback)
{
    KIRQL OldIrql;
    ULONG Index;

    KeAcquireSpinLock(&DMgrGdiViewLock, &OldIrql);
    for (Index = 0; Index < DMgrUsedGdiViewCount; Index++)
    {
        if (DMgrUsedGdiViews[Index] != GdiViewId)
            continue;

        DMgrUsedGdiViewCount--;
        RtlMoveMemory(&DMgrUsedGdiViews[Index],
                      &DMgrUsedGdiViews[Index + 1],
                      (DMgrUsedGdiViewCount - Index) * sizeof(ULONG));

        if (Rollback && (DMgrNextGdiViewId == GdiViewId + 1))
            DMgrNextGdiViewId = GdiViewId;
        break;
    }
    KeReleaseSpinLock(&DMgrGdiViewLock, OldIrql);
}

/**
 * @brief
 * Publishes the display device numbers in use under DEVICEMAP\VIDEO.
 *
 * @return
 * STATUS_SUCCESS, or the registry failure.
 */
NTSTATUS
NTAPI
DMgrWriteDeviceCountToRegistry(VOID)
{
    ULONG UsedGdiViews[DMGR_MAX_GDI_VIEWS];
    ULONG UsedGdiViewCount;
    ULONG MaxObjectNumber;
    KIRQL OldIrql;
    NTSTATUS Status;

    KeAcquireSpinLock(&DMgrGdiViewLock, &OldIrql);
    MaxObjectNumber = DMgrNextGdiViewId - 1;
    UsedGdiViewCount = DMgrUsedGdiViewCount;
    RtlCopyMemory(UsedGdiViews, DMgrUsedGdiViews, UsedGdiViewCount * sizeof(ULONG));
    KeReleaseSpinLock(&DMgrGdiViewLock, OldIrql);

    Status = RtlWriteRegistryValue(RTL_REGISTRY_DEVICEMAP,
                                   L"VIDEO",
                                   L"MaxObjectNumber",
                                   REG_DWORD,
                                   &MaxObjectNumber,
                                   sizeof(MaxObjectNumber));
    if (!NT_SUCCESS(Status))
        return Status;

    return RtlWriteRegistryValue(RTL_REGISTRY_DEVICEMAP,
                                 L"VIDEO",
                                 L"ObjectNumberList",
                                 REG_BINARY,
                                 UsedGdiViews,
                                 UsedGdiViewCount * sizeof(ULONG));
}








