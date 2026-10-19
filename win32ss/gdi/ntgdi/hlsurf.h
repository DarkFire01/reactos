#pragma once

/*
 * A logical surface: what a redirected window draws into and the DWM composes from. This is
 * the system memory form, a 32bpp bitmap over a section the DWM maps for itself.
 */
typedef struct _LOGICAL_SURFACE
{
    LIST_ENTRY Link;
    ULONG64 Handle;
    PVOID Section;
    HBITMAP hbm;
    SIZEL Size;
    LONG Stride;
    ULONG64 Cookie;
    ULONG64 Sprite;             /* named in dirty notifications */
    PREGION Dirty;              /* surface coordinates */
    BOOLEAN DirtyPending;       /* the DWM was told and has not taken the region yet */
} LOGICAL_SURFACE, *PLOGICAL_SURFACE;

CODE_SEG("INIT")
NTSTATUS
NTAPI
InitLogicalSurfaceImpl(VOID);

PLOGICAL_SURFACE
NTAPI
GreCreateLogicalSurface(
    _In_ ULONG Width,
    _In_ ULONG Height,
    _In_ ULONG64 Sprite);

VOID
NTAPI
GreDeleteLogicalSurface(
    _In_ PLOGICAL_SURFACE Surface);

/**
 * @brief Add a rectangle a drawing call touched to the surface's dirty region.
 *
 * @param[out] Sprite Receives the sprite to notify when the surface went from clean to dirty.
 * @return TRUE when the DWM has to be told.
 */
BOOLEAN
NTAPI
GreAccumulateLogicalSurfaceDirty(
    _In_ PSURFACE psurf,
    _In_ const RECTL *prcl,
    _Out_ PULONG64 Sprite);

/* Tells the DWM a sprite's surface has new content; callable from any drawing thread */
VOID
NTAPI
IntDwmNotifyDirtySprite(
    _In_ ULONG64 Sprite);

/* Make a redirected DC draw into a logical surface, or back to the screen with NULL */
BOOL
NTAPI
GreSelectRedirectionBitmap(
    _In_ HDC hdc,
    _In_opt_ HBITMAP hbm);

BOOL
APIENTRY
NtGdiHLSurfGetInformation(
    _In_ HANDLE Hlsurf,
    _In_ ULONG InformationClass,
    _Inout_updates_bytes_opt_(*Size) PVOID Buffer,
    _Inout_ PULONG Size);

BOOL
APIENTRY
NtGdiHLSurfSetInformation(
    _In_ HANDLE Hlsurf,
    _In_ ULONG InformationClass,
    _In_reads_bytes_opt_(Size) PVOID Buffer,
    _In_ ULONG Size);
