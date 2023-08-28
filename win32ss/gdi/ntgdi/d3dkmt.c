#include <win32k.h>
#include <reactos/drivers/wddm/dxgkinterface.h>
#include <debug.h>

BOOLEAN IsWDDMOn = FALSE;

#define IOCTL_VIDEO_I_AM_REACTOS \
	CTL_CODE(FILE_DEVICE_VIDEO, 0xB, METHOD_NEITHER, FILE_ANY_ACCESS)

/*
 * It looks like windows saves all the funciton pointers globally inside win32k,
 * Instead, we're going to keep it static to this file and keep it organized in struct
 * we obtained with the IOCTRL.
 */
static REACTOS_WIN32K_DXGKRNL_INTERFACE DxgAdapterCallbacks = {0};

BOOL
TryHackedDxgkrnlStartAdapter()
{
    PIRP Irp;
    KEVENT Event;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT DeviceObject;
    IO_STATUS_BLOCK IoStatusBlock;
    UNICODE_STRING DestinationString;
    NTSTATUS Status = STATUS_PROCEDURE_NOT_FOUND;

    DPRINT("TryHackedDxgkrnlAdapterStart: Attempting to see if this is windows Dxgkrnl\n");
    /* First let's grab the RDDM objects */
    RtlInitUnicodeString(&DestinationString, L"\\Device\\DxgKrnl");
    Status = IoGetDeviceObjectPointer(&DestinationString, FILE_ALL_ACCESS, &FileObject, &DeviceObject);
    if(Status != STATUS_SUCCESS)
    {
        DPRINT("Setting up DxgKrnl Failed\n");
        goto BypassDxgkrnl;
    }

    /* Build event and create IRP */
    DPRINT("TryHackedDxgkrnlAdapterStart: Building IOCTRL with DxgKrnl\n");
    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IOCTL_VIDEO_I_AM_REACTOS,
                                          DeviceObject,
                                          NULL,
                                          0,
                                          NULL,
                                          0,
                                          TRUE,
                                          &Event,
                                          &IoStatusBlock);
    Status = IofCallDriver(DeviceObject, Irp);
    DPRINT("TryHackedDxgkrnlAdapterStart: Status %d\n", IoStatusBlock.Status);
    if (IoStatusBlock.Status != STATUS_SUCCESS)
    {
        DPRINT1("Wait... This is Windows DXGKNRL.SYS >:(\n");
        IsWDDMOn = TRUE;
        return FALSE;
    }
    else
    {
        DPRINT("TryHackedDxgkrnlAdapterStart: ReactOS AdapterStart hack triggered\n");
        IsWDDMOn = TRUE;
        return TRUE;
    }
BypassDxgkrnl:
    DPRINT("TryHackedDxgkrnlAdapterStart: Dxgkrnl is not loaded\n");
    return TRUE;
}
/*
 * This looks like it's done inside DxDdStartupDxGraphics, but i'd rather keep this organized.
 * Dxg gets start inveitibly anyway it seems atleast on vista.
 */
VOID
DxStartupDxgkInt()
{
    DPRINT("DxStartupDxgkInt: Entry\n");
    /* FIXME: Eventually we will have to use Watchdog.sus for this */
    if (!TryHackedDxgkrnlStartAdapter())
    {
        DPRINT1("You're running with Vista DXGKRNL, I hope you have watchdog\n");
        DPRINT1("Starting DxgKrnl using the vista method:\n");

        /* Now start the adapter using watchdog by notify the state change */
        //DrvNotifySessionStateChange(1);
    }

    //TODO: Let DxgKrnl know it's time to start all adapters, and obtain the win32k<->dxgkrnl interface via an IOCTRL.
}

NTSTATUS
APIENTRY
EngQueryW32kCddInterface(HANDLE DriverHandle, UINT32 Something,
                          PVOID W32kCddInterface,
                          PVOID DxgAdapter,
                          PVOID OkayLol, PVOID ProcessLocal)
{
    UNIMPLEMENTED;
    return 0;
}

/*
 * The following APIs all have the same idea,
 * Most of the parameters are stuffed in custom typedefs with a bunch of types inside them.
 * The idea here is this:
 * if we're dealing with a d3dkmt API that directly calls into a miniport if the function pointer doesn't
 * exist we're returning STATUS_PROCEDURE_NOT_FOUND.
 *
 * This essentially means the Dxgkrnl interface was never made as Win32k doesn't do any handling for these routines.
 */

NTSTATUS
APIENTRY
NtGdiDdDDICreateAllocation(_Inout_ PD3DKMT_CREATEALLOCATION unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.DxgkIntPfnCreateAllocation)
        return STATUS_PROCEDURE_NOT_FOUND;

    return DxgAdapterCallbacks.DxgkIntPfnCreateAllocation(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICheckMonitorPowerState(_In_ PD3DKMT_CHECKMONITORPOWERSTATE unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.DxgkIntPfnCheckMonitorPowerState)
        return STATUS_PROCEDURE_NOT_FOUND;

    return DxgAdapterCallbacks.DxgkIntPfnCheckMonitorPowerState(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICheckOcclusion(_In_ PD3DKMT_CHECKOCCLUSION unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.DxgkIntPfnCheckOcclusion)
        return STATUS_PROCEDURE_NOT_FOUND;

    return DxgAdapterCallbacks.DxgkIntPfnCheckOcclusion(unnamedParam1);
}


NTSTATUS
APIENTRY
NtGdiDdDDICloseAdapter(_In_ PD3DKMT_CLOSEADAPTER unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.DxgkIntPfnCloseAdapter)
        return STATUS_PROCEDURE_NOT_FOUND;

    return DxgAdapterCallbacks.DxgkIntPfnCloseAdapter(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateContext(_Inout_ PD3DKMT_CREATECONTEXT unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.DxgkIntPfnCreateContext)
        return STATUS_PROCEDURE_NOT_FOUND;

    return DxgAdapterCallbacks.DxgkIntPfnCreateContext(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateDevice(_Inout_ PD3DKMT_CREATEDEVICE unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.DxgkIntPfnCreateDevice)
        return STATUS_PROCEDURE_NOT_FOUND;

    return DxgAdapterCallbacks.DxgkIntPfnCreateDevice(unnamedParam1);
}

NTSTATUS
APIENTRY
NtGdiDdDDICreateOverlay(_Inout_ PD3DKMT_CREATEOVERLAY unnamedParam1)
{
    if (!unnamedParam1)
        STATUS_INVALID_PARAMETER;

    if (!DxgAdapterCallbacks.DxgkIntPfnCreateOverlay)
        return STATUS_PROCEDURE_NOT_FOUND;

    return DxgAdapterCallbacks.DxgkIntPfnCreateOverlay(unnamedParam1);
}
