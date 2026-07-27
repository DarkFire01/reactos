/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared helpers for the d3dvis_* rendering tests
 * COPYRIGHT:   Copyright 2026 The ReactOS Project
 *
 * The d3dvis_* programs actually draw something and put it on screen, unlike
 * the d3dtest_* set which mostly pokes at the API. They still verify their own
 * output, so the whole set can run unattended: each one renders a short
 * animation into a visible window and then reads the framebuffer back to
 * confirm that something other than the clear colour ended up there.
 *
 * Run one with "-hold" to keep the window up until a key is pressed, which is
 * what you want when you are looking at it rather than scripting it.
 *
 * Everything here is API-agnostic: plain floats and DWORD pixel buffers, so
 * the same maths, geometry and textures feed the ddraw, d3d7, d3d8, d3d9,
 * d3d10 and d3d11 versions of each scene.
 */

#ifndef _D3DVIS_H_
#define _D3DVIS_H_

#include "d3dtest.h"
#include <math.h>
#include <conio.h>

#ifndef VIS_WIDTH
#define VIS_WIDTH  512
#endif
#ifndef VIS_HEIGHT
#define VIS_HEIGHT 384
#endif

/* Long enough to see the motion, short enough that a hundred of these still
   finish quickly. */
#ifndef VIS_FRAMES
#define VIS_FRAMES 45
#endif

static int vis_hold;

/* ------------------------------------------------------------------ setup */

static void D3DTEST_UNUSED vis_parse_args(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-hold") || !strcmp(argv[i], "/hold"))
            vis_hold = 1;
    }
}

static HWND D3DTEST_UNUSED vis_create_window(const char *title)
{
    HWND hwnd = test_create_window(title, VIS_WIDTH, VIS_HEIGHT);

    if (hwnd)
    {
        RECT r;

        /* Size the client area, not the frame, so the back buffer and the
           window agree and nothing gets letterboxed. */
        SetRect(&r, 0, 0, VIS_WIDTH, VIS_HEIGHT);
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        test_pump();
    }
    return hwnd;
}

/* Called once per frame. Returns FALSE when the animation is done. */
static BOOL D3DTEST_UNUSED vis_frame(int frame)
{
    test_pump();
    Sleep(8);
    return frame < VIS_FRAMES;
}

static void D3DTEST_UNUSED vis_wait_if_held(void)
{
    if (!vis_hold)
        return;

    info_("holding the window open; press a key in this console to close it");
    while (!_kbhit())
    {
        test_pump();
        Sleep(16);
    }
}

/* --------------------------------------------------------------- verifying */

/* A rendered frame should not be a flat field of the clear colour. Counting
   distinct colours catches both "nothing drawn" and "everything drawn in one
   colour", which is what a broken pipeline usually produces. */
static int D3DTEST_UNUSED vis_count_distinct(const DWORD *pixels, int count, DWORD mask)
{
    DWORD seen[64];
    int found = 0;
    int i, j;

    for (i = 0; i < count; i++)
    {
        DWORD c = pixels[i] & mask;

        for (j = 0; j < found; j++)
        {
            if (seen[j] == c)
                break;
        }
        if (j == found)
        {
            if (found == (int)ARRAYSIZE(seen))
                return found;
            seen[found++] = c;
        }
    }
    return found;
}

static void D3DTEST_UNUSED vis_check_rendered(const DWORD *pixels, int count, DWORD clear)
{
    int distinct = vis_count_distinct(pixels, count, 0x00ffffff);
    int non_clear = 0;
    int i;

    for (i = 0; i < count; i++)
    {
        if ((pixels[i] & 0x00ffffff) != (clear & 0x00ffffff))
            non_clear++;
    }

    ok_(non_clear > 0, "%d of %d sampled pixels differ from the clear colour",
        non_clear, count);
    ok_(distinct >= 2, "the frame holds %d distinct colour(s)", distinct);
}

/* ------------------------------------------------------------------- maths */

/* Row-major, row-vector convention: v' = v * M, which is what D3D expects. */
struct vis_mat { float m[4][4]; };

static void D3DTEST_UNUSED vis_identity(struct vis_mat *out)
{
    memset(out, 0, sizeof(*out));
    out->m[0][0] = out->m[1][1] = out->m[2][2] = out->m[3][3] = 1.0f;
}

static void D3DTEST_UNUSED vis_mul(struct vis_mat *out, const struct vis_mat *a,
                                   const struct vis_mat *b)
{
    struct vis_mat r;
    int i, j;

    for (i = 0; i < 4; i++)
    {
        for (j = 0; j < 4; j++)
        {
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j]
                      + a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
        }
    }
    *out = r;
}

static void D3DTEST_UNUSED vis_rotate_y(struct vis_mat *out, float angle)
{
    float s = (float)sin(angle), c = (float)cos(angle);

    vis_identity(out);
    out->m[0][0] = c;
    out->m[0][2] = -s;
    out->m[2][0] = s;
    out->m[2][2] = c;
}

static void D3DTEST_UNUSED vis_rotate_x(struct vis_mat *out, float angle)
{
    float s = (float)sin(angle), c = (float)cos(angle);

    vis_identity(out);
    out->m[1][1] = c;
    out->m[1][2] = s;
    out->m[2][1] = -s;
    out->m[2][2] = c;
}

static void D3DTEST_UNUSED vis_translate(struct vis_mat *out, float x, float y, float z)
{
    vis_identity(out);
    out->m[3][0] = x;
    out->m[3][1] = y;
    out->m[3][2] = z;
}

/* Left-handed perspective, matching D3DXMatrixPerspectiveFovLH. */
static void D3DTEST_UNUSED vis_perspective(struct vis_mat *out, float fov, float aspect,
                                           float zn, float zf)
{
    float h = 1.0f / (float)tan(fov * 0.5f);
    float w = h / aspect;

    memset(out, 0, sizeof(*out));
    out->m[0][0] = w;
    out->m[1][1] = h;
    out->m[2][2] = zf / (zf - zn);
    out->m[2][3] = 1.0f;
    out->m[3][2] = -zn * zf / (zf - zn);
}

/* Left-handed look-at, matching D3DXMatrixLookAtLH. */
static void D3DTEST_UNUSED vis_lookat(struct vis_mat *out, float ex, float ey, float ez,
                                      float ax, float ay, float az)
{
    float zx = ax - ex, zy = ay - ey, zz = az - ez;
    float xx, xy, xz, yx, yy, yz, len;

    len = (float)sqrt(zx * zx + zy * zy + zz * zz);
    if (len > 0.0f) { zx /= len; zy /= len; zz /= len; }

    /* up = (0,1,0); x = up cross z */
    xx = zz * 1.0f - zy * 0.0f;
    xy = zx * 0.0f - zz * 0.0f;
    xz = zy * 0.0f - zx * 1.0f;
    len = (float)sqrt(xx * xx + xy * xy + xz * xz);
    if (len > 0.0f) { xx /= len; xy /= len; xz /= len; }

    /* y = z cross x */
    yx = zy * xz - zz * xy;
    yy = zz * xx - zx * xz;
    yz = zx * xy - zy * xx;

    vis_identity(out);
    out->m[0][0] = xx; out->m[0][1] = yx; out->m[0][2] = zx;
    out->m[1][0] = xy; out->m[1][1] = yy; out->m[1][2] = zy;
    out->m[2][0] = xz; out->m[2][1] = yz; out->m[2][2] = zz;
    out->m[3][0] = -(xx * ex + xy * ey + xz * ez);
    out->m[3][1] = -(yx * ex + yy * ey + yz * ez);
    out->m[3][2] = -(zx * ex + zy * ey + zz * ez);
}

/* ---------------------------------------------------------------- geometry */

/* Position, normal, texture coordinate and a vertex colour. Individual tests
   pick out whichever fields their vertex format needs. */
struct vis_vertex
{
    float x, y, z;
    float nx, ny, nz;
    float u, v;
    DWORD colour;
};

/* A unit cube centred on the origin. Four vertices per face so each face gets
   its own normal, its own colour and a full 0..1 texture square. */
#define VIS_CUBE_VERTICES 24
#define VIS_CUBE_INDICES  36

static const struct vis_vertex vis_cube[VIS_CUBE_VERTICES] =
{
    /* +Z (front), red */
    { -1,  1,  1,  0,  0,  1,  0, 0, 0xffff4040 },
    {  1,  1,  1,  0,  0,  1,  1, 0, 0xffff4040 },
    {  1, -1,  1,  0,  0,  1,  1, 1, 0xffff4040 },
    { -1, -1,  1,  0,  0,  1,  0, 1, 0xffff4040 },
    /* -Z (back), green */
    {  1,  1, -1,  0,  0, -1,  0, 0, 0xff40ff40 },
    { -1,  1, -1,  0,  0, -1,  1, 0, 0xff40ff40 },
    { -1, -1, -1,  0,  0, -1,  1, 1, 0xff40ff40 },
    {  1, -1, -1,  0,  0, -1,  0, 1, 0xff40ff40 },
    /* +X (right), blue */
    {  1,  1,  1,  1,  0,  0,  0, 0, 0xff4040ff },
    {  1,  1, -1,  1,  0,  0,  1, 0, 0xff4040ff },
    {  1, -1, -1,  1,  0,  0,  1, 1, 0xff4040ff },
    {  1, -1,  1,  1,  0,  0,  0, 1, 0xff4040ff },
    /* -X (left), yellow */
    { -1,  1, -1, -1,  0,  0,  0, 0, 0xffffff40 },
    { -1,  1,  1, -1,  0,  0,  1, 0, 0xffffff40 },
    { -1, -1,  1, -1,  0,  0,  1, 1, 0xffffff40 },
    { -1, -1, -1, -1,  0,  0,  0, 1, 0xffffff40 },
    /* +Y (top), cyan */
    { -1,  1, -1,  0,  1,  0,  0, 0, 0xff40ffff },
    {  1,  1, -1,  0,  1,  0,  1, 0, 0xff40ffff },
    {  1,  1,  1,  0,  1,  0,  1, 1, 0xff40ffff },
    { -1,  1,  1,  0,  1,  0,  0, 1, 0xff40ffff },
    /* -Y (bottom), magenta */
    { -1, -1,  1,  0, -1,  0,  0, 0, 0xffff40ff },
    {  1, -1,  1,  0, -1,  0,  1, 0, 0xffff40ff },
    {  1, -1, -1,  0, -1,  0,  1, 1, 0xffff40ff },
    { -1, -1, -1,  0, -1,  0,  0, 1, 0xffff40ff },
};

static const WORD vis_cube_indices[VIS_CUBE_INDICES] =
{
     0,  1,  2,   0,  2,  3,
     4,  5,  6,   4,  6,  7,
     8,  9, 10,   8, 10, 11,
    12, 13, 14,  12, 14, 15,
    16, 17, 18,  16, 18, 19,
    20, 21, 22,  20, 22, 23,
};

/* An octahedron: the simplest thing that still reads as a "model" rather than
   a box, and its faces have genuinely different normals. */
#define VIS_MODEL_VERTICES 24
#define VIS_MODEL_INDICES  24

static const struct vis_vertex vis_model[VIS_MODEL_VERTICES] =
{
    {  0,  1.4f,  0,   0.577f,  0.577f,  0.577f,  0.5f, 0, 0xffff6060 },
    {  1,  0,  0,      0.577f,  0.577f,  0.577f,  1,    1, 0xffff6060 },
    {  0,  0,  1,      0.577f,  0.577f,  0.577f,  0,    1, 0xffff6060 },

    {  0,  1.4f,  0,  -0.577f,  0.577f,  0.577f,  0.5f, 0, 0xff60ff60 },
    {  0,  0,  1,     -0.577f,  0.577f,  0.577f,  1,    1, 0xff60ff60 },
    { -1,  0,  0,     -0.577f,  0.577f,  0.577f,  0,    1, 0xff60ff60 },

    {  0,  1.4f,  0,  -0.577f,  0.577f, -0.577f,  0.5f, 0, 0xff6060ff },
    { -1,  0,  0,     -0.577f,  0.577f, -0.577f,  1,    1, 0xff6060ff },
    {  0,  0, -1,     -0.577f,  0.577f, -0.577f,  0,    1, 0xff6060ff },

    {  0,  1.4f,  0,   0.577f,  0.577f, -0.577f,  0.5f, 0, 0xffffff60 },
    {  0,  0, -1,      0.577f,  0.577f, -0.577f,  1,    1, 0xffffff60 },
    {  1,  0,  0,      0.577f,  0.577f, -0.577f,  0,    1, 0xffffff60 },

    {  0, -1.4f,  0,   0.577f, -0.577f,  0.577f,  0.5f, 0, 0xff60ffff },
    {  0,  0,  1,      0.577f, -0.577f,  0.577f,  1,    1, 0xff60ffff },
    {  1,  0,  0,      0.577f, -0.577f,  0.577f,  0,    1, 0xff60ffff },

    {  0, -1.4f,  0,  -0.577f, -0.577f,  0.577f,  0.5f, 0, 0xffff60ff },
    { -1,  0,  0,     -0.577f, -0.577f,  0.577f,  1,    1, 0xffff60ff },
    {  0,  0,  1,     -0.577f, -0.577f,  0.577f,  0,    1, 0xffff60ff },

    {  0, -1.4f,  0,  -0.577f, -0.577f, -0.577f,  0.5f, 0, 0xffc0c0ff },
    {  0,  0, -1,     -0.577f, -0.577f, -0.577f,  1,    1, 0xffc0c0ff },
    { -1,  0,  0,     -0.577f, -0.577f, -0.577f,  0,    1, 0xffc0c0ff },

    {  0, -1.4f,  0,   0.577f, -0.577f, -0.577f,  0.5f, 0, 0xffffc060 },
    {  1,  0,  0,      0.577f, -0.577f, -0.577f,  1,    1, 0xffffc060 },
    {  0,  0, -1,      0.577f, -0.577f, -0.577f,  0,    1, 0xffffc060 },
};

static const WORD vis_model_indices[VIS_MODEL_INDICES] =
{
     0,  1,  2,   3,  4,  5,   6,  7,  8,   9, 10, 11,
    12, 14, 13,  15, 17, 16,  18, 20, 19,  21, 23, 22,
};

/* ---------------------------------------------------------------- textures */

/* All generators fill an A8R8G8B8 buffer the caller owns. */

static void D3DTEST_UNUSED vis_tex_checker(DWORD *dst, int w, int h, int cell,
                                           DWORD a, DWORD b)
{
    int x, y;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            dst[y * w + x] = (((x / cell) ^ (y / cell)) & 1) ? a : b;
}

static void D3DTEST_UNUSED vis_tex_gradient(DWORD *dst, int w, int h)
{
    int x, y;

    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            BYTE r = (BYTE)(x * 255 / (w > 1 ? w - 1 : 1));
            BYTE g = (BYTE)(y * 255 / (h > 1 ? h - 1 : 1));
            BYTE b = (BYTE)(255 - r / 2 - g / 2);
            dst[y * w + x] = 0xff000000 | ((DWORD)r << 16) | ((DWORD)g << 8) | b;
        }
    }
}

/* Concentric rings: obvious under filtering and mip selection, and easy to
   recognise on a stretched or perspective-mapped surface. */
static void D3DTEST_UNUSED vis_tex_rings(DWORD *dst, int w, int h)
{
    float cx = w * 0.5f, cy = h * 0.5f;
    int x, y;

    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            float dx = x - cx, dy = y - cy;
            float d = (float)sqrt(dx * dx + dy * dy);
            int band = ((int)(d / 3.0f)) & 1;
            BYTE v = (BYTE)(255 - (int)(d * 255.0f / (cx > cy ? cx : cy)) % 256);

            dst[y * w + x] = band ? (0xff000000 | ((DWORD)v << 16) | 0x000040)
                                  : (0xff000000 | ((DWORD)(255 - v) << 8) | 0x400000);
        }
    }
}

/* A single-colour level, used to make each mip level visually distinct so a
   mipmap test can show which level was picked. */
static void D3DTEST_UNUSED vis_tex_solid(DWORD *dst, int w, int h, DWORD colour)
{
    int i;

    for (i = 0; i < w * h; i++)
        dst[i] = colour;
}

/* A plasma field, cheap and animated: good for the 2D surface tests. */
static void D3DTEST_UNUSED vis_tex_plasma(DWORD *dst, int w, int h, float t)
{
    int x, y;

    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            float v = (float)(sin(x * 0.06f + t) + sin(y * 0.05f - t)
                            + sin((x + y) * 0.04f + t * 0.7f));
            BYTE r = (BYTE)(128 + 100 * sin(v * 1.4f));
            BYTE g = (BYTE)(128 + 100 * sin(v * 1.4f + 2.0f));
            BYTE b = (BYTE)(128 + 100 * sin(v * 1.4f + 4.0f));

            dst[y * w + x] = 0xff000000 | ((DWORD)r << 16) | ((DWORD)g << 8) | b;
        }
    }
}

#endif /* _D3DVIS_H_ */
