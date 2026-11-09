/*
 * PROJECT:     ReactOS Win32k (WDDM display path)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     win32k <-> dxgkrnl bootstrap - load DxgKrnl + acquire the win32k callback table
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#include <win32k.h>
#include <reactos/rddm/rddm_private.h>
#include "../ntgdi/dxgkfwd.h"

#define NDEBUG
#include <debug.h>

#if DL_DXGK_EXPORTS_D3DKMT
/* Exported by ntoskrnl */
NTSYSAPI
PVOID
NTAPI
RtlFindExportedRoutineByName(
    _In_ PVOID ImageBase,
    _In_ PCSTR RoutineName);

NTSYSAPI
PVOID
NTAPI
RtlPcToFileHeader(
    _In_ PVOID PcValue,
    _Out_ PVOID* BaseOfImage);
#endif

/* The engine interface dxgkrnl calls back through (gdi/eng/dxgkeng.c) */
struct _DXGKWIN32KENG_INTERFACE;
extern struct _DXGKWIN32KENG_INTERFACE gDxgkWin32kEngInterface;

typedef NTSTATUS (NTAPI *PFN_DxgkProcessCallout)(
    _Inout_ PVOID *DxProcess,
    _In_ const struct _DXGKWIN32KENG_INTERFACE *EngInterface,
    _In_ BOOLEAN Create);

#define DL_MODALITY_PATH_SIZE       sizeof(DL_MODALITY_PATH)
#if DL_DXGK_MODALITY_TWO_POINTERS
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
 *
 * CURRENT_SCALED selects the running topology with each path's scaling taken from the saved
 * configuration or the adapter default and the paths marked 0x10000. It is not used for a mode
 * change that carries a devmode. The running path already carries scaling intent 4 (boot 19 dump,
 * +0x8C), so it would change nothing here. Plain CURRENT is what the fixed VM run exercised.
 */
#define DL_MODALITY_CURRENT         0x00000040
#define DL_MODALITY_CURRENT_SCALED  0x02000000
#define DL_MODALITY_CDS             0x00009000
#define DL_MODALITY_CDS_RUNNING     DL_MODALITY_CURRENT

/*
 * Apply flags of a ChangeDisplaySettings mode change. Windows gives Apply APPLY alone, and
 * NO_OPTIMIZE only goes to Get, for SDC_NO_OPTIMIZATION/FORCE_MODE_ENUMERATION.
 * Kept at NO_OPTIMIZE | APPLY, what the boot apply and the fixed VM mode change ran with, until a
 * native mode change has worked once; then try DL_MODALITY_APPLY alone.
 */
#define DL_APPLY_CDS                (DL_MODALITY_NO_OPTIMIZE | DL_MODALITY_APPLY)

/* Functionalize for a legacy mode change, relaxed when the closest mode will do */
#define DL_FUNCTIONALIZE_CDS        0x00008000
#define DL_FUNCTIONALIZE_RELAXED    0x00020000

/* PersistPathsModality index that writes the paths to the display database */
#define DL_PERSIST_TO_DATABASE      1

/*
 * The display configuration dxgkrnl applies to an adapter: this header followed by PathArraySize
 * entries of DL_MODALITY_PATH_SIZE bytes. On Windows 11, AppliedCount is not a path count
 * (Functionalize zeroes it) and the ULONG at 0x28 counts the adapters the apply touched.
 */
typedef struct _D3DKMT_GETPATHSMODALITY
{
    ULONG  Reserved0[5];
    USHORT PathCount;
    USHORT PathArraySize;
    ULONG  Reserved24[3];
    ULONG  AppliedCount;
    PVOID  AppliedPaths;
#if DL_DXGK_MODALITY_TWO_POINTERS
    PVOID  Reserved48;
#endif
} D3DKMT_GETPATHSMODALITY, *PD3DKMT_GETPATHSMODALITY;

C_ASSERT(FIELD_OFFSET(D3DKMT_GETPATHSMODALITY, PathCount) == 20);
C_ASSERT(FIELD_OFFSET(D3DKMT_GETPATHSMODALITY, PathArraySize) == 22);
C_ASSERT(FIELD_OFFSET(D3DKMT_GETPATHSMODALITY, AppliedCount) == 36);
C_ASSERT(FIELD_OFFSET(D3DKMT_GETPATHSMODALITY, AppliedPaths) == 40);
C_ASSERT(sizeof(D3DKMT_GETPATHSMODALITY) == DL_MODALITY_HEADER_SIZE);
#if defined(_WIN64) && DL_DXGK_MODALITY_TWO_POINTERS
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
#elif DXGKRNL_TARGET == DXGKRNL_TARGET_19041
typedef struct _DL_MODALITY_PATH
{
    ULONG64            Flags;
    ULONG64            Reserved08;
    LUID               AdapterLuid;
    ULONG              VidPnSourceId;
    ULONG              VidPnTargetId;
    UCHAR              Reserved20[0x98];
    ULONG              CloneGroupId;
    ULONG              ReservedBC;
    LONG               StatusApply;
    UCHAR              ReservedC4[0x1C];
    PDEVMODEW          DevMode;
    UCHAR              ReservedE8[0x28];
} DL_MODALITY_PATH, *PDL_MODALITY_PATH;

C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, AdapterLuid) == 0x10);
C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, CloneGroupId) == 0xB8);
C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, StatusApply) == 0xC0);
C_ASSERT(FIELD_OFFSET(DL_MODALITY_PATH, DevMode) == 0xE0);
C_ASSERT(sizeof(DL_MODALITY_PATH) == 0x110);
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

#if DL_DXGK_APPLY_HAS_SCENARIO
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

/* dxgkrnl's CDS_JOURNAL::_PreVerify reads the DEVMODE pointer at 0x20 */
C_ASSERT(sizeof(D3DKMDT_VIDEO_OUTPUT_TECHNOLOGY) == sizeof(ULONG));
C_ASSERT(FIELD_OFFSET(DL_CDS_REQUEST, pDevMode) == 0x20);
C_ASSERT(sizeof(DL_CDS_REQUEST) == 0x28);

#define DL_CDS_TRY_CLOSEST          0x00000002
#define DL_CDS_VALIDATE_ONLY        0x00000004
#define DL_CDS_FULLSCREEN           0x00000008
#define DL_CDS_NOT_PHYSICAL         0x00000010

typedef NTSTATUS (NTAPI *PFN_DxgkAugmentCdsj)(
    _Inout_ PDL_CDS_REQUEST Request);

typedef NTSTATUS (NTAPI *PFN_DxgkRegisterDwmProcess)(VOID);

typedef NTSTATUS (NTAPI *PFN_DxgkReleaseProcessVidPnSourceOwners)(
    _In_ HANDLE Process);

typedef BOOLEAN (NTAPI *PFN_DxgkNeedToEnableCddPrimary)(
    _In_ PVOID Adapter,
    _In_ ULONG VidPnSourceId);

/*
 * Turns the scanout of one source off or back on, for use around a mode change and when DirectDraw
 * or the DWM let go of a display. Off disables the paths of the source: the source goes invisible
 * and the CDD primary and shadow are dropped. On enables the paths again. bReleaseOwners also
 * takes the source from any owner first.
 */
typedef NTSTATUS (NTAPI *PFN_DxgkDisplayOnOff)(
    _In_ const LUID *AdapterLuid,
    _In_ ULONG VidPnSourceId,
    _In_ BOOLEAN bOn,
    _In_ BOOLEAN bReleaseOwners);

/*
 * Answers DisplayConfigGetDeviceInfo (bGet TRUE) and DisplayConfigSetDeviceInfo (bGet FALSE)
 * for the info types dxgkrnl owns, on a kernel copy of the packet. It checks the size of the
 * type again.
 */
typedef NTSTATUS (NTAPI *PFN_DxgkDisplayConfigDeviceInfo)(
    _In_ BOOLEAN bGet,
    _Inout_ PVOID Packet);

/*
 * Fills dxgkrnl's part of the 0x808 byte display info packet: the adapter descriptions, the mode of
 * the source and the flags from bit 5 up. The third argument is reserved and passed as 0.
 */
typedef NTSTATUS (NTAPI *PFN_DxgkGetAdapterDeviceDesc)(
    _In_ LUID AdapterLuid,
    _In_ ULONG VidPnSourceId,
    _In_ ULONG Reserved,
    _Inout_ PVOID Packet);

/*
 * Maps the adapter and id of a DisplayConfig packet to the actual ones. Only an id that carries
 * dxgkrnl's virtual topology hash changes, everything else is copied through. Input and output
 * may be the same.
 */
typedef NTSTATUS (NTAPI *PFN_DxgkConvertLegacyQDCAdapterAndIdToActual)(
    _In_ const LUID *pAdapterLuid,
    _In_ ULONG Id,
    _Out_ LUID *pActualAdapterLuid,
    _Out_ ULONG *pActualId);

/* What win32k reports when a session connects; the console one is where display calls go */
typedef VOID (NTAPI *PFN_DxgkSessionConnected)(
    _In_ BOOLEAN bConsole);

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
    PVOID Reserved9[6];                                 /* 9..14 */
    PFN_DxgkReleaseProcessVidPnSourceOwners pfnDxgkReleaseProcessVidPnSourceOwners; /* 15 */
    PVOID Reserved16[2];                                /* 16..17 */
    PFN_DxgkPathsModality pfnDxgkGetPathsModality;      /* 18 */
    PFN_DxgkFunctionalizePathsModality pfnDxgkFunctionalizePathsModality; /* 19 */
    PFN_DxgkApplyPathsModality pfnDxgkApplyPathsModality; /* 20 */
    PFN_DxgkFinalizePathsModality pfnDxgkFinalizePathsModality; /* 21 */
    PFN_DxgkPersistPathsModality pfnDxgkPersistPathsModality; /* 22 */
    PFN_DxgkFreePathsModality pfnDxgkFreePathsModality; /* 23 */
    PFN_DxgkAugmentCdsj pfnDxgkAugmentCdsj;             /* 24 */
    PVOID Reserved25[6];                                /* 25..30 */
    PFN_DxgkDisplayConfigDeviceInfo pfnDxgkDisplayConfigDeviceInfo; /* 31 */
    PFN_DxgkGetAdapterDeviceDesc pfnDxgkGetAdapterDeviceDesc; /* 32 */
    PVOID Reserved33[3];                                /* 33..35 */
    PFN_DxgkNeedToEnableCddPrimary pfnDxgkNeedToEnableCddPrimary; /* 36 */
    PVOID Reserved37[8];                                /* 37..44 */
    PFN_DxgkSessionConnected pfnDxgkSessionConnected;   /* 45 */
    PVOID Reserved46[12];                               /* 46..57 */
    PFN_DxgkRegisterDwmProcess pfnDxgkRegisterDwmProcess; /* 58 */
    PVOID Reserved59[10];                               /* 59..68 */
    PFN_DxgkConvertLegacyQDCAdapterAndIdToActual pfnDxgkConvertLegacyQDCAdapterAndIdToActual; /* 69 */
    PFN_DxgkDisplayOnOff pfnDxgkDisplayOnOff;           /* 70 */
    PVOID Reserved71[4];                                /* 71..74 */
    PFN_DxgkApplyCdsjToPathsModality pfnDxgkApplyCdsjToPathsModality; /* 75 */
    PVOID Reserved76[28];                               /* 76..103 */
} DXGKWIN32K_INTERFACE_BUF;

C_ASSERT(sizeof(DXGKWIN32K_INTERFACE_BUF) == 104 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkProcessCallout) == 5 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkOpenAdapter) == 8 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkGetPathsModality) == 18 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkAugmentCdsj) == 24 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkReleaseProcessVidPnSourceOwners) == 15 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkDisplayConfigDeviceInfo) == 31 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkGetAdapterDeviceDesc) == 32 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkNeedToEnableCddPrimary) == 36 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkSessionConnected) == 45 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkRegisterDwmProcess) == 58 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkConvertLegacyQDCAdapterAndIdToActual) == 69 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkDisplayOnOff) == 70 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkApplyCdsjToPathsModality) == 75 * sizeof(PVOID));

#define DL_INTERFACE_VERSION    50

#elif DXGKRNL_TARGET == DXGKRNL_TARGET_19041
/*
 * The 19041 DXGKWIN32K_INTERFACE: Size and Version, the image dxgkrnl looks win32k routines up in,
 * Context/InterfaceReference/InterfaceDereference and 96 slots dxgkrnl fills, 808 bytes. dxgkrnl
 * takes no other size or version. It has no NeedToEnableCddPrimary.
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
    PVOID Reserved9[6];                                 /* 9..14 */
    PFN_DxgkReleaseProcessVidPnSourceOwners pfnDxgkReleaseProcessVidPnSourceOwners; /* 15 */
    PVOID Reserved16[2];                                /* 16..17 */
    PFN_DxgkPathsModality pfnDxgkGetPathsModality;      /* 18 */
    PFN_DxgkFunctionalizePathsModality pfnDxgkFunctionalizePathsModality; /* 19 */
    PFN_DxgkApplyPathsModality pfnDxgkApplyPathsModality; /* 20 */
    PFN_DxgkFinalizePathsModality pfnDxgkFinalizePathsModality; /* 21 */
    PFN_DxgkPersistPathsModality pfnDxgkPersistPathsModality; /* 22 */
    PFN_DxgkFreePathsModality pfnDxgkFreePathsModality; /* 23 */
    PFN_DxgkAugmentCdsj pfnDxgkAugmentCdsj;             /* 24 */
    PVOID Reserved25[33];                               /* 25..57 */
    PFN_DxgkRegisterDwmProcess pfnDxgkRegisterDwmProcess; /* 58 */
    PVOID Reserved59[17];                               /* 59..75 */
    PFN_DxgkApplyCdsjToPathsModality pfnDxgkApplyCdsjToPathsModality; /* 76 */
    PVOID Reserved77[24];                               /* 77..100 */
} DXGKWIN32K_INTERFACE_BUF;

C_ASSERT(sizeof(DXGKWIN32K_INTERFACE_BUF) == 101 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkProcessCallout) == 5 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkOpenAdapter) == 8 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkReleaseProcessVidPnSourceOwners) == 15 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkGetPathsModality) == 18 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkAugmentCdsj) == 24 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkRegisterDwmProcess) == 58 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKWIN32K_INTERFACE_BUF, pfnDxgkApplyCdsjToPathsModality) == 76 * sizeof(PVOID));

#define DL_INTERFACE_VERSION    44

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

#if DL_DXGK_EXPORTS_D3DKMT
/* The D3DKMT entry point of each slot number, taken from the exports of dxgkrnl */
static PVOID DlpD3DKMTEntries[236];
#endif


/* Fills the NtGdiDdDDI* D3DKMT callback table (gdi/ntgdi/d3dkmt.c) via IOCTL_VIDEO_REGISTER_RXGK. */
NTSTATUS NTAPI DxgRegisterAdapterCallbacks(_In_ PDEVICE_OBJECT pDxgkrnl);

/* The DxgKrnl device + the D3DKMT callback table win32k drives WDDM through. */
PDEVICE_OBJECT           gpDxgkDeviceObject = NULL;
PFILE_OBJECT             gpDxgkFileObject = NULL;
DXGKWIN32K_INTERFACE_BUF gDxgkInterface = { 0 };
BOOLEAN                  gbDxgkInitialized = FALSE;

/* dxgkrnl creates its session data when the session opens, and the DWM needs it to register */
static BOOLEAN           gbDlSessionOpened = FALSE;
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
static BOOLEAN           gbDlSessionConnected = FALSE;
#endif
static BOOLEAN           gbDlSessionOpenDeferred = FALSE;

VOID NTAPI DlNotifySessionOpen(VOID);

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

#if DL_DXGK_EXPORTS_D3DKMT
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

/* Exported by watchdog.sys, from 19041 on it keeps the callout for the session it opens */
#if DL_SMGR_SESSION_CALLOUT
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

/** @brief Is one of the driver's video function devices on a display class device? */
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
        if (DeviceObject->DeviceType != FILE_DEVICE_VIDEO)
            continue;

        /* The bus driver owns the adapter's PDO, only the device above it is the miniport's */
        Pdo = IoGetDeviceAttachmentBaseRef(DeviceObject);
        if (Pdo == DeviceObject)
        {
            ObDereferenceObject(Pdo);
            continue;
        }

        Status = IoGetDeviceProperty(Pdo, DevicePropertyClassGuid, sizeof(szClass), szClass, &cbClass);
        ObDereferenceObject(Pdo);

        if (NT_SUCCESS(Status) && (_wcsicmp(szClass, szDisplayClass) == 0))
            return TRUE;
    }

    return FALSE;
}

/* dxgkrnl's per-miniport driver object extension, keyed by the driver object (DpiInitialize) */
#define DLP_DPI_DRIVER_EXTENSION_SIGNATURE  0x74727044  /* 'Dprt' */
#define DLP_DPI_DRIVER_EXTENSION_SIGNATURE_OFFSET   0x10
#define DLP_DPI_DRIVER_EXTENSION_DRIVER_OFFSET      0x20

/*
 * DxgkInitialize points the miniport's dispatch routines and AddDevice into dxgkrnl and gives
 * the driver object dxgkrnl's extension. Vendor drivers may put their own routines in front
 * afterwards (nvlddmkm replaces all of them and passes the requests on), so the extension is
 * what tells, with the dispatch routines as the fallback.
 */
static BOOLEAN
DlpIsDxgkMiniport(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDRIVER_OBJECT DxgkDriver)
{
    ULONG_PTR Start = (ULONG_PTR)DxgkDriver->DriverStart;
    PUCHAR Extension;
    ULONG i;

    Extension = IoGetDriverObjectExtension(DriverObject, DriverObject);
    if (Extension &&
        (*(PULONG)(Extension + DLP_DPI_DRIVER_EXTENSION_SIGNATURE_OFFSET) == DLP_DPI_DRIVER_EXTENSION_SIGNATURE) &&
        (*(PDRIVER_OBJECT *)(Extension + DLP_DPI_DRIVER_EXTENSION_DRIVER_OFFSET) == DriverObject))
    {
        return TRUE;
    }

    if (DriverObject->DriverExtension &&
        ((ULONG_PTR)DriverObject->DriverExtension->AddDevice - Start < DxgkDriver->DriverSize))
    {
        return TRUE;
    }

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
    {
        if ((ULONG_PTR)DriverObject->MajorFunction[i] - Start < DxgkDriver->DriverSize)
            return TRUE;
    }

    return FALSE;
}

/**
 * @brief Is a WDDM display adapter there? dxgkrnl takes over the dispatch routines of every
 *        miniport, though a miniport may put its own in front of them again, and a display
 *        adapter, unlike a render only one, is in the display class. An XDDM miniport's
 *        dispatch is in videoprt, so it matches neither. This holds from the miniport's
 *        AddDevice on, before dxgkrnl starts the adapter.
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
            DlpDriverDrivesDisplay(DriverObject) &&
            (DlpIsDxgkMiniport(DriverObject, DxgkDriver) ||
             (ulDispatch - (ULONG_PTR)DriverObject->DriverStart < DriverObject->DriverSize)))
        {
            bFound = TRUE;
            DPRINT1("win32k: WDDM display driver %wZ\n", &DriverName);
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

#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
/* Exported by ntoskrnl */
PCHAR NTAPI PsGetProcessImageFileName(_In_ PEPROCESS Process);

/*
 * The device that holds a source exclusively, and its process. dxgkrnl 26100 only puts the CDD
 * primary back on the scanout when the owner's primary was made the displayed one through
 * D3DKMTSetDisplayMode, or when a DWM takes over; a legacy d3d9 device that flipped its own
 * primaries leaves the plane off once its primary goes while the source stays visible, so dxgkrnl
 * does not restore the GDI scanout on its own. win32k restores the scanout itself once that owner
 * is gone.
 */
static KSPIN_LOCK        gDlExclusiveOwnerLock;
static PEPROCESS         gpDlExclusiveOwnerProcess = NULL;
static D3DKMT_HANDLE     ghDlExclusiveOwnerDevice = 0;

/* D3DKMT_VIDPNSOURCEOWNER_TYPE and the DisableDWMVirtualMode bit of D3DKMT_VIDPNSOURCEOWNER_FLAGS */
#define DL_OWNER_UNOWNED            0
#define DL_OWNER_EXCLUSIVE          2
#define DL_OWNER_EXCLUSIVEGDI       3
#define DL_OWNER_FLAG_DISABLE_DWM_VIRTUAL_MODE 0x2

/**
 * @brief Forget the exclusive owner if it matches, telling whether it did.
 *
 * @param Process The process that let go, NULL for any.
 * @param hDevice The device that let go, 0 for any device of that process.
 */
static BOOLEAN
DlpForgetExclusiveOwner(
    _In_opt_ PEPROCESS Process,
    _In_ D3DKMT_HANDLE hDevice)
{
    BOOLEAN bForgot = FALSE;
    KIRQL OldIrql;

    KeAcquireSpinLock(&gDlExclusiveOwnerLock, &OldIrql);
    if ((gpDlExclusiveOwnerProcess != NULL) &&
        ((Process == NULL) || (Process == gpDlExclusiveOwnerProcess)) &&
        ((hDevice == 0) || (hDevice == ghDlExclusiveOwnerDevice)))
    {
        gpDlExclusiveOwnerProcess = NULL;
        ghDlExclusiveOwnerDevice = 0;
        bForgot = TRUE;
    }
    KeReleaseSpinLock(&gDlExclusiveOwnerLock, OldIrql);

    return bForgot;
}

/** @brief Put the CDD primary back on every WDDM display after an exclusive owner let go. */
static VOID
DlpRestoreGdiScanout(
    _In_ PCSTR Why)
{
    if (gDxgkInterface.pfnDxgkDisplayOnOff == NULL)
    {
        DPRINT1("win32k: %s, but dxgkrnl has no DxgkDisplayOnOff to restore the GDI scanout\n", Why);
        return;
    }

    DPRINT1("win32k: %s, restoring the GDI scanout\n", Why);
    PDEVOBJ_vRestoreCddScanout(gDxgkInterface.pfnDxgkDisplayOnOff);
}
#endif

/**
 * @brief Create or drop a process's DXGPROCESS.
 *        dxgkrnl frees the DXGPROCESS itself when the process dies; the drop only clears DxProcess.
 */
NTSTATUS NTAPI
DlProcessCallout(
    _Inout_ PPROCESSINFO ppi,
    _In_ BOOLEAN Create)
{
    NTSTATUS Status;

    if (!gbDxgkInitialized)
        return STATUS_SUCCESS;

    Status = gDxgkInterface.pfnDxgkProcessCallout(&ppi->DxProcess, &gDxgkWin32kEngInterface, Create);

#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
    /* A process that dies holding a source never releases it, dxgkrnl did while destroying its devices */
    if (!Create && DlpForgetExclusiveOwner(PsGetCurrentProcess(), 0))
        DlpRestoreGdiScanout("the exclusive owner's process went away");
#endif

    return Status;
}


/**
 * @brief Follow exclusive VidPN source ownership, from NtGdiDdDDISetVidPnSourceOwner once dxgkrnl
 *        returned. The GDI scanout comes back when the exclusive owner releases its sources.
 */
VOID
NTAPI
DlNoteVidPnSourceOwner(
    _In_ D3DKMT_HANDLE hDevice,
    _In_ ULONG cSources,
    _In_ ULONG FirstType,
    _In_ ULONG Flags,
    _In_ NTSTATUS Status)
{
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
    PEPROCESS Process = PsGetCurrentProcess();
    KIRQL OldIrql;

    if (!gbDxgkInitialized || !NT_SUCCESS(Status))
        return;

    /* DisableDWMVirtualMode only reapplies the topology, the type is not looked at */
    if (Flags & DL_OWNER_FLAG_DISABLE_DWM_VIRTUAL_MODE)
        return;

    if ((cSources != 0) && ((FirstType == DL_OWNER_EXCLUSIVE) || (FirstType == DL_OWNER_EXCLUSIVEGDI)))
    {
        KeAcquireSpinLock(&gDlExclusiveOwnerLock, &OldIrql);
        gpDlExclusiveOwnerProcess = Process;
        ghDlExclusiveOwnerDevice = hDevice;
        KeReleaseSpinLock(&gDlExclusiveOwnerLock, OldIrql);

        DPRINT1("win32k: device 0x%x of %s owns its source exclusively (type %lu)\n",
                hDevice, PsGetProcessImageFileName(Process), FirstType);
        return;
    }

    if ((cSources == 0) || ((cSources == 1) && (FirstType == DL_OWNER_UNOWNED)))
    {
        if (DlpForgetExclusiveOwner(Process, hDevice))
            DlpRestoreGdiScanout("the exclusive owner released its source");
    }
#else
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(cSources);
    UNREFERENCED_PARAMETER(FirstType);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Status);
#endif
}

/** @brief A destroyed device releases the sources it owned, dxgkrnl does that on destruction. */
VOID
NTAPI
DlNoteDeviceDestroyed(
    _In_ D3DKMT_HANDLE hDevice,
    _In_ NTSTATUS Status)
{
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
    if (!gbDxgkInitialized || !NT_SUCCESS(Status))
        return;

    if (DlpForgetExclusiveOwner(PsGetCurrentProcess(), hDevice))
        DlpRestoreGdiScanout("the exclusive owner's device was destroyed");
#else
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(Status);
#endif
}

/** @brief Mark the current process as the session's DWM, which may then share VidPN source ownership. */
NTSTATUS NTAPI
DlRegisterDwmProcess(VOID)
{
    if (!gbDxgkInitialized)
        return STATUS_SUCCESS;

    /* dxgkrnl has no session data before the session opens and does not check for it */
    if (!gbDlSessionOpened)
        return STATUS_DEVICE_NOT_READY;

#if DXGKRNL_TARGET >= DXGKRNL_TARGET_19041
    if (gDxgkInterface.pfnDxgkRegisterDwmProcess != NULL)
        return gDxgkInterface.pfnDxgkRegisterDwmProcess();
#endif
    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief Give every output the dying DWM owned back to GDI, from the DWM process's teardown.
 *        dxgkrnl restores the CDD's output while releasing, and primaries it reports as still
 *        needing it are asserted again.
 */
VOID NTAPI
DlDwmShutdown(VOID)
{
    DlForgetTokenThread();
    if (!gbDxgkInitialized)
        return;

#if DXGKRNL_TARGET >= DXGKRNL_TARGET_19041
    if (gDxgkInterface.pfnDxgkReleaseProcessVidPnSourceOwners != NULL)
        gDxgkInterface.pfnDxgkReleaseProcessVidPnSourceOwners(NtCurrentProcess());
#endif
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
    if (gDxgkInterface.pfnDxgkNeedToEnableCddPrimary != NULL)
        PDEVOBJ_vEnableCddPrimaries(gDxgkInterface.pfnDxgkNeedToEnableCddPrimary);
#endif
}

#if DL_DXGK_EXPORTS_D3DKMT
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
    { DXGK_SLOT_CreateHwQueue,                         "NtGdiDdDDICreateHwQueue" },
    { DXGK_SLOT_CreateKeyedMutex,                      "NtGdiDdDDICreateKeyedMutex" },
    { DXGK_SLOT_CreateKeyedMutex2,                     "NtGdiDdDDICreateKeyedMutex2" },
    { DXGK_SLOT_CreateOverlay,                         "NtGdiDdDDICreateOverlay" },
    { DXGK_SLOT_CreatePagingQueue,                     "NtGdiDdDDICreatePagingQueue" },
    { DXGK_SLOT_CreateSynchronizationObject,           "NtGdiDdDDICreateSynchronizationObject" },
    { DXGK_SLOT_DestroyAllocation,                     "NtGdiDdDDIDestroyAllocation" },
    { DXGK_SLOT_DestroyAllocation2,                    "NtGdiDdDDIDestroyAllocation2" },
    { DXGK_SLOT_DestroyContext,                        "NtGdiDdDDIDestroyContext" },
    { DXGK_SLOT_DestroyDevice,                         "NtGdiDdDDIDestroyDevice" },
    { DXGK_SLOT_DestroyHwQueue,                        "NtGdiDdDDIDestroyHwQueue" },
    { DXGK_SLOT_DestroyKeyedMutex,                     "NtGdiDdDDIDestroyKeyedMutex" },
    { DXGK_SLOT_DestroyOverlay,                        "NtGdiDdDDIDestroyOverlay" },
    { DXGK_SLOT_DestroyPagingQueue,                    "NtGdiDdDDIDestroyPagingQueue" },
    { DXGK_SLOT_DestroySynchronizationObject,          "NtGdiDdDDIDestroySynchronizationObject" },
    { DXGK_SLOT_EnumAdapters,                          "NtGdiDdDDIEnumAdapters" },
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
    { DXGK_SLOT_SubmitCommandToHwQueue,                "NtGdiDdDDISubmitCommandToHwQueue" },
    { DXGK_SLOT_SubmitSignalSyncObjectsToHwQueue,      "NtGdiDdDDISubmitSignalSyncObjectsToHwQueue" },
    { DXGK_SLOT_SubmitWaitForSyncObjectsToHwQueue,     "NtGdiDdDDISubmitWaitForSyncObjectsToHwQueue" },
    { DXGK_SLOT_Unlock,                                "NtGdiDdDDIUnlock" },
    { DXGK_SLOT_Unlock2,                               "NtGdiDdDDIUnlock2" },
    { DXGK_SLOT_UpdateAllocationProperty,              "NtGdiDdDDIUpdateAllocationProperty" },
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

#if DL_DXGK_EXPORTS_D3DKMT
    /* dxgkrnl looks the routines it calls back by name up in this image */
    Status = DlInitializeDxgkImports();
    if (!NT_SUCCESS(Status))
        return Status;
    RtlPcToFileHeader((PVOID)DlInitDxgkrnl, &gDxgkInterface.Win32kImageBase);
#endif

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

#if DL_DXGK_EXPORTS_D3DKMT
        DlpResolveD3DKMTExports();
        DlResolveDxgkForwards(gpDxgkDeviceObject->DriverObject->DriverStart);
#else
        /*
         * Also populate the D3DKMT callback table the NtGdiDdDDI* thunks route through.
         * Only ReactOS's own dxgkrnl knows this IOCTL: Windows 11 dxgkrnl uses the same code for
         * its capture interface and rejects the table. The thunks use the exports above instead.
         */
        Status = DxgRegisterAdapterCallbacks(gpDxgkDeviceObject);
        if (!NT_SUCCESS(Status))
            DPRINT1("win32k: DxgRegisterAdapterCallbacks failed 0x%lX\n", Status);
        Status = STATUS_SUCCESS;   /* the D3DKMT table is optional for the display path */
#endif

        /* CSRSS tried to open the session before dxgkrnl was ready, so open it now */
        if (gbDlSessionOpenDeferred)
            DlNotifySessionOpen();
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
 * @brief Bring-up aid: print a block of the modality dxgkrnl filled in, as raw 32 bit words, since
 *        part of the layout is still a guess.
 */
static VOID
DlpDumpWords(
    _In_ PCSTR What,
    _In_ ULONG Index,
    _In_reads_bytes_(Size) const VOID *Block,
    _In_ ULONG Size)
{
    const ULONG *Words = Block;
    ULONG Count = Size / sizeof(ULONG), i, j;
    CHAR Line[8 * 9 + 1];
    PSTR End;
    size_t Left;

    for (i = 0; i < Count; i += 8)
    {
        End = Line;
        Left = sizeof(Line);
        Line[0] = ANSI_NULL;
        for (j = i; (j < i + 8) && (j < Count); j++)
            RtlStringCbPrintfExA(End, Left, &End, &Left, 0, " %08lx", Words[j]);
        DPRINT1("win32k: %s %lu +%03lx:%s\n", What, Index, (ULONG)(i * sizeof(ULONG)), Line);
    }
}

/**
 * @brief Bring-up aid: print the paths of an applied modality and the mode each one hands GDI,
 *        including the refresh rate and pixel format dxgkrnl appends for the CDD.
 */
static VOID
DlpDumpModality(
    _In_ PD3DKMT_GETPATHSMODALITY Modality)
{
    PDL_MODALITY_PATH Paths = DL_MODALITY_PATHS(Modality);
    const ULONG *Extra;
    PDEVMODEW pdm;
    USHORT i;

    DlpDumpWords("modality", 0, Modality, DL_MODALITY_HEADER_SIZE);
    for (i = 0; (i < Modality->PathCount) && (i < 4); i++)
    {
        DlpDumpWords("path", i, &Paths[i], sizeof(Paths[i]));

        pdm = Paths[i].DevMode;
        if (pdm == NULL)
            continue;

        DPRINT1("win32k: path %u mode '%.32S' %lux%lux%lu %lu Hz, flags 0x%lx, fields 0x%lx, %u+%u bytes\n",
                i, pdm->dmDeviceName, pdm->dmPelsWidth, pdm->dmPelsHeight, pdm->dmBitsPerPel,
                pdm->dmDisplayFrequency, pdm->dmDisplayFlags, pdm->dmFields,
                pdm->dmSize, pdm->dmDriverExtra);
        if (pdm->dmDriverExtra >= 3 * sizeof(ULONG))
        {
            Extra = (const ULONG *)((const UCHAR *)pdm + pdm->dmSize);
            DPRINT1("win32k: path %u refresh %lu/%lu, format %lu\n", i, Extra[0], Extra[1], Extra[2]);
        }
    }
}

#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
/*
 * Where the 26100 path descriptor keeps the target timing, the source size and the scaling, as
 * the boot log's raw path dump and dxgkrnl's CDS_JOURNAL::_FillPathDescriptor show them.
 */
#define DL_PATH_TARGET_TOTAL        0x24    /* D3DKMDT_VIDEO_SIGNAL_INFO.TotalSize */
#define DL_PATH_TARGET_ACTIVE       0x2C    /* ...ActiveSize */
#define DL_PATH_TARGET_VSYNC        0x34    /* ...VSyncFreq, numerator and denominator */
#define DL_PATH_OUTPUT_TECHNOLOGY   0x58    /* D3DKMDT_VIDEO_OUTPUT_TECHNOLOGY of the target */
#define DL_PATH_SOURCE_SIZE         0x60    /* the source mode, what a CDS pins */
#define DL_PATH_ROTATION            0x84
#define DL_PATH_SCALING             0x88    /* D3DKMDT_VIDPN_PRESENT_PATH_SCALING the path uses */
#define DL_PATH_SCALING_INTENT      0x8C    /* the one asked for (4 = aspect ratio centered max) */
#define DL_PATH_PINNED_REFRESH      0xE0    /* the refresh rate a CDS pinned, numerator/denominator */

#define DL_PATH_ULONG(Path, Offset) (*(const ULONG *)((const UCHAR *)(Path) + (Offset)))
#endif

/**
 * @brief Bring-up aid: say in one line per path what a configuration will drive: the source mode,
 *        the target timing dxgkrnl picked for it and the scaling between the two. A target the
 *        size of the source runs the monitor in that mode, one larger than the source scales.
 */
static VOID
DlpLogPaths(
    _In_ PCSTR What,
    _In_ PD3DKMT_GETPATHSMODALITY Modality)
{
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
    PDL_MODALITY_PATH Paths = DL_MODALITY_PATHS(Modality);
    USHORT i;

    for (i = 0; (i < Modality->PathCount) && (i < 4); i++)
    {
        DPRINT1("win32k: %s path %u: source %lu %lux%lu -> target 0x%lx %lux%lu (total %lux%lu) %lu/%lu Hz, "
                "technology %ld, rotation %lu, scaling %lu (intent %lu), pinned %lu/%lu Hz, status 0x%lx\n",
                What, i, Paths[i].VidPnSourceId,
                DL_PATH_ULONG(&Paths[i], DL_PATH_SOURCE_SIZE),
                DL_PATH_ULONG(&Paths[i], DL_PATH_SOURCE_SIZE + 4),
                Paths[i].VidPnTargetId,
                DL_PATH_ULONG(&Paths[i], DL_PATH_TARGET_ACTIVE),
                DL_PATH_ULONG(&Paths[i], DL_PATH_TARGET_ACTIVE + 4),
                DL_PATH_ULONG(&Paths[i], DL_PATH_TARGET_TOTAL),
                DL_PATH_ULONG(&Paths[i], DL_PATH_TARGET_TOTAL + 4),
                DL_PATH_ULONG(&Paths[i], DL_PATH_TARGET_VSYNC),
                DL_PATH_ULONG(&Paths[i], DL_PATH_TARGET_VSYNC + 4),
                (LONG)DL_PATH_ULONG(&Paths[i], DL_PATH_OUTPUT_TECHNOLOGY),
                DL_PATH_ULONG(&Paths[i], DL_PATH_ROTATION),
                DL_PATH_ULONG(&Paths[i], DL_PATH_SCALING),
                DL_PATH_ULONG(&Paths[i], DL_PATH_SCALING_INTENT),
                DL_PATH_ULONG(&Paths[i], DL_PATH_PINNED_REFRESH),
                DL_PATH_ULONG(&Paths[i], DL_PATH_PINNED_REFRESH + 4),
                Paths[i].StatusApply);
    }
#else
    UNREFERENCED_PARAMETER(What);
    UNREFERENCED_PARAMETER(Modality);
#endif
}

/**
 * @brief Tell GDI which sources the applied configuration turned on and in what mode, which is
 *        what the desktop is then built from. A path the apply failed, or one without a mode, has
 *        no source on the desktop.
 *
 * @param bSaved The configuration is the saved one (or replaces it), so its modes are also what
 *        ENUM_REGISTRY_SETTINGS reports. FALSE for a temporary mode change, CDS_FULLSCREEN.
 */
static NTSTATUS
DlpRecordDisplayConfig(
    _In_ PD3DKMT_GETPATHSMODALITY Modality,
    _In_ BOOLEAN bSaved)
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

    Status = EngpSetDisplayConfig(ActiveCount, ActivePaths, bSaved);

    ExFreePoolWithTag(ActivePaths, GDITAG_TEMP);
    return Status;
}

/**
 * @brief Functionalize the paths, apply them, tell GDI which sources came on and build the desktop
 *        from them, then finalize. The desktop is built between the apply and the finalize, as
 *
 * Every step is logged before it runs: a step that never returns is the last line of the log.
 *
 * @param Modality The paths to commit.
 * @param FunctionalizeFlags What Functionalize is given.
 * @param ApplyFlags What Apply is given.
 * @param bValidateOnly Stop after Functionalize, which is all a CDS_TEST caller wants to know.
 * @param bSaved The configuration is the saved one, see DlpRecordDisplayConfig.
 * @param pmdevOld The desktop to replace, or NULL when the first one is being built.
 * @param ppmdevNew Receives the new desktop. NULL leaves the desktop alone.
 * @param pbApplied Receives TRUE once dxgkrnl applied the configuration, so the display runs it
 *        whatever happened to the desktop afterwards.
 */
static NTSTATUS
DlpCommitPathsModality(
    _Inout_ PD3DKMT_GETPATHSMODALITY Modality,
    _In_ ULONG FunctionalizeFlags,
    _In_ ULONG ApplyFlags,
    _In_ BOOLEAN bValidateOnly,
    _In_ BOOLEAN bSaved,
    _In_opt_ PMDEVOBJ pmdevOld,
    _Out_opt_ PMDEVOBJ *ppmdevNew,
    _Out_opt_ PBOOLEAN pbApplied)
{
    NTSTATUS Status = STATUS_SUCCESS;
    LONG     lRet;

    if (pbApplied != NULL)
        *pbApplied = FALSE;

    /*
     * Pick a source mode for each path. Enumerating a path says the adapter could drive it, not
     * what it should drive, so the mode fields stay empty until this runs. Apply then commits
     * what is there, and dxgkrnl compares that against the CDD's surface to decide whether it can
     * present the CDD shadow directly or has to blit through an allocation of its own
     * (ADAPTER_DISPLAY::IsIdenticalMode). Skip this and the comparison fails on an empty mode.
     */
    if (gDxgkInterface.pfnDxgkFunctionalizePathsModality != NULL)
    {
        DPRINT1("win32k: functionalizing %u path(s), flags 0x%lx\n", Modality->PathCount, FunctionalizeFlags);
        Status = gDxgkInterface.pfnDxgkFunctionalizePathsModality(FunctionalizeFlags, Modality);
        if (!NT_SUCCESS(Status))
        {
            DPRINT1("win32k: DxgkFunctionalizePathsModality failed 0x%lX\n", Status);
            return Status;
        }

        /* What the apply is about to program, the target timing above all */
        DlpLogPaths(bValidateOnly ? "validated" : "functional", Modality);
    }

    if (bValidateOnly)
        return Status;

    DPRINT1("win32k: applying the display configuration, flags 0x%lx\n", ApplyFlags);
#if DL_DXGK_APPLY_HAS_SCENARIO
    Status = gDxgkInterface.pfnDxgkApplyPathsModality(ApplyFlags, Modality, NULL);
#else
    Status = gDxgkInterface.pfnDxgkApplyPathsModality(ApplyFlags, Modality);
#endif
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("win32k: DxgkApplyPathsModality failed 0x%lX\n", Status);
        return Status;
    }

    if (pbApplied != NULL)
        *pbApplied = TRUE;

    /* AppliedCount is not a path count on Windows 11: Functionalize zeroes it, and the apply leaves it be */
    DPRINT1("win32k: display configuration applied\n");
    DlpDumpModality(Modality);

    Status = DlpRecordDisplayConfig(Modality, bSaved);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("win32k: no source of the display configuration reached GDI, 0x%lX\n", Status);
    }
    else if (ppmdevNew != NULL)
    {
        /* The desktop is made of the sources the configuration turned on */
        DPRINT1("win32k: building the desktop for the display configuration\n");
        lRet = PDEVOBJ_lChangeDisplaySettings(NULL, NULL, pmdevOld, ppmdevNew, (pmdevOld == NULL));
        if (lRet != DISP_CHANGE_SUCCESSFUL)
        {
            DPRINT1("win32k: building the desktop for the display configuration failed %ld\n", lRet);
            Status = STATUS_UNSUCCESSFUL;
        }
        else
        {
            DPRINT1("win32k: desktop built for the display configuration\n");
        }
    }

    if (gDxgkInterface.pfnDxgkFinalizePathsModality != NULL)
    {
        DPRINT1("win32k: finalizing the display configuration\n");
        gDxgkInterface.pfnDxgkFinalizePathsModality(Modality);
    }

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

    /* Saved or enumerated, this is the configuration the desktop returns to */
    Status = DlpCommitPathsModality(Modality,
                                    Flags & DL_MODALITY_FUNCTIONALIZE,
                                    DL_MODALITY_NO_OPTIMIZE | DL_MODALITY_APPLY,
                                    FALSE,
                                    TRUE,
                                    pmdevOld,
                                    ppmdevNew,
                                    NULL);

    DlpFreePathsModality(Modality);
    return Status;
}

/** @brief Does the source list this mode? A mode that names no orientation is the upright one. */
static BOOLEAN
DlpSourceHasMode(
    _In_ PGRAPHICS_DEVICE pGraphicsDevice,
    _In_ ULONG Width,
    _In_ ULONG Height,
    _In_ ULONG BitsPerPel,
    _In_ ULONG Frequency,
    _In_ ULONG Orientation)
{
    PDEVMODEW pdm;
    ULONG i;

    for (i = 0; (i < pGraphicsDevice->cDevModes) && (pGraphicsDevice->pDevModeList != NULL); i++)
    {
        pdm = pGraphicsDevice->pDevModeList[i].pdm;
        if ((pdm->dmPelsWidth == Width) &&
            (pdm->dmPelsHeight == Height) &&
            ((BitsPerPel == 0) || (pdm->dmBitsPerPel == BitsPerPel)) &&
            (pdm->dmDisplayFrequency == Frequency) &&
            (((pdm->dmFields & DM_DISPLAYORIENTATION) ? pdm->dmDisplayOrientation : DMDO_DEFAULT) == Orientation))
        {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * @brief Fill in what a ChangeDisplaySettings caller left out the way Windows does, starting
 *        from the mode the path runs now: the orientation stays, and a size asked for without a
 *        refresh rate gets the rate the source runs at now if the list has that size at it,
 *        otherwise 60 Hz.
 *
 * The CDD lists a size at several refresh rates, those of the monitor's own timings among them
 * (boot 19 lists 640x480 at 59, 60, 75, 100 and 120 Hz). Keeping the current rate leaves dxgkrnl
 * a target timing at that rate, normally the one the monitor runs now, with the GPU scaling the
 * smaller source; 60 Hz usually means the monitor's own 640x480 timing. LDEVOBJ's pick took the
 * rate of the last matching entry of the list (no WDDM device has a DefaultSettings.VRefresh).
 *
 * @param pdmRequest The caller's mode, a kernel copy of at most sizeof(DEVMODEW) bytes.
 * @param pdmCurrent The mode the source runs now, NULL if unknown.
 * @param pdmComplete Receives the completed request, without driver extra data.
 */
static VOID
DlpCompleteModeRequest(
    _In_ PGRAPHICS_DEVICE pGraphicsDevice,
    _In_ const DEVMODEW *pdmRequest,
    _In_opt_ const DEVMODEW *pdmCurrent,
    _Out_ PDEVMODEW pdmComplete)
{
    ULONG BitsPerPel, Frequency = 0;

    RtlZeroMemory(pdmComplete, sizeof(*pdmComplete));
    RtlCopyMemory(pdmComplete, pdmRequest, min(pdmRequest->dmSize, sizeof(*pdmComplete)));
    pdmComplete->dmSize = sizeof(*pdmComplete);
    pdmComplete->dmDriverExtra = 0;

    if (!(pdmComplete->dmFields & DM_DISPLAYORIENTATION))
    {
        pdmComplete->dmDisplayOrientation =
            ((pdmCurrent != NULL) && (pdmCurrent->dmFields & DM_DISPLAYORIENTATION)) ?
            pdmCurrent->dmDisplayOrientation : DMDO_DEFAULT;
        pdmComplete->dmFields |= DM_DISPLAYORIENTATION;
    }

    if (((pdmComplete->dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT)) != (DM_PELSWIDTH | DM_PELSHEIGHT)) ||
        ((pdmComplete->dmFields & DM_DISPLAYFREQUENCY) && (pdmComplete->dmDisplayFrequency != 0)))
    {
        return;
    }

    if (!LDEVOBJ_bBuildDevmodeList(pGraphicsDevice))
        return;

    BitsPerPel = pdmComplete->dmBitsPerPel;
    if (!(pdmComplete->dmFields & DM_BITSPERPEL) || (BitsPerPel == 0))
        BitsPerPel = (pdmCurrent != NULL) ? pdmCurrent->dmBitsPerPel : 0;

    /* 1 Hz stands for a rate the driver did not specify, as BasicDisplay reports */
    if ((pdmCurrent != NULL) && (pdmCurrent->dmDisplayFrequency > 1) &&
        DlpSourceHasMode(pGraphicsDevice, pdmComplete->dmPelsWidth, pdmComplete->dmPelsHeight,
                         BitsPerPel, pdmCurrent->dmDisplayFrequency, pdmComplete->dmDisplayOrientation))
    {
        Frequency = pdmCurrent->dmDisplayFrequency;
    }
    else if (DlpSourceHasMode(pGraphicsDevice, pdmComplete->dmPelsWidth, pdmComplete->dmPelsHeight,
                              BitsPerPel, 60, pdmComplete->dmDisplayOrientation))
    {
        Frequency = 60;
    }

    if (Frequency != 0)
    {
        pdmComplete->dmDisplayFrequency = Frequency;
        pdmComplete->dmFields |= DM_DISPLAYFREQUENCY;
    }
}

/**
 * @brief Change the mode of one WDDM source for a legacy ChangeDisplaySettings caller. The mode
 *        goes through dxgkrnl's CDS journal into the current paths, which are then committed like
 *        any other configuration. Must run in CSRSS or a GUI thread of the session.
 *
 * The sequence dxgkrnl expects for this: Get (the running topology, or the saved one), AugmentCdsj
 * to validate, capture the mode, put it in the paths with ApplyCdsjToPathsModality, Functionalize,
 * Apply, build the desktop, Finalize.
 *
 * @param pGraphicsDevice The source whose mode changes.
 * @param pdmRequest The mode asked for. NULL goes back to the saved configuration.
 * @param pdmCurrent The mode the source runs now, what unspecified fields default to.
 * @param bTryClosest Settle for the closest mode the source has.
 * @param bSetMode Program the mode. FALSE only checks it, or only saves it with bUpdateRegistry.
 * @param bUpdateRegistry Save the new configuration in the display database.
 * @param bFullScreen The caller changes the mode for a full screen application.
 * @param pmdevOld The current desktop.
 * @param ppmdevNew Receives the new desktop when the mode was set.
 * @param pbApplied Receives TRUE once dxgkrnl programmed a configuration, even if building the
 *        desktop for it failed afterwards and the display no longer matches the desktop.
 *
 * @return A DISP_CHANGE_* value.
 */
LONG NTAPI
DlChangeDisplaySettings(
    _In_ PGRAPHICS_DEVICE pGraphicsDevice,
    _In_opt_ PDEVMODEW pdmRequest,
    _In_opt_ const DEVMODEW *pdmCurrent,
    _In_ BOOLEAN bTryClosest,
    _In_ BOOLEAN bSetMode,
    _In_ BOOLEAN bUpdateRegistry,
    _In_ BOOLEAN bFullScreen,
    _In_ PMDEVOBJ pmdevOld,
    _Out_ PMDEVOBJ *ppmdevNew,
    _Out_ PBOOLEAN pbApplied)
{
    PPROCESSINFO             ppi = PsGetCurrentProcessWin32Process();
    PD3DKMT_GETPATHSMODALITY Modality = NULL;
    PDEVMODEW                pdmCaptured = NULL;
    DEVMODEW                 dmComplete;
    DL_CDS_REQUEST           Request;
    const ULONG             *Extra;
    ULONG                    FunctionalizeFlags;
    BOOLEAN                  bSaved;
    NTSTATUS                 Status;
    LONG                     lRet = DISP_CHANGE_FAILED;

    *ppmdevNew = NULL;
    *pbApplied = FALSE;

    if (!gbDxgkInitialized || (ppi == NULL) || (ppi->DxProcess == NULL) ||
        (gDxgkInterface.pfnDxgkGetPathsModality == NULL) ||
        (gDxgkInterface.pfnDxgkApplyPathsModality == NULL) ||
        (gDxgkInterface.pfnDxgkAugmentCdsj == NULL) ||
        (gDxgkInterface.pfnDxgkApplyCdsjToPathsModality == NULL))
    {
        DPRINT1("win32k: no WDDM mode change for '%S': interface %u, process %p, DXGPROCESS %p\n",
                pGraphicsDevice->szWinDeviceName, gbDxgkInitialized, ppi, ppi ? ppi->DxProcess : NULL);
        return DISP_CHANGE_FAILED;
    }

    if (pdmRequest != NULL)
    {
        DPRINT1("win32k: mode change of '%S' to %lux%lux%lu %lu Hz (fields 0x%lx)%s%s%s%s\n",
                pGraphicsDevice->szWinDeviceName,
                pdmRequest->dmPelsWidth, pdmRequest->dmPelsHeight, pdmRequest->dmBitsPerPel,
                pdmRequest->dmDisplayFrequency, pdmRequest->dmFields,
                bSetMode ? "" : ", check only", bUpdateRegistry ? ", saved" : "",
                bFullScreen ? ", full screen" : "", bTryClosest ? ", closest" : "");
    }
    else
    {
        DPRINT1("win32k: mode change of '%S' back to the saved configuration\n", pGraphicsDevice->szWinDeviceName);
    }

    /* The topology running now, or the saved one when the change is saved or undone */
    bSaved = bUpdateRegistry || (pdmRequest == NULL);
    Status = DlpQueryPathsModality(DL_MODALITY_CDS | (bSaved ? DL_MODALITY_PERSISTED : DL_MODALITY_CDS_RUNNING),
                                   &Modality);
    if (!NT_SUCCESS(Status) && (bSaved || (DL_MODALITY_CDS_RUNNING != DL_MODALITY_CURRENT)))
    {
        DPRINT1("win32k: no %s display configuration (0x%lX), changing the running one\n",
                bSaved ? "saved" : "scaled", Status);
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
    DlpCompleteModeRequest(pGraphicsDevice, pdmRequest, pdmCurrent, &dmComplete);
    if (!LDEVOBJ_bProbeAndCaptureDevmode(pGraphicsDevice, &dmComplete, &pdmCaptured, bTryClosest))
    {
        DPRINT1("win32k: '%S' lists no %lux%lux%lu %lu Hz mode\n", pGraphicsDevice->szWinDeviceName,
                dmComplete.dmPelsWidth, dmComplete.dmPelsHeight, dmComplete.dmBitsPerPel,
                dmComplete.dmDisplayFrequency);
        lRet = DISP_CHANGE_BADMODE;
        goto Cleanup;
    }

    Extra = (pdmCaptured->dmDriverExtra >= 3 * sizeof(ULONG)) ?
            (const ULONG *)((const UCHAR *)pdmCaptured + pdmCaptured->dmSize) : NULL;
    DPRINT1("win32k: mode change picks %lux%lux%lu %lu Hz (refresh %lu/%lu, format %lu), orientation %lu, fields 0x%lx\n",
            pdmCaptured->dmPelsWidth, pdmCaptured->dmPelsHeight, pdmCaptured->dmBitsPerPel,
            pdmCaptured->dmDisplayFrequency, Extra ? Extra[0] : 0, Extra ? Extra[1] : 0,
            Extra ? Extra[2] : 0, pdmCaptured->dmDisplayOrientation, pdmCaptured->dmFields);

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
                                    DL_APPLY_CDS,
                                    !bSetMode,
                                    bSaved,
                                    pmdevOld,
                                    bSetMode ? ppmdevNew : NULL,
                                    pbApplied);
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
    DPRINT1("win32k: mode change of '%S' ends with %ld%s\n", pGraphicsDevice->szWinDeviceName, lRet,
            *pbApplied ? ", the display runs the new configuration" : "");
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

    if (gbDlSessionOpened)
        return;

#if !DL_SMGR_SESSION_CALLOUT
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
        {
            /* DlInitDxgkrnl opens it once the interface arrives */
            gbDlSessionOpenDeferred = TRUE;
            return;
        }
    }
    else if ((ppi == NULL) || (ppi->DxProcess == NULL))
    {
        return;
    }
    else if (gbBaseVideo)
    {
        /* /BASEVIDEO: keep the desktop on VGA even with a WDDM driver installed */
        DPRINT1("win32k: VGA mode requested, the desktop stays on the XDDM drivers\n");
        return;
    }
    else if (!DlpHasWddmDisplayAdapter())
    {
        /* dxgkrnl would hold the session for a display adapter that never comes, GDI stays on XDDM */
        DPRINT1("win32k: no WDDM display adapter, the desktop stays on the XDDM drivers\n");
        return;
    }

    DPRINT1("win32k: opening session, dxgkrnl waits for its display and render adapters to start\n");
#if DL_SMGR_SESSION_CALLOUT
    Status = SMgrNotifySessionChange(DL_SESSION_OPEN, (PVOID)VideoPortCallout);
#else
    Status = SMgrNotifySessionChange(DL_SESSION_OPEN);
#endif
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("win32k: SMgrNotifySessionChange failed 0x%lX\n", Status);
        return;
    }

    gbDlSessionOpened = TRUE;
    gbDlSessionOpenDeferred = FALSE;
}

/**
 * @brief Tell dxgkrnl the session is connected to the console.
 *
 * Until then dxgkrnl has no console session, and what it only does for the console fails or is
 * skipped: GetDisplayModeList, which every Direct3D 9 adapter needs, returns
 * STATUS_INVALID_PARAMETER. Windows connects in CSRSS with the user lock held, once the first
 * desktop has been built. ReactOS has only the console session, so it connects at the same point.
 * The call takes locks of dxgkrnl's own, the same ones FinalizePathsModality just took on this
 * thread, and calls nothing in win32k back synchronously.
 */
static VOID
DlpConnectConsoleSession(VOID)
{
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
    if (gbDlSessionConnected || !gbDlSessionOpened || (gDxgkInterface.pfnDxgkSessionConnected == NULL))
        return;

    /* Logged before the call too, so a boot that stops in it says so */
    DPRINT1("win32k: connecting the console session to dxgkrnl\n");
    gDxgkInterface.pfnDxgkSessionConnected(TRUE);
    gbDlSessionConnected = TRUE;
    DPRINT1("win32k: console session connected to dxgkrnl\n");
#endif
}

/**
 * @brief Turn the adapter and id of a DisplayConfig packet into the actual ones, which Windows does
 *        before it answers any type whose id is not -1: the id of a QueryDisplayConfig path can
 *        carry a hash of dxgkrnl's virtual topology. Any other id stays as it is.
 *
 * @param pAdapterLuid The adapter, replaced by the actual one.
 * @param pId The source or target on it, replaced by the actual one.
 */
NTSTATUS NTAPI
DlConvertDisplayConfigAdapterAndId(
    _Inout_ PLUID pAdapterLuid,
    _Inout_ PULONG pId)
{
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
    if (gbDxgkInitialized && (gDxgkInterface.pfnDxgkConvertLegacyQDCAdapterAndIdToActual != NULL))
    {
        return gDxgkInterface.pfnDxgkConvertLegacyQDCAdapterAndIdToActual(pAdapterLuid, *pId,
                                                                          pAdapterLuid, pId);
    }
#endif
    UNREFERENCED_PARAMETER(pAdapterLuid);
    UNREFERENCED_PARAMETER(pId);
    return STATUS_SUCCESS;
}

/**
 * @brief Answer a DisplayConfigGetDeviceInfo info type that dxgkrnl owns.
 *
 * @param Packet A kernel copy of the caller's packet, of the size its type takes.
 *
 * @return STATUS_NOT_SUPPORTED when dxgkrnl offers no such routine.
 */
NTSTATUS NTAPI
DlDisplayConfigGetDeviceInfo(
    _Inout_ PVOID Packet)
{
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
    if (gbDxgkInitialized && (gDxgkInterface.pfnDxgkDisplayConfigDeviceInfo != NULL))
        return gDxgkInterface.pfnDxgkDisplayConfigDeviceInfo(TRUE, Packet);
#endif
    UNREFERENCED_PARAMETER(Packet);
    return STATUS_NOT_SUPPORTED;
}

/**
 * @brief Have dxgkrnl fill in its part of a display info packet (adapter IDs, description, driver
 *        model, the mode of the source, the flags from bit 5 up).
 *
 * @param AdapterLuid The adapter.
 * @param VidPnSourceId Its source, or -1 for the adapter alone.
 * @param Packet A kernel copy of the 0x808 byte packet.
 *
 * @return STATUS_NOT_SUPPORTED when there is no dxgkrnl to ask. The 26100 dxgkrnl itself fails with
 *         STATUS_INVALID_PARAMETER (unknown adapter or source), STATUS_DEVICE_REMOVED or what a
 *         lock of its own returned.
 */
NTSTATUS NTAPI
DlGetAdapterDeviceDesc(
    _In_ LUID AdapterLuid,
    _In_ ULONG VidPnSourceId,
    _Inout_ PVOID Packet)
{
#if DXGKRNL_TARGET == DXGKRNL_TARGET_WIN11
    if (gbDxgkInitialized && (gDxgkInterface.pfnDxgkGetAdapterDeviceDesc != NULL))
        return gDxgkInterface.pfnDxgkGetAdapterDeviceDesc(AdapterLuid, VidPnSourceId, 0, Packet);
#endif
    UNREFERENCED_PARAMETER(AdapterLuid);
    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(Packet);
    return STATUS_NOT_SUPPORTED;
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
    {
        DPRINT1("win32k: DlpApplyDisplayConfig failed 0x%lX\n", Status);
        return Status;
    }

    DlpConnectConsoleSession();

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
    NTSTATUS Status;

    *ppmdevNew = NULL;

    if (!gbDxgkInitialized || (ppi == NULL) || (ppi->DxProcess == NULL))
        return STATUS_NOT_SUPPORTED;

    Status = DlpApplyDisplayConfig(pmdevOld, ppmdevNew);

    /* The first desktop may have come up on XDDM, then this is the first WDDM apply */
    if (NT_SUCCESS(Status))
        DlpConnectConsoleSession();

    return Status;
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
