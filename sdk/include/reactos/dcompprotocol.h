/*
 * PROJECT:     ReactOS DirectComposition
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Channel protocol between dcomp.dll, win32k and the compositor
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Commands a client writes into its channel's command buffer; the first dword of each is the tag */
#define DCOMP_CMD_CREATE_RESOURCE           2
#define DCOMP_CMD_RELEASE_RESOURCE          4
#define DCOMP_CMD_SET_INTEGER_PROPERTY      11
#define DCOMP_CMD_SET_FLOAT_PROPERTY        12
#define DCOMP_CMD_SET_HANDLE_PROPERTY       13
#define DCOMP_CMD_SET_BUFFER_PROPERTY       15
#define DCOMP_CMD_SET_REFERENCE_PROPERTY    16

typedef struct _DCOMP_CMD_CREATE
{
    ULONG Tag;
    ULONG Resource;
    ULONG Type;
    ULONG External;
} DCOMP_CMD_CREATE;

typedef struct _DCOMP_CMD_RELEASE
{
    ULONG Tag;
    ULONG Resource;
} DCOMP_CMD_RELEASE;

typedef struct _DCOMP_CMD_SET_INTEGER
{
    ULONG Tag;
    ULONG Resource;
    ULONG Property;
    ULONG Reserved;
    ULONG64 Value;
} DCOMP_CMD_SET_INTEGER;

typedef struct _DCOMP_CMD_SET_FLOAT
{
    ULONG Tag;
    ULONG Resource;
    ULONG Property;
    FLOAT Value;
} DCOMP_CMD_SET_FLOAT;

typedef struct _DCOMP_CMD_SET_HANDLE
{
    ULONG Tag;
    ULONG Resource;
    ULONG Property;
    ULONG Reserved;
    ULONG64 Handle;
} DCOMP_CMD_SET_HANDLE;

/* Followed by Size bytes, padded to four */
typedef struct _DCOMP_CMD_SET_BUFFER
{
    ULONG Tag;
    ULONG Resource;
    ULONG Property;
    ULONG Size;
} DCOMP_CMD_SET_BUFFER;

/* Value is another resource of the same channel, or 0 for none */
typedef struct _DCOMP_CMD_SET_REFERENCE
{
    ULONG Tag;
    ULONG Resource;
    ULONG Property;
    ULONG Value;
} DCOMP_CMD_SET_REFERENCE;

/* Resource types; the values Windows uses where they are known */
#define DCOMP_RESOURCE_BITMAP_SOURCE                13
#define DCOMP_RESOURCE_COMPOSITION_SURFACE_BITMAP   41
#define DCOMP_RESOURCE_GDI_SPRITE_BITMAP            63
#define DCOMP_RESOURCE_COLOR_BRUSH                  162
#define DCOMP_RESOURCE_SPRITE_VISUAL                164
#define DCOMP_RESOURCE_SURFACE_BRUSH                167
#define DCOMP_RESOURCE_WINDOW_NODE                  188
#define DCOMP_RESOURCE_VISUAL                       200
#define DCOMP_RESOURCE_HWND_TARGET                  201
#define DCOMP_RESOURCE_BACKDROP_BRUSH               1000    /* ours; the Windows value is not known */

/* Visual properties */
#define DCOMP_VISUAL_OFFSET_X           1   /* float */
#define DCOMP_VISUAL_OFFSET_Y           2   /* float */
#define DCOMP_VISUAL_OPACITY            3   /* float */
#define DCOMP_VISUAL_INSERT_CHILD       4   /* reference, placed above every existing child */
#define DCOMP_VISUAL_REMOVE_CHILD       5   /* reference */
#define DCOMP_VISUAL_CONTENT            6   /* reference */
#define DCOMP_VISUAL_SIZE               7   /* integer: width in the low dword, height in the high */

/* Window node properties, besides the visual ones: the visual a redirected window is composed as */
#define DCOMP_WINDOW_NODE_SPRITE_BITMAP 66  /* reference */
#define DCOMP_WINDOW_NODE_WINDOW        69  /* integer: the HWND */

/* GDI sprite bitmap properties: the logical surface a redirected window draws into */
#define DCOMP_SPRITE_BITMAP_DIRTY_RECTS 0   /* buffer: RECTs that changed, surface coordinates */
#define DCOMP_SPRITE_BITMAP_FORMAT      1   /* integer: DXGI_FORMAT */
#define DCOMP_SPRITE_BITMAP_SURFACE     3   /* integer: the logical surface */
#define DCOMP_SPRITE_BITMAP_MARGINS     4   /* buffer: MARGINS of the surface left out, the frame GDI drew */

/* Sprite visual properties, besides the visual ones */
#define DCOMP_SPRITE_VISUAL_BRUSH       52  /* reference: fills the visual's size */

/* Color brush properties */
#define DCOMP_COLOR_BRUSH_COLOR         0   /* buffer: four floats, premultiplied red, green, blue, alpha */

/* Surface brush properties: a rectangle of a bitmap stretched over the visual */
#define DCOMP_SURFACE_BRUSH_SURFACE     0   /* reference: a bitmap source */
#define DCOMP_SURFACE_BRUSH_SOURCE_RECT 1   /* buffer: RECT in the bitmap */
#define DCOMP_SURFACE_BRUSH_NINE_GRID   2   /* buffer: MARGINS of the source rect kept unstretched */

/* Backdrop brush properties: what is composed under the visual, blurred, with a tint over it */
#define DCOMP_BACKDROP_BRUSH_BLUR       0   /* float: standard deviation in pixels */
#define DCOMP_BACKDROP_BRUSH_TINT       1   /* buffer: four floats, premultiplied red, green, blue, alpha */

/*
 * Bitmap source properties: premultiplied 32bpp BGRA pixels the DWM itself draws, in a section
 * of the DWM process. Setting the description makes the compositor read the pixels again.
 */
#define DCOMP_BITMAP_SOURCE_SECTION     0   /* integer: the section handle */
#define DCOMP_BITMAP_SOURCE_DESC        1   /* buffer: DCOMP_BITMAP_DESC */

typedef struct _DCOMP_BITMAP_DESC
{
    ULONG Width;
    ULONG Height;
    ULONG Stride;
    ULONG Reserved;
} DCOMP_BITMAP_DESC, *PDCOMP_BITMAP_DESC;

/* HWND target properties */
#define DCOMP_TARGET_WINDOW             1   /* integer: the HWND */
#define DCOMP_TARGET_TOPMOST            2   /* integer */
#define DCOMP_TARGET_ROOT               3   /* reference */

/* Commands in a compositor batch: [ULONG Size][ULONG Command][payload], Size covers both */
#define DCOMP_MILCMD_CREATE             1   /* ULONG Type, ULONG Channel */
#define DCOMP_MILCMD_RELEASE            2
#define DCOMP_MILCMD_SET_INTEGER        3   /* ULONG Property, ULONG Reserved, ULONG64 Value */
#define DCOMP_MILCMD_SET_FLOAT          4   /* ULONG Property, FLOAT Value */
#define DCOMP_MILCMD_SET_REFERENCE      5   /* ULONG Property, ULONG Resource (compositor id, 0 for none) */
#define DCOMP_MILCMD_SET_BUFFER         6   /* ULONG Property, ULONG Size, bytes */

/* One entry of the list NtDCompositionGetConnectionBatch returns to the compositor */
#define DCOMP_BATCH_OPEN_CHANNEL        5
#define DCOMP_BATCH_CLOSE_CHANNEL       6
#define DCOMP_BATCH_RESOURCE_COMMANDS   7

typedef struct _DCOMP_BATCH_NODE
{
    ULONG Type;
    ULONG Reserved;
    ULONG64 Next;           /* user address of the next node, 0 at the end */
    ULONG Channel;          /* open and close: the channel; commands: the compositor resource id */
    ULONG Value;            /* open: the owning process id; commands: bytes of commands */
    ULONG64 Commands;       /* commands: user address of the command stream */
    ULONG64 Reserved2;
} DCOMP_BATCH_NODE, *PDCOMP_BATCH_NODE;

/* What the compositor passes to NtDCompositionCreateConnection */
typedef struct _DCOMP_COMPOSITOR_INFO
{
    HANDLE WorkEvent;       /* signalled whenever a batch is queued */
} DCOMP_COMPOSITOR_INFO, *PDCOMP_COMPOSITOR_INFO;

/* Largest command buffer a channel is given */
#define DCOMP_CHANNEL_BUFFER_SIZE       (64 * 1024)

#ifdef __cplusplus
}
#endif
