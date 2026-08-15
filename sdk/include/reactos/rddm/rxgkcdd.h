/*
 * PROJECT:     ReactOS Display Driver Model (DxgKrnl_ms)
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DXGKCDD_INTERFACE - the dxgkrnl <-> CDD (Canonical Display Driver) interface
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 *
 * The CDD (cdd.dll, loaded by win32k as the GDI display driver) opens \Device\DxgKrnl and sends
 * IOCTL_VIDEO_QUERY_CDD_INTERFACE (0x23E05B) with a DXGKCDD_INTERFACE buffer; dxgkrnl fills it.
 * The CDD then drives the desktop onto the screen: Enable a source, CreateAllocation (the GDI
 * primary), Lock/Unlock (CPU draw access), Present (scan out). Verified against the decompiled CDD
 * (Reference/win10/cdd.c OpenDxgkrnl:1443) and dxgkrnl (struct _DXGKCDD_INTERFACE :53486).
 *
 * THE LAYOUT IS ABI-EXACT: the CDD calls members BY OFFSET, so every member is present in the
 * reference order and the whole table is exactly 248 (0xF8) bytes on i386 - the size the CDD's
 * IOCTL passes. Members dxgkrnl does not implement yet are NULL pointers (the CDD null-checks most
 * before calling). Uses real D3DKMT/D3DKMDT types (this is a dxgkrnl-internal table).
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
    PVOID pfnDxgkCddCreate;
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
    PVOID pfnDxgkCddPresentOnScreen;
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

#ifndef _WIN64
C_ASSERT(sizeof(DXGKCDD_INTERFACE) == 0xF8);   /* the CDD's IOCTL passes exactly 0xF8 bytes */
#endif

/**
 * @brief Fill a caller-supplied DXGKCDD_INTERFACE with dxgkrnl's CDD entry points (Reference :179858).
 */
NTSTATUS NTAPI DxgkCddQueryInterface(_Inout_ PDXGKCDD_INTERFACE pInterface, _Inout_ PUINT pSize);

#ifdef __cplusplus
}
#endif
