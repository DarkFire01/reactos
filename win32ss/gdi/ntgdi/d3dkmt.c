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
 * d3dkmthk.h gates this one behind DXGKDDI_INTERFACE_VERSION_WIN8 while the tree targets
 * Vista, so it is spelled out here the same way gdi32_vista does.
 */
#if (DXGKDDI_INTERFACE_VERSION < DXGKDDI_INTERFACE_VERSION_WIN8)
typedef struct _D3DKMT_OPENADAPTERFROMLUID
{
    LUID            AdapterLuid;
    D3DKMT_HANDLE   hAdapter;
} D3DKMT_OPENADAPTERFROMLUID;
#endif

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


/**
 * @brief Opens the dxgkrnl adapter that drives a graphics device.
 *
 * The Reference resolves the display itself and only then asks dxgkrnl for a handle, which is
 * why the interface carries no OpenAdapterFrom* of its own. The answer is already on the
 * device: EngpRegisterGraphicsDevice recorded the adapter and its LUID when the device was
 * registered.
 *
 * @param pGraphicsDevice The display in question.
 * @param phAdapter Receives the adapter handle.
 * @param pAdapterLuid Receives the adapter's LUID.
 * @param pVidPnSourceId Receives the source this display is driven from.
 *
 * @return STATUS_NOT_SUPPORTED when nothing WDDM drives the device.
 */
static
NTSTATUS
DxgkpOpenAdapterForDevice(
    _In_opt_ PGRAPHICS_DEVICE pGraphicsDevice,
    _Out_ D3DKMT_HANDLE *phAdapter,
    _Out_ LUID *pAdapterLuid,
    _Out_ ULONG *pVidPnSourceId)
{
    D3DKMT_OPENADAPTERFROMLUID Open;
    PFN_DXGK_D3DKMT pfn;
    NTSTATUS Status;

    if ((pGraphicsDevice == NULL) || (pGraphicsDevice->DxgAdapter == NULL))
        return STATUS_NOT_SUPPORTED;

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_OpenAdapterFromLuid);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("OpenAdapterFromLuid");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    RtlZeroMemory(&Open, sizeof(Open));
    Open.AdapterLuid = pGraphicsDevice->DxgAdapterLuid;

    Status = pfn(&Open);
    if (!NT_SUCCESS(Status))
        return Status;

    *phAdapter = Open.hAdapter;
    *pAdapterLuid = pGraphicsDevice->DxgAdapterLuid;
    *pVidPnSourceId = pGraphicsDevice->VidPnSourceId;

    return STATUS_SUCCESS;
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
    NTSTATUS Status;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnOpenAdapterFromGdiDisplayName)
        return DxgAdapterCallbacks.RxgkIntPfnOpenAdapterFromGdiDisplayName(unnamedParam1);

    WCHAR DeviceName[RTL_NUMBER_OF(unnamedParam1->DeviceName) + 1];
    D3DKMT_HANDLE hAdapter = 0;
    LUID AdapterLuid = { 0, 0 };
    ULONG VidPnSourceId = 0;
    UNICODE_STRING ustrDevice;

    /* Take a copy before anything is decided on it, so it cannot change underneath */
    _SEH2_TRY
    {
        RtlCopyMemory(DeviceName, unnamedParam1->DeviceName,
                      sizeof(unnamedParam1->DeviceName));
        Status = STATUS_SUCCESS;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
        return Status;

    DeviceName[RTL_NUMBER_OF(DeviceName) - 1] = UNICODE_NULL;
    RtlInitUnicodeString(&ustrDevice, DeviceName);

    Status = DxgkpOpenAdapterForDevice(EngpFindGraphicsDevice(&ustrDevice, 0),
                                       &hAdapter, &AdapterLuid, &VidPnSourceId);
    if (!NT_SUCCESS(Status))
        return Status;

    _SEH2_TRY
    {
        unnamedParam1->hAdapter = hAdapter;
        unnamedParam1->AdapterLuid = AdapterLuid;
        unnamedParam1->VidPnSourceId = VidPnSourceId;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return Status;
}

NTSTATUS
APIENTRY
NtGdiDdDDIOpenAdapterFromHdc(_Inout_ D3DKMT_OPENADAPTERFROMHDC* unnamedParam1)
{
    NTSTATUS Status;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnOpenAdapterFromHdc)
        return DxgAdapterCallbacks.RxgkIntPfnOpenAdapterFromHdc(unnamedParam1);

    D3DKMT_HANDLE hAdapter = 0;
    LUID AdapterLuid = { 0, 0 };
    ULONG VidPnSourceId = 0;
    HDC hDc;
    PDC pdc;

    /* Take a copy before anything is decided on it, so it cannot change underneath */
    _SEH2_TRY
    {
        hDc = unnamedParam1->hDc;
        Status = STATUS_SUCCESS;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    if (!NT_SUCCESS(Status))
        return Status;

    pdc = DC_LockDc(hDc);
    if (pdc == NULL)
        return STATUS_INVALID_PARAMETER;

    if (pdc->ppdev == NULL)
    {
        DC_UnlockDc(pdc);
        return STATUS_INVALID_PARAMETER;
    }

    /* The display is held still while it is resolved, as a mode change would move it */
    EngAcquireSemaphore(pdc->ppdev->hsemDevLock);
    Status = DxgkpOpenAdapterForDevice(pdc->ppdev->pGraphicsDevice,
                                       &hAdapter, &AdapterLuid, &VidPnSourceId);
    EngReleaseSemaphore(pdc->ppdev->hsemDevLock);
    DC_UnlockDc(pdc);

    if (!NT_SUCCESS(Status))
        return Status;

    _SEH2_TRY
    {
        unnamedParam1->hAdapter = hAdapter;
        unnamedParam1->AdapterLuid = AdapterLuid;
        unnamedParam1->VidPnSourceId = VidPnSourceId;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        Status = _SEH2_GetExceptionCode();
    }
    _SEH2_END;

    return Status;
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
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnCreateAllocation)
        return DxgAdapterCallbacks.RxgkIntPfnCreateAllocation(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_CreateAllocation);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("CreateAllocation");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICheckMonitorPowerState(_In_ const D3DKMT_CHECKMONITORPOWERSTATE* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnCheckMonitorPowerState)
        return DxgAdapterCallbacks.RxgkIntPfnCheckMonitorPowerState(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_CheckMonitorPowerState);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("CheckMonitorPowerState");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICheckOcclusion(_In_ const D3DKMT_CHECKOCCLUSION* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnCheckOcclusion)
        return DxgAdapterCallbacks.RxgkIntPfnCheckOcclusion(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_CheckOcclusion);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("CheckOcclusion");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}


NTSTATUS
APIENTRY
NtGdiDdDDICloseAdapter(_In_ const D3DKMT_CLOSEADAPTER* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnCloseAdapter)
        return DxgAdapterCallbacks.RxgkIntPfnCloseAdapter(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_CloseAdapter);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("CloseAdapter");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateContext(_Inout_ D3DKMT_CREATECONTEXT* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnCreateContext)
        return DxgAdapterCallbacks.RxgkIntPfnCreateContext(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_CreateContext);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("CreateContext");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateDevice(_Inout_ D3DKMT_CREATEDEVICE* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnCreateDevice)
        return DxgAdapterCallbacks.RxgkIntPfnCreateDevice(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_CreateDevice);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("CreateDevice");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateOverlay(_Inout_ D3DKMT_CREATEOVERLAY* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnCreateOverlay)
        return DxgAdapterCallbacks.RxgkIntPfnCreateOverlay(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_CreateOverlay);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("CreateOverlay");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateSynchronizationObject(_Inout_ D3DKMT_CREATESYNCHRONIZATIONOBJECT* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnCreateSynchronizationObject)
        return DxgAdapterCallbacks.RxgkIntPfnCreateSynchronizationObject(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_CreateSynchronizationObject);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("CreateSynchronizationObject");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyAllocation(_In_ const D3DKMT_DESTROYALLOCATION* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnDestroyAllocation)
        return DxgAdapterCallbacks.RxgkIntPfnDestroyAllocation(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_DestroyAllocation);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("DestroyAllocation");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyContext(_In_ const D3DKMT_DESTROYCONTEXT* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnDestroyContext)
        return DxgAdapterCallbacks.RxgkIntPfnDestroyContext(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_DestroyContext);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("DestroyContext");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyDevice(_In_ const D3DKMT_DESTROYDEVICE* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnDestroyDevice)
        return DxgAdapterCallbacks.RxgkIntPfnDestroyDevice(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_DestroyDevice);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("DestroyDevice");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyOverlay(_In_ const D3DKMT_DESTROYOVERLAY* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnDestroyOverlay)
        return DxgAdapterCallbacks.RxgkIntPfnDestroyOverlay(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_DestroyOverlay);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("DestroyOverlay");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIDestroySynchronizationObject(_In_ const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnDestroySynchronizationObject)
        return DxgAdapterCallbacks.RxgkIntPfnDestroySynchronizationObject(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_DestroySynchronizationObject);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("DestroySynchronizationObject");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIEscape(_In_ const D3DKMT_ESCAPE* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnEscape)
        return DxgAdapterCallbacks.RxgkIntPfnEscape(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_Escape);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("Escape");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIFlipOverlay(_In_ const D3DKMT_FLIPOVERLAY* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnFlipOverlay)
        return DxgAdapterCallbacks.RxgkIntPfnFlipOverlay(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_FlipOverlay);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("FlipOverlay");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetContextSchedulingPriority(_Inout_ D3DKMT_GETCONTEXTSCHEDULINGPRIORITY* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnGetContextSchedulingPriority)
        return DxgAdapterCallbacks.RxgkIntPfnGetContextSchedulingPriority(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_GetContextSchedulingPriority);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("GetContextSchedulingPriority");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetDeviceState(_Inout_ D3DKMT_GETDEVICESTATE* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnGetDeviceState)
        return DxgAdapterCallbacks.RxgkIntPfnGetDeviceState(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_GetDeviceState);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("GetDeviceState");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetDisplayModeList(_Inout_ D3DKMT_GETDISPLAYMODELIST* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnGetDisplayModeList)
        return DxgAdapterCallbacks.RxgkIntPfnGetDisplayModeList(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_GetDisplayModeList);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("GetDisplayModeList");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetMultisampleMethodList(_Inout_ D3DKMT_GETMULTISAMPLEMETHODLIST* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnGetMultisampleMethodList)
        return DxgAdapterCallbacks.RxgkIntPfnGetMultisampleMethodList(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_GetMultisampleMethodList);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("GetMultisampleMethodList");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetPresentHistory(_Inout_ D3DKMT_GETPRESENTHISTORY* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnGetPresentHistory)
        return DxgAdapterCallbacks.RxgkIntPfnGetPresentHistory(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_GetPresentHistory);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("GetPresentHistory");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetRuntimeData(_In_ const D3DKMT_GETRUNTIMEDATA* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnGetRuntimeData)
        return DxgAdapterCallbacks.RxgkIntPfnGetRuntimeData(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_GetRuntimeData);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("GetRuntimeData");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetScanLine(_In_ D3DKMT_GETSCANLINE* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnGetScanLine)
        return DxgAdapterCallbacks.RxgkIntPfnGetScanLine(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_GetScanLine);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("GetScanLine");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIGetSharedPrimaryHandle(_Inout_ D3DKMT_GETSHAREDPRIMARYHANDLE* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnGetSharedPrimaryHandle)
        return DxgAdapterCallbacks.RxgkIntPfnGetSharedPrimaryHandle(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_GetSharedPrimaryHandle);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("GetSharedPrimaryHandle");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIInvalidateActiveVidPn(_In_ const D3DKMT_INVALIDATEACTIVEVIDPN* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnInvalidateActiveVidPn)
        return DxgAdapterCallbacks.RxgkIntPfnInvalidateActiveVidPn(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_InvalidateActiveVidPn);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("InvalidateActiveVidPn");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDILock(_Inout_ D3DKMT_LOCK* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnLock)
        return DxgAdapterCallbacks.RxgkIntPfnLock(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_Lock);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("Lock");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIOpenResource(_Inout_ D3DKMT_OPENRESOURCE* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnOpenResource)
        return DxgAdapterCallbacks.RxgkIntPfnOpenResource(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_OpenResource);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("OpenResource");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIPollDisplayChildren(_In_ const D3DKMT_POLLDISPLAYCHILDREN* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnPollDisplayChildren)
        return DxgAdapterCallbacks.RxgkIntPfnPollDisplayChildren(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_PollDisplayChildren);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("PollDisplayChildren");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIPresent(_In_ D3DKMT_PRESENT* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnPresent)
        return DxgAdapterCallbacks.RxgkIntPfnPresent(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_Present);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("Present");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIQueryAdapterInfo(_Inout_ const D3DKMT_QUERYADAPTERINFO* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnQueryAdapterInfo)
        return DxgAdapterCallbacks.RxgkIntPfnQueryAdapterInfo(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_QueryAdapterInfo);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("QueryAdapterInfo");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIQueryAllocationResidency(_In_ const D3DKMT_QUERYALLOCATIONRESIDENCY* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnQueryAllocationResidency)
        return DxgAdapterCallbacks.RxgkIntPfnQueryAllocationResidency(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_QueryAllocationResidency);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("QueryAllocationResidency");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIQueryResourceInfo(_Inout_ D3DKMT_QUERYRESOURCEINFO* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnQueryResourceInfo)
        return DxgAdapterCallbacks.RxgkIntPfnQueryResourceInfo(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_QueryResourceInfo);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("QueryResourceInfo");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIQueryStatistics(_Inout_ const D3DKMT_QUERYSTATISTICS* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnQueryStatistics)
        return DxgAdapterCallbacks.RxgkIntPfnQueryStatistics(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_QueryStatistics);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("QueryStatistics");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIReleaseProcessVidPnSourceOwners(_In_ HANDLE unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnReleaseProcessVidPnSourceOwners)
        return DxgAdapterCallbacks.RxgkIntPfnReleaseProcessVidPnSourceOwners(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_ReleaseProcessVidPnSourceOwners);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("ReleaseProcessVidPnSourceOwners");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIRender(_In_ D3DKMT_RENDER* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnRender)
        return DxgAdapterCallbacks.RxgkIntPfnRender(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_Render);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("Render");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetAllocationPriority(_In_ const D3DKMT_SETALLOCATIONPRIORITY* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnSetAllocationPriority)
        return DxgAdapterCallbacks.RxgkIntPfnSetAllocationPriority(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_SetAllocationPriority);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("SetAllocationPriority");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetContextSchedulingPriority(_In_ const D3DKMT_SETCONTEXTSCHEDULINGPRIORITY* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnSetContextSchedulingPriority)
        return DxgAdapterCallbacks.RxgkIntPfnSetContextSchedulingPriority(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_SetContextSchedulingPriority);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("SetContextSchedulingPriority");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetDisplayMode(_In_ const D3DKMT_SETDISPLAYMODE* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnSetDisplayMode)
        return DxgAdapterCallbacks.RxgkIntPfnSetDisplayMode(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_SetDisplayMode);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("SetDisplayMode");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetDisplayPrivateDriverFormat(_In_ const D3DKMT_SETDISPLAYPRIVATEDRIVERFORMAT* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnSetDisplayPrivateDriverFormat)
        return DxgAdapterCallbacks.RxgkIntPfnSetDisplayPrivateDriverFormat(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_SetDisplayPrivateDriverFormat);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("SetDisplayPrivateDriverFormat");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetGammaRamp(_In_ const D3DKMT_SETGAMMARAMP* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnSetGammaRamp)
        return DxgAdapterCallbacks.RxgkIntPfnSetGammaRamp(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_SetGammaRamp);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("SetGammaRamp");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}


NTSTATUS
APIENTRY
NtGdiDdDDISetQueuedLimit(_Inout_ const D3DKMT_SETQUEUEDLIMIT* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnSetQueuedLimit)
        return DxgAdapterCallbacks.RxgkIntPfnSetQueuedLimit(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_SetQueuedLimit);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("SetQueuedLimit");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISetVidPnSourceOwner(_In_ const D3DKMT_SETVIDPNSOURCEOWNER* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnSetVidPnSourceOwner)
        return DxgAdapterCallbacks.RxgkIntPfnSetVidPnSourceOwner(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_SetVidPnSourceOwner);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("SetVidPnSourceOwner");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
WINAPI
NtGdiDdDDIUnlock(_In_ const D3DKMT_UNLOCK* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnUnlock)
        return DxgAdapterCallbacks.RxgkIntPfnUnlock(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_Unlock);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("Unlock");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIUpdateOverlay(_In_ const D3DKMT_UPDATEOVERLAY* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnUpdateOverlay)
        return DxgAdapterCallbacks.RxgkIntPfnUpdateOverlay(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_UpdateOverlay);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("UpdateOverlay");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIWaitForIdle(_In_ const D3DKMT_WAITFORIDLE* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnWaitForIdle)
        return DxgAdapterCallbacks.RxgkIntPfnWaitForIdle(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_WaitForIdle);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("WaitForIdle");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIWaitForSynchronizationObject(_In_ const D3DKMT_WAITFORSYNCHRONIZATIONOBJECT* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnWaitForSynchronizationObject)
        return DxgAdapterCallbacks.RxgkIntPfnWaitForSynchronizationObject(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_WaitForSynchronizationObject);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("WaitForSynchronizationObject");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDIWaitForVerticalBlankEvent(_In_ const D3DKMT_WAITFORVERTICALBLANKEVENT* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnWaitForVerticalBlankEvent)
        return DxgAdapterCallbacks.RxgkIntPfnWaitForVerticalBlankEvent(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_WaitForVerticalBlankEvent);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("WaitForVerticalBlankEvent");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDISignalSynchronizationObject(_In_ const D3DKMT_SIGNALSYNCHRONIZATIONOBJECT* unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    if (DxgAdapterCallbacks.RxgkIntPfnSignalSynchronizationObject)
        return DxgAdapterCallbacks.RxgkIntPfnSignalSynchronizationObject(unnamedParam1);

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_SignalSynchronizationObject);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("SignalSynchronizationObject");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn((PVOID)unnamedParam1);
}

/*
 * The argument is described by the caller and read by dxgkrnl, never here, so it
 * travels as a plain pointer rather than a shape win32k would have to agree on.
 */
NTSTATUS
APIENTRY
NtGdiDdDDICheckSharedResourceAccess(_Inout_ PVOID unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_CheckSharedResourceAccess);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("CheckSharedResourceAccess");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn(unnamedParam1);
}

/*
 * The argument is described by the caller and read by dxgkrnl, never here, so it
 * travels as a plain pointer rather than a shape win32k would have to agree on.
 */
NTSTATUS
APIENTRY
NtGdiDdDDIAdjustFullscreenGamma(_Inout_ PVOID unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_AdjustFullscreenGamma);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("AdjustFullscreenGamma");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn(unnamedParam1);
}

/*
 * The argument is described by the caller and read by dxgkrnl, never here, so it
 * travels as a plain pointer rather than a shape win32k would have to agree on.
 */
NTSTATUS
APIENTRY
NtGdiDdDDICheckMultiPlaneOverlaySupport3(_Inout_ PVOID unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_CheckMultiPlaneOverlaySupport3);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("CheckMultiPlaneOverlaySupport3");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn(unnamedParam1);
}

/*
 * The argument is described by the caller and read by dxgkrnl, never here, so it
 * travels as a plain pointer rather than a shape win32k would have to agree on.
 */
NTSTATUS
APIENTRY
NtGdiDdDDIPresentMultiPlaneOverlay3(_Inout_ PVOID unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_PresentMultiPlaneOverlay3);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("PresentMultiPlaneOverlay3");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn(unnamedParam1);
}

/*
 * The argument is described by the caller and read by dxgkrnl, never here, so it
 * travels as a plain pointer rather than a shape win32k would have to agree on.
 */
NTSTATUS
APIENTRY
NtGdiDdDDIQueryVidPnExclusiveOwnership(_Inout_ PVOID unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_QueryVidPnExclusiveOwnership);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("QueryVidPnExclusiveOwnership");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn(unnamedParam1);
}

/*
 * The argument is described by the caller and read by dxgkrnl, never here, so it
 * travels as a plain pointer rather than a shape win32k would have to agree on.
 */
NTSTATUS
APIENTRY
NtGdiDdDDISetHwProtectionTeardownRecovery(_Inout_ PVOID unnamedParam1)
{
    UNREFERENCED_PARAMETER(unnamedParam1);

    /* No slot in the interface serves this one */
    DXGKMT_TRACE_NOPROC("SetHwProtectionTeardownRecovery");
    return STATUS_PROCEDURE_NOT_FOUND;
}

/*
 * The argument is described by the caller and read by dxgkrnl, never here, so it
 * travels as a plain pointer rather than a shape win32k would have to agree on.
 */
NTSTATUS
APIENTRY
NtGdiDdDDISetVidPnSourceHwProtection(_Inout_ PVOID unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_SetVidPnSourceHwProtection);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("SetVidPnSourceHwProtection");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn(unnamedParam1);
}

/*
 * The argument is described by the caller and read by dxgkrnl, never here, so it
 * travels as a plain pointer rather than a shape win32k would have to agree on.
 */
NTSTATUS
APIENTRY
NtGdiDdDDIEnumAdapters2(_Inout_ PVOID unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_EnumAdapters2);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("EnumAdapters2");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn(unnamedParam1);
}

/*
 * The argument is described by the caller and read by dxgkrnl, never here, so it
 * travels as a plain pointer rather than a shape win32k would have to agree on.
 */
NTSTATUS
APIENTRY
NtGdiDdDDISetVidPnSourceOwner1(_Inout_ PVOID unnamedParam1)
{
    PFN_DXGK_D3DKMT pfn;

    if (!unnamedParam1)
        return STATUS_INVALID_PARAMETER;

    pfn = DxgkGetD3DKMTSlot(DXGK_SLOT_SetVidPnSourceOwner1);
    if (pfn == NULL)
    {
        DXGKMT_TRACE_NOPROC("SetVidPnSourceOwner1");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    return pfn(unnamedParam1);
}

/*
 * The argument is described by the caller and read by dxgkrnl, never here, so it
 * travels as a plain pointer rather than a shape win32k would have to agree on.
 */
NTSTATUS
APIENTRY
NtGdiDdDDISubmitPresentToHwQueue(_Inout_ PVOID unnamedParam1)
{
    UNREFERENCED_PARAMETER(unnamedParam1);

    /* No slot in the interface serves this one */
    DXGKMT_TRACE_NOPROC("SubmitPresentToHwQueue");
    return STATUS_PROCEDURE_NOT_FOUND;
}
