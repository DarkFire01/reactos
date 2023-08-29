#pragma once

#include <windef.h>
#include <d3dukmdt.h>
#include <d3dkmdt.h>

typedef struct _DXGK_SEGMENTBANKPREFERENCE
{
    union
    {
        struct
        {
            UINT Bank0          : 7;
            UINT Direction0     : 1;
            UINT Bank1          : 7;
            UINT Direction1     : 1;
            UINT Bank2          : 7;
            UINT Direction2     : 1;
            UINT Bank3          : 7;
            UINT Direction3     : 1;
        };
        UINT Value;
    };
} DXGK_SEGMENTBANKPREFERENCE;

typedef struct _DXGK_ALLOCATIONINFOFLAGS
{
    union
    {
        struct
        {
            UINT    CpuVisible              : 1;
            UINT    PermanentSysMem         : 1;
            UINT    Cached                  : 1;
            UINT    Protected               : 1;
            UINT    ExistingSysMem          : 1;
            UINT    ExistingKernelSysMem    : 1;
            UINT    FromEndOfSegment        : 1;
            UINT    Swizzled                : 1;
            UINT    Overlay                 : 1;
            UINT    Capture                 : 1;
            UINT    UseAlternateVA          : 1;
            UINT    SynchronousPaging       : 1;
            UINT    LinkMirrored            : 1;
            UINT    LinkInstanced           : 1;
            UINT    HistoryBuffer           : 1;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
            UINT    AccessedPhysically      : 1;
            UINT    ExplicitResidencyNotification : 1;
            UINT    HardwareProtected       : 1;
            UINT    CpuVisibleOnDemand      : 1;
#else
            UINT    Reserved                : 4;
#endif
            UINT    DXGK_ALLOC_RESERVED16   : 1;
            UINT    DXGK_ALLOC_RESERVED15   : 1;
            UINT    DXGK_ALLOC_RESERVED14   : 1;
            UINT    DXGK_ALLOC_RESERVED13   : 1;
            UINT    DXGK_ALLOC_RESERVED12   : 1;
            UINT    DXGK_ALLOC_RESERVED11   : 1;
            UINT    DXGK_ALLOC_RESERVED10   : 1;
            UINT    DXGK_ALLOC_RESERVED9    : 1;
            UINT    DXGK_ALLOC_RESERVED4    : 1;
            UINT    DXGK_ALLOC_RESERVED3    : 1;
            UINT    DXGK_ALLOC_RESERVED2    : 1;
            UINT    DXGK_ALLOC_RESERVED1    : 1;
            UINT    DXGK_ALLOC_RESERVED0    : 1;
        };
        UINT Value;
    };
} DXGK_ALLOCATIONINFOFLAGS;

typedef struct _DXGK_ALLOCATIONUSAGEINFO1
{
    union
    {
        struct
        {
            UINT        PrivateFormat  : 1;
            UINT        Swizzled       : 1;
            UINT        MipMap         : 1;
            UINT        Cube           : 1;
            UINT        Volume         : 1;
            UINT        Vertex         : 1;
            UINT        Index          : 1;
            UINT        Reserved       : 25;
        };
        UINT            Value;
    } Flags;

    union
    {
        D3DDDIFORMAT    Format;
        UINT            PrivateFormat;
    };

    UINT                SwizzledFormat;
    UINT                ByteOffset;
    UINT                Width;
    UINT                Height;
    UINT                Pitch;
    UINT                Depth;
    UINT                SlicePitch;
} DXGK_ALLOCATIONUSAGEINFO1;

typedef struct _DXGK_ALLOCATIONUSAGEHINT
{
    UINT                            Version;
    DXGK_ALLOCATIONUSAGEINFO1       v1;
} DXGK_ALLOCATIONUSAGEHINT;

typedef struct _DXGK_ALLOCATIONINFO
{
    VOID*                             pPrivateDriverData;
    UINT                              PrivateDriverDataSize;
    UINT                              Alignment;
    SIZE_T                            Size;
    SIZE_T                            PitchAlignedSize;
    DXGK_SEGMENTBANKPREFERENCE        HintedBank;
    DXGK_SEGMENTPREFERENCE            PreferredSegment;
    UINT                              SupportedReadSegmentSet;
    UINT                              SupportedWriteSegmentSet;
    UINT                              EvictionSegmentSet;
    union
    {
        UINT                          MaximumRenamingListLength;
        UINT                          PhysicalAdapterIndex;
    };
    HANDLE                            hAllocation;
    union
    {
        DXGK_ALLOCATIONINFOFLAGS            Flags;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM2_0)
        DXGK_ALLOCATIONINFOFLAGS_WDDM2_0    FlagsWddm2;
#endif
    };
    DXGK_ALLOCATIONUSAGEHINT*         pAllocationUsageHint;
    UINT                              AllocationPriority;
#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WDDM3_0)
    DXGK_ALLOCATIONINFOFLAGS2         Flags2;
#endif
} DXGK_ALLOCATIONINFO;

typedef enum _DXGK_PRESENT_DISPLAY_ONLY_PROGRESS_ID
{
    DXGK_PRESENT_DISPLAYONLY_PROGRESS_ID_COMPLETE  = 0,
    DXGK_PRESENT_DISPLAYONLY_PROGRESS_ID_FAILED    = 1,
} DXGK_PRESENT_DISPLAY_ONLY_PROGRESS_ID;

typedef struct _DXGKARGCB_PRESENT_DISPLAYONLY_PROGRESS
{
    D3DDDI_VIDEO_PRESENT_SOURCE_ID        VidPnSourceId;
    DXGK_PRESENT_DISPLAY_ONLY_PROGRESS_ID ProgressId;
} DXGKARGCB_PRESENT_DISPLAYONLY_PROGRESS, *PDXGKARGCB_PRESENT_DISPLAYONLY_PROGRESS;

typedef struct _D3DKMT_PRESENT_DISPLAY_ONLY_FLAGS
{
    union
    {
        struct
        {
            UINT    Rotate                      :  1;       // 0x00000001
            UINT    Reserved                    : 31;       // 0xFFFFFFFE
        };
        UINT    Value;
    };
} D3DKMT_PRESENT_DISPLAY_ONLY_FLAGS;

typedef
VOID (APIENTRY CALLBACK *DXGKCB_PRESENT_DISPLAYONLY_PROGRESS)(
    _In_ HANDLE hAdapter, _In_ PDXGKARGCB_PRESENT_DISPLAYONLY_PROGRESS
    );

typedef struct _DXGKARG_PRESENT_DISPLAYONLY
{
    D3DDDI_VIDEO_PRESENT_SOURCE_ID      VidPnSourceId;                  // in:  VidPn where the image is presented
    VOID*                               pSource;                        // in:  Start address of source image
    ULONG                               BytesPerPixel;                  // in:  Bytes per pixel in source image
    LONG                                Pitch;                          // in:  Bytes per line in source image
    D3DKMT_PRESENT_DISPLAY_ONLY_FLAGS   Flags;                          // in:  Flags of current present,
    ULONG                               NumMoves;                       // in:  Number of screen to screen moves
    _Field_size_(NumMoves)
    D3DKMT_MOVE_RECT*                   pMoves;                         // in:  Point to the list of moves
    ULONG                               NumDirtyRects;                  // in:  Number of direct rects
    _Field_size_(NumDirtyRects)
    RECT*                               pDirtyRect;                     // in:  Point to the list of dirty rects
    DXGKCB_PRESENT_DISPLAYONLY_PROGRESS pfnPresentDisplayOnlyProgress;  // in:  Point to present progress callback
} DXGKARG_PRESENT_DISPLAYONLY, *PDXGKARG_PRESENT_DISPLAYONLY;


typedef
NTSTATUS
APIENTRY
DXGKDDI_PRESENTDISPLAYONLY(_In_ HANDLE                        hAdapter,
                           _In_ PDXGKARG_PRESENT_DISPLAYONLY  pPresentDisplayOnly);

#if (DXGKDDI_INTERFACE_VERSION >= DXGKDDI_INTERFACE_VERSION_WIN8)
typedef DXGKDDI_PRESENTDISPLAYONLY              *PDXGKDDI_PRESENTDISPLAYONLY;
#endif