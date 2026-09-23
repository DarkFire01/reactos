/*
 * PROJECT:     ReactOS Display Driver Model (DxgKrnl_ms)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DXGKCDD_INTERFACE - the dxgkrnl <-> CDD (Canonical Display Driver) interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* CreateAllocation flags (Reference _DXGKCDD_CREATE_ALLOCATION_FLAGS :53318). */
typedef struct _DXGKCDD_CREATE_ALLOCATION_FLAGS
{
    union
    {
        struct
        {
            UINT Primary  : 1;
            UINT Reserved : 31;
        };
        UINT Value;
    };
} DXGKCDD_CREATE_ALLOCATION_FLAGS;

/* The CDD-side callback table dxgkrnl is handed at Enable (Reference _CDDDXGK_INTERFACE :26113). */
typedef struct _CDDDXGK_INTERFACE
{
    VOID *pCddPdev;
    VOID (NTAPI *pfnCddDxgkAddD3DDirtyRgn)(VOID *const, const RECT *const);
    VOID (NTAPI *pfnCddDxgkNotifyExclusiveGDIOwner)(VOID *const);
    VOID (NTAPI *pfnCddDxgkUpdatePresentRects)(VOID *const, const RECT **, UINT *);
    VOID (NTAPI *pfnCddDxgkUpdateBitmapPresentRects)(const VOID *, const RECT **, UINT *);
    UCHAR ProcessName[16];
} CDDDXGK_INTERFACE;

/* The GDI sysmem-allocator callback the CDD hands to CreateAllocation (Reference :53511). */
typedef VOID *(NTAPI *PFN_CDD_ALLOCATE_SYSMEM)(const VOID *, UINT);

/* DxgkCddQueryInterface accepts only this version (Reference :179867, "Size == 248 && Version == 5") */
#define DXGKCDD_INTERFACE_VERSION 5

/* Reference enum _DXGCDD_PRESENT_ON_SCREEN_TYPE :6313. */
typedef enum _DXGCDD_PRESENT_ON_SCREEN_TYPE
{
    DXGCDD_PRESENT_ON_SCREEN_TYPE_COLOR_FILL       = 0,
    DXGCDD_PRESENT_ON_SCREEN_TYPE_COPY_TO_SCREEN   = 1,
    DXGCDD_PRESENT_ON_SCREEN_TYPE_COPY_FROM_SCREEN = 2,
    DXGCDD_PRESENT_ON_SCREEN_TYPE_SCREEN_TO_SCREEN = 3,
} DXGCDD_PRESENT_ON_SCREEN_TYPE;

/*
 * What the CDD hands pfnDxgkCddPresentOnScreen: the shadow it drew into plus the damaged
 * rectangles. This is the desktop present path (Reference struct _DXGKCDD_PRESENT_ON_SCREEN
 * :53121, filled by CddPresentBlt).
 */
typedef struct _DXGKCDD_PRESENT_ON_SCREEN
{
    DXGCDD_PRESENT_ON_SCREEN_TYPE PresentType;
    VOID  *Adapter;
    UINT   VidPnSourceId;
    UINT   Color;
    VOID  *pShadow;
    UINT   ShadowWidth;
    UINT   ShadowHeight;
    UINT   ShadowStride;
    RECT   ScreenCopyRects[2];
    UINT   NumberRects;
    RECT  *pSubRects;
} DXGKCDD_PRESENT_ON_SCREEN, *PDXGKCDD_PRESENT_ON_SCREEN;

/*
 * A stock CDD fills this, so every offset has to land where dxgkrnl reads it. ScreenCopyRects is
 * the one to watch: it starts at an offset that is 4-aligned but not 8-aligned, and the fields
 * after it only stay put because RECT needs no more than 4-byte alignment.
 */
#ifndef _WIN64
C_ASSERT(sizeof(DXGKCDD_PRESENT_ON_SCREEN) == 72);
C_ASSERT(FIELD_OFFSET(DXGKCDD_PRESENT_ON_SCREEN, Adapter)         == 4);
C_ASSERT(FIELD_OFFSET(DXGKCDD_PRESENT_ON_SCREEN, pShadow)         == 16);
C_ASSERT(FIELD_OFFSET(DXGKCDD_PRESENT_ON_SCREEN, ScreenCopyRects) == 32);
C_ASSERT(FIELD_OFFSET(DXGKCDD_PRESENT_ON_SCREEN, NumberRects)     == 64);
C_ASSERT(FIELD_OFFSET(DXGKCDD_PRESENT_ON_SCREEN, pSubRects)       == 68);
#else
C_ASSERT(sizeof(DXGKCDD_PRESENT_ON_SCREEN) == 88);
C_ASSERT(FIELD_OFFSET(DXGKCDD_PRESENT_ON_SCREEN, Adapter)         == 8);
C_ASSERT(FIELD_OFFSET(DXGKCDD_PRESENT_ON_SCREEN, pShadow)         == 24);
C_ASSERT(FIELD_OFFSET(DXGKCDD_PRESENT_ON_SCREEN, ScreenCopyRects) == 44);
C_ASSERT(FIELD_OFFSET(DXGKCDD_PRESENT_ON_SCREEN, NumberRects)     == 76);
C_ASSERT(FIELD_OFFSET(DXGKCDD_PRESENT_ON_SCREEN, pSubRects)       == 80);
#endif

/*
 * The dxgkrnl->CDD interface, ABI-EXACT (Reference struct _DXGKCDD_INTERFACE :53486). Every slot
 * is in the reference order; unimplemented slots are PVOID (NULL). Implemented slots carry the
 * exact reference signature so the CDD calls them correctly. C_ASSERT pins the size to 0xF8.
 */
typedef struct _DXGKCDD_INTERFACE
{
    USHORT Size;
    USHORT Version;
    VOID  *Adapter;
    UINT   VidPnSourceId;
    VOID (NTAPI *InterfaceReference)(VOID *);
    VOID (NTAPI *InterfaceDereference)(VOID *);

    /* +0x18 */
    int  (NTAPI *pfnDxgkCddEtwLoggerEnabled)(VOID);
    /*
     * Create the CDD's D3D device + context (Reference cdd.c CreateAndEnableDevice:16887). The CDD
     * is a D3DKMT client: everything after this - Enable, CreateAllocation, Present - is scoped to
     * the returned handles, exactly as a user-mode D3D client's would be. hContext is what the CDD
     * later puts in D3DKMT_PRESENT::hDevice (Reference cdd.c:9150).
     */
    NTSTATUS (NTAPI *pfnDxgkCddCreate)(VOID *const Adapter, VOID *pDxgkW32kInterface,
                                       D3DKMT_HANDLE *phDevice, D3DKMT_HANDLE *phContext,
                                       VOID *pDriverInfo, VOID *pRenderAdapterDriverInfo,
                                       VOID **ppAdapterRender, VOID **ppSharedAllocObjectType);
    NTSTATUS (NTAPI *pfnDxgkCddDestroy)(D3DKMT_HANDLE hDevice, D3DKMT_HANDLE hContext,
                                        VOID *const Adapter, UCHAR DeviceRemoved);
    NTSTATUS (NTAPI *pfnDxgkCddEnable)(D3DKMT_HANDLE hDevice, UINT Win32kCommand,
                                       const CDDDXGK_INTERFACE *pCddInterface, UINT VidPnSourceId,
                                       const D3DKMT_DISPLAYMODE *pDisplayMode,
                                       VOID **ppShadow, UINT *pShadowSize, UINT *pShadowPitch);
    PVOID pfnDxgkCddEnableLite;
    NTSTATUS (NTAPI *pfnDxgkCddDisable)(D3DKMT_HANDLE hDevice, UINT Win32kCommand, UINT VidPnSourceId);
    NTSTATUS (NTAPI *pfnDxgkCddLock)(D3DKMT_LOCK *pLock, UINT VidPnSourceId);
    NTSTATUS (NTAPI *pfnDxgkCddUnlock)(D3DKMT_UNLOCK *pUnlock, UINT VidPnSourceId);
    NTSTATUS (NTAPI *pfnDxgkCddGetDisplayModeList)(VOID *const Adapter, D3DKMT_GETDISPLAYMODELIST *pModeList);
    NTSTATUS (NTAPI *pfnDxgkCddPresent)(D3DKMT_PRESENT *pPresent, UINT VidPnSourceId);
    int  (NTAPI *pfnDxgkCddSetPalette)(VOID *const, const VOID *);
    NTSTATUS (NTAPI *pfnDxgkCddSetPointerShape)(VOID *const Adapter, const VOID *pSetPos,
                                                const VOID *pSetShape, UINT ActualWidth,
                                                UINT ActualHeight, int bRemoteSession);
    NTSTATUS (NTAPI *pfnDxgkCddSetPointerPosition)(VOID *const Adapter, const VOID *pSetPos,
                                                   int bRemoteSession);
    PVOID pfnDxgkCddSetGammaRamp;
    VOID (NTAPI *pfnDxgkCddSetOrigin)(VOID *const Adapter, UINT VidPnSourceId, const POINT *const pOrigin);
    int  (NTAPI *pfnDxgkCddWaitForVerticalBlankEvent)(VOID *const, UINT, UINT, VOID *);
    VOID (NTAPI *pfnDxgkCddTerminateThread)(VOID *);
    NTSTATUS (NTAPI *pfnDxgkCddCreateAllocation)(D3DKMT_HANDLE hDevice, D3DDDIFORMAT Format,
                                                 DXGKCDD_CREATE_ALLOCATION_FLAGS Flags,
                                                 UINT Width, UINT Height,
                                                 D3DKMDT_GDISURFACETYPE AllocationType,
                                                 PFN_CDD_ALLOCATE_SYSMEM pfnAllocateSysMem,
                                                 VOID *pCddPrivateData, VOID *pPrivateRuntimeData,
                                                 UINT PrivateRuntimeDataSize,
                                                 D3DKMT_HANDLE *phAllocation, UINT *pGlobalHandle,
                                                 VOID **ppSection, UINT *pAllocationPitch);
    NTSTATUS (NTAPI *pfnDxgkCddDestroyAllocation)(D3DKMT_HANDLE hDevice, D3DKMT_HANDLE hAllocation);
    NTSTATUS (NTAPI *pfnDxgkCddSyncGPUAccess)(VOID *pA, VOID *pB, UINT VidPnSourceId, int Flag);
    PVOID pfnDxgkCddQueryResourceInfo;
    PVOID pfnDxgkCddQueryResourceInfoFromNtHandle;
    PVOID pfnDxgkCddOpenResource;
    PVOID pfnDxgkCddOpenResourceFromNtHandle;
    PVOID pfnDxgkCddLogEvent;
    VOID *(NTAPI *pfnDxgkCddGetCurrentDxgProcess)(VOID);
    PVOID pfnDxgkCddGdiCommand;
    PVOID pfnDxgkCddSubmitPresentHistory;
    VOID (NTAPI *pfnDxgkCddPushWorkerThreadOfOwner)(VOID *const, VOID *const, VOID *const);
    VOID (NTAPI *pfnDxgkCddPopWorkerThreadOfOwner)(VOID *const, VOID *const);
    int  (NTAPI *pfnDxgkCddGetDriverCaps)(VOID *const, VOID *, VOID *);
    int  (NTAPI *pfnDxgkCddVerifyCddDevMode)(const VOID *);
    int  (NTAPI *pfnDxgkCddWriteDiagEntry)(VOID *);
    VOID (NTAPI *pfnDxgkCddAdapterReference)(VOID *const, ULONGLONG *);
    VOID (NTAPI *pfnDxgkCddAdapterDereference)(VOID *const, ULONGLONG);
    PVOID pfnDxgkCddCreateSynchronizationObject;
    PVOID pfnDxgkCddDestroySynchronizationObject;
    PVOID pfnDxgkCddSignalSynchronizationObject;
    PVOID pfnDxgkCddWaitForSynchronizationObject;
    PVOID pfnDxgkCddOpenSynchronizationObject;
    int  (NTAPI *pfnDxgkCddNotifyGdiRendering)(VOID *, UINT);
    PVOID pfnDxgkCddIssueSyncObjectOpForDevice;
    /*
     * The desktop present. The CDD's worker thread calls this from CddPresentBlt once its damage
     * region holds at least one rectangle, and it is what reaches DxgkDdiPresentDisplayOnly.
     */
    NTSTATUS (NTAPI *pfnDxgkCddPresentOnScreen)(DXGKCDD_PRESENT_ON_SCREEN *pPresentOnScreen);
    PVOID pfnDxgkCddSubscribeWnfStateChange;
    PVOID pfnDxgkCddUnsubscribeWnfStateChange;
    PVOID pfnDxgkCddMakeResident;
    PVOID pfnDxgkCddEvict;
    PVOID pfnDxgkCddWaitForSynchronizationObjectFromCpu;
    PVOID pfnDxgkCddSignalSynchronizationObjectFromGpu;
    PVOID pfnDxgkCddCreatePagingQueue;
    PVOID pfnDxgkCddDestroyPagingQueue;
    PVOID pfnDxgkPresentVirtualFrameBuffer;
    PVOID pfnDxgkGetBootAnimationRelayState;
    PVOID pfnDxgkSetBootAnimationRelayState;
    PVOID pfnDxgkShutdownBootGraphics;
    PVOID pfnDxgkGetVirtualFrameBufferAccessCount;
    PVOID pfnDxgkIsPrimarySource;
} DXGKCDD_INTERFACE, *PDXGKCDD_INTERFACE;

/*
 * DxgkCddQueryInterface refuses anything but these, so the layout is pinned on both arches:
 * 6 header fields then 57 slots, which is 20 + 57*4 on x86 and 40 + 57*8 on x64.
 */
#ifndef _WIN64
C_ASSERT(sizeof(DXGKCDD_INTERFACE) == 0xF8);
#else
C_ASSERT(sizeof(DXGKCDD_INTERFACE) == 0x1F0);
#endif
C_ASSERT(FIELD_OFFSET(DXGKCDD_INTERFACE, pfnDxgkCddEtwLoggerEnabled) == 5 * sizeof(PVOID));
C_ASSERT(FIELD_OFFSET(DXGKCDD_INTERFACE, pfnDxgkIsPrimarySource) == 61 * sizeof(PVOID));

/**
 * @brief Fill a caller-supplied DXGKCDD_INTERFACE with dxgkrnl's CDD entry points (Reference :179858).
 */
NTSTATUS NTAPI DxgkCddQueryInterface(_Inout_ PDXGKCDD_INTERFACE pInterface, _Inout_ PUINT pSize);

#ifdef __cplusplus
}
#endif
