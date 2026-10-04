#pragma once

/* Convert WIN32 ROP into an ENG ROP */
#define WIN32_ROP3_TO_ENG_ROP4(dwRop4) ((((dwRop4) & 0x00FF0000) >> 16) | (((dwRop4) & 0x00FF0000) >> 8))
#define WIN32_ROP4_TO_ENG_ROP4(dwRop4) ((dwRop4) >> 16)

#define WIN32_ROP4_USES_SOURCE(Rop)  ((((Rop) & 0xCCCC0000) >> 2) != ((Rop) & 0x33330000))

/* The range of valid ROP2 values is 1 .. 16 */
#define FIXUP_ROP2(rop2) ((((rop2) - 1) & 0xF) + 1)

/* Brush functions */

extern HDC hSystemBM;
extern HSEMAPHORE hsemDriverMgmt;

/* Line functions */

BOOL FASTCALL
IntGdiLineTo(DC  *dc,
             int XEnd,
             int YEnd);

BOOL FASTCALL
IntGdiMoveToEx(DC      *dc,
               int     X,
               int     Y,
               LPPOINT Point);

BOOL FASTCALL
IntGdiPolyBezier(DC      *dc,
                 LPPOINT pt,
                 DWORD   Count);

BOOL FASTCALL
IntGdiPolyline(DC      *dc,
               LPPOINT pt,
               int     Count);

BOOL FASTCALL
IntGdiPolyBezierTo(DC      *dc,
                   LPPOINT pt,
                   DWORD   Count);

BOOL FASTCALL
IntGdiPolyPolyline(DC      *dc,
                   LPPOINT pt,
                   PULONG PolyPoints,
                   DWORD   Count);

BOOL FASTCALL
IntGdiPolylineTo(DC      *dc,
                 LPPOINT pt,
                 DWORD   Count);

BOOL FASTCALL
GreMoveTo( HDC hdc,
           INT x,
           INT y,
           LPPOINT pptOut);

/* Shape functions */

BOOL
NTAPI
GreGradientFill(
    HDC hdc,
    PTRIVERTEX pVertex,
    ULONG nVertex,
    PVOID pMesh,
    ULONG nMesh,
    ULONG ulMode);

/* DC functions */

HDC FASTCALL
IntGdiCreateDC(PUNICODE_STRING Driver,
               PUNICODE_STRING Device,
               PVOID pUMdhpdev,
               CONST PDEVMODEW InitData,
               BOOL CreateAsIC);

/* Stock objects */

VOID FASTCALL
IntSetSysColors(UINT nColors, CONST INT *Elements, CONST COLORREF *Colors);

HGDIOBJ FASTCALL
IntGetSysColorBrush(INT Object);

DWORD FASTCALL
IntGetSysColor(INT nIndex);

/* Other Stuff */

NTSTATUS
APIENTRY
NtGdiFlushUserBatch(
    VOID);

DWORD
APIENTRY
NtDxEngGetRedirectionBitmap(
    DWORD Unknown0);

/* D3DKMT calls newer than the psdk ntgdi.h, forwarded untyped to dxgkrnl */

NTSTATUS
APIENTRY
NtGdiDdDDICheckSharedResourceAccess(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIAdjustFullscreenGamma(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDICheckMultiPlaneOverlaySupport3(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIPresentMultiPlaneOverlay3(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIQueryVidPnExclusiveOwnership(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDISetHwProtectionTeardownRecovery(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDISetVidPnSourceHwProtection(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIEnumAdapters2(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIOpenAdapterFromLuid(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDISetVidPnSourceOwner1(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDISubmitPresentToHwQueue(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIAcquireKeyedMutex(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIAcquireKeyedMutex2(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIChangeVideoMemoryReservation(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDICheckMultiPlaneOverlaySupport2(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIConfigureSharedResource(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDICreateContextVirtual(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDICreateKeyedMutex(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDICreateKeyedMutex2(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDICreatePagingQueue(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyAllocation2(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyKeyedMutex(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIDestroyPagingQueue(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIEvict(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIFreeGpuVirtualAddress(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIGetOverlayState(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIInvalidateCache(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDILock2(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIMakeResident(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIMapGpuVirtualAddress(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIOpenKeyedMutex(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIOpenKeyedMutex2(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIOpenNtHandleFromName(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIOpenResourceFromNtHandle(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIOpenSyncObjectFromNtHandle(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIOpenSyncObjectFromNtHandle2(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIOpenSyncObjectNtHandleFromName(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIOpenSynchronizationObject(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIPresentMultiPlaneOverlay2(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIQueryResourceInfoFromNtHandle(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIReclaimAllocations2(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIReleaseKeyedMutex(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIReleaseKeyedMutex2(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIReserveGpuVirtualAddress(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDISetContextInProcessSchedulingPriority(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDISignalSynchronizationObjectFromCpu(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDISignalSynchronizationObjectFromGpu(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDISignalSynchronizationObjectFromGpu2(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDISubmitCommand(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIUnlock2(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIUpdateGpuVirtualAddress(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIWaitForSynchronizationObjectFromCpu(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIWaitForSynchronizationObjectFromGpu(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIShareObjects(
    _In_ ULONG ObjectCount,
    _In_ CONST D3DKMT_HANDLE *Objects,
    _In_ POBJECT_ATTRIBUTES ObjectAttributes,
    _In_ ULONG DesiredAccess,
    _Out_ HANDLE *SharedHandle);

NTSTATUS
APIENTRY
NtGdiDdDDIQueryClockCalibration(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIMarkDeviceAsError(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDICheckVidPnExclusiveOwnership(
    _Inout_ PVOID unnamedParam1);

NTSTATUS
APIENTRY
NtGdiDdDDIQueryVideoMemoryInfo(
    _Inout_ PVOID unnamedParam1);

/* Composition surface stubs, see ntuser/ntstubs.c */

NTSTATUS
APIENTRY
NtBindCompositionSurface(
    _In_opt_ PVOID Param1,
    _In_ INT Param2,
    _In_ INT Param3,
    _In_opt_ PVOID Param4,
    _In_ ULONGLONG Param5);

NTSTATUS
APIENTRY
NtUnBindCompositionSurface(
    _In_opt_ PVOID Param1,
    _In_ INT Param2,
    _In_ ULONGLONG Param3);

NTSTATUS
APIENTRY
NtQueryCompositionSurfaceStatistics(
    _In_opt_ PVOID Param1,
    _In_ ULONGLONG Param2);

NTSTATUS
APIENTRY
NtSetCompositionSurfaceHDRMetaData(
    _In_opt_ PVOID Param1,
    _In_opt_ PVOID Param2,
    _In_ ULONGLONG Param3,
    _In_ ULONGLONG Param4);

HBITMAP
FASTCALL
IntCreateCompatibleBitmap(
    _In_ PDC Dc,
    _In_ INT Width,
    _In_ INT Height,
    _In_ UINT Bpp,
    _In_ UINT Planes);

WORD APIENTRY IntGdiSetHookFlags(HDC hDC, WORD Flags);

UINT APIENTRY IntSetDIBColorTable(HDC hDC, UINT StartIndex, UINT Entries, CONST RGBQUAD *Colors);

UINT APIENTRY IntGetDIBColorTable(HDC hDC, UINT StartIndex, UINT Entries, RGBQUAD *Colors);

UINT APIENTRY
IntGetPaletteEntries(HPALETTE  hpal,
                     UINT  StartIndex,
                     UINT  Entries,
                     LPPALETTEENTRY  pe);

UINT APIENTRY
IntGetSystemPaletteEntries(HDC  hDC,
                           UINT  StartIndex,
                           UINT  Entries,
                           LPPALETTEENTRY  pe);

VOID  FASTCALL CreateStockObjects (VOID);
VOID  FASTCALL CreateSysColorObjects (VOID);

PPOINT GDI_Bezier (const POINT *Points, INT count, PINT nPtsOut);

BOOL FASTCALL IntFillArc( PDC dc, INT XLeft, INT YLeft, INT Width, INT Height, double StartArc, double EndArc, ARCTYPE arctype);
BOOL FASTCALL IntDrawArc( PDC dc, INT XLeft, INT YLeft, INT Width, INT Height, double StartArc, double EndArc, ARCTYPE arctype, PBRUSH pbrush);

BOOL FASTCALL IntFillEllipse( PDC dc, INT XLeft, INT YLeft, INT Width, INT Height, PBRUSH pbrush);
BOOL FASTCALL IntDrawEllipse( PDC dc, INT XLeft, INT YLeft, INT Width, INT Height, PBRUSH pbrush);
BOOL FASTCALL IntFillRoundRect( PDC dc, INT Left, INT Top, INT Right, INT Bottom, INT Wellipse, INT Hellipse, PBRUSH pbrush);
BOOL FASTCALL IntDrawRoundRect( PDC dc, INT Left, INT Top, INT Right, INT Bottom, INT Wellipse, INT Hellipse, PBRUSH pbrush);
