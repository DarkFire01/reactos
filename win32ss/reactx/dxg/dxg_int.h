#ifndef _DXG_PCH_
#define _DXG_PCH_

#include <ntifs.h>

/* Win32 Headers */
#define WINBASEAPI

/* 
 * DXG handle layout (index + type + unique).
 *
 * The lower bits form an index into the DXG handle table. A small type
 * field and an 8‑bit uniqueness counter live in the upper bits.  The bit
 * distribution is chosen so we can support a large number of handles while
 * still being able to cheaply extract the index and the type from any
 * handle value.
 */

typedef UCHAR DXG_OBJECT_TYPE;

/* Object type identifiers for DXG handle manager */
#define DXG_OBJTYPE_NONE        0
#define DXG_OBJTYPE_DIRECTDRAW  1
#define DXG_OBJTYPE_SURFACE     2
#define DXG_OBJTYPE_D3D         3
#define DXG_OBJTYPE_VIDEOPORT   4
#define DXG_OBJTYPE_MOTIONCOMP  5

/* Handle structure: 32-bit value with bit fields
 * Layout: [unique(8) | type(3) | index(21)]
 * This union allows both direct access to fields and raw handle value */
typedef union _DXG_HANDLE
{
    ULONG Value;
    struct
    {
        ULONG Index : 21;      /* Table index (0-2097151) */
        ULONG Type  : 3;       /* Object type (0-7) */
        ULONG Unique: 8;       /* Uniqueness counter */
    } Fields;
} DXG_HANDLE, *PDXG_HANDLE;

#define DXG_HANDLE_INDEX_BITS   21
#define DXG_HANDLE_TYPE_BITS    3
#define DXG_HANDLE_UNIQUE_BITS  8
#define DXG_HANDLE_NONINDEX_BITS (32 - DXG_HANDLE_INDEX_BITS)
#define DXG_HANDLE_INDEX_BITPOS   0
#define DXG_HANDLE_TYPE_BITPOS    DXG_HANDLE_INDEX_BITS
#define DXG_HANDLE_UNIQUE_BITPOS  (DXG_HANDLE_TYPE_BITPOS + DXG_HANDLE_TYPE_BITS)
#define DXG_HANDLE_CREATE_MASK(bitpos, width)  (((1UL << (width)) - 1) << (bitpos))
#define DXG_HANDLE_INDEX_MASK      DXG_HANDLE_CREATE_MASK(DXG_HANDLE_INDEX_BITPOS, DXG_HANDLE_INDEX_BITS)
#define DXG_HANDLE_TYPE_MASK       DXG_HANDLE_CREATE_MASK(DXG_HANDLE_TYPE_BITPOS, DXG_HANDLE_TYPE_BITS)
#define DXG_HANDLE_UNIQUE_MASK     DXG_HANDLE_CREATE_MASK(DXG_HANDLE_UNIQUE_BITPOS, DXG_HANDLE_UNIQUE_BITS)
#define DXG_HANDLE_FULLUNIQUE_MASK (DXG_HANDLE_UNIQUE_MASK | DXG_HANDLE_TYPE_MASK)
#define DXG_HANDLE_INDEX_SHIFT    DXG_HANDLE_INDEX_BITPOS
#define DXG_HANDLE_TYPE_SHIFT     DXG_HANDLE_TYPE_BITPOS
#define DXG_HANDLE_UNIQUE_SHIFT   DXG_HANDLE_UNIQUE_BITPOS
#define DXG_HANDLE_GET_INDEX(h)   (((DXG_HANDLE){.Value = (ULONG)(ULONG_PTR)(h)}).Fields.Index)
#define DXG_HANDLE_GET_UNIQUE(h)  (((DXG_HANDLE){.Value = (ULONG)(ULONG_PTR)(h)}).Fields.Unique)
#define DXG_HANDLE_GET_TYPE(h)    ((DXG_OBJECT_TYPE)(((DXG_HANDLE){.Value = (ULONG)(ULONG_PTR)(h)}).Fields.Type))
#define DXG_HANDLE_MAKE(index, type, unique) \
    ((HANDLE)(ULONG)((DXG_HANDLE){.Fields = {.Index = (index), .Type = (type), .Unique = (unique)}}.Value))
#define DXG_HANDLE_TABLE_GROWTH   ((PAGE_SIZE * 4) / sizeof(DXG_HANDLE_ENTRY))
#define DXG_HANDLE_MAX_COUNT     (1 << (32 - DXG_HANDLE_NONINDEX_BITS))
#define DXG_HANDLE_BASE          1
#define DXG_HANDLE_LIMIT          (1u << DXG_HANDLE_INDEX_BITS)

#include <windef.h>
#include <winerror.h>
#include <wingdi.h>
#include <winddi.h>
#include <ddkernel.h>
#include <initguid.h>
#include <ddrawi.h>
#include <ntgdityp.h>
#include <psfuncs.h>

DEFINE_GUID(GUID_NTCallbacks,             0x6fe9ecde, 0xdf89, 0x11d1, 0x9d, 0xb0, 0x00, 0x60, 0x08, 0x27, 0x71, 0xba);
DEFINE_GUID(GUID_DDMoreCaps,              0x880baf30, 0xb030, 0x11d0, 0x8e, 0xa7, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b);
DEFINE_GUID(GUID_NTPrivateDriverCaps,     0xfad16a23, 0x7b66, 0x11d2, 0x83, 0xd7, 0x00, 0xc0, 0x4f, 0x7c, 0xe5, 0x8c);

/* DXG treats this as opaque */
typedef PVOID PDC;
typedef PVOID PW32THREAD;

typedef struct _DD_BASEOBJECT
{
  HGDIOBJ     hHmgr;
  ULONG       ulShareCount;
  USHORT      cExclusiveLock;
  USHORT      BaseFlags;
  PW32THREAD  Tid;
} DD_BASEOBJECT, *PDD_BASEOBJECT;

#include <drivers/directx/directxint.h>
#include <drivers/directx/dxg.h>
#include <drivers/directx/dxeng.h>

/* Forward declarations for GDI types needed for palette support */
typedef struct _SURFACE *PSURFACE;
typedef struct _PALETTE *PPALETTE;
typedef struct _BASEOBJECT BASEOBJECT;
typedef struct _BASEOBJECT *POBJ;

/* GDI object type constants */
#define GDI_OBJECT_TYPE_PALETTE  0x06
#define GDI_OBJECT_TYPE_BITMAP   0x07

/* Forward declare GDI functions we need */
PVOID NTAPI GDIOBJ_ShareLockObj(HGDIOBJ hobj, ULONG ulType);
VOID FASTCALL GDIOBJ_vDereferenceObject(POBJ pobj);
VOID FASTCALL GDIOBJ_vReferenceObjectByPointer(POBJ pobj);

#include "tags.h"

#define CapOver_DisableAccel      0x1
#define CapOver_DisableD3DDDAccel 0x2
#define CapOver_DisableD3DAccel   0x4
#define CapOver_DisableOGL        0x8
#define CapOver_DisableEscapes    0x10

/* Legacy object type defines - use DXG_OBJTYPE_* instead */
#define ObjType_DDLOCAL_TYPE      DXG_OBJTYPE_DIRECTDRAW
#define ObjType_DDSURFACE_TYPE    DXG_OBJTYPE_SURFACE
#define ObjType_DDCONTEXT_TYPE    DXG_OBJTYPE_D3D
#define ObjType_DDVIDEOPORT_TYPE  DXG_OBJTYPE_VIDEOPORT
#define ObjType_DDMOTIONCOMP_TYPE DXG_OBJTYPE_MOTIONCOMP

typedef struct _DXG_HANDLE_ENTRY
{
    union
    {
        PDD_BASEOBJECT pobj;
        ULONG NextFree;
    };
    HANDLE Pid;
    USHORT FullUnique;
    UCHAR Objt;
} DXG_HANDLE_ENTRY, *PDXG_HANDLE_ENTRY;

typedef struct _EDD_SURFACE_LOCAL
{
     DD_BASEOBJECT Object;
     DD_SURFACE_LOCAL Surfacelcl;
} EDD_SURFACE_LOCAL, *PEDD_SURFACE_LOCAL;


typedef BOOLEAN   (APIENTRY* PFN_DxEngNUIsTermSrv)(VOID);
typedef DWORD     (APIENTRY* PFN_DxEngScreenAccessCheck)(VOID);
typedef BOOLEAN   (APIENTRY* PFN_DxEngRedrawDesktop)(VOID);
typedef ULONG     (APIENTRY* PFN_DxEngDispUniq)(VOID);
typedef BOOLEAN   (APIENTRY* PFN_DxEngIncDispUniq)(VOID);
typedef ULONG     (APIENTRY* PFN_DxEngVisRgnUniq)(VOID);
typedef BOOLEAN   (APIENTRY* PFN_DxEngLockShareSem)(VOID);
typedef BOOLEAN   (APIENTRY* PFN_DxEngUnlockShareSem)(VOID);
typedef HDEV*     (APIENTRY* PFN_DxEngEnumerateHdev)(HDEV*);
typedef BOOLEAN   (APIENTRY* PFN_DxEngLockHdev)(HDEV);
typedef BOOLEAN   (APIENTRY* PFN_DxEngUnlockHdev)(HDEV);
typedef BOOLEAN   (APIENTRY* PFN_DxEngIsHdevLockedByCurrentThread)(HDEV);
typedef BOOLEAN   (APIENTRY* PFN_DxEngReferenceHdev)(HDEV);
typedef BOOLEAN   (APIENTRY* PFN_DxEngUnreferenceHdev)(HDEV);
typedef BOOL      (APIENTRY* PFN_DxEngGetDeviceGammaRamp)(HDEV, PGAMMARAMP);
typedef BOOLEAN   (APIENTRY* PFN_DxEngSetDeviceGammaRamp)(HDEV, PGAMMARAMP, BOOL);
typedef DWORD     (APIENTRY* PFN_DxEngSpTearDownSprites)(DWORD, DWORD, DWORD);
typedef DWORD     (APIENTRY* PFN_DxEngSpUnTearDownSprites)(DWORD, DWORD, DWORD);
typedef DWORD     (APIENTRY* PFN_DxEngSpSpritesVisible)(DWORD);
typedef DWORD_PTR (APIENTRY* PFN_DxEngGetHdevData)(HDEV, DXEGSHDEVDATA);
typedef BOOLEAN   (APIENTRY* PFN_DxEngSetHdevData)(HDEV, DXEGSHDEVDATA, DWORD_PTR);
typedef HDC       (APIENTRY* PFN_DxEngCreateMemoryDC)(HDEV);
typedef HDC       (APIENTRY* PFN_DxEngGetDesktopDC)(ULONG, BOOL, BOOL);
typedef BOOLEAN   (APIENTRY* PFN_DxEngDeleteDC)(HDC, BOOL);
typedef BOOLEAN   (APIENTRY* PFN_DxEngCleanDC)(HDC hdc);
typedef BOOL      (APIENTRY* PFN_DxEngSetDCOwner)(HGDIOBJ, DWORD);
typedef PDC       (APIENTRY* PFN_DxEngLockDC)(HDC);
typedef BOOLEAN   (APIENTRY* PFN_DxEngUnlockDC)(PDC);
typedef BOOLEAN   (APIENTRY* PFN_DxEngSetDCState)(HDC, DWORD, DWORD);
typedef DWORD_PTR (APIENTRY* PFN_DxEngGetDCState)(HDC, DWORD);
typedef HBITMAP   (APIENTRY* PFN_DxEngSelectBitmap)(HDC, HBITMAP);
typedef BOOLEAN   (APIENTRY* PFN_DxEngSetBitmapOwner)(HBITMAP, ULONG);
typedef BOOLEAN   (APIENTRY* PFN_DxEngDeleteSurface)(HSURF);
typedef DWORD     (APIENTRY* PFN_DxEngGetSurfaceData)(DWORD, DWORD);
typedef SURFOBJ * (APIENTRY* PFN_DxEngAltLockSurface)(HSURF);
typedef DWORD     (APIENTRY* PFN_DxEngUploadPaletteEntryToSurface)(DWORD, DWORD, DWORD, DWORD);
typedef DWORD     (APIENTRY* PFN_DxEngMarkSurfaceAsDirectDraw)(DWORD, DWORD);
typedef DWORD     (APIENTRY* PFN_DxEngSelectPaletteToSurface)(DWORD, DWORD);
typedef DWORD     (APIENTRY* PFN_DxEngSyncPaletteTableWithDevice)(DWORD, DWORD);
typedef DWORD     (APIENTRY* PFN_DxEngSetPaletteState)(DWORD, DWORD, DWORD);
typedef DWORD     (APIENTRY* PFN_DxEngGetRedirectionBitmap)(DWORD);
typedef DWORD     (APIENTRY* PFN_DxEngLoadImage)(DWORD, DWORD);


typedef struct _DXENG_FUNCTIONS
{
    PVOID                                   Reserved;
    PFN_DxEngNUIsTermSrv                    DxEngNUIsTermSrv;
    PFN_DxEngScreenAccessCheck              DxEngScreenAccessCheck;
    PFN_DxEngRedrawDesktop                  DxEngRedrawDesktop;
    PFN_DxEngDispUniq                       DxEngDispUniq;
    PFN_DxEngIncDispUniq                    DxEngIncDispUniq;
    PFN_DxEngVisRgnUniq                     DxEngVisRgnUniq;
    PFN_DxEngLockShareSem                   DxEngLockShareSem;
    PFN_DxEngUnlockShareSem                 DxEngUnlockShareSem;
    PFN_DxEngEnumerateHdev                  DxEngEnumerateHdev;
    PFN_DxEngLockHdev                       DxEngLockHdev;
    PFN_DxEngUnlockHdev                     DxEngUnlockHdev;
    PFN_DxEngIsHdevLockedByCurrentThread    DxEngIsHdevLockedByCurrentThread;
    PFN_DxEngReferenceHdev                  DxEngReferenceHdev;
    PFN_DxEngUnreferenceHdev                DxEngUnreferenceHdev;
    PFN_DxEngGetDeviceGammaRamp             DxEngGetDeviceGammaRamp;
    PFN_DxEngSetDeviceGammaRamp             DxEngSetDeviceGammaRamp;
    PFN_DxEngSpTearDownSprites              DxEngSpTearDownSprites;
    PFN_DxEngSpUnTearDownSprites            DxEngSpUnTearDownSprites;
    PFN_DxEngSpSpritesVisible               DxEngSpSpritesVisible;
    PFN_DxEngGetHdevData                    DxEngGetHdevData;
    PFN_DxEngSetHdevData                    DxEngSetHdevData;
    PFN_DxEngCreateMemoryDC                 DxEngCreateMemoryDC;
    PFN_DxEngGetDesktopDC                   DxEngGetDesktopDC;
    PFN_DxEngDeleteDC                       DxEngDeleteDC;
    PFN_DxEngCleanDC                        DxEngCleanDC;
    PFN_DxEngSetDCOwner                     DxEngSetDCOwner;
    PFN_DxEngLockDC                         DxEngLockDC;
    PFN_DxEngUnlockDC                       DxEngUnlockDC;
    PFN_DxEngSetDCState                     DxEngSetDCState;
    PFN_DxEngGetDCState                     DxEngGetDCState;
    PFN_DxEngSelectBitmap                   DxEngSelectBitmap;
    PFN_DxEngSetBitmapOwner                 DxEngSetBitmapOwner;
    PFN_DxEngDeleteSurface                  DxEngDeleteSurface;
    PFN_DxEngGetSurfaceData                 DxEngGetSurfaceData;
    PFN_DxEngAltLockSurface                 DxEngAltLockSurface;
    PFN_DxEngUploadPaletteEntryToSurface    DxEngUploadPaletteEntryToSurface;
    PFN_DxEngMarkSurfaceAsDirectDraw        DxEngMarkSurfaceAsDirectDraw;
    PFN_DxEngSelectPaletteToSurface         DxEngSelectPaletteToSurface;
    PFN_DxEngSyncPaletteTableWithDevice     DxEngSyncPaletteTableWithDevice;
    PFN_DxEngSetPaletteState                DxEngSetPaletteState;
    PFN_DxEngGetRedirectionBitmap           DxEngGetRedirectionBitmap;
    PFN_DxEngLoadImage                      DxEngLoadImage;
} DXENG_FUNCTIONS, *PDXENG_FUNCTIONS;

/* exported functions */
NTSTATUS NTAPI DriverEntry(IN PVOID Context1, IN PVOID Context2);
NTSTATUS NTAPI GsDriverEntry(IN PVOID Context1, IN PVOID Context2);
NTSTATUS APIENTRY DxDdCleanupDxGraphics(VOID);
BOOL NTAPI DxDdEnableDirectDraw(HANDLE hDev, BOOL arg2);
DWORD NTAPI DxDdCreateDirectDrawObject(HDC hDC);

/* Global pointers */
extern ULONG gcSizeDdHmgr;
extern PDXG_HANDLE_ENTRY gpentDdHmgr;
extern ULONG gcMaxDdHmgr;
extern PDXG_HANDLE_ENTRY gpentDdHmgrLast;
extern ULONG ghFreeDdHmgr;
extern HSEMAPHORE ghsemHmgr;
extern LONG gcDummyPageRefCnt;
extern HSEMAPHORE ghsemDummyPage;
extern VOID *gpDummyPage;
extern PEPROCESS gpepSession;
extern PLARGE_INTEGER gpLockShortDelay;
extern DXENG_FUNCTIONS gpEngFuncs;

/* Driver list export functions */
DWORD NTAPI DxDxgGenericThunk(ULONG_PTR ulIndex, ULONG_PTR ulHandle, SIZE_T *pdwSizeOfPtr1, PVOID pvPtr1, SIZE_T *pdwSizeOfPtr2, PVOID pvPtr2);
DWORD NTAPI DxDdIoctl(ULONG ulIoctl, PVOID pBuffer, ULONG ulBufferSize);
PDD_SURFACE_LOCAL NTAPI DxDdLockDirectDrawSurface(HANDLE hDdSurface);
BOOL NTAPI DxDdUnlockDirectDrawSurface(PDD_SURFACE_LOCAL pSurface);
DWORD NTAPI DxDdGetDriverInfo(HANDLE DdHandle, PDD_GETDRIVERINFODATA drvInfoData);
BOOL NTAPI DxDdQueryDirectDrawObject(HANDLE DdHandle, DD_HALINFO* pDdHalInfo, DWORD* pCallBackFlags, LPD3DNTHAL_CALLBACKS pd3dNtHalCallbacks, 
                                     LPD3DNTHAL_GLOBALDRIVERDATA pd3dNtGlobalDriverData, PDD_D3DBUFCALLBACKS pd3dBufCallbacks, LPDDSURFACEDESC pTextureFormats,
                                     DWORD* p8, VIDEOMEMORY* p9, DWORD* pdwNumFourCC, DWORD* pdwFourCC);
DWORD NTAPI DxDdReenableDirectDrawObject(HANDLE DdHandle, PVOID p2);
DWORD NTAPI DxDdCanCreateSurface(HANDLE DdHandle, PDD_CANCREATESURFACEDATA SurfaceData);
DWORD NTAPI DxDdCanCreateD3DBuffer(HANDLE DdHandle, PDD_CANCREATESURFACEDATA SurfaceData);
DWORD NTAPI DxDdCreateSurface(HANDLE hDirectDrawLocal, HANDLE *hSurface, DDSURFACEDESC *puSurfaceDescription,
                              DD_SURFACE_GLOBAL *puSurfaceGlobalData, DD_SURFACE_LOCAL *puSurfaceLocalData,
                              DD_SURFACE_MORE *puSurfaceMoreData, PDD_CREATESURFACEDATA puCreateSurfaceData,
                              HANDLE *puhSurface);
DWORD NTAPI DxDdCreateD3DBuffer(HANDLE hDirectDrawLocal, PEDD_SURFACE pDdSurfList, DDSURFACEDESC2 *a3, DD_SURFACE_GLOBAL *pDdSurfGlob, DD_SURFACE_LOCAL *pDdSurfLoc,
                                DD_SURFACE_MORE *pDdSurfMore, DD_CREATESURFACEDATA *pDdCreateSurfaceData, PVOID Address);
DWORD NTAPI DxDdLock(HANDLE hSurface, PDD_LOCKDATA puLockData, HDC hdcClip);
DWORD NTAPI DxDdUnlock(HANDLE hSurface, PDD_UNLOCKDATA puUnlockData);
HANDLE NTAPI DxDdCreateSurfaceObject(HANDLE hDirectDrawLocal, HANDLE hSurface, PDD_SURFACE_LOCAL puSurfaceLocal, PDD_SURFACE_MORE puSurfaceMore, PDD_SURFACE_GLOBAL puSurfaceGlobal, BOOL bComplete);


/* Internal functions */
BOOL FASTCALL VerifyObjectOwner(PDXG_HANDLE_ENTRY pEntry);
BOOL FASTCALL DdHmgCreate(VOID);
BOOL FASTCALL DdHmgDestroy(VOID);
PVOID FASTCALL DdHmgLock(HANDLE DdHandle, UCHAR ObjectType, BOOLEAN LockOwned);
HANDLE FASTCALL DdHmgAlloc(ULONG objSize, CHAR objType, BOOLEAN objLock);
PEDD_SURFACE NTAPI intDdCreateNewSurfaceObject(PEDD_DIRECTDRAW_LOCAL peDdL, HANDLE hDirectDrawLocal, 
                                               PDD_SURFACE_GLOBAL pDdSurfGlob, PDD_SURFACE_LOCAL pDdSurfLoc, PDD_SURFACE_MORE pDdSurfMore);

#endif /* _DXG_PCH_ */
