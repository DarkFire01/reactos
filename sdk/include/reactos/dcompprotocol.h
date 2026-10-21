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

/*
 * Client channel commands. The tag is the first dword; layouts match the commands Windows'
 * dcomp.dll writes, so either client works against this kernel.
 */
#define DCOMP_CMD_OUT_OF_LINE                       0
#define DCOMP_CMD_ACTIVATE_CHANNEL_TRIGGER          1
#define DCOMP_CMD_CREATE_RESOURCE                   2
#define DCOMP_CMD_OPEN_SHARED_RESOURCE              3
#define DCOMP_CMD_RELEASE_RESOURCE                  4
#define DCOMP_CMD_SET_CHANNEL_INTEGER_PROPERTY      5
#define DCOMP_CMD_GET_ANIMATION_TIME                6
#define DCOMP_CMD_CAPTURE_POINTER                   7
#define DCOMP_CMD_ACTIVATE_RESOURCE_TRIGGER         8
#define DCOMP_CMD_OPEN_SHARED_RESOURCE_HANDLE       9
#define DCOMP_CMD_SET_RESOURCE_CALLBACK_ID          10
#define DCOMP_CMD_SET_INTEGER_PROPERTY              11
#define DCOMP_CMD_SET_FLOAT_PROPERTY                12
#define DCOMP_CMD_SET_HANDLE_PROPERTY               13
#define DCOMP_CMD_SET_HANDLE_ARRAY_PROPERTY         14
#define DCOMP_CMD_SET_BUFFER_PROPERTY               15
#define DCOMP_CMD_SET_REFERENCE_PROPERTY            16
#define DCOMP_CMD_SET_REFERENCE_ARRAY_PROPERTY      17
#define DCOMP_CMD_SET_ANIMATION_PROPERTY            18
#define DCOMP_CMD_SET_DELETED_NOTIFICATION_TAG      19
#define DCOMP_CMD_ADD_VISUAL_CHILD                  20
#define DCOMP_CMD_REDIRECT_MOUSE_TO_HWND            21
#define DCOMP_CMD_SET_VISUAL_INPUT_SINK             22
#define DCOMP_CMD_REMOVE_VISUAL_CHILD               23

#include <pshpack4.h>

/* The real command is in a user buffer; only allowed at the top level */
typedef struct _DCOMP_CMD_OUT_OF_LINE_BUFFER
{
    ULONG Tag;
    ULONG Reserved;
    ULONG64 Buffer;
    ULONG Size;
    ULONG Reserved2;
} DCOMP_CMD_OUT_OF_LINE_BUFFER;

typedef struct _DCOMP_CMD_CREATE
{
    ULONG Tag;
    ULONG Resource;
    ULONG Type;
    ULONG External;
} DCOMP_CMD_CREATE;

typedef struct _DCOMP_CMD_OPEN_SHARED
{
    ULONG Tag;
    ULONG Resource;
    ULONG64 Handle;
    ULONG Type;
    ULONG Write;
} DCOMP_CMD_OPEN_SHARED;

typedef struct _DCOMP_CMD_RELEASE
{
    ULONG Tag;
    ULONG Resource;
} DCOMP_CMD_RELEASE;

typedef struct _DCOMP_CMD_SET_CHANNEL_INTEGER
{
    ULONG Tag;
    ULONG Property;
    ULONG64 Value;
} DCOMP_CMD_SET_CHANNEL_INTEGER;

typedef struct _DCOMP_CMD_ANIMATION_TIME
{
    ULONG Tag;
    ULONG Animation;
    ULONG64 Requested;
    ULONG64 Answer;
} DCOMP_CMD_ANIMATION_TIME;

typedef struct _DCOMP_CMD_RESOURCE_VALUE
{
    ULONG Tag;
    ULONG Resource;
    ULONG Value;
} DCOMP_CMD_RESOURCE_VALUE;

typedef struct _DCOMP_CMD_OPEN_SHARED_HANDLE
{
    ULONG Tag;
    ULONG Resource;
    ULONG64 Handle;
} DCOMP_CMD_OPEN_SHARED_HANDLE;

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

/* Followed by Count ULONG64 handles */
typedef struct _DCOMP_CMD_SET_HANDLE_ARRAY
{
    ULONG Tag;
    ULONG Resource;
    ULONG Property;
    ULONG Count;
} DCOMP_CMD_SET_HANDLE_ARRAY;

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

/* Followed by Count ULONG resources */
typedef struct _DCOMP_CMD_SET_REFERENCE_ARRAY
{
    ULONG Tag;
    ULONG Resource;
    ULONG Property;
    ULONG Count;
} DCOMP_CMD_SET_REFERENCE_ARRAY;

/* Drives Property of Resource with Animation from now on; setting a static value unbinds it */
typedef struct _DCOMP_CMD_SET_ANIMATION
{
    ULONG Tag;
    ULONG Resource;
    ULONG Property;
    ULONG Animation;
} DCOMP_CMD_SET_ANIMATION;

typedef struct _DCOMP_CMD_SET_DELETED_TAG
{
    ULONG Tag;
    ULONG Resource;
    ULONG64 Value;
} DCOMP_CMD_SET_DELETED_TAG;

/* At the top: InsertAbove 0, Reference 0. Above a sibling: 1, sibling. At the bottom: 1, 0 */
typedef struct _DCOMP_CMD_ADD_CHILD
{
    ULONG Tag;
    ULONG Parent;
    ULONG Child;
    ULONG InsertAbove;
    ULONG Reference;
} DCOMP_CMD_ADD_CHILD;

typedef struct _DCOMP_CMD_SET_INPUT_SINK
{
    ULONG Tag;
    ULONG Visual;
    ULONG64 Sink;
} DCOMP_CMD_SET_INPUT_SINK;

/* Child 0 removes every child */
typedef struct _DCOMP_CMD_REMOVE_CHILD
{
    ULONG Tag;
    ULONG Parent;
    ULONG Child;
} DCOMP_CMD_REMOVE_CHILD;

#include <poppack.h>

#define DCOMP_REDIRECT_MOUSE_COMMAND_SIZE   72

/*
 * Resource types, the values Windows uses. The ids follow the alphabetical order of the
 * compositor's classes; abstract ones are only used for typed lookups.
 */
#define DCOMP_RESOURCE_ANIMATION                    2
#define DCOMP_RESOURCE_ANIMATION_TRIGGER            5
#define DCOMP_RESOURCE_BACKDROP_BRUSH               9
#define DCOMP_RESOURCE_BASE_ANIMATION               10  /* abstract */
#define DCOMP_RESOURCE_BITMAP_RESOURCE              13
#define DCOMP_RESOURCE_BRUSH                        17  /* abstract */
#define DCOMP_RESOURCE_CACHED_VISUAL_IMAGE          18
#define DCOMP_RESOURCE_COLOR_BRUSH                  22
#define DCOMP_RESOURCE_COMPOSITION_SURFACE_BITMAP   41
#define DCOMP_RESOURCE_CONTENT                      45  /* abstract */
#define DCOMP_RESOURCE_EFFECT                       54  /* abstract */
#define DCOMP_RESOURCE_EFFECT_GROUP                 56
#define DCOMP_RESOURCE_GDI_SPRITE_BITMAP            63
#define DCOMP_RESOURCE_GEOMETRY                     65  /* abstract */
#define DCOMP_RESOURCE_LEGACY_ANIMATION_TRIGGER     92
#define DCOMP_RESOURCE_MATRIX_TRANSFORM             107
#define DCOMP_RESOURCE_MATRIX_TRANSFORM_3D          108
#define DCOMP_RESOURCE_PRIMITIVE_GROUP              116
#define DCOMP_RESOURCE_RECTANGLE_CLIP               125
#define DCOMP_RESOURCE_RESOURCE                     133 /* abstract, matches every type */
#define DCOMP_RESOURCE_ROTATE_TRANSFORM             134
#define DCOMP_RESOURCE_ROTATE_TRANSFORM_3D          135
#define DCOMP_RESOURCE_SCALAR                       137
#define DCOMP_RESOURCE_SCALE_TRANSFORM              138
#define DCOMP_RESOURCE_SCALE_TRANSFORM_3D           139
#define DCOMP_RESOURCE_SHARED_SECTION               155
#define DCOMP_RESOURCE_SKEW_TRANSFORM               159
#define DCOMP_RESOURCE_SPRITE_VISUAL                164
#define DCOMP_RESOURCE_SURFACE_BRUSH                167
#define DCOMP_RESOURCE_TRANSFORM                    170 /* abstract */
#define DCOMP_RESOURCE_TRANSFORM_3D                 171 /* abstract */
#define DCOMP_RESOURCE_TRANSFORM_3D_GROUP           172
#define DCOMP_RESOURCE_TRANSFORM_GROUP              173
#define DCOMP_RESOURCE_TRANSLATE_TRANSFORM          174
#define DCOMP_RESOURCE_TRANSLATE_TRANSFORM_3D       175
#define DCOMP_RESOURCE_VISUAL                       180
#define DCOMP_RESOURCE_WINDOW_NODE                  188
#define DCOMP_RESOURCE_LAST_WINDOWS                 189

/* Ours: a target for a window's client area, until targets are opened from CreateDCompositionHwndTarget */
#define DCOMP_RESOURCE_HWND_TARGET                  201

/* Property ids below 1000 are Windows'; 1000 and up are ours */

/* Visual properties */
#define DCOMP_VISUAL_OFFSET_X               1   /* float */
#define DCOMP_VISUAL_OFFSET_Y               2   /* float */
#define DCOMP_VISUAL_OFFSET_Z               3   /* float */
#define DCOMP_VISUAL_TRANSFORM              4   /* reference: Transform3D */
#define DCOMP_VISUAL_TRANSFORM_PARENT       5   /* reference */
#define DCOMP_VISUAL_EFFECT                 6   /* reference */
#define DCOMP_VISUAL_CLIP                   7   /* reference: Geometry */
#define DCOMP_VISUAL_INTERPOLATION          8   /* integer: DCOMPOSITION_BITMAP_INTERPOLATION_MODE */
#define DCOMP_VISUAL_BORDER_MODE            9   /* integer: DCOMPOSITION_BORDER_MODE */
#define DCOMP_VISUAL_COMPOSITE_MODE         10  /* integer: DCOMPOSITION_COMPOSITE_MODE */
#define DCOMP_VISUAL_CONTENT                11  /* reference */
#define DCOMP_VISUAL_TRANSFORM_MATRIX       12  /* buffer: DCOMP_MATRIX4X4 */
#define DCOMP_VISUAL_CLIP_RECT              13  /* buffer: DCOMP_RECT; right below left is no clip */
#define DCOMP_VISUAL_OPACITY_MODE           14  /* integer */
#define DCOMP_VISUAL_BACKFACE_VISIBILITY    20  /* integer */
#define DCOMP_VISUAL_DEPTH_MODE             22  /* integer */
#define DCOMP_VISUAL_WIDTH                  24  /* float */
#define DCOMP_VISUAL_HEIGHT                 25  /* float */
#define DCOMP_VISUAL_OPACITY                26  /* float, 0 to 1 */
#define DCOMP_VISUAL_OFFSET_VECTOR          28  /* buffer: three floats */
#define DCOMP_VISUAL_SIZE_VECTOR            29  /* buffer: two floats */
#define DCOMP_VISUAL_VISIBLE                38  /* integer */

/* Sprite visual: its brush fills its size */
#define DCOMP_SPRITE_VISUAL_BRUSH           52  /* reference: Brush */

/* A 4x4 matrix in rows; a 2D transform is m11 m12 0 0, m21 m22 0 0, 0 0 1 0, dx dy 0 1 */
typedef struct _DCOMP_MATRIX4X4
{
    FLOAT M[4][4];
} DCOMP_MATRIX4X4, *PDCOMP_MATRIX4X4;

/* A 2D affine transform: x' = x * M11 + y * M21 + Dx, y' = x * M12 + y * M22 + Dy */
typedef struct _DCOMP_MATRIX
{
    FLOAT M11;
    FLOAT M12;
    FLOAT M21;
    FLOAT M22;
    FLOAT Dx;
    FLOAT Dy;
} DCOMP_MATRIX, *PDCOMP_MATRIX;

typedef struct _DCOMP_RECT
{
    FLOAT Left;
    FLOAT Top;
    FLOAT Right;
    FLOAT Bottom;
} DCOMP_RECT, *PDCOMP_RECT;

/* Transforms; every float can also be driven by an animation */
#define DCOMP_TRANSLATE_OFFSET_X            1
#define DCOMP_TRANSLATE_OFFSET_Y            2
#define DCOMP_SCALE_X                       1
#define DCOMP_SCALE_Y                       2
#define DCOMP_SCALE_CENTER_X                3
#define DCOMP_SCALE_CENTER_Y                4
#define DCOMP_ROTATE_ANGLE                  1   /* degrees */
#define DCOMP_ROTATE_CENTER_X               2
#define DCOMP_ROTATE_CENTER_Y               3
#define DCOMP_SKEW_ANGLE_X                  1
#define DCOMP_SKEW_ANGLE_Y                  2
#define DCOMP_SKEW_CENTER_X                 3
#define DCOMP_SKEW_CENTER_Y                 4
#define DCOMP_MATRIX_TRANSFORM_MATRIX       7   /* buffer: DCOMP_MATRIX; elements 1 to 6 as floats */
#define DCOMP_MATRIX_TRANSFORM_3D_MATRIX    17  /* buffer: DCOMP_MATRIX4X4; elements 1 to 16 as floats */
#define DCOMP_TRANSLATE_3D_OFFSET_Z         3
#define DCOMP_SCALE_3D_CENTER_X             4   /* scale x, y, z are 1 to 3, centers 4 to 6 */
#define DCOMP_ROTATE_3D_AXIS_X              2   /* angle 1, axis 2 to 4, center 5 to 7 */
#define DCOMP_TRANSFORM_GROUP_CHILDREN      1   /* reference array */

/* Rectangle clip */
#define DCOMP_CLIP_LEFT                     5
#define DCOMP_CLIP_TOP                      6
#define DCOMP_CLIP_RIGHT                    7
#define DCOMP_CLIP_BOTTOM                   8
#define DCOMP_CLIP_RECT                     17  /* buffer: DCOMP_RECT */

/* Effect group */
#define DCOMP_EFFECT_GROUP_OPACITY          1
#define DCOMP_EFFECT_GROUP_TRANSFORM        3   /* reference: Transform3D */

/* Color brush */
#define DCOMP_COLOR_BRUSH_COLOR             0   /* buffer: four floats, premultiplied red, green, blue, alpha */

/*
 * Composition surface: a texture an application draws and the compositor samples. Windows sets
 * the handle of a dxgkrnl composition surface object that swapchains bind to. Our own surfaces
 * instead set the texture's global share handle, which any device on the adapter can open.
 */
#define DCOMP_COMPOSITION_SURFACE_HANDLE        0       /* handle: dxgkrnl composition surface */
#define DCOMP_COMPOSITION_SURFACE_DESC          1000    /* ours, buffer: DCOMP_SURFACE_DESC */
#define DCOMP_COMPOSITION_SURFACE_GENERATION    1001    /* ours, integer: changes whenever it was drawn */
#define DCOMP_COMPOSITION_SURFACE_SHARED        1002    /* ours, handle: D3D global share handle */

typedef struct _DCOMP_SURFACE_DESC
{
    ULONG Width;
    ULONG Height;
    ULONG Format;               /* DXGI_FORMAT */
    ULONG AlphaMode;            /* DXGI_ALPHA_MODE */
} DCOMP_SURFACE_DESC, *PDCOMP_SURFACE_DESC;

/* Window node properties, besides the visual ones: the visual a redirected window is composed as */
#define DCOMP_WINDOW_NODE_SPRITE_BITMAP 66      /* reference: GdiSpriteBitmap */
#define DCOMP_WINDOW_NODE_WINDOW        1002    /* ours, integer: the HWND */
#define DCOMP_WINDOW_NODE_CONTENT_SCALE 1003    /* ours, buffer: FLOAT screen pixels per surface pixel */

/*
 * Ours: MARGINS of the content the frame's glass extends under. Content there is blended by its
 * alpha, so what GDI leaves black shows the glass. A negative margin covers all of the content.
 */
#define DCOMP_WINDOW_NODE_GLASS_MARGINS 1000    /* buffer: MARGINS */

/* Ours: rectangles of the content blurred glass shows through, blended by its alpha the same way */
#define DCOMP_WINDOW_NODE_BLUR_RECTS    1001    /* buffer: RECTs in content coordinates */

/* GDI sprite bitmap properties: the logical surface a redirected window draws into */
#define DCOMP_SPRITE_BITMAP_DIRTY_RECTS 0   /* buffer: RECTs that changed, surface coordinates */
#define DCOMP_SPRITE_BITMAP_FORMAT      1   /* integer: DXGI_FORMAT */
#define DCOMP_SPRITE_BITMAP_SURFACE     3   /* integer: the logical surface */
#define DCOMP_SPRITE_BITMAP_MARGINS     4   /* buffer: MARGINS of the surface left out, the frame GDI drew */

/* Surface brush properties: a rectangle of a bitmap stretched over the visual */
#define DCOMP_SURFACE_BRUSH_SURFACE     0       /* reference: a bitmap resource */
#define DCOMP_SURFACE_BRUSH_SOURCE_RECT 1000    /* ours, buffer: RECT in the bitmap */
#define DCOMP_SURFACE_BRUSH_NINE_GRID   1001    /* ours, buffer: MARGINS of the source rect kept unstretched */

/* Ours, backdrop brush properties: what is composed under the visual, blurred, with a tint over it */
#define DCOMP_BACKDROP_BRUSH_BLUR       1000    /* float: standard deviation in pixels */
#define DCOMP_BACKDROP_BRUSH_TINT       1001    /* buffer: four floats, premultiplied red, green, blue, alpha */

/*
 * Ours, bitmap resource properties: premultiplied 32bpp BGRA pixels the DWM itself draws, in a
 * section of the DWM process. Setting the description makes the compositor read the pixels again.
 */
#define DCOMP_BITMAP_SOURCE_SECTION     1000    /* integer: the section handle */
#define DCOMP_BITMAP_SOURCE_DESC        1001    /* buffer: DCOMP_BITMAP_DESC */

typedef struct _DCOMP_BITMAP_DESC
{
    ULONG Width;
    ULONG Height;
    ULONG Stride;
    ULONG Reserved;
} DCOMP_BITMAP_DESC, *PDCOMP_BITMAP_DESC;

/* HWND target properties */
#define DCOMP_TARGET_ROOT               52      /* reference: the root visual, as on a Windows visual target */
#define DCOMP_TARGET_WINDOW             1000    /* ours, integer: the HWND */
#define DCOMP_TARGET_TOPMOST            1001    /* ours, integer */

/*
 * Ours, cached visual image properties: a visual's subtree drawn into an image other visuals show
 * as their content. The image follows the visual every frame until it is frozen.
 */
#define DCOMP_CACHED_IMAGE_VISUAL       1000    /* reference: the visual drawn */
#define DCOMP_CACHED_IMAGE_RECT         1001    /* buffer: DCOMP_RECT drawn, in the space of the visual's parent */
#define DCOMP_CACHED_IMAGE_SNAPSHOT     1002    /* buffer: RECT; drawn once right away and kept */
#define DCOMP_CACHED_IMAGE_FREEZE       1003    /* integer: keep what was drawn last */

/*
 * Animation properties. Its segments live in a shared section (DCOMP_RESOURCE_SHARED_SECTION)
 * the client writes; times are in performance counter ticks.
 */
#define DCOMP_ANIMATION_SECTION         0   /* reference: shared section */
#define DCOMP_ANIMATION_SECTION_OFFSET  1   /* integer: byte offset of the first segment */
#define DCOMP_ANIMATION_SECTION_SIZE    2   /* integer: bytes of segments */
#define DCOMP_ANIMATION_BEGIN_TIME      4   /* buffer: LONGLONG absolute begin */
#define DCOMP_ANIMATION_END_TIME        5   /* buffer: LONGLONG absolute end */
#define DCOMP_ANIMATION_FINAL_VALUE     6   /* float */
#define DCOMP_ANIMATION_TRIGGER         7   /* reference: legacy animation trigger */

/* One animation segment; Type says what the payload holds */
#define DCOMP_SEGMENT_CUBIC             1   /* value = Cubic t^3 + Quadratic t^2 + Linear t + Constant */
#define DCOMP_SEGMENT_SINUSOIDAL        2   /* value = Bias + Amplitude sin(Frequency t + Phase) */
#define DCOMP_SEGMENT_REPEAT            3   /* start over from the beginning every Duration */
#define DCOMP_SEGMENT_END               4   /* hold Final from here */
#define DCOMP_SEGMENT_REVERSE           5
#define DCOMP_SEGMENT_FREEZE            6

typedef struct _DCOMP_ANIMATION_SEGMENT
{
    ULONG Type;
    ULONG Reserved;
    LONGLONG Begin;             /* ticks from the animation's begin */
    union
    {
        struct
        {
            FLOAT Cubic;
            FLOAT Quadratic;
            FLOAT Linear;
            FLOAT Constant;
        } Polynomial;
        struct
        {
            FLOAT Bias;
            FLOAT Amplitude;
            FLOAT Frequency;    /* radians per second */
            FLOAT Phase;        /* radians */
        } Sinusoid;
        LONGLONG Duration;      /* repeat, ticks */
        FLOAT Final;            /* end */
    };
} DCOMP_ANIMATION_SEGMENT, *PDCOMP_ANIMATION_SEGMENT;

/*
 * Compositor commands in a batch: [ULONG Size][ULONG Command][ULONG Resource][payload]. Size
 * covers the whole record; the ids are Windows' MILCMD values.
 */
#define DCOMP_MILCMD_ANIMATION_SET_PRIMITIVES       9   /* section, offset, size */
#define DCOMP_MILCMD_ANIMATION_UPDATE_BEGIN_TIME    11  /* LONGLONG begin, LONGLONG end */
#define DCOMP_MILCMD_BASE_ANIMATION_ADD_BINDING     12  /* target, property */
#define DCOMP_MILCMD_BASE_ANIMATION_REMOVE_BINDING  13  /* target, property */
#define DCOMP_MILCMD_CACHED_VISUAL_IMAGE_FREEZE     21
#define DCOMP_MILCMD_CACHED_VISUAL_IMAGE_SNAPSHOT   22  /* RECT */
#define DCOMP_MILCMD_CACHED_VISUAL_IMAGE_UPDATE     23  /* DCOMP_CACHED_IMAGE_UPDATE */
#define DCOMP_MILCMD_CHANNEL_CREATE_RESOURCE        40  /* type */
#define DCOMP_MILCMD_CHANNEL_RELEASE_RESOURCE       41
#define DCOMP_MILCMD_COLOR_BRUSH_SET_COLOR          51  /* four floats */
#define DCOMP_MILCMD_COMPOSITION_SURFACE_UPDATE     116 /* ULONG64 handle, two BOOLEANs */
#define DCOMP_MILCMD_RESOURCE_SET_FLOAT             344 /* property, float */
#define DCOMP_MILCMD_RESOURCE_SET_HANDLE            345 /* property, reserved, ULONG64 */
#define DCOMP_MILCMD_RESOURCE_SET_INTEGER           346 /* property, reserved, ULONG64 */
#define DCOMP_MILCMD_RESOURCE_SET_REFERENCE         347 /* property, resource */
#define DCOMP_MILCMD_SPRITE_VISUAL_SET_BRUSH        372 /* brush */
#define DCOMP_MILCMD_SURFACE_BRUSH_SET_SURFACE      379 /* surface */
#define DCOMP_MILCMD_VISUAL_INSERT_CHILD            395 /* child, reference, BOOLEAN insert above */
#define DCOMP_MILCMD_VISUAL_REMOVE_ALL_CHILDREN     397
#define DCOMP_MILCMD_VISUAL_REMOVE_CHILD            398 /* child */
#define DCOMP_MILCMD_VISUAL_SET_CLIP                402 /* geometry */
#define DCOMP_MILCMD_VISUAL_SET_CONTENT             404 /* content */
#define DCOMP_MILCMD_VISUAL_SET_EFFECT              406 /* effect */
#define DCOMP_MILCMD_VISUAL_SET_OFFSET              411 /* three floats */
#define DCOMP_MILCMD_VISUAL_SET_OPACITY             412 /* float */
#define DCOMP_MILCMD_VISUAL_SET_SIZE                420 /* two floats */
#define DCOMP_MILCMD_VISUAL_SET_TRANSFORM           422 /* transform */
#define DCOMP_MILCMD_VISUAL_SET_VISIBLE             424 /* BOOLEAN */
#define DCOMP_MILCMD_SHARED_SECTION_UPDATE          471 /* ULONG64 section handle in the compositor, size */
#define DCOMP_MILCMD_GDI_SPRITE_BITMAP_SET_DIRTY    522 /* RECTs */
#define DCOMP_MILCMD_GDI_SPRITE_BITMAP_SET_FORMAT   524 /* DXGI_FORMAT */
#define DCOMP_MILCMD_GDI_SPRITE_BITMAP_SET_SURFACE  526 /* ULONG64 logical surface */
#define DCOMP_MILCMD_GDI_SPRITE_BITMAP_SET_MARGINS  527 /* MARGINS */
#define DCOMP_MILCMD_WINDOW_NODE_SET_SPRITE_BITMAP  668 /* sprite bitmap */
#define DCOMP_MILCMD_WINDOW_NODE_SET_WINDOW_HANDLE  671 /* ULONG64 HWND */

/* The payload of DCOMP_MILCMD_CACHED_VISUAL_IMAGE_UPDATE, after the resource */
typedef struct _DCOMP_CACHED_IMAGE_UPDATE
{
    DCOMP_RECT Rect;
    ULONG64 Reserved;
    ULONG RectResource;
    ULONG Reserved2;
    ULONG Visual;
    ULONG Reserved3[5];
} DCOMP_CACHED_IMAGE_UPDATE, *PDCOMP_CACHED_IMAGE_UPDATE;

/* Ours, for buffers and reference arrays that have no command of their own */
#define DCOMP_MILCMD_RESOURCE_SET_BUFFER            0x8000  /* property, size, bytes */
#define DCOMP_MILCMD_RESOURCE_SET_REFERENCE_ARRAY   0x8001  /* property, count, resources */

/* One entry of the list NtDCompositionGetConnectionBatch returns to the compositor */
#define DCOMP_BATCH_OPEN_CHANNEL        5
#define DCOMP_BATCH_CLOSE_CHANNEL       6
#define DCOMP_BATCH_RESOURCE_COMMANDS   7

typedef struct _DCOMP_BATCH_NODE
{
    ULONG Type;
    ULONG Reserved;
    ULONG64 Next;           /* user address of the next node, 0 at the end */
    ULONG Channel;
    ULONG Value;            /* open: the owning process id; commands: bytes of commands */
    ULONG64 Commands;       /* commands: user address of the command stream */
    ULONG64 Reserved2;
} DCOMP_BATCH_NODE, *PDCOMP_BATCH_NODE;

/* What the compositor passes to NtDCompositionCreateConnection */
typedef struct _DCOMP_COMPOSITOR_INFO
{
    HANDLE WorkEvent;       /* signalled whenever a batch is queued */
} DCOMP_COMPOSITOR_INFO, *PDCOMP_COMPOSITOR_INFO;

/* Largest command buffer a channel is given; Windows' dcomp asks for one page */
#define DCOMP_CHANNEL_BUFFER_SIZE       (64 * 1024)

/* Most channels a connection may have open, as the compositor indexes them */
#define DCOMP_MAX_CHANNELS              0x10000

/*
 * Composition frames. The compositor begins and confirms each frame it shows; dxgkrnl numbers
 * them and hands the presents to composition surfaces that made it into a frame. All times
 * are performance counter values and all durations performance counter ticks.
 */

/* An output a frame was shown on; a UniqueId of 0 matches any */
typedef struct _DCOMP_TARGET_ID
{
    LUID DisplayAdapterLuid;
    LUID RenderAdapterLuid;
    ULONG VidPnSourceId;
    ULONG VidPnTargetId;
    ULONG UniqueId;
} DCOMP_TARGET_ID, *PDCOMP_TARGET_ID;

typedef struct _DCOMP_PRESENT_STATS
{
    ULONG PresentCount;
    ULONG RefreshCount;
    ULONG VirtualRefreshCount;
    ULONG64 Time;               /* the vblank the present was shown at */
    ULONG64 VirtualTime;
} DCOMP_PRESENT_STATS, *PDCOMP_PRESENT_STATS;

/* What happened on one output in a frame */
typedef struct _DCOMP_FRAME_TARGET
{
    DCOMP_TARGET_ID Id;
    ULONG Reserved;
    ULONG OutstandingPresents;
    ULONG64 PresentTime;        /* 0 when the output did not present in this frame */
    ULONG64 VblankDuration;
    DCOMP_PRESENT_STATS Presented;  /* this frame's present */
    DCOMP_PRESENT_STATS Completed;  /* the newest present known to be on screen */
} DCOMP_FRAME_TARGET, *PDCOMP_FRAME_TARGET;

/* What NtDCompositionBeginFrame is given */
typedef struct _DCOMP_FRAME_INFO
{
    ULONG64 BaseTime;           /* the vblank before the target one */
    ULONG64 TargetTime;         /* the vblank the frame is aimed at */
    ULONG64 FramePeriod;
    ULONG64 Deadline;           /* presents made before this join the frame */
    DCOMP_TARGET_ID ClockTarget;    /* the output the compositor clock follows */
    ULONG64 VblankTime;         /* the last vblank, and the same on the virtual refresh grid */
    ULONG64 VirtualVblankTime;
    ULONG64 VblankPeriod;
    ULONG64 VirtualVblankPeriod;
    ULONG64 VblankBase;         /* presents after this wait for a later frame */
    ULONG64 VirtualVblankBase;
    UCHAR Capabilities[20];
    ULONG64 Reserved1;
    ULONG64 BatchIds;           /* user address of ULONG64 batch ids, unused by ReactOS */
    ULONG BatchIdCount;
} DCOMP_FRAME_INFO, *PDCOMP_FRAME_INFO;

/* What NtDCompositionConfirmFrame is given once a frame was presented */
typedef struct _DCOMP_FRAME_CONFIRM
{
    ULONG64 FrameId;
    ULONG64 StartTime;          /* when the compositor woke for the frame */
    ULONG64 TargetTime;
    ULONG64 FramePeriod;
    ULONG64 FlipAwayFence;
    ULONG TargetCount;
    ULONG64 Targets;            /* user address of DCOMP_FRAME_TARGET[TargetCount] */
} DCOMP_FRAME_CONFIRM, *PDCOMP_FRAME_CONFIRM;

/* The frame statistics applications read, as COMPOSITION_FRAME_STATS and COMPOSITION_TARGET_STATS */
typedef struct _DCOMP_FRAME_STATS
{
    ULONG64 StartTime;
    ULONG64 TargetTime;
    ULONG64 FramePeriod;
} DCOMP_FRAME_STATS, *PDCOMP_FRAME_STATS;

typedef struct _DCOMP_STATS
{
    ULONG PresentCount;
    ULONG RefreshCount;
    ULONG VirtualRefreshCount;
    ULONG64 Time;
} DCOMP_STATS, *PDCOMP_STATS;

typedef struct _DCOMP_TARGET_STATS
{
    ULONG OutstandingPresents;
    ULONG64 PresentTime;
    ULONG64 VblankDuration;
    DCOMP_STATS PresentedStats;
    DCOMP_STATS CompletedStats;
} DCOMP_TARGET_STATS, *PDCOMP_TARGET_STATS;

/* COMPOSITION_FRAME_ID_TYPE */
#define DCOMP_FRAME_ID_CREATED          0
#define DCOMP_FRAME_ID_CONFIRMED        1
#define DCOMP_FRAME_ID_COMPLETED        2

/* A surface update dxgkrnl writes into the token manager section; up to 10 fit */
#define DCOMP_SURFACE_UPDATE_SIZE       376

#if defined(_WIN64)
C_ASSERT(sizeof(DCOMP_TARGET_ID) == 28);
C_ASSERT(sizeof(DCOMP_FRAME_TARGET) == 120);
C_ASSERT(sizeof(DCOMP_FRAME_INFO) == 0xA0);
C_ASSERT(sizeof(DCOMP_FRAME_CONFIRM) == 0x38);
C_ASSERT(sizeof(DCOMP_TARGET_STATS) == 0x48);
#endif

#ifdef __cplusplus
}
#endif
