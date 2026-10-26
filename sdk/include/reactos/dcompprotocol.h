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
#define DCOMP_RESOURCE_BLURRED_WALLPAPER_BRUSH      15
#define DCOMP_RESOURCE_BRUSH                        17  /* abstract */
#define DCOMP_RESOURCE_CACHED_VISUAL_IMAGE          18
#define DCOMP_RESOURCE_COLOR_BRUSH                  22
#define DCOMP_RESOURCE_COLOR_GRADIENT_STOP          23
#define DCOMP_RESOURCE_COMPILED_EFFECT_TEMPLATE     28
#define DCOMP_RESOURCE_COMPOSITION_AMBIENT_LIGHT    32
#define DCOMP_RESOURCE_COMPOSITION_DISTANT_LIGHT    33
#define DCOMP_RESOURCE_COMPOSITION_LIGHT            35  /* abstract */
#define DCOMP_RESOURCE_COMPOSITION_POINT_LIGHT      38
#define DCOMP_RESOURCE_COMPOSITION_SPOT_LIGHT       40
#define DCOMP_RESOURCE_COMPOSITION_SURFACE_BITMAP   41
#define DCOMP_RESOURCE_CONTENT                      45  /* abstract */
#define DCOMP_RESOURCE_EFFECT                       54  /* abstract */
#define DCOMP_RESOURCE_EFFECT_BRUSH                 55
#define DCOMP_RESOURCE_EFFECT_GROUP                 56
#define DCOMP_RESOURCE_GDI_SPRITE_BITMAP            63
#define DCOMP_RESOURCE_GEOMETRY                     65  /* abstract */
#define DCOMP_RESOURCE_GRADIENT_BRUSH               71  /* abstract */
#define DCOMP_RESOURCE_LEGACY_ANIMATION_TRIGGER     92
#define DCOMP_RESOURCE_LINEAR_GRADIENT_BRUSH        97
#define DCOMP_RESOURCE_MASK_BRUSH                   105
#define DCOMP_RESOURCE_MATRIX_TRANSFORM             107
#define DCOMP_RESOURCE_MATRIX_TRANSFORM_3D          108
#define DCOMP_RESOURCE_NINE_GRID_BRUSH              111
#define DCOMP_RESOURCE_PRIMITIVE_GROUP              116
#define DCOMP_RESOURCE_RADIAL_GRADIENT_BRUSH        124
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
#define DCOMP_RESOURCE_WINDOW_BACKDROP_BRUSH        187
#define DCOMP_RESOURCE_WINDOW_NODE                  188
#define DCOMP_RESOURCE_LAST_WINDOWS                 189

/* Ours: a target for a window's client area, until targets are opened from CreateDCompositionHwndTarget */
#define DCOMP_RESOURCE_HWND_TARGET                  201

/* Ours, for Windows.UI.Composition: named values, a clip by insets and one started animation */
#define DCOMP_RESOURCE_PROPERTY_SET                 210
#define DCOMP_RESOURCE_INSET_CLIP                   211
#define DCOMP_RESOURCE_COMPOSITION_ANIMATION        212

/* Windows' ids of the later Windows.UI.Composition types of visuals, shadows and animations */
#define DCOMP_RESOURCE_ANIMATION_CONTROLLER         3
#define DCOMP_RESOURCE_DROP_SHADOW                  53
#define DCOMP_RESOURCE_LAYER_VISUAL                 91
#define DCOMP_RESOURCE_PROJECTED_SHADOW_CASTER      118
#define DCOMP_RESOURCE_PROJECTED_SHADOW_RECEIVER    119
#define DCOMP_RESOURCE_PROJECTED_SHADOW             120
#define DCOMP_RESOURCE_REDIRECT_VISUAL              127
#define DCOMP_RESOURCE_VISUAL_SURFACE               186
#define DCOMP_RESOURCE_DELEGATED_INK_CANVAS         51  /* content: the trail of a delegated ink trail visual */
#define DCOMP_RESOURCE_INTERACTION_TRACKER          88

/* Windows' ids of the Windows.UI.Composition shape types; rectangle geometries are rectangle clips */
#define DCOMP_RESOURCE_CONTAINER_SHAPE              44
#define DCOMP_RESOURCE_ELLIPSE_GEOMETRY             57
#define DCOMP_RESOURCE_LINE_GEOMETRY                100
#define DCOMP_RESOURCE_PATH_GEOMETRY                113
#define DCOMP_RESOURCE_SHAPE_VISUAL                 154
#define DCOMP_RESOURCE_SPRITE_SHAPE                 163
#define DCOMP_RESOURCE_SHAPE                        177 /* abstract */
#define DCOMP_RESOURCE_VIEW_BOX                     178

/* Ours: a visual's clip by a geometry */
#define DCOMP_RESOURCE_GEOMETRIC_CLIP               255
#define DCOMP_RESOURCE_VISUAL_CAPTURE               267 /* ours: CompositionGraphicsDevice.CaptureAsync */

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

/* Ours, buffer: RECTs in the visual's space nothing outside of is drawn; none is no region, one empty RECT hides it */
#define DCOMP_VISUAL_CLIP_REGION            1100

/* Ours, the parts of a Windows.UI.Composition visual's transform; animations write them typed */
#define DCOMP_VISUAL_ANCHOR_POINT           1101    /* buffer: two floats */
#define DCOMP_VISUAL_CENTER_POINT           1102    /* buffer: three floats */
#define DCOMP_VISUAL_SCALE                  1103    /* buffer: three floats */
#define DCOMP_VISUAL_ROTATION_ANGLE         1104    /* float, radians */
#define DCOMP_VISUAL_ROTATION_AXIS          1105    /* buffer: three floats */
#define DCOMP_VISUAL_ORIENTATION            1106    /* buffer: quaternion X, Y, Z, W */
#define DCOMP_VISUAL_ROTATION_DEGREES       1107    /* float, the same angle in degrees */

/* Ours, the later visual properties; relative adjustments are in parts of the parent's size */
#define DCOMP_VISUAL_RELATIVE_OFFSET        1108    /* buffer: three floats */
#define DCOMP_VISUAL_RELATIVE_SIZE          1109    /* buffer: two floats */
#define DCOMP_VISUAL_PIXEL_SNAPPING         1111    /* integer */
#define DCOMP_VISUAL_SHADOW                 1112    /* reference: drop shadow of a sprite or a layer */
#define DCOMP_VISUAL_LAYER_EFFECT           1113    /* reference: effect brush a layer visual is drawn through */
#define DCOMP_VISUAL_REDIRECT_SOURCE        1114    /* reference: visual a redirect visual draws again */

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

/* Ours, inset clip: how far in from each edge of the clipped visual's size, floats */
#define DCOMP_INSET_CLIP_LEFT               1000
#define DCOMP_INSET_CLIP_TOP                1001
#define DCOMP_INSET_CLIP_RIGHT              1002
#define DCOMP_INSET_CLIP_BOTTOM             1003
#define DCOMP_INSET_CLIP_ABSOLUTE           1004    /* integer: the four sides are a rectangle, as a RectangleClip's */
#define DCOMP_INSET_CLIP_RADIUS             1020    /* plus the corner from the top left on, buffer: two floats */

/* Ours, the transform of inset and rectangle clips, in the clipped visual's space */
#define DCOMP_CLIP_ANCHOR_POINT             1010    /* buffer: two floats, in parts of the clip's size */
#define DCOMP_CLIP_CENTER_POINT             1011    /* buffer: two floats */
#define DCOMP_CLIP_OFFSET                   1012    /* buffer: two floats */
#define DCOMP_CLIP_ROTATION_ANGLE           1013    /* float, radians */
#define DCOMP_CLIP_SCALE                    1014    /* buffer: two floats */
#define DCOMP_CLIP_TRANSFORM_MATRIX         1015    /* buffer: six floats */
#define DCOMP_CLIP_ROTATION_DEGREES         1016    /* float, the same angle in degrees */

/* Ours, drop shadow */
#define DCOMP_DROP_SHADOW_BLUR_RADIUS       1000    /* float */
#define DCOMP_DROP_SHADOW_COLOR             1001    /* buffer: four floats, A R G B from 0 to 255 */
#define DCOMP_DROP_SHADOW_MASK              1002    /* reference: brush whose alpha casts the shadow */
#define DCOMP_DROP_SHADOW_OFFSET            1003    /* buffer: three floats */
#define DCOMP_DROP_SHADOW_OPACITY           1004    /* float */
#define DCOMP_DROP_SHADOW_SOURCE_POLICY     1005    /* integer: CompositionDropShadowSourcePolicy */

/* Ours, visual surface: a rectangle of a visual's subtree, drawn as a surface */
#define DCOMP_VISUAL_SURFACE_SOURCE         1000    /* reference: visual */
#define DCOMP_VISUAL_SURFACE_OFFSET         1001    /* buffer: two floats */
#define DCOMP_VISUAL_SURFACE_SIZE           1002    /* buffer: two floats; 0 by 0 shows nothing */

/* Ours, animation controller: an animated progress its animations follow */
#define DCOMP_CONTROLLER_PROGRESS           1000    /* float, 0 to 1 */
#define DCOMP_CONTROLLER_PROGRESS_DRIVEN    1001    /* integer: progress comes from an animation of it */

/*
 * Ours, interaction trackers. dcomp runs the tracker; the compositor works out where it is from the
 * motion dcomp last sent, so expressions read it every frame. The other values are kept for expressions.
 */
#define DCOMP_TRACKER_MOTION                1000    /* buffer: LONGLONG performance counter start, then a COMP_TRACKER_MOTION */
#define DCOMP_TRACKER_POSITION              1001    /* Vector3; an animation of it holds the tracker there */
#define DCOMP_TRACKER_SCALE                 1002    /* float */
#define DCOMP_TRACKER_MIN_POSITION          1003    /* buffer: three floats */
#define DCOMP_TRACKER_MAX_POSITION          1004    /* buffer: three floats */
#define DCOMP_TRACKER_MIN_SCALE             1005    /* float */
#define DCOMP_TRACKER_MAX_SCALE             1006    /* float */
#define DCOMP_TRACKER_NATURAL_POSITION      1007    /* buffer: three floats, the natural resting position */
#define DCOMP_TRACKER_NATURAL_SCALE         1008    /* float */
#define DCOMP_TRACKER_POSITION_VELOCITY     1009    /* read only: units per second */
#define DCOMP_TRACKER_SCALE_VELOCITY        1010    /* read only: percent per second */

/* Ours, delegated ink canvases: one trail, opaque round points joined by their hull */
#define DCOMP_INK_COLOR                     1000    /* buffer: four floats, A R G B from 0 to 1 */
#define DCOMP_INK_POINTS                    1001    /* buffer: x, y and radius of each point, as floats */

/* Projected shadow casters, receivers and the shadows that pair them under a light */
#define DCOMP_PROJECTED_CASTER_BRUSH        1000    /* reference: brush whose alpha casts, else the visual's rectangle */
#define DCOMP_PROJECTED_CASTER_VISUAL       1001    /* reference */
#define DCOMP_PROJECTED_RECEIVER_VISUAL     1000    /* reference */
#define DCOMP_PROJECTED_SHADOW_CASTERS      1000    /* reference array, bottom first */
#define DCOMP_PROJECTED_SHADOW_RECEIVERS    1001    /* reference array */
#define DCOMP_PROJECTED_SHADOW_LIGHT        1002    /* reference: the light the shadows are cast by */
#define DCOMP_PROJECTED_SHADOW_MULTIPLIER   1003    /* float: blur radius per unit of depth between caster and receiver */
#define DCOMP_PROJECTED_SHADOW_MAX_BLUR     1004    /* float */
#define DCOMP_PROJECTED_SHADOW_MIN_BLUR     1005    /* float */

/* Ours, property set: each property id the client chose holds a buffer of a ULONG value type and its floats */

/*
 * Ours, a started Windows.UI.Composition animation. The description and references come first; the
 * animation drives the target's property from the begin time on.
 */
#define DCOMP_COMP_ANIMATION_DESC           1000    /* buffer: COMP_ANIMATION_DESC and what follows it */
#define DCOMP_COMP_ANIMATION_REFERENCE      0x10000 /* reference: an object the expressions read, plus its slot */
#define DCOMP_COMP_ANIMATION_TARGET         1002    /* reference */
#define DCOMP_COMP_ANIMATION_PROPERTY       1003    /* integer: DCOMP_ANIMATED_PROPERTY */
#define DCOMP_COMP_ANIMATION_BEGIN          1004    /* buffer: LONGLONG performance counter time */
#define DCOMP_COMP_ANIMATION_CLOCK          1005    /* buffer: DCOMP_ANIMATION_CLOCK, in place of a begin time */
#define DCOMP_COMP_ANIMATION_CONTROLLER     1006    /* reference: controller whose driven progress it follows */
#define DCOMP_COMP_ANIMATION_PATHS          1007    /* buffer: a path animation's frames, a ULONG count then a ULONG size and DCOMP_PATH_* records each */

/* The animated property in the low 32 bits; a member count and up to four member indices of 4 bits each above */
#define DCOMP_ANIMATED_PROPERTY(Property, Count, Members)   ((ULONG64)(Property) | ((ULONG64)(Count) << 32) | ((ULONG64)(Members) << 36))

/* Where an animation is in its time: at Anchor it was at Offset, and it moves on at Rate unless paused */
typedef struct _DCOMP_ANIMATION_CLOCK
{
    LONGLONG Anchor;            /* performance counter time */
    LONGLONG Offset;            /* 100 ns units of the animation's time */
    FLOAT Rate;
    ULONG Paused;
} DCOMP_ANIMATION_CLOCK, *PDCOMP_ANIMATION_CLOCK;

/*
 * Composition surface: a texture an application draws and the compositor samples. Windows sets
 * the handle of a dxgkrnl composition surface object that swapchains bind to. Our own surfaces
 * instead set the texture's global share handle, which any device on the adapter can open.
 */
#define DCOMP_COMPOSITION_SURFACE_HANDLE        0       /* handle: dxgkrnl composition surface */
#define DCOMP_COMPOSITION_SURFACE_DESC          1000    /* ours, buffer: DCOMP_SURFACE_DESC */
#define DCOMP_COMPOSITION_SURFACE_GENERATION    1001    /* ours, integer: changes whenever it was drawn */
#define DCOMP_COMPOSITION_SURFACE_SHARED        1002    /* ours, handle: D3D global share handle */
#define DCOMP_COMPOSITION_SURFACE_NT_SHARED     1003    /* ours, handle: NT handle of a shared texture, opened into the compositor */

/* Visual capture properties: the subtree to draw, the surface to draw it into, and the section whose first ULONG it sets once done */
#define DCOMP_VISUAL_CAPTURE_VISUAL     0       /* reference: visual */
#define DCOMP_VISUAL_CAPTURE_SURFACE    1       /* reference: composition surface bitmap */
#define DCOMP_VISUAL_CAPTURE_SECTION    2       /* reference: shared section */

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
#define DCOMP_WINDOW_NODE_LAYERED       1004    /* ours, integer: 0, or 0x100 | constant alpha to blend a premultiplied layered surface */

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
#define DCOMP_SURFACE_BRUSH_SOURCE      1       /* buffer: RECT of the surface's pixels in the bitmap */
#define DCOMP_SURFACE_BRUSH_UPDATE      2       /* buffer: RECT of the pixels last drawn, surface coordinates */
#define DCOMP_SURFACE_BRUSH_HORIZONTAL  3       /* float: horizontal alignment ratio, 0 to 1 */
#define DCOMP_SURFACE_BRUSH_VERTICAL    4       /* float: vertical alignment ratio, 0 to 1 */
#define DCOMP_SURFACE_BRUSH_STRETCH     5       /* integer: CompositionStretch */
#define DCOMP_SURFACE_BRUSH_FILTER      6       /* integer: 0 nearest neighbor, 1 linear */
#define DCOMP_SURFACE_BRUSH_HAS_SOURCE  10      /* buffer: one BOOLEAN, the source rect is set */
#define DCOMP_SURFACE_BRUSH_ANCHOR      11      /* vector2, of the visual's size */
#define DCOMP_SURFACE_BRUSH_CENTER      12      /* vector2 */
#define DCOMP_SURFACE_BRUSH_OFFSET      13      /* vector2 */
#define DCOMP_SURFACE_BRUSH_SCALE       14      /* vector2 */
#define DCOMP_SURFACE_BRUSH_ANGLE       15      /* float: rotation in radians */
#define DCOMP_SURFACE_BRUSH_MATRIX      16      /* matrix3x2 */
#define DCOMP_SURFACE_BRUSH_SNAP        17      /* boolean: snap the content to pixels */
#define DCOMP_SURFACE_BRUSH_DEGREES     18      /* float: rotation in degrees */
#define DCOMP_SURFACE_BRUSH_LOGICAL     19      /* buffer: 4 LONGs, origin and whole size of a virtual surface */
#define DCOMP_SURFACE_BRUSH_SOURCE_RECT 1000    /* ours, buffer: RECT in the bitmap */
#define DCOMP_SURFACE_BRUSH_NINE_GRID   1001    /* ours, buffer: MARGINS of the source rect kept unstretched */

/*
 * Compiled effect template, a Windows.UI.Composition effect factory: the description lives in a
 * shared section, laid out as include/compeffects.hpp of the DWM describes.
 */
#define DCOMP_EFFECT_TEMPLATE_SECTION   0       /* reference: shared section */
#define DCOMP_EFFECT_TEMPLATE_OFFSET    1       /* integer: byte offset of the description */
#define DCOMP_EFFECT_TEMPLATE_SIZE      2       /* integer: bytes of description */
#define DCOMP_EFFECT_TEMPLATE_INPUTS    3       /* integer: graph source parameters */
#define DCOMP_EFFECT_TEMPLATE_HASH      4       /* buffer: ULONG64 identity of the description */

/* Effect brush: a template with a brush for each graph source parameter */
#define DCOMP_EFFECT_BRUSH_TEMPLATE     0       /* reference: compiled effect template */
#define DCOMP_EFFECT_BRUSH_PROPERTIES   1       /* reference: property set with the animatable values */
#define DCOMP_EFFECT_BRUSH_PROPERTY_IDS 2       /* buffer: ULONG property set id of each animatable property */
#define DCOMP_EFFECT_BRUSH_SOURCE(Index)    (0x80000000UL | ((Index) & 0x3FFFFFFF))   /* reference: brush */

/*
 * Lights, Windows' property ids. Colors are buffers of four floats, A, R, G, B from 0 to 255, as
 * animations write them; vectors are buffers of three floats; angles are radians.
 */
#define DCOMP_AMBIENT_LIGHT_COLOR               3
#define DCOMP_AMBIENT_LIGHT_INTENSITY           4
#define DCOMP_DISTANT_LIGHT_COLOR               3
#define DCOMP_DISTANT_LIGHT_COORDINATE_SPACE    4   /* reference: visual */
#define DCOMP_DISTANT_LIGHT_DIRECTION           5
#define DCOMP_DISTANT_LIGHT_INTENSITY           6
#define DCOMP_POINT_LIGHT_CONSTANT_ATTENUATION  3
#define DCOMP_POINT_LIGHT_LINEAR_ATTENUATION    4
#define DCOMP_POINT_LIGHT_QUADRATIC_ATTENUATION 5
#define DCOMP_POINT_LIGHT_COLOR                 6
#define DCOMP_POINT_LIGHT_COORDINATE_SPACE      7   /* reference: visual */
#define DCOMP_POINT_LIGHT_OFFSET                8
#define DCOMP_POINT_LIGHT_INTENSITY             9
#define DCOMP_POINT_LIGHT_MIN_CUTOFF            10
#define DCOMP_POINT_LIGHT_MAX_CUTOFF            11
#define DCOMP_SPOT_LIGHT_CONSTANT_ATTENUATION   3
#define DCOMP_SPOT_LIGHT_LINEAR_ATTENUATION     4
#define DCOMP_SPOT_LIGHT_QUADRATIC_ATTENUATION  5
#define DCOMP_SPOT_LIGHT_COORDINATE_SPACE       6   /* reference: visual */
#define DCOMP_SPOT_LIGHT_DIRECTION              7
#define DCOMP_SPOT_LIGHT_INNER_ANGLE            8
#define DCOMP_SPOT_LIGHT_INNER_DEGREES          9   /* animations only; kept as radians */
#define DCOMP_SPOT_LIGHT_INNER_COLOR            10
#define DCOMP_SPOT_LIGHT_INNER_INTENSITY        11
#define DCOMP_SPOT_LIGHT_OFFSET                 12
#define DCOMP_SPOT_LIGHT_OUTER_ANGLE            13
#define DCOMP_SPOT_LIGHT_OUTER_DEGREES          14  /* animations only; kept as radians */
#define DCOMP_SPOT_LIGHT_OUTER_COLOR            15
#define DCOMP_SPOT_LIGHT_OUTER_INTENSITY        16
#define DCOMP_SPOT_LIGHT_MIN_CUTOFF             17
#define DCOMP_SPOT_LIGHT_MAX_CUTOFF             18

/* Ours, every light: the visuals it lights with their subtrees, the ones it leaves out, and whether it is on */
#define DCOMP_LIGHT_TARGETS                     1000    /* reference array: visuals */
#define DCOMP_LIGHT_EXCLUSIONS                  1001    /* reference array: visuals */
#define DCOMP_LIGHT_ENABLED                     1002    /* integer */

/*
 * Ours, shapes. Lists of shapes are buffers of ULONG handles, bottom first. Two floats are a
 * Vector2, six a Matrix3x2 in rows; animations write the same ids typed.
 */
#define DCOMP_SHAPE_VISUAL_SHAPES       1200    /* buffer: shapes */
#define DCOMP_SHAPE_VISUAL_VIEW_BOX     1201    /* reference: view box */
#define DCOMP_SHAPE_CENTER_POINT        1210    /* buffer: two floats */
#define DCOMP_SHAPE_OFFSET              1211    /* buffer: two floats */
#define DCOMP_SHAPE_ROTATION_ANGLE      1212    /* float, radians */
#define DCOMP_SHAPE_ROTATION_DEGREES    1213    /* float, the same angle in degrees */
#define DCOMP_SHAPE_SCALE               1214    /* buffer: two floats */
#define DCOMP_SHAPE_TRANSFORM_MATRIX    1215    /* buffer: six floats */
#define DCOMP_CONTAINER_SHAPE_SHAPES    1220    /* buffer: shapes */
#define DCOMP_SPRITE_SHAPE_GEOMETRY     1230    /* reference: geometry */
#define DCOMP_SPRITE_SHAPE_FILL         1231    /* reference: brush */
#define DCOMP_SPRITE_SHAPE_STROKE       1232    /* reference: brush */
#define DCOMP_SPRITE_SHAPE_THICKNESS    1233    /* float */
#define DCOMP_SPRITE_SHAPE_MITER_LIMIT  1234    /* float, 1 or more */
#define DCOMP_SPRITE_SHAPE_DASH_OFFSET  1235    /* float */
#define DCOMP_SPRITE_SHAPE_DASH_ARRAY   1236    /* buffer: floats, dashes and gaps in strokes */
#define DCOMP_SPRITE_SHAPE_START_CAP    1237    /* integer: CompositionStrokeCap */
#define DCOMP_SPRITE_SHAPE_END_CAP      1238    /* integer: CompositionStrokeCap */
#define DCOMP_SPRITE_SHAPE_DASH_CAP     1239    /* integer: CompositionStrokeCap */
#define DCOMP_SPRITE_SHAPE_LINE_JOIN    1240    /* integer: CompositionStrokeLineJoin */
#define DCOMP_SPRITE_SHAPE_NON_SCALING  1241    /* integer: the stroke keeps its thickness under transforms */

/* Ours, geometries; a rectangle clip takes the rectangle ones as a Windows.UI.Composition rectangle */
#define DCOMP_GEOMETRY_TRIM_START       1250    /* float, 0 to 1 */
#define DCOMP_GEOMETRY_TRIM_END         1251    /* float, 0 to 1 */
#define DCOMP_GEOMETRY_TRIM_OFFSET      1252    /* float */
#define DCOMP_ELLIPSE_GEOMETRY_CENTER   1253    /* buffer: two floats */
#define DCOMP_ELLIPSE_GEOMETRY_RADIUS   1254    /* buffer: two floats */
#define DCOMP_LINE_GEOMETRY_START       1255    /* buffer: two floats */
#define DCOMP_LINE_GEOMETRY_END         1256    /* buffer: two floats */
#define DCOMP_RECTANGLE_GEOMETRY_OFFSET 1257    /* buffer: two floats */
#define DCOMP_RECTANGLE_GEOMETRY_SIZE   1258    /* buffer: two floats */
#define DCOMP_RECTANGLE_GEOMETRY_RADIUS 1259    /* buffer: two floats, the corner radius */
#define DCOMP_PATH_GEOMETRY_PATH        1260    /* buffer: DCOMP_PATH_* records */

/* Path data: a ULONG fill mode, then records that each start with a ULONG tag */
#define DCOMP_PATH_FILL_ALTERNATE       0
#define DCOMP_PATH_FILL_WINDING         1
#define DCOMP_PATH_BEGIN                0       /* x, y as floats, then a ULONG, 1 when the figure is filled */
#define DCOMP_PATH_LINE                 1       /* x, y */
#define DCOMP_PATH_BEZIER               2       /* two control points, then the end point */
#define DCOMP_PATH_END                  3       /* a ULONG, 1 when the figure is closed */

/* Ours, view box: the part of the shapes' space a shape visual or geometric clip shows */
#define DCOMP_VIEW_BOX_OFFSET           1270    /* buffer: two floats */
#define DCOMP_VIEW_BOX_SIZE             1271    /* buffer: two floats */
#define DCOMP_VIEW_BOX_STRETCH          1272    /* integer: CompositionStretch */
#define DCOMP_VIEW_BOX_HORIZONTAL       1273    /* float, 0 to 1 */
#define DCOMP_VIEW_BOX_VERTICAL         1274    /* float, 0 to 1 */

/* Ours, geometric clip; its transform parts have the shape ids */
#define DCOMP_GEOMETRIC_CLIP_GEOMETRY   1280    /* reference: geometry */
#define DCOMP_GEOMETRIC_CLIP_VIEW_BOX   1281    /* reference: view box */
#define DCOMP_GEOMETRIC_CLIP_ANCHOR     1282    /* buffer: two floats, of the clipped visual's size */

/* Ours, backdrop brush properties: what is composed under the visual, blurred, with a tint over it */
#define DCOMP_BACKDROP_BRUSH_BLUR       1000    /* float: standard deviation in pixels */
#define DCOMP_BACKDROP_BRUSH_TINT       1001    /* buffer: four floats, premultiplied red, green, blue, alpha */

/* Ours, mask brush properties: the source shown through the mask's alpha */
#define DCOMP_MASK_BRUSH_SOURCE         1000    /* reference: brush */
#define DCOMP_MASK_BRUSH_MASK           1001    /* reference: brush */

/* Ours, nine-grid brush properties; sides go left, top, right, bottom */
#define DCOMP_NINE_GRID_BRUSH_SOURCE        1000    /* reference: brush */
#define DCOMP_NINE_GRID_BRUSH_HOLLOW        1001    /* integer: the center is left out */
#define DCOMP_NINE_GRID_BRUSH_INSET(Side)   (1002 + (Side))     /* float: in the source's pixels */
#define DCOMP_NINE_GRID_BRUSH_SCALE(Side)   (1006 + (Side))     /* float: of the inset, as drawn */

/*
 * Ours, linear and radial gradient brush properties. Vectors and matrices go as buffers of floats, the
 * layout animations write them in.
 */
#define DCOMP_GRADIENT_BRUSH_STOPS      1000    /* reference array: color gradient stops */
#define DCOMP_GRADIENT_BRUSH_EXTEND     1001    /* integer: CompositionGradientExtendMode */
#define DCOMP_GRADIENT_BRUSH_SPACE      1002    /* integer: CompositionColorSpace */
#define DCOMP_GRADIENT_BRUSH_MAPPING    1003    /* integer: CompositionMappingMode */
#define DCOMP_GRADIENT_BRUSH_ANCHOR     1004    /* vector2, of the brush's size */
#define DCOMP_GRADIENT_BRUSH_CENTER     1005    /* vector2 */
#define DCOMP_GRADIENT_BRUSH_OFFSET     1006    /* vector2 */
#define DCOMP_GRADIENT_BRUSH_SCALE      1007    /* vector2 */
#define DCOMP_GRADIENT_BRUSH_ANGLE      1008    /* float: radians */
#define DCOMP_GRADIENT_BRUSH_DEGREES    1009    /* float: the angle in degrees */
#define DCOMP_GRADIENT_BRUSH_MATRIX     1010    /* matrix3x2 */
#define DCOMP_GRADIENT_BRUSH_START      1011    /* vector2: a linear brush's start, a radial brush's ellipse center */
#define DCOMP_GRADIENT_BRUSH_END        1012    /* vector2: a linear brush's end, a radial brush's ellipse radius */
#define DCOMP_GRADIENT_BRUSH_ORIGIN     1013    /* vector2: a radial brush's gradient origin offset */

/* Ours, color gradient stop properties */
#define DCOMP_GRADIENT_STOP_COLOR       1000    /* color: A, R, G, B from 0 to 255 */
#define DCOMP_GRADIENT_STOP_OFFSET      1001    /* float */

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
#define DCOMP_TARGET_BACKDROP           1002    /* ours, reference: brush drawn over the whole target, under the root */

/*
 * Ours, cached visual image properties: a visual's subtree drawn into an image other visuals show
 * as their content. The image follows the visual every frame until it is frozen.
 */
#define DCOMP_CACHED_IMAGE_VISUAL       1000    /* reference: the visual drawn */
#define DCOMP_CACHED_IMAGE_RECT         1001    /* buffer: DCOMP_RECT drawn, in the space of the visual's parent */
#define DCOMP_CACHED_IMAGE_SNAPSHOT     1002    /* buffer: RECT; drawn once right away and kept */
#define DCOMP_CACHED_IMAGE_FREEZE       1003    /* integer: keep what was drawn last */
#define DCOMP_CACHED_IMAGE_MIPMAPS      1004    /* integer: keep smaller copies too, for an image shown shrunk */

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
