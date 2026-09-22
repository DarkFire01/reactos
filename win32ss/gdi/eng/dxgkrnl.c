/*
 * PROJECT:     ReactOS Win32k (WDDM display path)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     win32k <-> dxgkrnl bootstrap - load DxgKrnl + acquire the win32k callback table
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 *
 * Ported from Reference/win10/win32kbase.c (DlpLoadDxgkrnl:110414, DlInitDxgkrnl:110290). This is
 * the win32k side of WDDM: it opens \Device\DxgKrnl when a miniport has loaded dxgkrnl, and sends
 * IOCTL_VIDEO_GIVE_CALLSBACK (0x23E057, INTERNAL_DEVICE_CONTROL) with a 944-byte DXGKWIN32K_INTERFACE
 * (Version 22) that dxgkrnl fills with the D3DKMT entry points - the table win32k routes the
 * D3DKMT* APIs through (gDxgkInterface). DarkFire's WDDM upgrade to the otherwise-XPDM win32k.
 */

#include <win32k.h>
#include <reactos/rddm/rddm_private.h>

#define NDEBUG
#include <debug.h>

/*
 * The DXGKWIN32K_INTERFACE is 944 (0x3B0) bytes: USHORT Size + USHORT Version, then Context/
 * InterfaceReference/InterfaceDereference and 232 pfnDxgk* slots dxgkrnl fills. The fully-typed
 * layout (rxgkwddminterface.h) forward-declares ~157 D3DKMT structs opaquely, which clash with the
 * partial D3DKMT types win32k.h already pulls in - so the bootstrap uses a sized opaque buffer
 * (it only needs to stamp Size/Version and hand the table to dxgkrnl). D3DKMT* routing through the
 * filled pfn slots, with the real win32k D3DKMT types, is the next step.
 */
typedef struct _DXGKWIN32K_INTERFACE_BUF
{
    USHORT Size;
    USHORT Version;
    UCHAR  Payload[0x3B0 - 4];   /* Context + Ref/Deref + 232 pfnDxgk* slots */
} DXGKWIN32K_INTERFACE_BUF;

C_ASSERT(sizeof(DXGKWIN32K_INTERFACE_BUF) == 0x3B0);

/* Fills the NtGdiDdDDI* D3DKMT callback table (gdi/ntgdi/d3dkmt.c) via IOCTL_VIDEO_REGISTER_RXGK. */
NTSTATUS NTAPI DxgRegisterAdapterCallbacks(_In_ PDEVICE_OBJECT pDxgkrnl);

/* The DxgKrnl device + the D3DKMT callback table win32k drives WDDM through. */
PDEVICE_OBJECT           gpDxgkDeviceObject = NULL;
PFILE_OBJECT             gpDxgkFileObject = NULL;
DXGKWIN32K_INTERFACE_BUF gDxgkInterface = { 0 };
BOOLEAN                  gbDxgkInitialized = FALSE;

/* Exported by watchdog.sys */
NTSTATUS NTAPI SMgrNotifySessionChange(_In_ ULONG SessionState);

#define DL_SESSION_OPEN     0

/**
 * @brief Open \Device\DxgKrnl. Unlike Windows, win32k never loads dxgkrnl itself: only a WDDM
 *        miniport does, so a boot without one stays on XDDM.
 */
static NTSTATUS
DlpOpenDxgkrnl(VOID)
{
    UNICODE_STRING DeviceName;

    RtlInitUnicodeString(&DeviceName, L"\\Device\\DxgKrnl");
    return IoGetDeviceObjectPointer(&DeviceName, GENERIC_READ | GENERIC_WRITE,
                                    &gpDxgkFileObject, &gpDxgkDeviceObject);
}

/**
 * @brief Open dxgkrnl + acquire the DXGKWIN32K_INTERFACE. Reference win32kbase.c:110290 (DlInitDxgkrnl).
 *        After this, gDxgkInterface.pfnDxgk* are the D3DKMT entry points win32k calls.
 */
NTSTATUS NTAPI
DlInitDxgkrnl(VOID)
{
    KEVENT          Event;
    IO_STATUS_BLOCK Iosb;
    PIRP            Irp;
    NTSTATUS        Status;

    if (gbDxgkInitialized)
        return STATUS_SUCCESS;

    Status = DlpOpenDxgkrnl();
    if (!NT_SUCCESS(Status))
        return Status;

    /* win32k stamps Version/Size; dxgkrnl fills the pfn slots (ref :110322). */
    gDxgkInterface.Version = 22;
    gDxgkInterface.Size    = sizeof(gDxgkInterface);   /* 944 (0x3B0) */

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IOCTL_VIDEO_GIVE_CALLSBACK,
                                        gpDxgkDeviceObject,
                                        &gDxgkInterface, sizeof(gDxgkInterface),
                                        &gDxgkInterface, sizeof(gDxgkInterface),
                                        TRUE,   /* INTERNAL_DEVICE_CONTROL (ref :110331 = 1) */
                                        &Event, &Iosb);
    if (Irp == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(gpDxgkDeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }

    if (NT_SUCCESS(Status))
    {
        gbDxgkInitialized = TRUE;
        DPRINT1("win32k: DxgKrnl WDDM interface acquired (Version %u)\n", gDxgkInterface.Version);

        /* Also populate the D3DKMT callback table the NtGdiDdDDI* thunks route through. */
        Status = DxgRegisterAdapterCallbacks(gpDxgkDeviceObject);
        if (!NT_SUCCESS(Status))
            DPRINT1("win32k: DxgRegisterAdapterCallbacks failed 0x%lX\n", Status);
        Status = STATUS_SUCCESS;   /* the D3DKMT table is optional for the display path */
    }
    else
    {
        DPRINT1("win32k: IOCTL_VIDEO_GIVE_CALLSBACK failed 0x%lX\n", Status);
    }
    return Status;
}

/**
 * @brief Report the console session to watchdog, which is what lets dxgkrnl start its adapters.
 *        Must run in CSRSS. Reference win32kbase DrvNotifySessionStateChange.
 */
VOID NTAPI
DlNotifySessionOpen(VOID)
{
    NTSTATUS Status;

    /* dxgkrnl blocks here until an adapter has started, so only call when a miniport loaded it */
    if (gpDxgkDeviceObject == NULL)
        return;

    Status = SMgrNotifySessionChange(DL_SESSION_OPEN);
    if (!NT_SUCCESS(Status))
        DPRINT1("win32k: SMgrNotifySessionChange failed 0x%lX\n", Status);
}

/** @brief Release the dxgkrnl device (Reference DlpUnloadDxgkrnl). */
VOID NTAPI
DlUnloadDxgkrnl(VOID)
{
    if (gpDxgkFileObject != NULL)
    {
        ObDereferenceObject(gpDxgkFileObject);
        gpDxgkFileObject = NULL;
        gpDxgkDeviceObject = NULL;
    }
    gbDxgkInitialized = FALSE;
}
