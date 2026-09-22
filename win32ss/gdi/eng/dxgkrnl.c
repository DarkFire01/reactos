/*
 * PROJECT:     ReactOS Win32k (WDDM display path)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     win32k <-> dxgkrnl bootstrap - load DxgKrnl + acquire the win32k callback table
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 *
 * Ported from Reference/win10/win32kbase.c (DlpLoadDxgkrnl:110414, DlInitDxgkrnl:110290). This is
 * the win32k side of WDDM: it loads dxgkrnl.sys, opens \Device\DxgKrnl, and sends
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

/**
 * @brief Load dxgkrnl.sys and open \Device\DxgKrnl. Reference win32kbase.c:110414 DlpLoadDxgkrnl.
 */
static NTSTATUS
DlpLoadDxgkrnl(VOID)
{
    UNICODE_STRING ServiceName, DeviceName;
    LARGE_INTEGER  Delay;
    NTSTATUS       Status;
    ULONG          Retries = 10;

    RtlInitUnicodeString(&ServiceName,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\DXGKrnl");
    Status = ZwLoadDriver(&ServiceName);
    if (!NT_SUCCESS(Status) && Status != STATUS_IMAGE_ALREADY_LOADED)
    {
        DPRINT1("win32k: ZwLoadDriver(DXGKrnl) failed 0x%lX\n", Status);
        return Status;
    }

    /* The device may appear a moment after the service starts - retry briefly (ref :110437). */
    RtlInitUnicodeString(&DeviceName, L"\\Device\\DxgKrnl");
    for (;;)
    {
        Status = IoGetDeviceObjectPointer(&DeviceName, GENERIC_READ | GENERIC_WRITE,
                                          &gpDxgkFileObject, &gpDxgkDeviceObject);
        if (NT_SUCCESS(Status))
            break;
        Delay.QuadPart = -50000;   /* 5 ms */
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        if (--Retries == 0)
            return Status;
    }
    return STATUS_SUCCESS;
}

/**
 * @brief Load dxgkrnl + acquire the DXGKWIN32K_INTERFACE. Reference win32kbase.c:110290 (DlInitDxgkrnl).
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

    Status = DlpLoadDxgkrnl();
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
