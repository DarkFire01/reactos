/*
 * PROJECT:     ReactOS Win32k subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Logical surfaces redirected windows draw into and the DWM composes from
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <win32k.h>
#include <reactos/dwmport.h>

#define NDEBUG
#include <debug.h>

#define TAG_LSURF 'fsLG'

/* The DWM opens a surface's section with exactly this access */
#define LSURF_SECTION_ACCESS (SECTION_MAP_READ | SECTION_MAP_WRITE)

static LIST_ENTRY gLogicalSurfaces;
static HSEMAPHORE ghsemLogicalSurfaces;
static ULONG64 gNextLogicalSurface;

CODE_SEG("INIT")
NTSTATUS
NTAPI
InitLogicalSurfaceImpl(VOID)
{
    InitializeListHead(&gLogicalSurfaces);
    gNextLogicalSurface = 0x10;
    ghsemLogicalSurfaces = EngCreateSemaphore();
    return ghsemLogicalSurfaces ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

PLOGICAL_SURFACE
NTAPI
GreCreateLogicalSurface(
    _In_ ULONG Width,
    _In_ ULONG Height,
    _In_ ULONG64 Sprite)
{
    PLOGICAL_SURFACE Surface;
    PSURFACE psurf;
    PVOID pvBits;
    ULONG cjSize;

    Width = max(Width, 1);
    Height = max(Height, 1);
    if (!NT_SUCCESS(RtlULongMult(Width * 4, Height, &cjSize)))
        return NULL;

    Surface = ExAllocatePoolZero(PagedPool, sizeof(*Surface), TAG_LSURF);
    if (!Surface)
        return NULL;

    /* The surface frees the view; the section object is ours to release */
    pvBits = EngAllocSectionMem(&Surface->Section, FL_ZERO_MEMORY, cjSize, TAG_LSURF);
    if (!pvBits)
        goto Fail;

    Surface->hbm = GreCreateBitmapEx(Width, Height, Width * 4, BMF_32BPP, BMF_TOPDOWN | BMF_KMSECTION,
                                     cjSize, pvBits, 0);
    if (!Surface->hbm)
    {
        EngFreeSectionMem(Surface->Section, pvBits);
        goto Fail;
    }
    GreSetObjectOwner(Surface->hbm, GDI_OBJ_HMGR_PUBLIC);

    Surface->Dirty = IntSysCreateRectpRgn(0, 0, 0, 0);
    if (!Surface->Dirty)
    {
        GreDeleteObject(Surface->hbm);
        EngFreeSectionMem(Surface->Section, NULL);
        goto Fail;
    }

    Surface->Size.cx = Width;
    Surface->Size.cy = Height;
    Surface->Stride = Width * 4;
    Surface->Cookie = (ULONG64)(ULONG_PTR)Surface->hbm;
    Surface->Sprite = Sprite;

    EngAcquireSemaphore(ghsemLogicalSurfaces);
    Surface->Handle = gNextLogicalSurface++;
    InsertTailList(&gLogicalSurfaces, &Surface->Link);
    psurf = SURFACE_ShareLockSurface(Surface->hbm);
    if (psurf)
    {
        psurf->pLogicalSurface = Surface;
        SURFACE_ShareUnlockSurface(psurf);
    }
    EngReleaseSemaphore(ghsemLogicalSurfaces);

    return Surface;

Fail:
    ExFreePoolWithTag(Surface, TAG_LSURF);
    return NULL;
}

VOID
NTAPI
GreDeleteLogicalSurface(
    _In_ PLOGICAL_SURFACE Surface)
{
    PSURFACE psurf;

    EngAcquireSemaphore(ghsemLogicalSurfaces);
    RemoveEntryList(&Surface->Link);
    psurf = SURFACE_ShareLockSurface(Surface->hbm);
    if (psurf)
    {
        psurf->pLogicalSurface = NULL;
        SURFACE_ShareUnlockSurface(psurf);
    }
    EngReleaseSemaphore(ghsemLogicalSurfaces);

    REGION_Delete(Surface->Dirty);
    GreDeleteObject(Surface->hbm);
    EngFreeSectionMem(Surface->Section, NULL);
    ExFreePoolWithTag(Surface, TAG_LSURF);
}

BOOLEAN
NTAPI
GreAccumulateLogicalSurfaceDirty(
    _In_ PSURFACE psurf,
    _In_ const RECTL *prcl,
    _Out_ PULONG64 Sprite)
{
    PLOGICAL_SURFACE Surface;
    BOOLEAN Notify = FALSE;
    RECTL rcl;

    *Sprite = 0;

    EngAcquireSemaphore(ghsemLogicalSurfaces);
    Surface = psurf->pLogicalSurface;
    if (Surface)
    {
        rcl.left = max(prcl->left, 0);
        rcl.top = max(prcl->top, 0);
        rcl.right = min(prcl->right, Surface->Size.cx);
        rcl.bottom = min(prcl->bottom, Surface->Size.cy);
        if (rcl.left < rcl.right && rcl.top < rcl.bottom)
        {
            REGION_UnionRectWithRgn(Surface->Dirty, &rcl);
            Notify = !Surface->DirtyPending;
            Surface->DirtyPending = TRUE;
            *Sprite = Surface->Sprite;
        }
    }
    EngReleaseSemaphore(ghsemLogicalSurfaces);

    return Notify;
}

BOOL
NTAPI
GreSelectRedirectionBitmap(
    _In_ HDC hdc,
    _In_opt_ HBITMAP hbm)
{
    PSURFACE psurf;
    PDC pdc;

    pdc = DC_LockDc(hdc);
    if (!pdc)
        return FALSE;

    if (hbm)
    {
        psurf = SURFACE_ShareLockSurface(hbm);
        if (!psurf)
        {
            DC_UnlockDc(pdc);
            return FALSE;
        }

        DC_vSelectSurface(pdc, psurf);
        pdc->dclevel.sizl = psurf->SurfObj.sizlBitmap;
        pdc->fs |= DC_REDIRECTION | DC_DIRTY_RAO;
        SURFACE_ShareUnlockSurface(psurf);
    }
    else if (pdc->fs & DC_REDIRECTION)
    {
        /* Back on the screen, the way a display DC follows a mode change */
        pdc->fs &= ~DC_REDIRECTION;
        pdc->fs |= DC_DIRTY_RAO;
        DC_vUpdateDC(pdc);
    }

    DC_UnlockDc(pdc);
    return TRUE;
}

/* Caller holds ghsemLogicalSurfaces */
static
PLOGICAL_SURFACE
LookupLogicalSurface(
    _In_ ULONG64 Handle)
{
    PLIST_ENTRY Entry;
    PLOGICAL_SURFACE Surface;

    for (Entry = gLogicalSurfaces.Flink; Entry != &gLogicalSurfaces; Entry = Entry->Flink)
    {
        Surface = CONTAINING_RECORD(Entry, LOGICAL_SURFACE, Link);
        if (Surface->Handle == Handle)
            return Surface;
    }
    return NULL;
}

static
NTSTATUS
GetSurfaceData(
    _In_ ULONG64 Handle,
    _Out_ PDWM_SURFACE_DATA Data)
{
    PLOGICAL_SURFACE Surface;
    HANDLE Section;
    NTSTATUS Status;

    RtlZeroMemory(Data, sizeof(*Data));

    EngAcquireSemaphore(ghsemLogicalSurfaces);
    Surface = LookupLogicalSurface(Handle);
    if (!Surface)
    {
        EngReleaseSemaphore(ghsemLogicalSurfaces);
        return STATUS_INVALID_HANDLE;
    }

    Status = ObOpenObjectByPointer(((PENGSECTION)Surface->Section)->pvSectionObject, 0, NULL,
                                   LSURF_SECTION_ACCESS, MmSectionObjectType, UserMode, &Section);
    if (NT_SUCCESS(Status))
    {
        Data->Type = DWM_SURFACE_SECTION;
        Data->Width = Surface->Size.cx;
        Data->Height = Surface->Size.cy;
        Data->Format = BMF_32BPP;
        Data->Stride = Surface->Stride;
        Data->Cookie = Surface->Cookie;
        Data->Handle = (ULONG64)(ULONG_PTR)Section;
    }
    EngReleaseSemaphore(ghsemLogicalSurfaces);

    return Status;
}

/* Hands what was drawn since the last call to the DWM and starts a new accumulation */
static
NTSTATUS
TakeDirtyRegion(
    _In_ ULONG64 Handle,
    _Inout_ PDWM_SURFACE_DIRTY Dirty)
{
    PLOGICAL_SURFACE Surface;
    PREGION Region;
    RECTL Box;

    Region = REGION_LockRgn((HRGN)(ULONG_PTR)Dirty->Region);
    if (!Region)
        return STATUS_INVALID_HANDLE;

    EngAcquireSemaphore(ghsemLogicalSurfaces);
    Surface = LookupLogicalSurface(Handle);
    if (Surface)
    {
        REGION_bCopy(Region, Surface->Dirty);
        REGION_SetRectRgn(Surface->Dirty, 0, 0, 0, 0);
        Surface->DirtyPending = FALSE;
    }
    EngReleaseSemaphore(ghsemLogicalSurfaces);

    Dirty->Dirty = Surface && REGION_GetRgnBox(Region, &Box) != NULLREGION;
    REGION_UnlockRgn(Region);

    return Surface ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

/* Only the DWM reads logical surfaces */
BOOL
APIENTRY
NtGdiHLSurfGetInformation(
    _In_ HANDLE Hlsurf,
    _In_ ULONG InformationClass,
    _Inout_updates_bytes_opt_(*Size) PVOID Buffer,
    _Inout_ PULONG Size)
{
    union
    {
        DWM_SURFACE_DATA Data;
        DWM_SURFACE_DIRTY Dirty;
    } Info;
    ULONG Needed;
    ULONG BufferSize;
    NTSTATUS Status;

    if (!IntDwmIsCurrentProcess())
    {
        EngSetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    switch (InformationClass)
    {
        case HLSURF_INFO_SURFACE_DATA:
            Needed = sizeof(Info.Data);
            break;

        case HLSURF_INFO_DIRTY_REGION:
            Needed = sizeof(Info.Dirty);
            break;

        default:
            EngSetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
    }

    _SEH2_TRY
    {
        ProbeForWrite(Size, sizeof(*Size), sizeof(ULONG));
        BufferSize = *Size;
        if (Buffer && BufferSize >= Needed)
        {
            ProbeForWrite(Buffer, Needed, sizeof(ULONG));
            RtlCopyMemory(&Info, Buffer, Needed);
        }
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        SetLastNtError(_SEH2_GetExceptionCode());
        _SEH2_YIELD(return FALSE);
    }
    _SEH2_END;

    if (!Buffer || BufferSize < Needed)
    {
        EngSetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    if (InformationClass == HLSURF_INFO_SURFACE_DATA)
        Status = GetSurfaceData((ULONG64)(ULONG_PTR)Hlsurf, &Info.Data);
    else
        Status = TakeDirtyRegion((ULONG64)(ULONG_PTR)Hlsurf, &Info.Dirty);

    if (!NT_SUCCESS(Status))
    {
        SetLastNtError(Status);
        return FALSE;
    }

    _SEH2_TRY
    {
        RtlCopyMemory(Buffer, &Info, Needed);
        *Size = Needed;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        SetLastNtError(_SEH2_GetExceptionCode());
        _SEH2_YIELD(return FALSE);
    }
    _SEH2_END;

    return TRUE;
}

/* Dirty notifications always go out as sprite messages, so the settings are accepted as they are */
BOOL
APIENTRY
NtGdiHLSurfSetInformation(
    _In_ HANDLE Hlsurf,
    _In_ ULONG InformationClass,
    _In_reads_bytes_opt_(Size) PVOID Buffer,
    _In_ ULONG Size)
{
    BOOL Known;

    if (!IntDwmIsCurrentProcess())
    {
        EngSetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }

    EngAcquireSemaphore(ghsemLogicalSurfaces);
    Known = LookupLogicalSurface((ULONG64)(ULONG_PTR)Hlsurf) != NULL;
    EngReleaseSemaphore(ghsemLogicalSurfaces);

    if (!Known)
        EngSetLastError(ERROR_INVALID_HANDLE);
    return Known;
}
