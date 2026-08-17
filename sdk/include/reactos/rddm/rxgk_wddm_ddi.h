/*
 * PROJECT:     ReactOS Display Driver Model (DxgKrnl_ms)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     WDDM DDI types the ReactOS DDK currently stubs out (segment query + submit)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 *
 * ReactOS's <ddk/d3dkmddi.h> leaves the WDDM memory-segment query and command-submission DDI
 * structures undefined (PDXGKDDI_SUBMITCOMMAND is a UINT32* placeholder, DXGK_QUERYSEGMENTOUT
 * is absent). These are the layouts dxgkrnl and the miniport agree on; defined here to the
 * documented WDDM ABI so VidMm can read segments and VidSch can submit. Include after
 * <dispmprt.h> / <d3dkmddi.h>. See RDDM-WDDM-ROADMAP.md §7.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef UINT DXGK_SEGMENTID;

/** @brief Per-segment capability flags (DXGKQAITYPE_QUERYSEGMENT). */
typedef struct _DXGK_SEGMENTFLAGS
{
    union
    {
        struct
        {
            /*
             * Bit assignment verified against Reference/win10/dxgmms2.h:18620. VidMm branches on
             * Aperture (0x1), CpuVisible (0x4), ReservedSysMem (0x1000) and
             * SupportsCpuHostAperture (0x2000) when mapping an allocation for CPU access
             * (VIDMM_PAGE_TABLE_BASE::GetCpuVisibleAddress, dxgmms2.c:88797), so the whole set is
             * named here rather than folded into Reserved.
             */
            UINT Aperture                        : 1;   /* 0x00000001 */
            UINT Agp                             : 1;   /* 0x00000002 */
            UINT CpuVisible                      : 1;   /* 0x00000004 */
            UINT UseBanking                      : 1;   /* 0x00000008 */
            UINT CacheCoherent                   : 1;   /* 0x00000010 */
            UINT PitchAlignment                  : 1;   /* 0x00000020 */
            UINT PopulatedFromSystemMemory       : 1;   /* 0x00000040 */
            UINT PreservedDuringStandby          : 1;   /* 0x00000080 */
            UINT PreservedDuringHibernate        : 1;   /* 0x00000100 */
            UINT PartiallyPreservedDuringHibernate : 1; /* 0x00000200 */
            UINT DirectFlip                      : 1;   /* 0x00000400 */
            UINT Use64KBPages                    : 1;   /* 0x00000800 */
            UINT ReservedSysMem                  : 1;   /* 0x00001000 */
            UINT SupportsCpuHostAperture         : 1;   /* 0x00002000 */
            UINT SupportsCachedCpuHostAperture   : 1;   /* 0x00004000 */
            UINT ApplicationTarget               : 1;   /* 0x00008000 */
            UINT VprSupported                    : 1;   /* 0x00010000 */
            UINT VprPreservedDuringStandby       : 1;   /* 0x00020000 */
            UINT EncryptedPagingSupported        : 1;   /* 0x00040000 */
            UINT LocalBudgetGroup                : 1;   /* 0x00080000 */
            UINT NonLocalBudgetGroup             : 1;   /* 0x00100000 */
            UINT Reserved                        : 11;
        };
        UINT Value;
    };
} DXGK_SEGMENTFLAGS;

/** @brief One memory segment the miniport exposes. */
typedef struct _DXGK_SEGMENTDESCRIPTOR
{
    PHYSICAL_ADDRESS  BaseAddress;
    PHYSICAL_ADDRESS  CpuTranslatedAddress;
    SIZE_T            Size;
    UINT              NbOfBanks;
    SIZE_T           *pBankRangeTable;
    SIZE_T            CommitLimit;
    DXGK_SEGMENTFLAGS Flags;
} DXGK_SEGMENTDESCRIPTOR;

/** @brief Input to DXGKQAITYPE_QUERYSEGMENT. */
typedef struct _DXGK_QUERYSEGMENTIN
{
    PHYSICAL_ADDRESS  AgpApertureBase;
    LARGE_INTEGER     AgpApertureSize;
    DXGK_SEGMENTFLAGS AgpFlags;
} DXGK_QUERYSEGMENTIN;

/** @brief Output of DXGKQAITYPE_QUERYSEGMENT. First call (pSegmentDescriptor==NULL) yields
 *         NbSegment; the second call fills the caller-allocated descriptor array. */
typedef struct _DXGK_QUERYSEGMENTOUT
{
    ULONG                   NbSegment;
    DXGK_SEGMENTDESCRIPTOR *pSegmentDescriptor;
    ULONG                   PagingBufferSegmentId;
    ULONG                   PagingBufferSize;
    ULONG                   PagingBufferPrivateDataSize;
} DXGK_QUERYSEGMENTOUT;

/** @brief Flags for DXGKARG_SUBMITCOMMAND (only Value is consumed at submit time). */
typedef struct _DXGK_SUBMITCOMMANDFLAGS
{
    union
    {
        struct
        {
            UINT RepeatedCommandBuffer : 1;
            UINT ResizeCommandBuffer   : 1;
            UINT Reserved              : 30;
        };
        UINT Value;
    };
} DXGK_SUBMITCOMMANDFLAGS;

/**
 * @brief Argument to the miniport DxgkDdiSubmitCommand, matching the WDK d3dkmddi.h layout for
 *        DXGKDDI_INTERFACE_VERSION 0x5023 (WDDM 2.0). The trailing WIN7/WIN8 members are part of
 *        the pinned ABI. WDDM typedefs are spelled with their underlying primitive sizes
 *        (D3DDDI_VIDEO_PRESENT_SOURCE_ID = UINT, D3DDDI_FLIPINTERVAL_TYPE = enum/UINT,
 *        D3DGPU_VIRTUAL_ADDRESS = UINT64) so the header is self-contained.
 */
typedef struct _DXGKARG_SUBMITCOMMAND
{
    union
    {
        HANDLE              hDevice;
        HANDLE              hContext;
    };
    UINT                    DmaBufferSegmentId;
    PHYSICAL_ADDRESS        DmaBufferPhysicalAddress;
    UINT                    DmaBufferSize;
    UINT                    DmaBufferSubmissionStartOffset;
    UINT                    DmaBufferSubmissionEndOffset;
    VOID                   *pDmaBufferPrivateData;
    UINT                    DmaBufferPrivateDataSize;
    UINT                    DmaBufferPrivateDataSubmissionStartOffset;
    UINT                    DmaBufferPrivateDataSubmissionEndOffset;
    UINT                    SubmissionFenceId;
    UINT                    VidPnSourceId;          /* D3DDDI_VIDEO_PRESENT_SOURCE_ID */
    UINT                    FlipInterval;           /* D3DDDI_FLIPINTERVAL_TYPE */
    DXGK_SUBMITCOMMANDFLAGS Flags;
    UINT                    EngineOrdinal;
    UINT64                  DmaBufferVirtualAddress; /* WDDM >= WIN7 */
    UINT                    NodeOrdinal;             /* WDDM >= WIN8 */
} DXGKARG_SUBMITCOMMAND;

/*
 * DxgkDdiCreateContext argument chain, matching the WDK d3dkmddi.h for
 * DXGKDDI_INTERFACE_VERSION 0x5023 (WDDM 2.0). DXGKARG_CREATECONTEXT is forward-declared (and
 * left incomplete) in the ReactOS DDK; we complete it here. The miniport fills hContext and the
 * ContextInfo (DMA-buffer requirements); dxgkrnl supplies NodeOrdinal/EngineAffinity/Flags.
 */
typedef struct _DXGK_CREATECONTEXTFLAGS
{
    union
    {
        struct
        {
            UINT SystemContext     : 1;
            UINT GdiContext        : 1;
            UINT VirtualAddressing : 1;   /* WDDM 2.0 */
            UINT Reserved          : 29;
        };
        UINT Value;
    };
} DXGK_CREATECONTEXTFLAGS;

typedef struct _DXGK_CONTEXTINFO_CAPS
{
    union
    {
        struct
        {
            UINT NoPatchingRequired     : 1;
            UINT DriverManagesResidency : 1;
            UINT UseIoMmu               : 1;
            UINT Reserved               : 29;
        };
        UINT Value;
    };
} DXGK_CONTEXTINFO_CAPS;

typedef struct _DXGK_CONTEXTINFO
{
    UINT                  DmaBufferSize;
    UINT                  DmaBufferSegmentSet;
    UINT                  DmaBufferPrivateDataSize;
    UINT                  AllocationListSize;
    UINT                  PatchLocationListSize;
    UINT                  Reserved;                 /* WDDM >= WIN7 */
    DXGK_CONTEXTINFO_CAPS Caps;                     /* WDDM 2.0 */
    ULONG                 PagingCompanionNodeId;    /* WDDM 2.0 */
} DXGK_CONTEXTINFO;

struct _DXGKARG_CREATECONTEXT
{
    HANDLE                  hContext;               /* out */
    UINT                    NodeOrdinal;
    UINT                    EngineAffinity;
    DXGK_CREATECONTEXTFLAGS Flags;
    VOID                   *pPrivateDriverData;
    UINT                    PrivateDriverDataSize;
    DXGK_CONTEXTINFO        ContextInfo;            /* out */
};

/*
 * RecommendFunctionalVidPn - the ReactOS DDK's DXGKARG_RECOMMENDFUNCTIONALVIDPN is WRONG: it has
 * only the hRecommendedFunctionalVidPn field, but the real WDDM ABI puts that field THIRD, behind
 * NumberOfVidPnTargets + pVidPnTargetPrioritizationVector. Using the DDK layout would hand the
 * miniport a VidPN handle read from the wrong offset. RXGK_ARG_RECOMMENDFUNCTIONALVIDPN is the
 * correct WDDM2.0 ABI (verified against WDK d3dkmddi.h); the pfn is cast to RXGK_PFN_RECOMMENDFUNCTIONALVIDPN.
 */
typedef enum _RXGK_RECOMMENDFUNCTIONALVIDPN_REASON
{
    RXGK_RFVR_UNINITIALIZED = 0,
    RXGK_RFVR_HOTKEY        = 1,
    RXGK_RFVR_USERMODE      = 2,
    RXGK_RFVR_FIRMWARE      = 3,
} RXGK_RECOMMENDFUNCTIONALVIDPN_REASON;

typedef struct _RXGK_ARG_RECOMMENDFUNCTIONALVIDPN
{
    UINT                                        NumberOfVidPnTargets;            /* in */
    const D3DDDI_VIDEO_PRESENT_TARGET_ID       *pVidPnTargetPrioritizationVector; /* in */
    D3DKMDT_HVIDPN                              hRecommendedFunctionalVidPn;     /* in: handle to fill */
    RXGK_RECOMMENDFUNCTIONALVIDPN_REASON       RequestReason;                   /* in */
    VOID                                       *pPrivateDriverData;             /* in opt */
    UINT                                        PrivateDriverDataSize;          /* in */
} RXGK_ARG_RECOMMENDFUNCTIONALVIDPN;

typedef NTSTATUS (NTAPI *RXGK_PFN_RECOMMENDFUNCTIONALVIDPN)(
    _In_ const PVOID MiniportDeviceContext,
    _In_ const RXGK_ARG_RECOMMENDFUNCTIONALVIDPN *pRecommendFunctionalVidPn);

/*
 * SetVidPnSourceAddress (scanout) - the ReactOS DDK's DXGKARG_SETVIDPNSOURCEADDRESS is TRUNCATED
 * for WDDM2.0: it stops at Flags, omitting the WDDM1.3+/2.0 trailing fields a 0x5023-compiled
 * miniport reads (Duration, PrimaryData[], DriverPrivateData*). RXGK_ARG_SETVIDPNSOURCEADDRESS is
 * the correct WDDM2.0 ABI (verified against WDK d3dkmddi.h); it reuses the DDK's already-correct
 * DXGK_SETVIDPNSOURCEADDRESS_FLAGS + D3DDDI_MAX_BROADCAST_CONTEXT and defines the missing
 * DXGK_PRIMARYDATA. The pfn is cast to RXGK_PFN_SETVIDPNSOURCEADDRESS.
 */
typedef struct _DXGK_PRIMARYDATA
{
    HANDLE           hAllocation;
    WORD             SegmentId;
    PHYSICAL_ADDRESS SegmentAddress;
} DXGK_PRIMARYDATA;

typedef struct _RXGK_ARG_SETVIDPNSOURCEADDRESS
{
    D3DDDI_VIDEO_PRESENT_SOURCE_ID   VidPnSourceId;
    UINT                             PrimarySegment;
    PHYSICAL_ADDRESS                 PrimaryAddress;
    HANDLE                           hAllocation;
    UINT                             ContextCount;
    HANDLE                           Context[1 + D3DDDI_MAX_BROADCAST_CONTEXT];
    DXGK_SETVIDPNSOURCEADDRESS_FLAGS Flags;
    UINT                             Duration;                                   /* WDDM1.3+ */
    DXGK_PRIMARYDATA                 PrimaryData[D3DDDI_MAX_BROADCAST_CONTEXT];   /* WDDM2.0 */
    UINT                             DriverPrivateDataSize;                       /* WDDM2.0 */
    PVOID                            pDriverPrivateData;                          /* WDDM2.0 */
} RXGK_ARG_SETVIDPNSOURCEADDRESS;

typedef NTSTATUS (NTAPI *RXGK_PFN_SETVIDPNSOURCEADDRESS)(
    _In_ const PVOID MiniportDeviceContext,
    _In_ const RXGK_ARG_SETVIDPNSOURCEADDRESS *pSetVidPnSourceAddress);

/*
 * Hardware pointer (cursor). The ReactOS DDK's DXGKARG_SETPOINTERSHAPE / DXGKARG_SETPOINTERPOSITION
 * are MIS-LAID-OUT vs the WDDM ABI (shape: missing Flags, reordered fields, HotSpot POINT instead
 * of XHot/YHot UINTs; position: Flags/Position instead of X/Y/Flags) - a miniport would read garbage.
 * RXGK_ARG_SETPOINTER* are the correct WDDM2.0 ABI (WDK-verified); the pfns are cast.
 */
typedef struct _DXGK_POINTERFLAGS
{
    union
    {
        struct
        {
            UINT Monochrome  : 1;   /* 0x00000001 */
            UINT Color       : 1;   /* 0x00000002 */
            UINT MaskedColor : 1;   /* 0x00000004 */
            UINT Reserved    : 29;
        };
        UINT Value;
    };
} DXGK_POINTERFLAGS;

typedef struct _DXGK_SETPOINTERPOSITIONFLAGS
{
    union
    {
        struct
        {
            UINT Visible    : 1;    /* 0x00000001 */
            UINT Procedural : 1;    /* 0x00000002 */
            UINT Reserved   : 30;
        };
        UINT Value;
    };
} DXGK_SETPOINTERPOSITIONFLAGS;

typedef struct _RXGK_ARG_SETPOINTERSHAPE
{
    DXGK_POINTERFLAGS              Flags;
    UINT                          Width;
    UINT                          Height;
    UINT                          Pitch;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId;
    const VOID                   *pPixels;
    UINT                          XHot;
    UINT                          YHot;
} RXGK_ARG_SETPOINTERSHAPE;

typedef struct _RXGK_ARG_SETPOINTERPOSITION
{
    D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId;
    INT                            X;
    INT                            Y;
    DXGK_SETPOINTERPOSITIONFLAGS   Flags;
} RXGK_ARG_SETPOINTERPOSITION;

typedef NTSTATUS (NTAPI *RXGK_PFN_SETPOINTERSHAPE)(
    _In_ const PVOID MiniportDeviceContext, _In_ const RXGK_ARG_SETPOINTERSHAPE *pSetPointerShape);
typedef NTSTATUS (NTAPI *RXGK_PFN_SETPOINTERPOSITION)(
    _In_ const PVOID MiniportDeviceContext, _In_ const RXGK_ARG_SETPOINTERPOSITION *pSetPointerPosition);

/*
 * Present (the blt/flip DMA pipeline). The ReactOS DDK only forward-declares DXGKARG_PRESENT
 * (incomplete) - we COMPLETE that same struct tag here so the DDK's real callable PDXGKDDI_PRESENT
 * (DXGKDDI_PRESENT(const HANDLE hContext, DXGKARG_PRESENT*)) can be invoked. DXGK_PRESENTFLAGS,
 * DXGK_ALLOCATIONLIST and DXGK_PRESENTALLOCATIONINFO are absent from the DDK too. WDDM2.0 layout,
 * verified against WDK d3dkmddi.h. D3DDDI_PATCHLOCATIONLIST + D3DDDI_FLIPINTERVAL_TYPE come from
 * the DDK (d3dukmdt.h). The MPO union arm is PVOID (we do not extract the overlay info struct).
 */
typedef struct _DXGK_PRESENTFLAGS
{
    union
    {
        struct
        {
            UINT Blt                     : 1;   /* 0x00000001 */
            UINT ColorFill               : 1;   /* 0x00000002 */
            UINT Flip                    : 1;   /* 0x00000004 */
            UINT FlipWithNoWait          : 1;   /* 0x00000008 */
            UINT SrcColorKey             : 1;   /* 0x00000010 */
            UINT DstColorKey             : 1;   /* 0x00000020 */
            UINT LinearToSrgb            : 1;   /* 0x00000040 */
            UINT Rotate                  : 1;   /* 0x00000080 */
            UINT FlipStereo              : 1;   /* 0x00000100 (WIN8) */
            UINT FlipStereoTemporaryMono : 1;   /* 0x00000200 */
            UINT FlipStereoPreferRight   : 1;   /* 0x00000400 */
            UINT BltStereoUseRight       : 1;   /* 0x00000800 */
            UINT FlipWithMultiPlaneOverlay : 1; /* 0x00001000 */
            UINT RedirectedFlip          : 1;   /* 0x00002000 (WDDM2.0) */
            UINT Reserved                : 18;  /* 0xFFFFC000 */
        };
        UINT Value;
    };
} DXGK_PRESENTFLAGS;

typedef struct _DXGK_ALLOCATIONLIST
{
    HANDLE hDeviceSpecificAllocation;
    struct
    {
        UINT WriteOperation : 1;    /* 0x00000001 */
        UINT SegmentId      : 5;    /* 0x0000003E */
        UINT Reserved       : 26;   /* 0xFFFFFFC0 */
    };
    union                            /* WDDM2.0 */
    {
        PHYSICAL_ADDRESS       PhysicalAddress;
        D3DGPU_VIRTUAL_ADDRESS VirtualAddress;
    };
} DXGK_ALLOCATIONLIST;

typedef struct _DXGK_PRESENTALLOCATIONINFO
{
    HANDLE                 hDeviceSpecificAllocation;
    D3DGPU_VIRTUAL_ADDRESS AllocationVirtualAddress;
    PHYSICAL_ADDRESS       PhysicalAddress;
    WORD                   SegmentId;
    WORD                   PhysicalAdapterIndex;
} DXGK_PRESENTALLOCATIONINFO;

/* Completes the DDK's forward-declared DXGKARG_PRESENT (d3dkmddi.h ~1978). WDDM2.0 layout. */
struct _DXGKARG_PRESENT
{
    VOID                       *pDmaBuffer;
    UINT                        DmaSize;
    VOID                       *pDmaBufferPrivateData;
    UINT                        DmaBufferPrivateDataSize;
    union                                                    /* WIN7+ */
    {
        DXGK_ALLOCATIONLIST        *pAllocationList;
        DXGK_PRESENTALLOCATIONINFO *pAllocationInfo;
        PVOID                       pPresentMultiPlaneOverlayInfo;  /* WIN8 (opaque) */
    };
    D3DDDI_PATCHLOCATIONLIST   *pPatchLocationListOut;       /* not used */
    UINT                        PatchLocationListOutSize;    /* not used */
    UINT                        MultipassOffset;
    UINT                        Color;
    RECT                        DstRect;
    RECT                        SrcRect;
    UINT                        SubRectCnt;
    const RECT                 *pDstSubRects;
    D3DDDI_FLIPINTERVAL_TYPE    FlipInterval;
    DXGK_PRESENTFLAGS           Flags;
    UINT                        DmaBufferSegmentId;
    PHYSICAL_ADDRESS            DmaBufferPhysicalAddress;
    UINT                        Reserved;                    /* WDDM1.3+ */
    D3DGPU_VIRTUAL_ADDRESS      DmaBufferGpuVirtualAddress;  /* WDDM2.0 */
    UINT                        NumSrcAllocations;           /* WDDM2.0 */
    UINT                        NumDstAllocations;           /* WDDM2.0 */
    UINT                        PrivateDriverDataSize;       /* WDDM2.0 */
    PVOID                       pPrivateDriverData;          /* WDDM2.0 */
};

/*
 * Render (the legacy command-buffer render pipeline). Like present, the DDK only forward-declares
 * DXGKARG_RENDER (d3dkmddi.h ~1979) + has a real callable PDXGKDDI_RENDER - we COMPLETE the same
 * struct tag here. The miniport reads the UMD's command buffer (pCommand) and patches it into the
 * hardware DMA buffer (pDmaBuffer) using the resolved allocation list + the UMD's patch list.
 * WDDM2.0 layout, verified against WDK d3dkmddi.h.
 */
struct _DXGKARG_RENDER
{
    const VOID               *pCommand;                  /* in: UMD command buffer (+ CommandOffset) */
    UINT                      CommandLength;
    VOID                     *pDmaBuffer;                /* out: hardware DMA buffer the miniport writes */
    UINT                      DmaSize;
    VOID                     *pDmaBufferPrivateData;
    UINT                      DmaBufferPrivateDataSize;
    DXGK_ALLOCATIONLIST      *pAllocationList;           /* in: resolved allocations the commands touch */
    UINT                      AllocationListSize;
    D3DDDI_PATCHLOCATIONLIST *pPatchLocationListIn;      /* in: UMD patch list */
    UINT                      PatchLocationListInSize;
    D3DDDI_PATCHLOCATIONLIST *pPatchLocationListOut;     /* out: DMA-buffer patch-list scratch */
    UINT                      PatchLocationListOutSize;
    UINT                      MultipassOffset;
    UINT                      DmaBufferSegmentId;
    PHYSICAL_ADDRESS          DmaBufferPhysicalAddress;
};

/* Callable function-pointer types (the DDK's PDXGKDDI_* are UINT32* stubs - cast to these). */
typedef NTSTATUS (NTAPI *RXGK_PFN_SUBMITCOMMAND)(_In_ const PVOID MiniportDeviceContext,
                                                 _In_ const DXGKARG_SUBMITCOMMAND *pSubmitCommand);
typedef NTSTATUS (NTAPI *RXGK_PFN_QUERYADAPTERINFO)(_In_ const PVOID MiniportDeviceContext,
                                                    _In_ const DXGKARG_QUERYADAPTERINFO *pQuery);
typedef NTSTATUS (NTAPI *RXGK_PFN_CREATECONTEXT)(_In_ const PVOID MiniportDeviceContext,
                                                 _Inout_ DXGKARG_CREATECONTEXT *pCreateContext);
typedef NTSTATUS (NTAPI *RXGK_PFN_DESTROYCONTEXT)(_In_ const HANDLE hContext);

#ifdef __cplusplus
}
#endif
