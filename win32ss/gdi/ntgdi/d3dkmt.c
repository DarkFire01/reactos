/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     D3DKMT dxgkrnl callbacks
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 */

#include <win32k.h>
#include <reactos/rddm/rxgkinterface.h>
#include <reactos/rddm/rddm_private.h>
#include <debug.h>

/*
 * It looks like Windows saves all the function pointers globally inside win32k.
 * Instead, we're going to keep it static to this file and keep it organized in struct
 * we obtained with the IOCTL.
 */
static REACTOS_WIN32K_DXGKRNL_INTERFACE DxgAdapterCallbacks = {0};

/* WDDM bootstrap (gdi/eng/dxgkrnl.c) - idempotent, returns success if already up. */
NTSTATUS NTAPI DlInitDxgkrnl(VOID);

/**
 * @brief Report a D3DKMT entry point that win32k answers itself without ever reaching dxgkrnl.
 *
 * These return a canned value, so nothing downstream logs them and an application relying on one
 * gets a plausible answer with no trace anywhere. Logged on every call, deliberately.
 */
static VOID
DxgkpTraceCanned(PCSTR Name, volatile LONG *pCount)
{
    DPRINT1("win32k: D3DKMT %s answered by win32k, not implemented (call %d)\n",
            Name, InterlockedIncrement(pCount));
}

#define DXGKMT_TRACE_CANNED(name) \
    do { static volatile LONG C = 0; DxgkpTraceCanned(name, &C); } while (0)

/** @brief The dxgkrnl callback table has no entry for this call - also silent until now. */
static VOID
DxgkpTraceNoProc(PCSTR Name, volatile LONG *pCount)
{
    DPRINT1("win32k: D3DKMT %s has no dxgkrnl entry point (call %d)\n",
            Name, InterlockedIncrement(pCount));
}

#define DXGKMT_TRACE_NOPROC(name) \
    do { static volatile LONG C = 0; DxgkpTraceNoProc(name, &C); } while (0)

/**
 * @brief Populate DxgAdapterCallbacks from dxgkrnl. Sends IOCTL_VIDEO_REGISTER_RXGK to \Device\DxgKrnl;
 *        DxgKrnl_ms fills the RxgkIntPfn* slots with its D3DKMT entry points (device/reactosif.cpp).
 *        Called once from the WDDM bootstrap (gdi/eng/dxgkrnl.c) after the device is open.
 */
NTSTATUS
NTAPI
DxgRegisterAdapterCallbacks(
    _In_ PDEVICE_OBJECT pDxgkrnl)
{
    KEVENT          Event;
    IO_STATUS_BLOCK Iosb;
    PIRP            Irp;
    NTSTATUS        Status;

    if (pDxgkrnl == NULL)
        return STATUS_INVALID_PARAMETER;

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IOCTL_VIDEO_REGISTER_RXGK,
                                        pDxgkrnl,
                                        &DxgAdapterCallbacks, sizeof(DxgAdapterCallbacks),
                                        &DxgAdapterCallbacks, sizeof(DxgAdapterCallbacks),
                                        TRUE, &Event, &Iosb);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(pDxgkrnl, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    return Status;
}

/**
 * @brief Can WDDM actually drive a display right now?
 *
 * Asks dxgkrnl how many adapters have finished starting. This is deliberately NOT cached and NOT
 * the same question as gbDxgkInitialized: dxgkrnl.sys ships in the ISO and loads on every boot, so
 * "dxgkrnl answered its interface IOCTL" is true even on an install with no WDDM display driver at
 * all. Asking for a live count instead means the answer is correct both on a stock install (0 - use
 * the legacy XPDM path) and on a WDDM one, whatever order the miniport happens to start in.
 */
BOOLEAN
APIENTRY
DxIsWddmDisplayAvailable(VOID)
{
    ULONG    AdapterCount = 0;
    NTSTATUS Status;

    if (!DxgAdapterCallbacks.RxgkIntPfnGetAdapterCount)
        return FALSE;

    Status = DxgAdapterCallbacks.RxgkIntPfnGetAdapterCount(&AdapterCount);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("win32k: RxgkIntPfnGetAdapterCount failed 0x%lX\n", Status);
        return FALSE;
    }

    return (AdapterCount != 0);
}

/*
 * This looks like it's done inside DxDdStartupDxGraphics, but I'd rather keep this organized.
 * Dxg gets start inevitably anyway it seems at least on vista.
 */
VOID
APIENTRY
DxStartupDxgkInt(VOID)
{
    NTSTATUS Status;

    DPRINT("DxStartupDxgkInt: Entry\n");

    /*
     * Load dxgkrnl and obtain the win32k<->dxgkrnl interfaces (CORE-20027).
     *
     * This has to happen here rather than only in InitVideo, because InitializeGreCSRSS decides
     * whether to start the legacy dxg.sys DirectDraw path immediately after calling us - and
     * InitVideo does not run until later, inside UserInitialize. DlInitDxgkrnl is idempotent, so
     * InitVideo's own call becomes a no-op.
     */
    Status = DlInitDxgkrnl();
    if (!NT_SUCCESS(Status))
        DPRINT("DxStartupDxgkInt: no WDDM stack (0x%lX); legacy display path\n", Status);
}

BOOLEAN
APIENTRY
NtGdiDdDDICheckExclusiveOwnership(VOID)
{
    /* We don't support DWM at this time, exclusive ownership is always false. */
    DXGKMT_TRACE_CANNED("CheckExclusiveOwnership");
    return FALSE;
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetProcessSchedulingPriorityClass(_In_  HANDLE unnamedParam1,
                                            _Out_ D3DKMT_SCHEDULINGPRIORITYCLASS *unnamedParam2)
{
    UNREFERENCED_PARAMETER(unnamedParam1);

    DXGKMT_TRACE_CANNED("GetProcessSchedulingPriorityClass");

    /* Answer something defined - the caller reads this back whatever we return. */
    if (unnamedParam2)
        *unnamedParam2 = D3DKMT_SCHEDULINGPRIORITYCLASS_NORMAL;

    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
NtGdiDdDDISetProcessSchedulingPriorityClass(_In_ HANDLE unnamedParam1,
                                            _In_ D3DKMT_SCHEDULINGPRIORITYCLASS unnamedParam2)
{
    UNREFERENCED_PARAMETER(unnamedParam1);
    UNREFERENCED_PARAMETER(unnamedParam2);
    DXGKMT_TRACE_CANNED("SetProcessSchedulingPriorityClass");
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
NtGdiDdDDISharedPrimaryLockNotification(_In_ const D3DKMT_SHAREDPRIMARYLOCKNOTIFICATION* unnamedParam1)
{
    UNREFERENCED_PARAMETER(unnamedParam1);
    DXGKMT_TRACE_CANNED("SharedPrimaryLockNotification");
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
NtGdiDdDDISharedPrimaryUnLockNotification(_In_ const D3DKMT_SHAREDPRIMARYUNLOCKNOTIFICATION* unnamedParam1)
{
    UNREFERENCED_PARAMETER(unnamedParam1);
    DXGKMT_TRACE_CANNED("SharedPrimaryUnLockNotification");
    return STATUS_SUCCESS;
}

/*
 * The adapter-open family. These used to `return 0` - STATUS_SUCCESS with the output struct
 * untouched - so a caller believed it had an adapter and carried on with a zero handle. Dispatch
 * them like every other D3DKMT entry point instead.
 */
NTSTATUS
APIENTRY
NtGdiDdDDIOpenAdapterFromGdiDisplayName(_Inout_ D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME* unnamedParam1)
{
    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnOpenAdapterFromGdiDisplayName)
    {
        DXGKMT_TRACE_NOPROC("OpenAdapterFromGdiDisplayName");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnOpenAdapterFromGdiDisplayName(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIOpenAdapterFromHdc(_Inout_ D3DKMT_OPENADAPTERFROMHDC* unnamedParam1)
{
    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnOpenAdapterFromHdc)
    {
        DXGKMT_TRACE_NOPROC("OpenAdapterFromHdc");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnOpenAdapterFromHdc(unnamedParam1);
}


NTSTATUS
APIENTRY
NtGdiDdDDIOpenAdapterFromDeviceName(_Inout_ D3DKMT_OPENADAPTERFROMDEVICENAME* unnamedParam1)
{
    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnOpenAdapterFromDeviceName)
    {
        DXGKMT_TRACE_NOPROC("OpenAdapterFromDeviceName");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnOpenAdapterFromDeviceName(unnamedParam1);
}


/*
 * The following APIs all have the same idea.
 * Most of the parameters are stuffed in custom typedefs with a bunch of types inside them.
 * The idea here is this:
 * if we're dealing with a d3dkmt API that directly calls into a miniport if the function pointer doesn't
 * exist we're returning STATUS_PROCEDURE_NOT_FOUND.
 *
 * This essentially means the Dxgkrnl interface was never made as Win32k doesn't do any handling for these routines.
 */

NTSTATUS
APIENTRY
NtGdiDdDDICreateAllocation(_Inout_ D3DKMT_CREATEALLOCATION* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnCreateAllocation)
    {
        DXGKMT_TRACE_NOPROC("CreateAllocation");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnCreateAllocation(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICheckMonitorPowerState(_In_ const D3DKMT_CHECKMONITORPOWERSTATE* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnCheckMonitorPowerState)
    {
        DXGKMT_TRACE_NOPROC("CheckMonitorPowerState");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnCheckMonitorPowerState(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICheckOcclusion(_In_ const D3DKMT_CHECKOCCLUSION* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnCheckOcclusion)
    {
        DXGKMT_TRACE_NOPROC("CheckOcclusion");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnCheckOcclusion(unnamedParam1);
}


NTSTATUS
APIENTRY
NtGdiDdDDICloseAdapter(_In_ const D3DKMT_CLOSEADAPTER* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnCloseAdapter)
    {
        DXGKMT_TRACE_NOPROC("CloseAdapter");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnCloseAdapter(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateContext(_Inout_ D3DKMT_CREATECONTEXT* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnCreateContext)
    {
        DXGKMT_TRACE_NOPROC("CreateContext");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnCreateContext(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateDevice(_Inout_ D3DKMT_CREATEDEVICE* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnCreateDevice)
    {
        DXGKMT_TRACE_NOPROC("CreateDevice");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnCreateDevice(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateOverlay(_Inout_ D3DKMT_CREATEOVERLAY* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnCreateOverlay)
    {
        DXGKMT_TRACE_NOPROC("CreateOverlay");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnCreateOverlay(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateSynchronizationObject(_Inout_ D3DKMT_CREATESYNCHRONIZATIONOBJECT* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnCreateSynchronizationObject)
    {
        DXGKMT_TRACE_NOPROC("CreateSynchronizationObject");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnCreateSynchronizationObject(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyAllocation(_In_ const D3DKMT_DESTROYALLOCATION* unnamedParam1)
{
  if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnDestroyAllocation)
    {
        DXGKMT_TRACE_NOPROC("DestroyAllocation");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnDestroyAllocation(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyContext(_In_ const D3DKMT_DESTROYCONTEXT* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnDestroyContext)
    {
        DXGKMT_TRACE_NOPROC("DestroyContext");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnDestroyContext(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyDevice(_In_ const D3DKMT_DESTROYDEVICE* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnDestroyDevice)
    {
        DXGKMT_TRACE_NOPROC("DestroyDevice");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnDestroyDevice(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyOverlay(_In_ const D3DKMT_DESTROYOVERLAY* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnDestroyOverlay)
    {
        DXGKMT_TRACE_NOPROC("DestroyOverlay");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnDestroyOverlay(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDestroySynchronizationObject(_In_ const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnDestroySynchronizationObject)
    {
        DXGKMT_TRACE_NOPROC("DestroySynchronizationObject");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnDestroySynchronizationObject(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIEscape(_In_ const D3DKMT_ESCAPE* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnEscape)
    {
        DXGKMT_TRACE_NOPROC("Escape");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnEscape(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIFlipOverlay(_In_ const D3DKMT_FLIPOVERLAY* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnFlipOverlay)
    {
        DXGKMT_TRACE_NOPROC("FlipOverlay");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnFlipOverlay(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetContextSchedulingPriority(_Inout_ D3DKMT_GETCONTEXTSCHEDULINGPRIORITY* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnGetContextSchedulingPriority)
    {
        DXGKMT_TRACE_NOPROC("GetContextSchedulingPriority");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnGetContextSchedulingPriority(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetDeviceState(_Inout_ D3DKMT_GETDEVICESTATE* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnGetDeviceState)
    {
        DXGKMT_TRACE_NOPROC("GetDeviceState");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnGetDeviceState(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetDisplayModeList(_Inout_ D3DKMT_GETDISPLAYMODELIST* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnGetDisplayModeList)
    {
        DXGKMT_TRACE_NOPROC("GetDisplayModeList");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnGetDisplayModeList(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetMultisampleMethodList(_Inout_ D3DKMT_GETMULTISAMPLEMETHODLIST* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnGetMultisampleMethodList)
    {
        DXGKMT_TRACE_NOPROC("GetMultisampleMethodList");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnGetMultisampleMethodList(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetPresentHistory(_Inout_ D3DKMT_GETPRESENTHISTORY* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnGetPresentHistory)
    {
        DXGKMT_TRACE_NOPROC("GetPresentHistory");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnGetPresentHistory(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetRuntimeData(_In_ const D3DKMT_GETRUNTIMEDATA* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnGetRuntimeData)
    {
        DXGKMT_TRACE_NOPROC("GetRuntimeData");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnGetRuntimeData(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetScanLine(_In_ D3DKMT_GETSCANLINE* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnGetScanLine)
    {
        DXGKMT_TRACE_NOPROC("GetScanLine");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnGetScanLine(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetSharedPrimaryHandle(_Inout_ D3DKMT_GETSHAREDPRIMARYHANDLE* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnGetSharedPrimaryHandle)
    {
        DXGKMT_TRACE_NOPROC("GetSharedPrimaryHandle");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnGetSharedPrimaryHandle(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIInvalidateActiveVidPn(_In_ const D3DKMT_INVALIDATEACTIVEVIDPN* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnInvalidateActiveVidPn)
    {
        DXGKMT_TRACE_NOPROC("InvalidateActiveVidPn");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnInvalidateActiveVidPn(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDILock(_Inout_ D3DKMT_LOCK* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnLock)
    {
        DXGKMT_TRACE_NOPROC("Lock");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnLock(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIOpenResource(_Inout_ D3DKMT_OPENRESOURCE* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnOpenResource)
    {
        DXGKMT_TRACE_NOPROC("OpenResource");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnOpenResource(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIPollDisplayChildren(_In_ const D3DKMT_POLLDISPLAYCHILDREN* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnPollDisplayChildren)
    {
        DXGKMT_TRACE_NOPROC("PollDisplayChildren");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnPollDisplayChildren(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIPresent(_In_ D3DKMT_PRESENT* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnPresent)
    {
        DXGKMT_TRACE_NOPROC("Present");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnPresent(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIQueryAdapterInfo(_Inout_ const D3DKMT_QUERYADAPTERINFO* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnQueryAdapterInfo)
    {
        DXGKMT_TRACE_NOPROC("QueryAdapterInfo");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnQueryAdapterInfo(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIQueryAllocationResidency(_In_ const D3DKMT_QUERYALLOCATIONRESIDENCY* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnQueryAllocationResidency)
    {
        DXGKMT_TRACE_NOPROC("QueryAllocationResidency");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnQueryAllocationResidency(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIQueryResourceInfo(_Inout_ D3DKMT_QUERYRESOURCEINFO* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnQueryResourceInfo)
    {
        DXGKMT_TRACE_NOPROC("QueryResourceInfo");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnQueryResourceInfo(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIQueryStatistics(_Inout_ const D3DKMT_QUERYSTATISTICS* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnQueryStatistics)
    {
        DXGKMT_TRACE_NOPROC("QueryStatistics");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnQueryStatistics(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIReleaseProcessVidPnSourceOwners(_In_ HANDLE unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnReleaseProcessVidPnSourceOwners)
    {
        DXGKMT_TRACE_NOPROC("ReleaseProcessVidPnSourceOwners");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnReleaseProcessVidPnSourceOwners(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIRender(_In_ D3DKMT_RENDER* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnRender)
    {
        DXGKMT_TRACE_NOPROC("Render");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnRender(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetAllocationPriority(_In_ const D3DKMT_SETALLOCATIONPRIORITY* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnSetAllocationPriority)
    {
        DXGKMT_TRACE_NOPROC("SetAllocationPriority");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnSetAllocationPriority(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetContextSchedulingPriority(_In_ const D3DKMT_SETCONTEXTSCHEDULINGPRIORITY* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnSetContextSchedulingPriority)
    {
        DXGKMT_TRACE_NOPROC("SetContextSchedulingPriority");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnSetContextSchedulingPriority(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetDisplayMode(_In_ const D3DKMT_SETDISPLAYMODE* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnSetDisplayMode)
    {
        DXGKMT_TRACE_NOPROC("SetDisplayMode");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnSetDisplayMode(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetDisplayPrivateDriverFormat(_In_ const D3DKMT_SETDISPLAYPRIVATEDRIVERFORMAT* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnSetDisplayPrivateDriverFormat)
    {
        DXGKMT_TRACE_NOPROC("SetDisplayPrivateDriverFormat");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnSetDisplayPrivateDriverFormat(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetGammaRamp(_In_ const D3DKMT_SETGAMMARAMP* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnSetGammaRamp)
    {
        DXGKMT_TRACE_NOPROC("SetGammaRamp");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnSetGammaRamp(unnamedParam1);
}


NTSTATUS
APIENTRY
NtGdiDdDDISetQueuedLimit(_Inout_ const D3DKMT_SETQUEUEDLIMIT* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnSetQueuedLimit)
    {
        DXGKMT_TRACE_NOPROC("SetQueuedLimit");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnSetQueuedLimit(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetVidPnSourceOwner(_In_ const D3DKMT_SETVIDPNSOURCEOWNER* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnSetVidPnSourceOwner)
    {
        DXGKMT_TRACE_NOPROC("SetVidPnSourceOwner");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnSetVidPnSourceOwner(unnamedParam1);
}

NTSTATUS
WINAPI
NtGdiDdDDIUnlock(_In_ const D3DKMT_UNLOCK* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnUnlock)
    {
        DXGKMT_TRACE_NOPROC("Unlock");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnUnlock(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIUpdateOverlay(_In_ const D3DKMT_UPDATEOVERLAY* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnUpdateOverlay)
    {
        DXGKMT_TRACE_NOPROC("UpdateOverlay");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnUpdateOverlay(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIWaitForIdle(_In_ const D3DKMT_WAITFORIDLE* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnWaitForIdle)
    {
        DXGKMT_TRACE_NOPROC("WaitForIdle");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnWaitForIdle(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIWaitForSynchronizationObject(_In_ const D3DKMT_WAITFORSYNCHRONIZATIONOBJECT* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnWaitForSynchronizationObject)
    {
        DXGKMT_TRACE_NOPROC("WaitForSynchronizationObject");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnWaitForSynchronizationObject(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIWaitForVerticalBlankEvent(_In_ const D3DKMT_WAITFORVERTICALBLANKEVENT* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnWaitForVerticalBlankEvent)
    {
        DXGKMT_TRACE_NOPROC("WaitForVerticalBlankEvent");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnWaitForVerticalBlankEvent(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISignalSynchronizationObject(_In_ const D3DKMT_SIGNALSYNCHRONIZATIONOBJECT* unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.RxgkIntPfnSignalSynchronizationObject)
    {
        DXGKMT_TRACE_NOPROC("SignalSynchronizationObject");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return DxgAdapterCallbacks.RxgkIntPfnSignalSynchronizationObject(unnamedParam1);
}
