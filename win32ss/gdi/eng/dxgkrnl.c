/*
 * PROJECT:     ReactOS Win32k (WDDM display path)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     win32k <-> dxgkrnl bootstrap - load DxgKrnl + acquire the win32k callback table
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include <win32k.h>
#include <reactos/rddm/rddm_private.h>

#define NDEBUG
#include <debug.h>

#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
/* Exported by ntoskrnl */
NTSYSAPI
PVOID
NTAPI
RtlFindExportedRoutineByName(
    _In_ PVOID ImageBase,
    _In_ PCSTR RoutineName);
#endif

/* The engine interface dxgkrnl calls back through (gdi/eng/dxgkeng.c) */
struct _DXGKWIN32KENG_INTERFACE;
extern struct _DXGKWIN32KENG_INTERFACE gDxgkWin32kEngInterface;

typedef NTSTATUS (NTAPI *PFN_DxgkProcessCallout)(
    _Inout_ PVOID *DxProcess,
    _In_ const struct _DXGKWIN32KENG_INTERFACE *EngInterface,
    _In_ BOOLEAN Create);

#define DL_MODALITY_PATH_SIZE       sizeof(DL_MODALITY_PATH)
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
/* The fixed fields and two pointers dxgkrnl frees along with the modality */
#define DL_MODALITY_HEADER_SIZE     (40 + 2 * sizeof(PVOID))
#else
/* The fixed fields plus the applied paths pointer, so the header is narrower on a 32 bit build */
#define DL_MODALITY_HEADER_SIZE     (40 + sizeof(PVOID))
#endif
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

/* The only flags Functionalize is given, whatever the caller asked Get for. */
#define DL_MODALITY_FUNCTIONALIZE   0x00028000

/*
 * A legacy mode change starts from the topology running now, or the saved one when the result is
 * to be saved, and asks Get for the CDS flavor of it.
 */
#define DL_MODALITY_CURRENT         0x00000040
#define DL_MODALITY_CDS             0x00009000

/* Functionalize for a legacy mode change, relaxed when the closest mode will do */
#define DL_FUNCTIONALIZE_CDS        0x00008000
#define DL_FUNCTIONALIZE_RELAXED    0x00020000

/* PersistPathsModality index that writes the paths to the display database */
#define DL_PERSIST_TO_DATABASE      1

/*
 * The display configuration dxgkrnl applies to an adapter: this header followed by PathArraySize
 * entries of DL_MODALITY_PATH_SIZE bytes. AppliedCount/AppliedPaths are filled by the apply.
 */
typedef struct _D3DKMT_GETPATHSMODALITY
{
    ULONG  Reserved0[5];
    USHORT PathCount;
    USHORT PathArraySize;
    ULONG  Reserved24[3];
    ULONG  AppliedCount;
    PVOID  AppliedPaths;
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
    PVOID  Reserved48;
#endif
} D3DKMT_GETPATHSMODALITY, *PD3DKMT_GETPATHSMODALITY;

C_ASSERT(FIELD_OFFSET(D3DKMT_GETPATHSMODALITY, PathCount) == 20);
C_ASSERT(FIELD_OFFSET(D3DKMT_GETPATHSMODALITY, PathArraySize) == 22);
C_ASSERT(FIELD_OFFSET(D3DKMT_GETPATHSMODALITY, AppliedCount) == 36);
C_ASSERT(FIELD_OFFSET(D3DKMT_GETPATHSMODALITY, AppliedPaths) == 40);
C_ASSERT(sizeof(D3DKMT_GETPATHSMODALITY) == DL_MODALITY_HEADER_SIZE);
#if defined(_WIN64) && (DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11)
C_ASSERT(DL_MODALITY_HEADER_SIZE == 56);
#elif defined(_WIN64)
/* Windows is 64 bit, where the header comes to 48 bytes */
C_ASSERT(DL_MODALITY_HEADER_SIZE == 48);
#endif

/* The target signal a path drives, laid out as the WDK's D3DKMDT_VIDEO_SIGNAL_INFO */
typedef struct _DL_MODALITY_SIGNAL
{
    ULONG     VideoStandard;
    ULONG     TotalSize[2];
    ULONG     ActiveSize[2];
    ULONG     VSyncFreq[2];
    ULONG     HSyncFreq[2];
    SIZE_T    PixelRate;
    ULONG     ScanLineOrdering;
} DL_MODALITY_SIGNAL;

/*
 * One display path in the modality, source to target. Functionalize fills DevMode with the mode
 * the source runs in, desktop position included, and the apply writes StatusApply. Only the first
 * path of a clone group names the source's mode.
 */
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
typedef struct _DL_MODALITY_PATH
{
    ULONG64            Flags;
    ULONG64            Reserved08;
    LUID               AdapterLuid;
    ULONG              VidPnSourceId;
    ULONG              VidPnTargetId;
    UCHAR              Reserved20[0x98];
    ULONG              CloneGroupId;
    UCHAR              ReservedBC[0x3C];
    PDEVMODEW          DevMode;
    UCHAR              Reserved100[0x10];
    LONG               StatusApply;
    UCHAR              Reserved114[0x14];
} DL_MODALITY_PATH, *PDL_MODALITY_PATH;

C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, AdapterLuid) == 0x10);
C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, CloneGroupId) == 0xB8);
C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, DevMode) == 0xF8);
C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, StatusApply) == 0x110);
C_ASSERT(sizeof(DL_MODALITY_PATH) == 0x128);
#else
typedef struct _DL_MODALITY_PATH
{
    ULONG              Flags;
    ULONG              FixedFlags;
    LUID               AdapterLuid;
    ULONG              VidPnSourceId;
    ULONG              VidPnTargetId;
    DL_MODALITY_SIGNAL TargetMode;
    ULONG              TargetOutputTechnology;
    ULONG              TargetBaseOutputTechnology;
    ULONG              SourcePrimSurfSize[2];
    ULONG              SourceVisibleRegionSize[2];
    ULONG              SourceStride;
    ULONG              SourcePixelFormat;
    ULONG              SourceColorBasis;
    ULONG              SourcePixelValueAccessMode;
    UCHAR              IsStereo;
    UCHAR              VirtualModeSupported;
    ULONG              Rotation;
    ULONG              ScalingToApply;
    ULONG              ScalingIntent;
    POINTL             Position;
    POINTL             ContentResolution;
    POINTL             ContentSizeRecommended;
    RECTL              DwmClipBox;
    PDEVMODEW          DevMode;
    ULONG              Reserved0;
    ULONG              Reserved1;
    PVOID              Reserved2;
    ULONG              CloneGroupId;
    ULONG              DisplayId;
    LONG               StatusApply;
    ULONG              DatabaseVersion;
} DL_MODALITY_PATH, *PDL_MODALITY_PATH;

#ifdef _WIN64
C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, AdapterLuid) + DL_MODALITY_HEADER_SIZE == 56);
C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, VidPnSourceId) + DL_MODALITY_HEADER_SIZE == 64);
C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, DevMode) + DL_MODALITY_HEADER_SIZE == 224);
C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, CloneGroupId) + DL_MODALITY_HEADER_SIZE == 248);
C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, StatusApply) + DL_MODALITY_HEADER_SIZE == 256);
C_ASSERT(sizeof(DL_MODALITY_PATH) == 216);
#else
C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, DevMode) == 164);
C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, CloneGroupId) == 180);
C_ASSERT(sizeof(DL_MODALITY_PATH) == 196);
#endif
#endif /* DXGKRNL_TARGET */

/* The paths follow the header directly */
#define DL_MODALITY_PATHS(Modality) \
    ((PDL_MODALITY_PATH)((PUCHAR)(Modality) + DL_MODALITY_HEADER_SIZE))

/* RequiredPaths takes the path count dxgkrnl found, which is how it reports the array too small */
typedef NTSTATUS (NTAPI *PFN_DxgkPathsModality)(
    _In_ ULONG Flags,
    _Inout_ PD3DKMT_GETPATHSMODALITY Modality,
    _Out_opt_ PUSHORT RequiredPaths);

/*
 * Choosing a source mode for each path, which is a separate step from enumerating them: the paths
 * come back from Get as candidates, and Apply only commits a mode that this put there.
 */
typedef NTSTATUS (NTAPI *PFN_DxgkFunctionalizePathsModality)(
    _In_ ULONG Flags,
    _Inout_ PD3DKMT_GETPATHSMODALITY Modality);

#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
/* The display scenario context may be NULL */
typedef NTSTATUS (NTAPI *PFN_DxgkApplyPathsModality)(
    _In_ ULONG Flags,
    _Inout_ PD3DKMT_GETPATHSMODALITY Modality,
    _In_opt_ PVOID ScenarioContext);
#else
typedef NTSTATUS (NTAPI *PFN_DxgkApplyPathsModality)(
    _In_ ULONG Flags,
    _Inout_ PD3DKMT_GETPATHSMODALITY Modality);
#endif

typedef NTSTATUS (NTAPI *PFN_DxgkFinalizePathsModality)(
    _Inout_ PD3DKMT_GETPATHSMODALITY Modality);

typedef VOID (NTAPI *PFN_DxgkFreePathsModality)(
    _In_ PD3DKMT_GETPATHSMODALITY Modality);

/* Saves the paths as the source's configuration. Index 1 writes them to the display database */
typedef NTSTATUS (NTAPI *PFN_DxgkPersistPathsModality)(
    _In_ ULONG Index,
    _In_ PD3DKMT_GETPATHSMODALITY Modality);

/*
 * A mode a legacy ChangeDisplaySettings caller asked of one source. dxgkrnl keeps it in its CDS
 * journal, and applying the journal to a modality is how that mode reaches the source's path.
 */
typedef struct _DL_CDS_REQUEST
{
    ULONG Flags;
    LUID AdapterLuid;
    ULONG VidPnSourceId;
    ULONG VidPnTargetId;
    D3DKMDT_VIDEO_OUTPUT_TECHNOLOGY OutputTechnology;
    D3DKMDT_VIDEO_OUTPUT_TECHNOLOGY BaseOutputTechnology;
    const DEVMODEW *pDevMode;
} DL_CDS_REQUEST, *PDL_CDS_REQUEST;

#define DL_CDS_TRY_CLOSEST          0x00000002
#define DL_CDS_VALIDATE_ONLY        0x00000004
#define DL_CDS_FULLSCREEN           0x00000008
#define DL_CDS_NOT_PHYSICAL         0x00000010

typedef NTSTATUS (NTAPI *PFN_DxgkAugmentCdsj)(
    _Inout_ PDL_CDS_REQUEST Request);

typedef NTSTATUS (NTAPI *PFN_DxgkApplyCdsjToPathsModality)(
    _Inout_ PD3DKMT_GETPATHSMODALITY *Modality,
    _Inout_ PDL_CDS_REQUEST Request);

#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
/*
 * The Windows 11 DXGKWIN32K_INTERFACE: USHORT Size + USHORT Version, the image dxgkrnl looks up
 * win32k routines in, Context/InterfaceReference/InterfaceDereference and 99 slots dxgkrnl fills,
 * 832 bytes. The D3DKMT entry points are exports of dxgkrnl instead of slots.
 */
typedef struct _DXGKWIN32K_INTERFACE_BUF
{
    USHORT Size;
    USHORT Version;
    PVOID Win32kImageBase;                              /* 1 */
    PVOID Context;                                      /* 2 */
    PVOID InterfaceReference;                           /* 3 */
    PVOID InterfaceDereference;                         /* 4 */
    PFN_DxgkProcessCallout pfnDxgkProcessCallout;       /* 5 */
    PVOID Reserved6[2];                                 /* 6..7 */
    PVOID pfnDxgkOpenAdapter;                           /* 8 */
    PVOID Reserved9[9];                                 /* 9..17 */
    PFN_DxgkPathsModality pfnDxgkGetPathsModality;      /* 18 */
    PFN_DxgkFunctionalizePathsModality pfnDxgkFunctionalizePathsModality; /* 19 */
    PFN_DxgkApplyPathsModality pfnDxgkApplyPathsModality; /* 20 */
    PFN_DxgkFinalizePathsModality pfnDxgkFinalizePathsModality; /* 21 */
    PFN_DxgkPersistPathsModality pfnDxgkPersistPathsModality; /* 22 */
    PFN_DxgkFreePathsModality pfnDxgkFreePathsModality; /* 23 */
    PFN_DxgkAugmentCdsj pfnDxgkAugmentCdsj;             /* 24 */
    PVOID Reserved25[50];                               /* 25..74 */
    PFN_DxgkApplyCdsjToPathsModality pfnDxgkApplyCdsjToPathsModality; /* 75 */
    PVOID Reserved76[28];                               /* 76..103 */
} DXGKWIN32K_INTERFACE_BUF;

C_ASSERT(sizeof(DXGKWIN32K_INTERFACE_BUF) == 104 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkProcessCallout) == 5 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkOpenAdapter) == 8 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkGetPathsModality) == 18 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkAugmentCdsj) == 24 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkApplyCdsjToPathsModality) == 75 * sizeof(PVOID));

#define DL_INTERFACE_VERSION    50

/* The D3DKMT entry point of each slot number, taken from the exports of dxgkrnl */
static PVOID DlpD3DKMTEntries[236];

#else
/*
 * The DXGKWIN32K_INTERFACE: USHORT Size + USHORT Version, then Context/InterfaceReference/
 * InterfaceDereference and 232 pfnDxgk* slots dxgkrnl fills, 944 bytes on x86 and 1888 on x64.
 * The fully-typed layout (rxgkwddminterface.h) forward-declares ~157 D3DKMT structs opaquely,
 * which clash with the partial D3DKMT types win32k.h already pulls in - so the bootstrap names
 * only the slots it uses. Slot numbers are the pointer index from the start of the interface
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
    PFN_DxgkFunctionalizePathsModality pfnDxgkFunctionalizePathsModality; /* 76 */
    PFN_DxgkApplyPathsModality pfnDxgkApplyPathsModality; /* 77 */
    PFN_DxgkFinalizePathsModality pfnDxgkFinalizePathsModality; /* 78 */
    PFN_DxgkPersistPathsModality pfnDxgkPersistPathsModality; /* 79 */
    PFN_DxgkFreePathsModality pfnDxgkFreePathsModality; /* 80 */
    PFN_DxgkAugmentCdsj pfnDxgkAugmentCdsj;             /* 81 */
    PVOID Reserved82[127];                              /* 82..208 */
    PFN_DxgkApplyCdsjToPathsModality pfnDxgkApplyCdsjToPathsModality; /* 209 */
    PVOID Reserved210[26];                              /* 210..235 */
} DXGKWIN32K_INTERFACE_BUF;

C_ASSERT(sizeof(DXGKWIN32K_INTERFACE_BUF) == 236 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkGetPathsModality) == 75 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkFreePathsModality) == 80 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkAugmentCdsj) == 81 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkApplyCdsjToPathsModality) == 209 * sizeof(PVOID));

#define DL_INTERFACE_VERSION    22
#endif /* DXGKRNL_TARGET */


/* Fills the NtGdiDdDDI* D3DKMT callback table (gdi/ntgdi/d3dkmt.c) via IOCTL_VIDEO_REGISTER_RXGK. */
NTSTATUS NTAPI DxgRegisterAdapterCallbacks(_In_ PDEVICE_OBJECT pDxgkrnl);

/* The DxgKrnl device + the D3DKMT callback table win32k drives WDDM through. */
PDEVICE_OBJECT           gpDxgkDeviceObject = NULL;
PFILE_OBJECT             gpDxgkFileObject = NULL;
DXGKWIN32K_INTERFACE_BUF gDxgkInterface = { 0 };
BOOLEAN                  gbDxgkInitialized = FALSE;

/**
 * @brief Hands out a D3DKMT entry point from the interface dxgkrnl filled.
 *
 * @param Slot The pointer index of the entry point (DXGK_SLOT_*).
 *
 * @return The routine, or NULL when the interface is not up or dxgkrnl left the slot empty.
 */
PFN_DXGK_D3DKMT
NTAPI
DxgkGetD3DKMTSlot(
    _In_ ULONG Slot)
{
    if (!gbDxgkInitialized)
        return NULL;

#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
    if (Slot == DXGK_SLOT_OpenAdapter)
        return (PFN_DXGK_D3DKMT)gDxgkInterface.pfnDxgkOpenAdapter;

    if (Slot >= ARRAYSIZE(DlpD3DKMTEntries))
        return NULL;

    return (PFN_DXGK_D3DKMT)DlpD3DKMTEntries[Slot];
#else
    if (Slot >= sizeof(gDxgkInterface) / sizeof(PVOID))
        return NULL;

    return (PFN_DXGK_D3DKMT)(((PVOID *)&gDxgkInterface)[Slot]);
#endif
}

/* Exported by watchdog.sys, the Windows 11 one keeps the callout for the session it opens */
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
NTSTATUS NTAPI SMgrNotifySessionChange(_In_ ULONG SessionState, _In_opt_ PVOID VideoPortCallout);
#else
NTSTATUS NTAPI SMgrNotifySessionChange(_In_ ULONG SessionState);
#endif

#define DL_SESSION_OPEN     0

/**
 * @brief Open \Device\DxgKrnl. dxgkrnl is boot start, so it is there on every boot; whether the
 *        desktop runs on it depends on a WDDM display adapter starting.
 */
static NTSTATUS
DlpOpenDxgkrnl(VOID)
{
    UNICODE_STRING DeviceName;

    RtlInitUnicodeString(&DeviceName, L"\\Device\\DxgKrnl");
    return IoGetDeviceObjectPointer(&DeviceName, GENERIC_READ | GENERIC_WRITE,
                                    &gpDxgkFileObject, &gpDxgkDeviceObject);
}

/** @brief Does one of the driver's devices sit on a display class device? */
static BOOLEAN
DlpDriverDrivesDisplay(
    _In_ PDRIVER_OBJECT DriverObject)
{
    static const WCHAR szDisplayClass[] = L"{4d36e968-e325-11ce-bfc1-08002be10318}";
    WCHAR szClass[ARRAYSIZE(szDisplayClass)];
    PDEVICE_OBJECT DeviceObject, Pdo;
    ULONG cbClass;
    NTSTATUS Status;

    for (DeviceObject = DriverObject->DeviceObject;
         DeviceObject != NULL;
         DeviceObject = DeviceObject->NextDevice)
    {
        Pdo = IoGetDeviceAttachmentBaseRef(DeviceObject);
        Status = IoGetDeviceProperty(Pdo, DevicePropertyClassGuid, sizeof(szClass), szClass, &cbClass);
        ObDereferenceObject(Pdo);

        if (NT_SUCCESS(Status) && (_wcsicmp(szClass, szDisplayClass) == 0))
            return TRUE;
    }

    return FALSE;
}

/**
 * @brief Is a WDDM display adapter there? dxgkrnl takes over the dispatch routines of every
 *        miniport, and a display adapter, unlike a render only one, is in the display class.
 *        This holds from the miniport's AddDevice on, before dxgkrnl starts the adapter.
 */
static BOOLEAN
DlpHasWddmDisplayAdapter(VOID)
{
    UNICODE_STRING DirectoryName = RTL_CONSTANT_STRING(L"\\Driver");
    union
    {
        OBJECT_DIRECTORY_INFORMATION Info;
        UCHAR Buffer[sizeof(OBJECT_DIRECTORY_INFORMATION) + 128 * sizeof(WCHAR)];
    } Entry;
    OBJECT_ATTRIBUTES ObjectAttributes;
    UNICODE_STRING DriverName;
    WCHAR szDriverName[160];
    PDRIVER_OBJECT DxgkDriver, DriverObject;
    ULONG_PTR ulDispatch;
    HANDLE hDirectory;
    ULONG ulContext = 0;
    ULONG cbReturned;
    BOOLEAN bFound = FALSE;
    NTSTATUS Status;

    if (gpDxgkDeviceObject == NULL)
        return FALSE;
    DxgkDriver = gpDxgkDeviceObject->DriverObject;

    InitializeObjectAttributes(&ObjectAttributes,
                               &DirectoryName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);
    Status = ZwOpenDirectoryObject(&hDirectory, DIRECTORY_QUERY, &ObjectAttributes);
    if (!NT_SUCCESS(Status))
        return FALSE;

    while (!bFound &&
           NT_SUCCESS(ZwQueryDirectoryObject(hDirectory,
                                             &Entry,
                                             sizeof(Entry),
                                             TRUE,
                                             FALSE,
                                             &ulContext,
                                             &cbReturned)))
    {
        Status = RtlStringCbPrintfW(szDriverName, sizeof(szDriverName), L"\\Driver\\%wZ", &Entry.Info.Name);
        if (!NT_SUCCESS(Status))
            continue;

        RtlInitUnicodeString(&DriverName, szDriverName);
        Status = ObReferenceObjectByName(&DriverName,
                                         OBJ_CASE_INSENSITIVE,
                                         NULL,
                                         0,
                                         IoDriverObjectType,
                                         KernelMode,
                                         NULL,
                                         (PVOID *)&DriverObject);
        if (!NT_SUCCESS(Status))
            continue;

        ulDispatch = (ULONG_PTR)DriverObject->MajorFunction[IRP_MJ_PNP];
        if ((DriverObject != DxgkDriver) &&
            (ulDispatch - (ULONG_PTR)DxgkDriver->DriverStart < DxgkDriver->DriverSize))
        {
            bFound = DlpDriverDrivesDisplay(DriverObject);
        }

        ObDereferenceObject(DriverObject);
    }

    ZwClose(hDirectory);
    return bFound;
}

/** @brief Is dxgkrnl loaded at all, whether or not win32k got its interface? */
static BOOLEAN
DlpIsDxgkrnlLoaded(VOID)
{
    UNICODE_STRING DeviceName;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status;

    RtlInitUnicodeString(&DeviceName, L"\\Device\\DxgKrnl");
    Status = IoGetDeviceObjectPointer(&DeviceName, FILE_READ_ATTRIBUTES, &FileObject, &DeviceObject);
    if (!NT_SUCCESS(Status))
        return FALSE;

    ObDereferenceObject(FileObject);
    return TRUE;
}

/**
 * @brief Create or drop a process's DXGPROCESS.
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

#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
/* Each D3DKMT slot win32k uses and the dxgkrnl export that took its place */
static const struct
{
    USHORT Slot;
    PCSTR Export;
} DlpD3DKMTExports[] =
{
    { DXGK_SLOT_AcquireKeyedMutex,                     "NtGdiDdDDIAcquireKeyedMutex" },
    { DXGK_SLOT_AcquireKeyedMutex2,                    "NtGdiDdDDIAcquireKeyedMutex2" },
    { DXGK_SLOT_AdjustFullscreenGamma,                 "NtGdiDdDDIAdjustFullscreenGamma" },
    { DXGK_SLOT_CacheHybridQueryValue,                 "NtGdiDdDDICacheHybridQueryValue" },
    { DXGK_SLOT_ChangeVideoMemoryReservation,          "NtGdiDdDDIChangeVideoMemoryReservation" },
    { DXGK_SLOT_CheckExclusiveOwnership,               "NtGdiDdDDICheckExclusiveOwnership" },
    { DXGK_SLOT_CheckMonitorPowerState,                "NtGdiDdDDICheckMonitorPowerState" },
    { DXGK_SLOT_CheckMultiPlaneOverlaySupport2,        "NtGdiDdDDICheckMultiPlaneOverlaySupport2" },
    { DXGK_SLOT_CheckMultiPlaneOverlaySupport3,        "NtGdiDdDDICheckMultiPlaneOverlaySupport3" },
    { DXGK_SLOT_CheckOcclusion,                        "NtGdiDdDDICheckOcclusion" },
    { DXGK_SLOT_CheckSharedResourceAccess,             "NtGdiDdDDICheckSharedResourceAccess" },
    { DXGK_SLOT_CheckVidPnExclusiveOwnership,          "NtGdiDdDDICheckVidPnExclusiveOwnership" },
    { DXGK_SLOT_CloseAdapter,                          "NtGdiDdDDICloseAdapter" },
    { DXGK_SLOT_ConfigureSharedResource,               "NtGdiDdDDIConfigureSharedResource" },
    { DXGK_SLOT_CreateAllocation,                      "NtGdiDdDDICreateAllocation" },
    { DXGK_SLOT_CreateContext,                         "NtGdiDdDDICreateContext" },
    { DXGK_SLOT_CreateContextVirtual,                  "NtGdiDdDDICreateContextVirtual" },
    { DXGK_SLOT_CreateDevice,                          "NtGdiDdDDICreateDevice" },
    { DXGK_SLOT_CreateKeyedMutex,                      "NtGdiDdDDICreateKeyedMutex" },
    { DXGK_SLOT_CreateKeyedMutex2,                     "NtGdiDdDDICreateKeyedMutex2" },
    { DXGK_SLOT_CreateOverlay,                         "NtGdiDdDDICreateOverlay" },
    { DXGK_SLOT_CreatePagingQueue,                     "NtGdiDdDDICreatePagingQueue" },
    { DXGK_SLOT_CreateSynchronizationObject,           "NtGdiDdDDICreateSynchronizationObject" },
    { DXGK_SLOT_DestroyAllocation,                     "NtGdiDdDDIDestroyAllocation" },
    { DXGK_SLOT_DestroyAllocation2,                    "NtGdiDdDDIDestroyAllocation2" },
    { DXGK_SLOT_DestroyContext,                        "NtGdiDdDDIDestroyContext" },
    { DXGK_SLOT_DestroyDevice,                         "NtGdiDdDDIDestroyDevice" },
    { DXGK_SLOT_DestroyKeyedMutex,                     "NtGdiDdDDIDestroyKeyedMutex" },
    { DXGK_SLOT_DestroyOverlay,                        "NtGdiDdDDIDestroyOverlay" },
    { DXGK_SLOT_DestroyPagingQueue,                    "NtGdiDdDDIDestroyPagingQueue" },
    { DXGK_SLOT_DestroySynchronizationObject,          "NtGdiDdDDIDestroySynchronizationObject" },
    { DXGK_SLOT_EnumAdapters2,                         "NtGdiDdDDIEnumAdapters2" },
    { DXGK_SLOT_Escape,                                "NtGdiDdDDIEscape" },
    { DXGK_SLOT_Evict,                                 "NtGdiDdDDIEvict" },
    { DXGK_SLOT_FlipOverlay,                           "NtGdiDdDDIFlipOverlay" },
    { DXGK_SLOT_FreeGpuVirtualAddress,                 "NtGdiDdDDIFreeGpuVirtualAddress" },
    { DXGK_SLOT_GetCachedHybridQueryValue,             "NtGdiDdDDIGetCachedHybridQueryValue" },
    { DXGK_SLOT_GetContextSchedulingPriority,          "NtGdiDdDDIGetContextSchedulingPriority" },
    { DXGK_SLOT_GetDeviceState,                        "NtGdiDdDDIGetDeviceState" },
    { DXGK_SLOT_GetDisplayModeList,                    "NtGdiDdDDIGetDisplayModeList" },
    { DXGK_SLOT_GetMultisampleMethodList,              "NtGdiDdDDIGetMultisampleMethodList" },
    { DXGK_SLOT_GetOverlayState,                       "NtGdiDdDDIGetOverlayState" },
    { DXGK_SLOT_GetPresentHistory,                     "NtGdiDdDDIGetPresentHistory" },
    { DXGK_SLOT_GetProcessSchedulingPriorityClass,     "NtGdiDdDDIGetProcessSchedulingPriorityClass" },
    { DXGK_SLOT_GetRuntimeData,                        "NtGdiDdDDIGetRuntimeData" },
    { DXGK_SLOT_GetScanLine,                           "NtGdiDdDDIGetScanLine" },
    { DXGK_SLOT_GetSharedPrimaryHandle,                "NtGdiDdDDIGetSharedPrimaryHandle" },
    { DXGK_SLOT_InvalidateActiveVidPn,                 "NtGdiDdDDIInvalidateActiveVidPn" },
    { DXGK_SLOT_InvalidateCache,                       "NtGdiDdDDIInvalidateCache" },
    { DXGK_SLOT_Lock,                                  "NtGdiDdDDILock" },
    { DXGK_SLOT_Lock2,                                 "NtGdiDdDDILock2" },
    { DXGK_SLOT_MakeResident,                          "NtGdiDdDDIMakeResident" },
    { DXGK_SLOT_MapGpuVirtualAddress,                  "NtGdiDdDDIMapGpuVirtualAddress" },
    { DXGK_SLOT_MarkDeviceAsError,                     "NtGdiDdDDIMarkDeviceAsError" },
    { DXGK_SLOT_OpenAdapterFromLuid,                   "NtGdiDdDDIOpenAdapterFromLuid" },
    { DXGK_SLOT_OpenKeyedMutex,                        "NtGdiDdDDIOpenKeyedMutex" },
    { DXGK_SLOT_OpenKeyedMutex2,                       "NtGdiDdDDIOpenKeyedMutex2" },
    { DXGK_SLOT_OpenNtHandleFromName,                  "NtGdiDdDDIOpenNtHandleFromName" },
    { DXGK_SLOT_OpenResource,                          "NtGdiDdDDIOpenResource" },
    { DXGK_SLOT_OpenResourceFromNtHandle,              "NtGdiDdDDIOpenResourceFromNtHandle" },
    { DXGK_SLOT_OpenSyncObjectFromNtHandle,            "NtGdiDdDDIOpenSyncObjectFromNtHandle" },
    { DXGK_SLOT_OpenSyncObjectFromNtHandle2,           "NtGdiDdDDIOpenSyncObjectFromNtHandle2" },
    { DXGK_SLOT_OpenSyncObjectNtHandleFromName,        "NtGdiDdDDIOpenSyncObjectNtHandleFromName" },
    { DXGK_SLOT_OpenSynchronizationObject,             "NtGdiDdDDIOpenSynchronizationObject" },
    { DXGK_SLOT_PollDisplayChildren,                   "NtGdiDdDDIPollDisplayChildren" },
    { DXGK_SLOT_Present,                               "NtGdiDdDDIPresent" },
    { DXGK_SLOT_PresentMultiPlaneOverlay2,             "NtGdiDdDDIPresentMultiPlaneOverlay2" },
    { DXGK_SLOT_PresentMultiPlaneOverlay3,             "NtGdiDdDDIPresentMultiPlaneOverlay3" },
    { DXGK_SLOT_QueryAdapterInfo,                      "NtGdiDdDDIQueryAdapterInfo" },
    { DXGK_SLOT_QueryAllocationResidency,              "NtGdiDdDDIQueryAllocationResidency" },
    { DXGK_SLOT_QueryClockCalibration,                 "NtGdiDdDDIQueryClockCalibration" },
    { DXGK_SLOT_QueryResourceInfo,                     "NtGdiDdDDIQueryResourceInfo" },
    { DXGK_SLOT_QueryResourceInfoFromNtHandle,         "NtGdiDdDDIQueryResourceInfoFromNtHandle" },
    { DXGK_SLOT_QueryStatistics,                       "NtGdiDdDDIQueryStatistics" },
    { DXGK_SLOT_QueryVidPnExclusiveOwnership,          "NtGdiDdDDIQueryVidPnExclusiveOwnership" },
    { DXGK_SLOT_QueryVideoMemoryInfo,                  "NtGdiDdDDIQueryVideoMemoryInfo" },
    { DXGK_SLOT_ReclaimAllocations2,                   "NtGdiDdDDIReclaimAllocations2" },
    { DXGK_SLOT_ReleaseKeyedMutex,                     "NtGdiDdDDIReleaseKeyedMutex" },
    { DXGK_SLOT_ReleaseKeyedMutex2,                    "NtGdiDdDDIReleaseKeyedMutex2" },
    { DXGK_SLOT_ReleaseProcessVidPnSourceOwners,       "NtGdiDdDDIReleaseProcessVidPnSourceOwners" },
    { DXGK_SLOT_Render,                                "NtGdiDdDDIRender" },
    { DXGK_SLOT_ReserveGpuVirtualAddress,              "NtGdiDdDDIReserveGpuVirtualAddress" },
    { DXGK_SLOT_SetAllocationPriority,                 "NtGdiDdDDISetAllocationPriority" },
    { DXGK_SLOT_SetContextInProcessSchedulingPriority, "NtGdiDdDDISetContextInProcessSchedulingPriority" },
    { DXGK_SLOT_SetContextSchedulingPriority,          "NtGdiDdDDISetContextSchedulingPriority" },
    { DXGK_SLOT_SetDisplayMode,                        "NtGdiDdDDISetDisplayMode" },
    { DXGK_SLOT_SetGammaRamp,                          "NtGdiDdDDISetGammaRamp" },
    { DXGK_SLOT_SetProcessSchedulingPriorityClass,     "NtGdiDdDDISetProcessSchedulingPriorityClass" },
    { DXGK_SLOT_SetQueuedLimit,                        "NtGdiDdDDISetQueuedLimit" },
    { DXGK_SLOT_SetVidPnSourceHwProtection,            "NtGdiDdDDISetVidPnSourceHwProtection" },
    { DXGK_SLOT_SetVidPnSourceOwner,                   "NtGdiDdDDISetVidPnSourceOwner" },
    { DXGK_SLOT_ShareObjects,                          "NtGdiDdDDIShareObjects" },
    { DXGK_SLOT_SignalSynchronizationObject,           "NtGdiDdDDISignalSynchronizationObject" },
    { DXGK_SLOT_SignalSynchronizationObjectFromCpu,    "NtGdiDdDDISignalSynchronizationObjectFromCpu" },
    { DXGK_SLOT_SignalSynchronizationObjectFromGpu,    "NtGdiDdDDISignalSynchronizationObjectFromGpu" },
    { DXGK_SLOT_SignalSynchronizationObjectFromGpu2,   "NtGdiDdDDISignalSynchronizationObjectFromGpu2" },
    { DXGK_SLOT_SubmitCommand,                         "NtGdiDdDDISubmitCommand" },
    { DXGK_SLOT_Unlock,                                "NtGdiDdDDIUnlock" },
    { DXGK_SLOT_Unlock2,                               "NtGdiDdDDIUnlock2" },
    { DXGK_SLOT_UpdateGpuVirtualAddress,               "NtGdiDdDDIUpdateGpuVirtualAddress" },
    { DXGK_SLOT_UpdateOverlay,                         "NtGdiDdDDIUpdateOverlay" },
    { DXGK_SLOT_WaitForIdle,                           "NtGdiDdDDIWaitForIdle" },
    { DXGK_SLOT_WaitForSynchronizationObject,          "NtGdiDdDDIWaitForSynchronizationObject" },
    { DXGK_SLOT_WaitForSynchronizationObjectFromCpu,   "NtGdiDdDDIWaitForSynchronizationObjectFromCpu" },
    { DXGK_SLOT_WaitForSynchronizationObjectFromGpu,   "NtGdiDdDDIWaitForSynchronizationObjectFromGpu" },
    { DXGK_SLOT_WaitForVerticalBlankEvent,             "NtGdiDdDDIWaitForVerticalBlankEvent" },
};

/**
 * @brief Finds the D3DKMT entry points among the exports of the loaded dxgkrnl.
 */
static VOID
DlpResolveD3DKMTExports(VOID)
{
    PVOID ImageBase = gpDxgkDeviceObject->DriverObject->DriverStart;
    ULONG i;

    for (i = 0; i < ARRAYSIZE(DlpD3DKMTExports); i++)
    {
        DlpD3DKMTEntries[DlpD3DKMTExports[i].Slot] =
            RtlFindExportedRoutineByName(ImageBase, DlpD3DKMTExports[i].Export);

        if (!DlpD3DKMTEntries[DlpD3DKMTExports[i].Slot])
            DPRINT1("win32k: dxgkrnl does not export %s\n", DlpD3DKMTExports[i].Export);
    }
}
#endif

/**
 * @brief Open dxgkrnl + acquire the DXGKWIN32K_INTERFACE.
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

    /* win32k stamps Version/Size; dxgkrnl fills the pfn slots. */
    gDxgkInterface.Version = DL_INTERFACE_VERSION;
    gDxgkInterface.Size    = sizeof(gDxgkInterface);

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(IOCTL_VIDEO_GIVE_CALLSBACK,
                                        gpDxgkDeviceObject,
                                        &gDxgkInterface, sizeof(gDxgkInterface),
                                        &gDxgkInterface, sizeof(gDxgkInterface),
                                        TRUE,   /* INTERNAL_DEVICE_CONTROL */
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

#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
        DlpResolveD3DKMTExports();
#endif

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
 *        the way Windows does.
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
 * @brief Is this path the first of its clone group? Only that one carries the source's mode.
 */
static BOOLEAN
DlpIsFirstPathOfCloneGroup(
    _In_ PD3DKMT_GETPATHSMODALITY Modality,
    _In_ USHORT PathIndex)
{
    PDL_MODALITY_PATH Paths = DL_MODALITY_PATHS(Modality);
    USHORT i;

    for (i = 0; i < Modality->PathCount; i++)
    {
        if (Paths[i].CloneGroupId == Paths[PathIndex].CloneGroupId)
            return (i == PathIndex);
    }

    return FALSE;
}

/**
 * @brief Tell GDI which sources the applied configuration turned on and in what mode, which is
 *        what the desktop is then built from. A path the apply failed, or one without a mode, has
 *        no source on the desktop.
 */
static NTSTATUS
DlpRecordDisplayConfig(
    _In_ PD3DKMT_GETPATHSMODALITY Modality)
{
    PDL_MODALITY_PATH Paths = DL_MODALITY_PATHS(Modality);
    PENGP_DISPLAY_PATH ActivePaths;
    ULONG ActiveCount = 0;
    NTSTATUS Status;
    USHORT i;

    if (Modality->PathCount == 0)
        return STATUS_UNSUCCESSFUL;

    ActivePaths = ExAllocatePoolZero(PagedPool, Modality->PathCount * sizeof(*ActivePaths), GDITAG_TEMP);
    if (ActivePaths == NULL)
        return STATUS_NO_MEMORY;

    for (i = 0; i < Modality->PathCount; i++)
    {
        if (!DlpIsFirstPathOfCloneGroup(Modality, i))
            continue;

        if ((Paths[i].StatusApply < 0) || (Paths[i].DevMode == NULL))
            continue;

        DPRINT1("win32k: source %lu of adapter %lx:%lx on the desktop, %lux%lu at (%ld,%ld)\n",
                Paths[i].VidPnSourceId,
                Paths[i].AdapterLuid.HighPart, Paths[i].AdapterLuid.LowPart,
                Paths[i].DevMode->dmPelsWidth, Paths[i].DevMode->dmPelsHeight,
                Paths[i].DevMode->dmPosition.x, Paths[i].DevMode->dmPosition.y);

        ActivePaths[ActiveCount].AdapterLuid = Paths[i].AdapterLuid;
        ActivePaths[ActiveCount].VidPnSourceId = Paths[i].VidPnSourceId;
        ActivePaths[ActiveCount].pdm = Paths[i].DevMode;
        ActiveCount++;
    }

    Status = EngpSetDisplayConfig(ActiveCount, ActivePaths);

    ExFreePoolWithTag(ActivePaths, GDITAG_TEMP);
    return Status;
}

/**
 * @brief Functionalize the paths, apply them, tell GDI which sources came on and build the desktop
 *        from them, then finalize. The desktop is built between the apply and the finalize, as
 *
 * @param Modality The paths to commit.
 * @param FunctionalizeFlags What Functionalize is given.
 * @param bValidateOnly Stop after Functionalize, which is all a CDS_TEST caller wants to know.
 * @param pmdevOld The desktop to replace, or NULL when the first one is being built.
 * @param ppmdevNew Receives the new desktop. NULL leaves the desktop alone.
 */
static NTSTATUS
DlpCommitPathsModality(
    _Inout_ PD3DKMT_GETPATHSMODALITY Modality,
    _In_ ULONG FunctionalizeFlags,
    _In_ BOOLEAN bValidateOnly,
    _In_opt_ PMDEVOBJ pmdevOld,
    _Out_opt_ PMDEVOBJ *ppmdevNew)
{
    NTSTATUS Status = STATUS_SUCCESS;
    LONG     lRet;

    /*
     * Pick a source mode for each path. Enumerating a path says the adapter could drive it, not
     * what it should drive, so the mode fields stay empty until this runs. Apply then commits
     * what is there, and dxgkrnl compares that against the CDD's surface to decide whether it can
     * present the CDD shadow directly or has to blit through an allocation of its own
     * (ADAPTER_DISPLAY::IsIdenticalMode). Skip this and the comparison fails on an empty mode.
     */
    if (gDxgkInterface.pfnDxgkFunctionalizePathsModality != NULL)
    {
        Status = gDxgkInterface.pfnDxgkFunctionalizePathsModality(FunctionalizeFlags, Modality);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("win32k: DxgkFunctionalizePathsModality failed 0x%lX\n", Status);
            return Status;
        }
    }

    if (bValidateOnly)
        return Status;

#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
    Status = gDxgkInterface.pfnDxgkApplyPathsModality(DL_MODALITY_NO_OPTIMIZE | DL_MODALITY_APPLY,
                                                     Modality,
                                                     NULL);
#else
    Status = gDxgkInterface.pfnDxgkApplyPathsModality(DL_MODALITY_NO_OPTIMIZE | DL_MODALITY_APPLY,
                                                     Modality);
#endif
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("win32k: DxgkApplyPathsModality failed 0x%lX\n", Status);
        return Status;
    }

    DPRINT1("win32k: display configuration applied, %u path(s)\n", Modality->AppliedCount);

    Status = DlpRecordDisplayConfig(Modality);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("win32k: no source of the display configuration reached GDI, 0x%lX\n", Status);
    }
    else if (ppmdevNew != NULL)
    {
        /* The desktop is made of the sources the configuration turned on */
        lRet = PDEVOBJ_lChangeDisplaySettings(NULL, NULL, pmdevOld, ppmdevNew, (pmdevOld == NULL));
        if (lRet != DISP_CHANGE_SUCCESSFUL)
        {
            DPRINT1("win32k: building the desktop for the display configuration failed %ld\n", lRet);
            Status = STATUS_UNSUCCESSFUL;
        }
    }

    if (gDxgkInterface.pfnDxgkFinalizePathsModality != NULL)
        gDxgkInterface.pfnDxgkFinalizePathsModality(Modality);

    return Status;
}

/**
 * @brief Build the display configuration and hand it to dxgkrnl. Without this no VidPn is ever
 *        committed, so the adapter's display sources keep the mode and CDD allocations they were
 *        constructed with, which is nothing.
 *
 * @param pmdevOld The desktop to replace, or NULL while the first one does not exist yet.
 * @param ppmdevNew Receives the desktop built from the configuration. NULL builds none.
 */
static NTSTATUS
DlpApplyDisplayConfig(
    _In_opt_ PMDEVOBJ pmdevOld,
    _Out_opt_ PMDEVOBJ *ppmdevNew)
{
    PD3DKMT_GETPATHSMODALITY Modality = NULL;
    ULONG                    Flags = DL_MODALITY_PERSISTED;
    NTSTATUS                 Status;

    if (ppmdevNew != NULL)
        *ppmdevNew = NULL;

    if ((gDxgkInterface.pfnDxgkGetPathsModality == NULL) ||
        (gDxgkInterface.pfnDxgkApplyPathsModality == NULL))
        return STATUS_NOT_SUPPORTED;

    /* The saved configuration first, then whatever the adapter can drive */
    Status = DlpQueryPathsModality(Flags, &Modality);
    if (NT_SUCCESS(Status) && (Modality->PathCount == 0))
    {
        DlpFreePathsModality(Modality);
        Status = STATUS_UNSUCCESSFUL;
    }

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("win32k: no saved display configuration (0x%lX), enumerating paths\n", Status);
        Flags = DL_MODALITY_ALL_PATHS;
        Status = DlpQueryPathsModality(Flags, &Modality);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("win32k: DxgkGetPathsModality failed 0x%lX\n", Status);
            return Status;
        }
    }

    DPRINT1("win32k: display configuration has %u path(s)\n", Modality->PathCount);

    Status = DlpCommitPathsModality(Modality,
                                    Flags & DL_MODALITY_FUNCTIONALIZE,
                                    FALSE,
                                    pmdevOld,
                                    ppmdevNew);

    DlpFreePathsModality(Modality);
    return Status;
}

/**
 * @brief Change the mode of one WDDM source for a legacy ChangeDisplaySettings caller. The mode
 *        goes through dxgkrnl's CDS journal into the current paths, which are then committed like
 *        any other configuration. Must run in CSRSS or a GUI thread of the session.
 *
 * @param pGraphicsDevice The source whose mode changes.
 * @param pdmRequest The mode asked for. NULL goes back to the saved configuration.
 * @param bTryClosest Settle for the closest mode the source has.
 * @param bSetMode Program the mode. FALSE only checks it, or only saves it with bUpdateRegistry.
 * @param bUpdateRegistry Save the new configuration in the display database.
 * @param bFullScreen The caller changes the mode for a full screen application.
 * @param pmdevOld The current desktop.
 * @param ppmdevNew Receives the new desktop when the mode was set.
 *
 * @return A DISP_CHANGE_* value.
 */
LONG NTAPI
DlChangeDisplaySettings(
    _In_ PGRAPHICS_DEVICE pGraphicsDevice,
    _In_opt_ PDEVMODEW pdmRequest,
    _In_ BOOLEAN bTryClosest,
    _In_ BOOLEAN bSetMode,
    _In_ BOOLEAN bUpdateRegistry,
    _In_ BOOLEAN bFullScreen,
    _In_ PMDEVOBJ pmdevOld,
    _Out_ PMDEVOBJ *ppmdevNew)
{
    PPROCESSINFO             ppi = PsGetCurrentProcessWin32Process();
    PD3DKMT_GETPATHSMODALITY Modality = NULL;
    PDEVMODEW                pdmCaptured = NULL;
    DL_CDS_REQUEST           Request;
    ULONG                    FunctionalizeFlags;
    BOOLEAN                  bSaved;
    NTSTATUS                 Status;
    LONG                     lRet = DISP_CHANGE_FAILED;

    *ppmdevNew = NULL;

    if (!gbDxgkInitialized || (ppi == NULL) || (ppi->DxProcess == NULL) ||
        (gDxgkInterface.pfnDxgkGetPathsModality == NULL) ||
        (gDxgkInterface.pfnDxgkApplyPathsModality == NULL) ||
        (gDxgkInterface.pfnDxgkAugmentCdsj == NULL) ||
        (gDxgkInterface.pfnDxgkApplyCdsjToPathsModality == NULL))
    {
        return DISP_CHANGE_FAILED;
    }

    /* The topology running now, or the saved one when the change is saved or undone */
    bSaved = bUpdateRegistry || (pdmRequest == NULL);
    Status = DlpQueryPathsModality(DL_MODALITY_CDS | (bSaved ? DL_MODALITY_PERSISTED : DL_MODALITY_CURRENT),
                                   &Modality);
    if (!NT_SUCCESS(Status) && bSaved)
    {
        DPRINT1("win32k: no saved display configuration (0x%lX), changing the running one\n", Status);
        Status = DlpQueryPathsModality(DL_MODALITY_CDS | DL_MODALITY_CURRENT, &Modality);
    }
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("win32k: DxgkGetPathsModality for a mode change failed 0x%lX\n", Status);
        return DISP_CHANGE_FAILED;
    }

    if (pdmRequest == NULL)
        goto Commit;

    /* dxgkrnl first checks the mode against what the source can do */
    RtlZeroMemory(&Request, sizeof(Request));
    Request.Flags = (bTryClosest ? DL_CDS_TRY_CLOSEST : 0) | DL_CDS_VALIDATE_ONLY;
    Request.AdapterLuid = pGraphicsDevice->DxgAdapterLuid;
    Request.VidPnSourceId = pGraphicsDevice->VidPnSourceId;
    Request.VidPnTargetId = (ULONG)-1;
    Request.OutputTechnology = D3DKMDT_VOT_UNINITIALIZED;
    Request.BaseOutputTechnology = D3DKMDT_VOT_UNINITIALIZED;
    Request.pDevMode = pdmRequest;

    Status = gDxgkInterface.pfnDxgkAugmentCdsj(&Request);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("win32k: dxgkrnl refused the mode %lux%lu, 0x%lX\n",
                pdmRequest->dmPelsWidth, pdmRequest->dmPelsHeight, Status);
        lRet = DISP_CHANGE_BADMODE;
        goto Cleanup;
    }

    /* Then it becomes a mode the source actually lists */
    if (!LDEVOBJ_bProbeAndCaptureDevmode(pGraphicsDevice, pdmRequest, &pdmCaptured, bTryClosest))
    {
        lRet = DISP_CHANGE_BADMODE;
        goto Cleanup;
    }

    Request.Flags = (bTryClosest ? DL_CDS_TRY_CLOSEST : 0) | (bFullScreen ? DL_CDS_FULLSCREEN : 0);
    Request.pDevMode = pdmCaptured;

    /* Saving without setting only journals it, anything else puts it in the paths */
    if (!bUpdateRegistry || bSetMode)
        Status = gDxgkInterface.pfnDxgkApplyCdsjToPathsModality(&Modality, &Request);
    else
        Status = gDxgkInterface.pfnDxgkAugmentCdsj(&Request);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("win32k: journaling the mode change failed 0x%lX\n", Status);
        goto Cleanup;
    }

    if (!bSetMode && bUpdateRegistry)
    {
        lRet = DISP_CHANGE_SUCCESSFUL;
        goto Cleanup;
    }

Commit:
    FunctionalizeFlags = DL_FUNCTIONALIZE_CDS;
    if (bTryClosest || (pdmRequest == NULL))
        FunctionalizeFlags |= DL_FUNCTIONALIZE_RELAXED;

    Status = DlpCommitPathsModality(Modality,
                                    FunctionalizeFlags,
                                    !bSetMode,
                                    pmdevOld,
                                    bSetMode ? ppmdevNew : NULL);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    if (bSetMode && bUpdateRegistry && (gDxgkInterface.pfnDxgkPersistPathsModality != NULL))
    {
        Status = gDxgkInterface.pfnDxgkPersistPathsModality(DL_PERSIST_TO_DATABASE, Modality);
        if (!NT_SUCCESS(Status))
            DPRINT1("win32k: saving the display configuration failed 0x%lX\n", Status);
    }

    lRet = DISP_CHANGE_SUCCESSFUL;

Cleanup:
    if (pdmCaptured != NULL)
        ExFreePoolWithTag(pdmCaptured, GDITAG_DEVMODE);
    if (Modality != NULL)
        DlpFreePathsModality(Modality);
    return lRet;
}

/**
 * @brief Report the console session to watchdog, which is what lets dxgkrnl start its adapters.
 *        Must run in CSRSS.
 */
VOID NTAPI
DlNotifySessionOpen(VOID)
{
    PPROCESSINFO ppi = PsGetCurrentProcessWin32Process();
    NTSTATUS Status;

#if DXGKRNL_TARGET == DXGKRNL_TARGET_1607
    /* Adapter arrival, removal and TDR reach win32k through this, whoever raises them */
    Status = SMgrRegisterGdiCallout((PVOID)VideoPortCallout);
    if (!NT_SUCCESS(Status))
        DPRINT1("win32k: SMgrRegisterGdiCallout failed 0x%lX\n", Status);
#endif

    /*
     * dxgkrnl blocks here until its adapters have started and needs CSRSS's DXGPROCESS, so only
     * call once win32k has its interface and the process callout ran. Without dxgkrnl nothing
     * waits, and watchdog still needs the session to deliver videoprt's callouts.
     */
    if (!gbDxgkInitialized)
    {
        if (DlpIsDxgkrnlLoaded())
            return;
    }
    else if ((ppi == NULL) || (ppi->DxProcess == NULL))
    {
        return;
    }
    else if (!DlpHasWddmDisplayAdapter())
    {
        /* dxgkrnl would hold the session for a display adapter that never comes, GDI stays on XDDM */
        DPRINT1("win32k: no WDDM display adapter, the desktop stays on the XDDM drivers\n");
        return;
    }

    DPRINT1("win32k: opening session, dxgkrnl waits for its display and render adapters to start\n");
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
    Status = SMgrNotifySessionChange(DL_SESSION_OPEN, (PVOID)VideoPortCallout);
#else
    Status = SMgrNotifySessionChange(DL_SESSION_OPEN);
#endif
    if (!NT_SUCCESS(Status))
        DPRINT1("win32k: SMgrNotifySessionChange failed 0x%lX\n", Status);
}

/**
 * @brief Commit the display configuration and build the first desktop from it. Must run in CSRSS,
 *        and only once the session owns the adapter: the CCD topology is built from the session's
 *        adapter views, which come from the session usage claim EngpRegisterGraphicsDevice sends.
 *
 * @param ppmdev Receives the desktop, NULL when no WDDM configuration could be applied.
 */
NTSTATUS NTAPI
DlApplyDisplayConfig(
    _Out_ PMDEVOBJ *ppmdev)
{
    PPROCESSINFO ppi = PsGetCurrentProcessWin32Process();
    NTSTATUS Status;

    *ppmdev = NULL;

    if (!gbDxgkInitialized || (ppi == NULL) || (ppi->DxProcess == NULL))
        return STATUS_NOT_SUPPORTED;

    Status = DlpApplyDisplayConfig(NULL, ppmdev);
    if (!NT_SUCCESS(Status))
        DPRINT1("win32k: DlpApplyDisplayConfig failed 0x%lX\n", Status);

    return Status;
}

/**
 * @brief Apply the display configuration again and rebuild the desktop from it, after an adapter
 *        came or went. Must run in CSRSS.
 *
 * @param pmdevOld The current desktop.
 * @param ppmdevNew Receives the new desktop on success. The caller installs it.
 */
NTSTATUS NTAPI
DlSetDisplayConfig(
    _In_ PMDEVOBJ pmdevOld,
    _Out_ PMDEVOBJ *ppmdevNew)
{
    PPROCESSINFO ppi = PsGetCurrentProcessWin32Process();

    *ppmdevNew = NULL;

    if (!gbDxgkInitialized || (ppi == NULL) || (ppi->DxProcess == NULL))
        return STATUS_NOT_SUPPORTED;

    return DlpApplyDisplayConfig(pmdevOld, ppmdevNew);
}

/** @brief Release the dxgkrnl device. */
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

/*
 * Per session data the Windows 11 CDD keeps with win32k. The CDD allocates its own block and
 * stores the pointer at offset 0x48, nothing else in here is read by it.
 */
typedef struct _W32_SESSION_STATE_BLOCK
{
    PVOID Reserved[9];
    PVOID CddSessionData;
    PVOID Reserved50[22];
} W32_SESSION_STATE_BLOCK;

C_ASSERT(FIELD_OFFSET(W32_SESSION_STATE_BLOCK, CddSessionData) == 9 * sizeof(PVOID));

static W32_SESSION_STATE_BLOCK gW32SessionState;

/**
 * @brief Gets the win32k data of the current session.
 * @remarks Only the console session exists, so every caller shares one block.
 */
PVOID
NTAPI
W32GetSessionState(VOID)
{
    return &gW32SessionState;
}
