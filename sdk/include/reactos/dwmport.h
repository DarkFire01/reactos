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
#define DWM_SPRITE_LAYERED          0x00000010  /* ours: Attributes holds UpdateLayeredWindow's blending */

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

/* How the DWM blends an UpdateLayeredWindow surface, which holds premultiplied alpha */
typedef struct _DWM_LAYERED_ATTRIBUTES
{
    COLORREF Key;               /* already applied to the surface's alpha */
    BLENDFUNCTION Blend;
    ULONG Flags;                /* ULW_* */
} DWM_LAYERED_ATTRIBUTES, *PDWM_LAYERED_ATTRIBUTES;

typedef struct _DWM_UPDATE_SPRITE
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG64 Sprite;
    ULONG Flags;
    ULONG HasInfo;
    DWM_SPRITE_WINDOW Info;
    DWM_LAYERED_ATTRIBUTES Attributes;
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

/*
 * A region the DWM clips a window by, in window coordinates, sent in as many messages as its
 * rectangles take; First and Total place each message's rectangles. Total is zero when the window
 * has none, and an empty region is one empty rectangle.
 */
#define DWM_PORT_VIS_RGN_UPDATE     0x4000009E

/* VisRgnUpdate kinds */
#define DWM_VIS_RGN_WINDOW_REGION   2   /* SetWindowRgn's */

#ifdef _WIN64
#define DWM_VIS_RGN_MAX_RECTS       27
#else
#define DWM_VIS_RGN_MAX_RECTS       12
#endif

typedef struct _DWM_VIS_RGN_UPDATE
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG64 Window;
    ULONG First;
    ULONG Total;
    ULONG Kind;
    ULONG Count;
    RECT Rects[DWM_VIS_RGN_MAX_RECTS];
} DWM_VIS_RGN_UPDATE, *PDWM_VIS_RGN_UPDATE;

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

/* A flip swapchain bound to a window: its composition surface, opened into the DWM, or none to unbind */
#define DWM_PORT_BIND_SWAPCHAIN     0x4000003B

typedef struct _DWM_BIND_SWAPCHAIN
{
    PORT_MESSAGE Header;
    ULONG Command;
    ULONG64 Window;
    ULONG64 Surface;            /* the DWM owns and closes this handle */
} DWM_BIND_SWAPCHAIN, *PDWM_BIND_SWAPCHAIN;

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

/* Thumbnails, iconic bitmaps and live preview */
#define DWM_API_REGISTER_THUMBNAIL      0x4000001F
#define DWM_API_UPDATE_THUMBNAIL        0x40000020
#define DWM_API_UNREGISTER_THUMBNAIL    0x40000021
#define DWM_API_QUERY_THUMBNAIL_SIZE    0x40000023
#define DWM_API_ACTIVATE_LIVE_PREVIEW   0x40000039
#define DWM_API_SET_ICONIC_THUMBNAIL    0x4000003A
#define DWM_API_QUERY_THUMBNAIL_TYPE    0x4000003C
#define DWM_API_SET_ICONIC_LIVE_PREVIEW 0x4000003D
#define DWM_API_INVALIDATE_ICONIC       0x4000003E

/* Ours: DwmSetWindowAttribute's attributes the DWM acts on */
#define DWM_API_SET_WINDOW_ATTRIBUTE    0x40000082

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

/* A thumbnail is named by the id its process gave it; the DWM keys it by that process too */
typedef struct _DWM_REGISTER_THUMBNAIL_REQUEST
{
    PORT_MESSAGE Header;
    ULONG Command;
    LONG Result;
    ULONG Thumbnail;
    ULONG Flags;
    ULONG64 Destination;
    ULONG64 Source;
} DWM_REGISTER_THUMBNAIL_REQUEST, *PDWM_REGISTER_THUMBNAIL_REQUEST;

/* DWM_THUMBNAIL_PROPERTIES, field for field */
typedef struct _DWM_UPDATE_THUMBNAIL_REQUEST
{
    PORT_MESSAGE Header;
    ULONG Command;
    LONG Result;
    ULONG Thumbnail;
    ULONG Flags;                /* DWM_TNP_* */
    RECT Destination;
    RECT Source;
    ULONG Opacity;
    ULONG Visible;
    ULONG SourceClientAreaOnly;
} DWM_UPDATE_THUMBNAIL_REQUEST, *PDWM_UPDATE_THUMBNAIL_REQUEST;

/* Unregistering a thumbnail, or a question about it */
typedef struct _DWM_THUMBNAIL_REQUEST
{
    PORT_MESSAGE Header;
    ULONG Command;
    LONG Result;
    ULONG Thumbnail;
    ULONG Type;                 /* out: what the thumbnail shows */
    SIZE Size;                  /* out: the size of what it shows */
} DWM_THUMBNAIL_REQUEST, *PDWM_THUMBNAIL_REQUEST;

/* What a thumbnail shows, as DwmpQueryThumbnailType answers */
#define DWM_THUMBNAIL_TYPE_LIVE         0
#define DWM_THUMBNAIL_TYPE_SNAPSHOT     1   /* the window as it was before it was minimized */
#define DWM_THUMBNAIL_TYPE_NONE         2
#define DWM_THUMBNAIL_TYPE_ICONIC_WAIT  3   /* asked the window for its iconic bitmap */
#define DWM_THUMBNAIL_TYPE_ICONIC       4

typedef struct _DWM_LIVE_PREVIEW_REQUEST
{
    PORT_MESSAGE Header;
    ULONG Command;
    LONG Result;
    ULONG Activate;
    ULONG Trigger;
    ULONG64 Window;             /* none shows the desktop */
    ULONG64 InsertBehind;
} DWM_LIVE_PREVIEW_REQUEST, *PDWM_LIVE_PREVIEW_REQUEST;

/*
 * An iconic bitmap's premultiplied 32bpp top down pixels, in a section the sender made. Section is
 * the sender's handle; the DWM duplicates it and the sender closes its own once answered.
 */
typedef struct _DWM_ICONIC_BITMAP_REQUEST
{
    PORT_MESSAGE Header;
    ULONG Command;
    LONG Result;
    ULONG64 Window;
    ULONG64 Section;
    ULONG Width;
    ULONG Height;
    ULONG Flags;                /* DWM_SIT_* */
    ULONG HasOrigin;            /* live preview: Origin is where the client area is in the bitmap's frame */
    POINT Origin;
} DWM_ICONIC_BITMAP_REQUEST, *PDWM_ICONIC_BITMAP_REQUEST;

typedef struct _DWM_WINDOW_ATTRIBUTE_REQUEST
{
    PORT_MESSAGE Header;
    ULONG Command;
    LONG Result;
    ULONG64 Window;
    ULONG Attribute;            /* DWMWA_* */
    ULONG Value;
} DWM_WINDOW_ATTRIBUTE_REQUEST, *PDWM_WINDOW_ATTRIBUTE_REQUEST;

#include <poppack.h>

#define DWM_PORT_DATA_SIZE(Type) ((ULONG)(sizeof(Type) - sizeof(PORT_MESSAGE)))

C_ASSERT(sizeof(DWM_SPRITE_WINDOW) == 0x88);
C_ASSERT(sizeof(DWM_LAYERED_ATTRIBUTES) == 0xC);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_CREATE_SPRITE) == 0xB4);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_UPDATE_SPRITE) == 0xC4);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_SPRITE_HANDLE) == 0x0C);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_SHOW_SPRITE) == 0x10);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_ZORDER_SPRITE) == 0x14);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_DIRTY_SPRITE) == 0x18);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_CHILD_MOVE_SIZE) == 0x40);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_FRAME_MARGINS_CHANGE) == 0x1C);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_WINDOW_HANDLE) == 0x0C);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_BIND_SWAPCHAIN) == 0x14);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_ACTIVATION_CHANGE) == 0x10);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_ICON_CHANGE) == 0x1C);
C_ASSERT(DWM_PORT_DATA_SIZE(DWM_HIT_TEST_QUERY) == 0x34);
C_ASSERT(FIELD_OFFSET(DWM_VIS_RGN_UPDATE, Rects) - sizeof(PORT_MESSAGE) == 0x1C);
C_ASSERT(sizeof(DWM_VIS_RGN_UPDATE) <= PORT_MAXIMUM_MESSAGE_LENGTH);
C_ASSERT(sizeof(DWM_UPDATE_THUMBNAIL_REQUEST) <= PORT_MAXIMUM_MESSAGE_LENGTH);
C_ASSERT(sizeof(DWM_ICONIC_BITMAP_REQUEST) <= PORT_MAXIMUM_MESSAGE_LENGTH);

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
