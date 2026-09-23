/*
 * PROJECT:     ReactOS Win32k (WDDM display path)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     win32k <-> dxgkrnl bootstrap - load DxgKrnl + acquire the win32k callback table
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 *
 * Ported from Reference/win10/win32kbase.c (DlpLoadDxgkrnl:110414, DlInitDxgkrnl:110290). This is
 * the win32k side of WDDM: it opens \Device\DxgKrnl when a miniport has loaded dxgkrnl, and sends
 * IOCTL_VIDEO_GIVE_CALLSBACK (0x23E057, INTERNAL_DEVICE_CONTROL) with a DXGKWIN32K_INTERFACE
 * (Version 22) that dxgkrnl fills with the D3DKMT entry points - the table win32k routes the
 * D3DKMT* APIs through (gDxgkInterface). DarkFire's WDDM upgrade to the otherwise-XPDM win32k.
 */

#include <win32k.h>
#include <reactos/rddm/rddm_private.h>

#define NDEBUG
#include <debug.h>

/* The engine interface dxgkrnl calls back through (gdi/eng/dxgkeng.c) */
struct _DXGKWIN32KENG_INTERFACE;
extern struct _DXGKWIN32KENG_INTERFACE gDxgkWin32kEngInterface;

typedef NTSTATUS (NTAPI *PFN_DxgkProcessCallout)(
    _Inout_ PVOID *DxProcess,
    _In_ const struct _DXGKWIN32KENG_INTERFACE *EngInterface,
    _In_ BOOLEAN Create);

#define DL_MODALITY_PATH_SIZE       216
#define DL_MODALITY_HEADER_SIZE     48
#define DL_MODALITY_INITIAL_PATHS   8
#define DL_MODALITY_MAX_PATHS       64

/*
 * How dxgkrnl builds the path list. These are mutually exclusive and it picks in this order:
 * ALL_PATHS enumerates what the adapter can drive, PERSISTED reads the saved configuration and
 * fails when there is none. NO_OPTIMIZE does not query, it only marks the paths already there.
 */
#define DL_MODALITY_PERSISTED       0x0000000F
#define DL_MODALITY_ALL_PATHS       0x00000010
#define DL_MODALITY_NO_OPTIMIZE     0x00004000
#define DL_MODALITY_APPLY           0x00020000

/*
 * The display configuration dxgkrnl applies to an adapter: this header followed by PathArraySize
 * entries of DL_MODALITY_PATH_SIZE bytes. AppliedCount/AppliedPaths are filled by the apply.
 * Reference win32kbase.c:16845 (AllocPathsModality).
 */
typedef struct _D3DKMT_GETPATHSMODALITY
{
    ULONG  Reserved0[5];
    USHORT PathCount;
    USHORT PathArraySize;
    ULONG  Reserved24[3];
    ULONG  AppliedCount;
    PVOID  AppliedPaths;
} D3DKMT_GETPATHSMODALITY, *PD3DKMT_GETPATHSMODALITY;

C_ASSERT(FIELD_OFFSET(D3DKMT_GETPATHSMODALITY, PathCount) == 20);
C_ASSERT(FIELD_OFFSET(D3DKMT_GETPATHSMODALITY, PathArraySize) == 22);
C_ASSERT(FIELD_OFFSET(D3DKMT_GETPATHSMODALITY, AppliedCount) == 36);
C_ASSERT(FIELD_OFFSET(D3DKMT_GETPATHSMODALITY, AppliedPaths) == 40);
C_ASSERT(sizeof(D3DKMT_GETPATHSMODALITY) == DL_MODALITY_HEADER_SIZE);

/* RequiredPaths takes the path count dxgkrnl found, which is how it reports the array too small */
typedef NTSTATUS (NTAPI *PFN_DxgkPathsModality)(
    _In_ ULONG Flags,
    _Inout_ PD3DKMT_GETPATHSMODALITY Modality,
    _Out_opt_ PUSHORT RequiredPaths);

typedef NTSTATUS (NTAPI *PFN_DxgkApplyPathsModality)(
    _In_ ULONG Flags,
    _Inout_ PD3DKMT_GETPATHSMODALITY Modality);

typedef NTSTATUS (NTAPI *PFN_DxgkFinalizePathsModality)(
    _Inout_ PD3DKMT_GETPATHSMODALITY Modality);

typedef VOID (NTAPI *PFN_DxgkFreePathsModality)(
    _In_ PD3DKMT_GETPATHSMODALITY Modality);

/*
 * The DXGKWIN32K_INTERFACE: USHORT Size + USHORT Version, then Context/InterfaceReference/
 * InterfaceDereference and 232 pfnDxgk* slots dxgkrnl fills, 944 bytes on x86 and 1888 on x64.
 * The fully-typed layout (rxgkwddminterface.h) forward-declares ~157 D3DKMT structs opaquely,
 * which clash with the partial D3DKMT types win32k.h already pulls in - so the bootstrap names
 * only the slots it uses. Slot numbers are the pointer index from the start of the interface
 * (Reference win32kbase, which caches them one global per slot from 0x1C011D0E0).
 */
typedef struct _DXGKWIN32K_INTERFACE_BUF
{
    USHORT Size;
    USHORT Version;
    PVOID Context;                                      /* 1 */
    PVOID InterfaceReference;                           /* 2 */
    PVOID InterfaceDereference;                         /* 3 */
    PFN_DxgkProcessCallout pfnDxgkProcessCallout;       /* 4 */
    PVOID Reserved5[70];                                /* 5..74 */
    PFN_DxgkPathsModality pfnDxgkGetPathsModality;      /* 75 */
    PVOID pfnDxgkFunctionalizePathsModality;            /* 76, takes four arguments, unused here */
    PFN_DxgkApplyPathsModality pfnDxgkApplyPathsModality; /* 77 */
    PFN_DxgkFinalizePathsModality pfnDxgkFinalizePathsModality; /* 78 */
    PVOID Reserved79;                                   /* 79 */
    PFN_DxgkFreePathsModality pfnDxgkFreePathsModality; /* 80 */
    PVOID Reserved81[155];                              /* 81..235 */
} DXGKWIN32K_INTERFACE_BUF;

C_ASSERT(sizeof(DXGKWIN32K_INTERFACE_BUF) == 236 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkGetPathsModality) == 75 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkFreePathsModality) == 80 * sizeof(PVOID));

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
 * @brief Create or drop a process's DXGPROCESS. Reference win32kbase GdiProcessCallout.
 *        dxgkrnl frees the DXGPROCESS itself when the process dies; the drop only clears DxProcess.
 */
NTSTATUS NTAPI
DlProcessCallout(
    _Inout_ PPROCESSINFO ppi,
    _In_ BOOLEAN Create)
{
    if (!gbDxgkInitialized)
        return STATUS_SUCCESS;

    return gDxgkInterface.pfnDxgkProcessCallout(&ppi->DxProcess, &gDxgkWin32kEngInterface, Create);
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
    gDxgkInterface.Size    = sizeof(gDxgkInterface);   /* 944 on x86, 1888 on x64 */

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

        /* CSRSS became a GUI process before the interface existed, so it gets its DXGPROCESS now */
        Status = DlProcessCallout(PsGetCurrentProcessWin32Process(), TRUE);
        if (!NT_SUCCESS(Status))
            DPRINT1("win32k: DxgkProcessCallout for CSRSS failed 0x%lX\n", Status);

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

static VOID
DlpFreePathsModality(
    _In_ PD3DKMT_GETPATHSMODALITY Modality)
{
    if (gDxgkInterface.pfnDxgkFreePathsModality != NULL)
        gDxgkInterface.pfnDxgkFreePathsModality(Modality);
    ExFreePoolWithTag(Modality, GDITAG_TEMP);
}

/**
 * @brief Ask dxgkrnl for the display paths. Grows the array while dxgkrnl reports it too small,
 *        the way the Reference GetPathsModality wrapper does.
 */
static NTSTATUS
DlpQueryPathsModality(
    _In_ ULONG Flags,
    _Outptr_result_maybenull_ PD3DKMT_GETPATHSMODALITY *Result)
{
    PD3DKMT_GETPATHSMODALITY Modality;
    USHORT                   PathCount = DL_MODALITY_INITIAL_PATHS;
    USHORT                   Required;
    SIZE_T                   Size;
    NTSTATUS                 Status;

    *Result = NULL;

    for (;;)
    {
        Size = DL_MODALITY_HEADER_SIZE + PathCount * DL_MODALITY_PATH_SIZE;
        Modality = ExAllocatePoolZero(PagedPool, Size, GDITAG_TEMP);
        if (Modality == NULL)
            return STATUS_NO_MEMORY;

        Modality->PathCount = PathCount;
        Modality->PathArraySize = PathCount;

        Required = 0;
        Status = gDxgkInterface.pfnDxgkGetPathsModality(Flags, Modality, &Required);
        if (NT_SUCCESS(Status))
        {
            *Result = Modality;
            return Status;
        }

        DlpFreePathsModality(Modality);
        if (Status != STATUS_BUFFER_TOO_SMALL)
            return Status;

        /* dxgkrnl told us how many paths it has, so go straight to that */
        if (Required > PathCount)
            PathCount = Required;
        else
            PathCount = (USHORT)(PathCount + DL_MODALITY_INITIAL_PATHS);

        if (PathCount > DL_MODALITY_MAX_PATHS)
            return STATUS_BUFFER_TOO_SMALL;
    }
}

/**
 * @brief Build the display configuration and hand it to dxgkrnl. Without this no VidPn is ever
 *        committed, so the adapter's display sources keep the mode and CDD allocations they were
 *        constructed with, which is nothing. Reference win32kbase DrvSetDisplayConfig, minus the
 *        CDS registry store and the GDI mode change win32k drives its own way.
 */
static NTSTATUS
DlpApplyDisplayConfig(VOID)
{
    PD3DKMT_GETPATHSMODALITY Modality = NULL;
    NTSTATUS                 Status;

    if ((gDxgkInterface.pfnDxgkGetPathsModality == NULL) ||
        (gDxgkInterface.pfnDxgkApplyPathsModality == NULL))
        return STATUS_NOT_SUPPORTED;

    /* The saved configuration first, then whatever the adapter can drive */
    Status = DlpQueryPathsModality(DL_MODALITY_PERSISTED, &Modality);
    if (NT_SUCCESS(Status) && (Modality->PathCount == 0))
    {
        DlpFreePathsModality(Modality);
        Status = STATUS_UNSUCCESSFUL;
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("win32k: no saved display configuration (0x%lX), enumerating paths\n", Status);
        Status = DlpQueryPathsModality(DL_MODALITY_ALL_PATHS, &Modality);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("win32k: DxgkGetPathsModality failed 0x%lX\n", Status);
            return Status;
        }
    }

    DPRINT1("win32k: display configuration has %u path(s)\n", Modality->PathCount);

    /* This one marks the paths for the apply, it does not query again */
    Status = gDxgkInterface.pfnDxgkGetPathsModality(DL_MODALITY_NO_OPTIMIZE, Modality, NULL);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("win32k: DxgkGetPathsModality(NoOptimize) failed 0x%lX\n", Status);
        goto Cleanup;
    }

    Status = gDxgkInterface.pfnDxgkApplyPathsModality(DL_MODALITY_NO_OPTIMIZE | DL_MODALITY_APPLY,
                                                     Modality);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("win32k: DxgkApplyPathsModality failed 0x%lX\n", Status);
        goto Cleanup;
    }

    DPRINT1("win32k: display configuration applied, %u path(s)\n", Modality->AppliedCount);

    if (gDxgkInterface.pfnDxgkFinalizePathsModality != NULL)
        gDxgkInterface.pfnDxgkFinalizePathsModality(Modality);

Cleanup:
    DlpFreePathsModality(Modality);
    return Status;
}

/**
 * @brief Report the console session to watchdog, which is what lets dxgkrnl start its adapters.
 *        Must run in CSRSS. Reference win32kbase DrvNotifySessionStateChange.
 */
VOID NTAPI
DlNotifySessionOpen(VOID)
{
    PPROCESSINFO ppi = PsGetCurrentProcessWin32Process();
    NTSTATUS Status;

    /*
     * dxgkrnl blocks here until an adapter has started and needs CSRSS's DXGPROCESS, so only
     * call once a miniport loaded dxgkrnl and the process callout ran.
     */
    if (!gbDxgkInitialized || (ppi == NULL) || (ppi->DxProcess == NULL))
        return;

    Status = SMgrNotifySessionChange(DL_SESSION_OPEN);
    if (!NT_SUCCESS(Status))
        DPRINT1("win32k: SMgrNotifySessionChange failed 0x%lX\n", Status);
}

/**
 * @brief Commit the display configuration. Must run in CSRSS, and only once the session owns the
 *        adapter: the CCD topology is built from the session's adapter views, which come from the
 *        session usage claim EngpRegisterGraphicsDevice sends.
 */
VOID NTAPI
DlApplyDisplayConfig(VOID)
{
    PPROCESSINFO ppi = PsGetCurrentProcessWin32Process();
    NTSTATUS Status;

    if (!gbDxgkInitialized || (ppi == NULL) || (ppi->DxProcess == NULL))
        return;

    Status = DlpApplyDisplayConfig();
    if (!NT_SUCCESS(Status))
        DPRINT1("win32k: DlpApplyDisplayConfig failed 0x%lX\n", Status);
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
