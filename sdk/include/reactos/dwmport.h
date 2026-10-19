/*
 * PROJECT:     ReactOS Desktop Window Manager
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Messages win32k sends to the DWM session port
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Window state changes, sent as kernel datagrams after DwmKernelStartup */
#define DWM_PORT_WINDOW_NOTIFICATION 0x40000100

typedef struct _DWM_WINDOW_NOTIFICATION
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG Event;
    ULONG64 Window;
    ULONG64 InsertAfter;
    RECT WindowRect;
    RECT ClientRect;
    ULONG Style;
    ULONG ExStyle;
    ULONG ProcessId;
    ULONG Reserved;
} DWM_WINDOW_NOTIFICATION, *PDWM_WINDOW_NOTIFICATION;

/*
 * Sprites: the composed form of a redirected top level window. The command values and
 * payload sizes are the ones Windows sends, and the receiver requires the sizes exactly.
 */
#define DWM_PORT_CREATE_SPRITE      0x40000002
#define DWM_PORT_DESTROY_SPRITE     0x40000003
#define DWM_PORT_DIRTY_SPRITE       0x40000004
#define DWM_PORT_ZORDER_SPRITE      0x40000005
#define DWM_PORT_UPDATE_SPRITE      0x40000006
#define DWM_PORT_SHOW_SPRITE        0x40000007

/* UpdateSprite flags */
#define DWM_SPRITE_VISIBLE          0x00000001
#define DWM_SPRITE_NEW_SURFACE      0x00000008

/* ZOrderSprite insert-after values that are not sprites */
#define DWM_SPRITE_INSERT_TOP       0
#define DWM_SPRITE_INSERT_BOTTOM    1

#include <pshpack4.h>

/* The window as a sprite update describes it */
typedef struct _DWM_SPRITE_WINDOW
{
    RECT WindowRect;
    RECT ClientRect;
    ULONG Style;
    ULONG ExStyle;
    ULONG ExStyle2;
    ULONG ActiveFrame;
    ULONG64 DesktopId;
    UCHAR Reserved[0x40];
    ULONG Reserved2[4];
} DWM_SPRITE_WINDOW, *PDWM_SPRITE_WINDOW;

typedef struct _DWM_CREATE_SPRITE
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG64 Sprite;
    ULONG64 Window;
    RECT Rect;
    ULONG Flags;
    DWM_SPRITE_WINDOW Info;
    ULONG ExpWinVer;
} DWM_CREATE_SPRITE, *PDWM_CREATE_SPRITE;

typedef struct _DWM_UPDATE_SPRITE
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG64 Sprite;
    ULONG Flags;
    ULONG HasInfo;
    DWM_SPRITE_WINDOW Info;
    UCHAR CachedAttributes[0xC];
    ULONG64 Surface;            /* the logical surface, read with NtGdiHLSurfGetInformation */
    ULONG SurfaceKind;
    ULONG Width;
    ULONG Height;
    ULONG64 Sync;
} DWM_UPDATE_SPRITE, *PDWM_UPDATE_SPRITE;

typedef struct _DWM_SPRITE_HANDLE
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG64 Sprite;
} DWM_SPRITE_HANDLE, *PDWM_SPRITE_HANDLE;

typedef struct _DWM_SHOW_SPRITE
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG64 Sprite;
    ULONG Show;
} DWM_SHOW_SPRITE, *PDWM_SHOW_SPRITE;

typedef struct _DWM_ZORDER_SPRITE
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG64 Sprite;
    ULONG64 InsertAfter;
} DWM_ZORDER_SPRITE, *PDWM_ZORDER_SPRITE;

/* Where a top level window's client area and content are; content is what the DWM shows of GDI's drawing */
#define DWM_PORT_CHILD_MOVE_SIZE    0x40000015

typedef struct _DWM_CHILD_MOVE_SIZE
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG64 Window;
    RECT WindowRect;
    RECT ClientRect;
    RECT ContentRect;
    LONG BorderThickness;
} DWM_CHILD_MOVE_SIZE, *PDWM_CHILD_MOVE_SIZE;

typedef struct _DWM_DIRTY_SPRITE
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG DirtyFlags;
    ULONG64 Sprite;
    ULONG64 Token;
} DWM_DIRTY_SPRITE, *PDWM_DIRTY_SPRITE;

#include <poppack.h>

#define DWM_PORT_DATA_SIZE(Type) ((ULONG)(sizeof(Type) - sizeof(PORT_MESSAGE)))

C_ASSERT(sizeof(DWM_SPRITE_WINDOW) == 0x88);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_CREATE_SPRITE) == 0xB4);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_UPDATE_SPRITE) == 0xC4);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_SPRITE_HANDLE) == 0x0C);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_SHOW_SPRITE) == 0x10);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_ZORDER_SPRITE) == 0x14);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_DIRTY_SPRITE) == 0x18);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_CHILD_MOVE_SIZE) == 0x40);

/* NtGdiHLSurfGetInformation classes */
#define HLSURF_INFO_SURFACE_DATA    3
#define HLSURF_INFO_DIRTY_REGION    4

/* What the DWM learns about a logical surface */
typedef struct _DWM_SURFACE_DATA
{
    ULONG Type;                 /* DWM_SURFACE_* */
    ULONG Width;
    ULONG Height;
    ULONG Format;               /* BMF_32BPP */
    LONG Stride;
    ULONG Reserved;
    ULONG64 Cookie;             /* changes whenever the backing is replaced */
    ULONG64 Luid;
    ULONG64 Handle;             /* DWM_SURFACE_SECTION: a section handle opened into the caller */
} DWM_SURFACE_DATA, *PDWM_SURFACE_DATA;

C_ASSERT(sizeof(DWM_SURFACE_DATA) == 48);

#define DWM_SURFACE_NONE            0
#define DWM_SURFACE_SECTION         1
#define DWM_SURFACE_SHARED_HANDLE   2

/* In: Region, a region handle of the caller's; out: Dirty is TRUE and Region holds what changed */
typedef struct _DWM_SURFACE_DIRTY
{
    ULONG Dirty;
    ULONG Reserved;
    ULONG64 Region;
    UCHAR Reserved2[40];
} DWM_SURFACE_DIRTY, *PDWM_SURFACE_DIRTY;

C_ASSERT(sizeof(DWM_SURFACE_DIRTY) == 56);

#ifdef __cplusplus
}
#endif
