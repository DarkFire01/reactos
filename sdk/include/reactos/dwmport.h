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
    ULONG ContentDpi;       /* ours: what the window draws at when the DWM stretches it to ScreenDpi, else 0 */
    ULONG ScreenDpi;
    ULONG Reserved2[2];
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

/* The frame the DWM draws, from the window's style alone; an application may cover it with its client area */
#define DWM_PORT_FRAME_MARGINS_CHANGE   0x40000054

typedef struct _DWM_FRAME_MARGINS_CHANGE
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG64 Window;
    LONG Left;
    LONG Right;
    LONG Top;
    LONG Bottom;
} DWM_FRAME_MARGINS_CHANGE, *PDWM_FRAME_MARGINS_CHANGE;

typedef struct _DWM_DIRTY_SPRITE
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG DirtyFlags;
    ULONG64 Sprite;
    ULONG64 Token;
} DWM_DIRTY_SPRITE, *PDWM_DIRTY_SPRITE;

/* What the frame the DWM draws shows: the window's icons, its caption text and whether it is active */
#define DWM_PORT_ICON_CHANGE        0x40000008
#define DWM_PORT_TEXT_CHANGE        0x40000009
#define DWM_PORT_ACTIVATION_CHANGE  0x4000000C

typedef struct _DWM_WINDOW_HANDLE
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG64 Window;
} DWM_WINDOW_HANDLE, *PDWM_WINDOW_HANDLE;

typedef struct _DWM_ACTIVATION_CHANGE
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG64 Window;
    ULONG Active;
} DWM_ACTIVATION_CHANGE, *PDWM_ACTIVATION_CHANGE;

typedef struct _DWM_ICON_CHANGE
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG64 Window;
    ULONG64 Icon;
    ULONG64 SmallIcon;
} DWM_ICON_CHANGE, *PDWM_ICON_CHANGE;

/*
 * A request the DWM answers in place: what of its frame is under a point. The DWM shows hover
 * and pressed caption buttons from these; Captured is the hit test code of a button being tracked.
 */
#define DWM_PORT_HIT_TEST_QUERY     0x40000018
#define DWM_HIT_TEST_NONE           (-2)

typedef struct _DWM_HIT_TEST_QUERY
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG64 Window;
    ULONG Message;
    POINT Point;
    ULONG64 WParam;
    ULONG64 LParam;
    LONG Captured;
    LONG HitTest;               /* out */
    ULONG Handled;              /* out */
} DWM_HIT_TEST_QUERY, *PDWM_HIT_TEST_QUERY;

/*
 * Requests applications make through dwmapi on a connection of their own to the session port.
 * The DWM answers each in place, with Result. Windows carries these over ALPC; the command values
 * are its own.
 */
#define DWM_API_HIT_TEST            0x40000018
#define DWM_API_EXTEND_FRAME        0x4000001A
#define DWM_API_MOUSE_LEAVE         0x4000001B
#define DWM_API_ENABLE_BLUR_BEHIND  0x40000022

/* Ours: the shared surface a GPU presenter draws a window's client area into, and word of a new frame in it */
#define DWM_API_GET_SHARED_SURFACE      0x40000080
#define DWM_API_UPDATE_SHARED_SURFACE   0x40000081

/* As many of a blur region's rectangles as a port message holds; a region with more sends its bounds */
#define DWM_BLUR_BEHIND_MAX_RECTS   12

typedef struct _DWM_API_REQUEST
{
    PORT_MESSAGE Header;
    ULONG Command;
    LONG Result;
} DWM_API_REQUEST, *PDWM_API_REQUEST;

typedef struct _DWM_WINDOW_REQUEST
{
    PORT_MESSAGE Header;
    ULONG Command;
    LONG Result;
    ULONG64 Window;
} DWM_WINDOW_REQUEST, *PDWM_WINDOW_REQUEST;

/* DwmDefWindowProc's question about a frame the application draws itself; as DWM_HIT_TEST_QUERY */
typedef struct _DWM_HIT_TEST_REQUEST
{
    PORT_MESSAGE Header;
    ULONG Command;
    LONG Result;
    ULONG64 Window;
    ULONG Message;
    POINT Point;
    ULONG64 WParam;
    ULONG64 LParam;
    LONG Captured;
    LONG HitTest;               /* out */
    ULONG Handled;              /* out */
} DWM_HIT_TEST_REQUEST, *PDWM_HIT_TEST_REQUEST;

typedef struct _DWM_EXTEND_FRAME_REQUEST
{
    PORT_MESSAGE Header;
    ULONG Command;
    LONG Result;
    ULONG64 Window;
    LONG Left;                  /* MARGINS; a negative one turns the whole window to glass */
    LONG Right;
    LONG Top;
    LONG Bottom;
} DWM_EXTEND_FRAME_REQUEST, *PDWM_EXTEND_FRAME_REQUEST;

typedef struct _DWM_BLUR_BEHIND_REQUEST
{
    PORT_MESSAGE Header;
    ULONG Command;
    LONG Result;
    ULONG64 Window;
    ULONG Flags;                /* DWM_BB_*: which of the fields below to take */
    ULONG Enable;
    ULONG TransitionOnMaximized;
    ULONG RectCount;            /* with DWM_BB_BLURREGION, zero is the whole client area */
    RECT Rects[DWM_BLUR_BEHIND_MAX_RECTS];  /* client coordinates */
} DWM_BLUR_BEHIND_REQUEST, *PDWM_BLUR_BEHIND_REQUEST;

/* DwmpDxGetWindowSharedSurface and DwmpDxUpdateWindowSharedSurface */
typedef struct _DWM_SHARED_SURFACE_REQUEST
{
    PORT_MESSAGE Header;
    ULONG Command;
    LONG Result;
    ULONG64 Window;
    LUID AdapterLuid;
    ULONG Format;               /* DXGI_FORMAT; in, the one asked for, and out, the surface's */
    ULONG Flags;
    ULONG64 Surface;            /* out: global share handle of the surface */
    ULONG64 UpdateId;           /* out on a get, in on an update */
} DWM_SHARED_SURFACE_REQUEST, *PDWM_SHARED_SURFACE_REQUEST;

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
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_FRAME_MARGINS_CHANGE) == 0x1C);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_WINDOW_HANDLE) == 0x0C);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_ACTIVATION_CHANGE) == 0x10);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_ICON_CHANGE) == 0x1C);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_HIT_TEST_QUERY) == 0x34);

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
