/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 7 visual: a cube swinging in and out of linear vertex fog
 * COPYRIGHT:   Copyright 2026 The ReactOS Project
 */


#include "d3dvis.h"
#include <ddraw.h>
#include <d3d.h>

struct d7_scene
{
    IDirectDraw7 *ddraw;
    IDirectDrawSurface7 *primary;
    IDirectDrawSurface7 *target;
    IDirectDrawSurface7 *depth;
    IDirectDrawClipper *clipper;
    IDirect3D7 *d3d;
    IDirect3DDevice7 *device;
    HWND hwnd;
};

/* Build an offscreen 3D render target plus a matching z-buffer, and present by
   blitting the target to the window. Windowed 3D through ddraw is fiddly; this
   keeps every test out of that business. */
static D3DTEST_UNUSED BOOL d7_open(struct d7_scene *s, HWND hwnd)
{
    DDSURFACEDESC2 desc;
    DDPIXELFORMAT zfmt;
    int have_z = 0;

    memset(s, 0, sizeof(*s));
    s->hwnd = hwnd;

    if (FAILED(DirectDrawCreateEx(NULL, (void **)&s->ddraw, &IID_IDirectDraw7, NULL)))
        return FALSE;
    if (FAILED(IDirectDraw7_SetCooperativeLevel(s->ddraw, hwnd, DDSCL_NORMAL)))
        return FALSE;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS;
    desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    if (FAILED(IDirectDraw7_CreateSurface(s->ddraw, &desc, &s->primary, NULL)))
        return FALSE;

    if (SUCCEEDED(IDirectDraw7_CreateClipper(s->ddraw, 0, &s->clipper, NULL)))
    {
        IDirectDrawClipper_SetHWnd(s->clipper, 0, hwnd);
        IDirectDrawSurface7_SetClipper(s->primary, s->clipper);
    }

    if (FAILED(IDirectDraw7_QueryInterface(s->ddraw, &IID_IDirect3D7, (void **)&s->d3d)))
        return FALSE;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
    desc.dwWidth = VIS_WIDTH;
    desc.dwHeight = VIS_HEIGHT;
    if (FAILED(IDirectDraw7_CreateSurface(s->ddraw, &desc, &s->target, NULL)))
        return FALSE;

    /* A z-buffer, if the runtime will give us one. */
    memset(&zfmt, 0, sizeof(zfmt));
    {
        struct { DDPIXELFORMAT *fmt; int *found; } ctx = { &zfmt, &have_z };
        (void)ctx;
    }
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_SYSTEMMEMORY;
    desc.dwWidth = VIS_WIDTH;
    desc.dwHeight = VIS_HEIGHT;
    desc.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    desc.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
    desc.ddpfPixelFormat.dwZBufferBitDepth = 16;
    desc.ddpfPixelFormat.dwZBitMask = 0x0000ffff;
    if (SUCCEEDED(IDirectDraw7_CreateSurface(s->ddraw, &desc, &s->depth, NULL)))
        IDirectDrawSurface7_AddAttachedSurface(s->target, s->depth);

    if (FAILED(IDirect3D7_CreateDevice(s->d3d, &IID_IDirect3DHALDevice, s->target, &s->device))
        && FAILED(IDirect3D7_CreateDevice(s->d3d, &IID_IDirect3DRGBDevice, s->target, &s->device)))
        return FALSE;

    return TRUE;
}

static D3DTEST_UNUSED void d7_close(struct d7_scene *s)
{
    if (s->device) IDirect3DDevice7_Release(s->device);
    if (s->depth) IDirectDrawSurface7_Release(s->depth);
    if (s->target) IDirectDrawSurface7_Release(s->target);
    if (s->clipper) IDirectDrawClipper_Release(s->clipper);
    if (s->primary) IDirectDrawSurface7_Release(s->primary);
    if (s->d3d) IDirect3D7_Release(s->d3d);
    if (s->ddraw) IDirectDraw7_Release(s->ddraw);
    memset(s, 0, sizeof(*s));
}

static D3DTEST_UNUSED void d7_setup_camera(struct d7_scene *s, float dist)
{
    struct vis_mat proj, view;
    D3DVIEWPORT7 vp;

    memset(&vp, 0, sizeof(vp));
    vp.dwWidth = VIS_WIDTH;
    vp.dwHeight = VIS_HEIGHT;
    vp.dvMaxZ = 1.0f;
    IDirect3DDevice7_SetViewport(s->device, &vp);

    vis_perspective(&proj, 1.05f, (float)VIS_WIDTH / VIS_HEIGHT, 1.0f, 100.0f);
    vis_lookat(&view, 0.0f, 1.2f, -dist, 0.0f, 0.0f, 0.0f);
    IDirect3DDevice7_SetTransform(s->device, D3DTRANSFORMSTATE_PROJECTION, (D3DMATRIX *)&proj);
    IDirect3DDevice7_SetTransform(s->device, D3DTRANSFORMSTATE_VIEW, (D3DMATRIX *)&view);
}

static D3DTEST_UNUSED void d7_present(struct d7_scene *s)
{
    RECT dst;
    POINT tl = { 0, 0 };

    GetClientRect(s->hwnd, &dst);
    ClientToScreen(s->hwnd, &tl);
    OffsetRect(&dst, tl.x, tl.y);
    IDirectDrawSurface7_Blt(s->primary, &dst, s->target, NULL, DDBLT_WAIT, NULL);
}

/* Where the readback looks: a contiguous run straight down the middle of the
   frame. */
#define FOG_COLUMN_X   (VIS_WIDTH / 2)
#define FOG_COLUMN_TOP 64
#define FOG_COLUMN_LEN 256

/* A run down the centre column rather than the usual scatter. The cube here
   swings along z, and d7_setup_camera puts the eye above the origin looking
   down at it, so a receding cube climbs towards the horizon and walks off any
   fixed row. It never leaves the centre column, and the scatter pattern is far
   too sparse to be relied on for a subject this small at its far end. */
static D3DTEST_UNUSED int d7_sample(struct d7_scene *s, DWORD *out, int max)
{
    DDSURFACEDESC2 desc;
    int i, n = 0;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (FAILED(IDirectDrawSurface7_Lock(s->target, NULL, &desc, DDLOCK_WAIT | DDLOCK_READONLY, NULL)))
        return 0;
    if (desc.ddpfPixelFormat.dwRGBBitCount == 32)
    {
        for (i = 0; i < max && FOG_COLUMN_TOP + i < VIS_HEIGHT; i++)
        {
            out[n++] = *(DWORD *)((BYTE *)desc.lpSurface
                    + (FOG_COLUMN_TOP + i) * desc.lPitch + FOG_COLUMN_X * 4);
        }
    }
    IDirectDrawSurface7_Unlock(s->target, NULL);
    return n;
}

/* The FVF these scenes use: position, normal, one texture coordinate. */
struct d7_vertex { float x, y, z, nx, ny, nz, u, v; };
#define D7_FVF (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)

static D3DTEST_UNUSED void d7_fill_vertices(struct d7_vertex *dst,
                                            const struct vis_vertex *src, int count)
{
    int i;

    for (i = 0; i < count; i++)
    {
        dst[i].x = src[i].x;   dst[i].y = src[i].y;   dst[i].z = src[i].z;
        dst[i].nx = src[i].nx; dst[i].ny = src[i].ny; dst[i].nz = src[i].nz;
        dst[i].u = src[i].u;   dst[i].v = src[i].v;
    }
}

struct cvertex { float x, y, z; DWORD colour; };
#define CFVF (D3DFVF_XYZ | D3DFVF_DIFFUSE)

/* Deliberately none of the cube's own face colours, and deliberately not the
   clear colour either: if the fogged cube came out the same colour as the
   background, the readback could not tell "fogged out completely" from "drew
   nothing at all". */
#define FOG_CLEAR  0x00202838
#define FOG_COLOUR 0x00c0c0c0

/* Draw one frame with the cube parked at a fixed depth. */
static void draw_at_depth(struct d7_scene *s, struct cvertex *verts, float depth)
{
    struct vis_mat world, ry, tr;

    vis_rotate_y(&ry, 0.4f);
    vis_translate(&tr, 0.0f, 0.0f, depth);
    vis_mul(&world, &ry, &tr);
    IDirect3DDevice7_SetTransform(s->device, D3DTRANSFORMSTATE_WORLD, (D3DMATRIX *)&world);

    IDirect3DDevice7_Clear(s->device, 0, NULL,
            D3DCLEAR_TARGET | (s->depth ? D3DCLEAR_ZBUFFER : 0), FOG_CLEAR, 1.0f, 0);
    if (SUCCEEDED(IDirect3DDevice7_BeginScene(s->device)))
    {
        IDirect3DDevice7_DrawIndexedPrimitive(s->device, D3DPT_TRIANGLELIST,
                CFVF, verts, VIS_CUBE_VERTICES,
                (WORD *)vis_cube_indices, VIS_CUBE_INDICES, 0);
        IDirect3DDevice7_EndScene(s->device);
    }
    d7_present(s);
}

/* Average the cube's own pixels down the centre column: whatever is not the
   clear colour there is the cube, and its mean colour says how much fog it
   picked up. Returns the number of pixels that went into the average. */
static int cube_average(struct d7_scene *s, DWORD *average)
{
    DWORD pixels[FOG_COLUMN_LEN];
    unsigned int r = 0, g = 0, b = 0;
    int i, n, count = 0;

    *average = 0;
    n = d7_sample(s, pixels, (int)ARRAYSIZE(pixels));
    for (i = 0; i < n; i++)
    {
        DWORD c = pixels[i] & 0x00ffffff;

        if (c == (FOG_CLEAR & 0x00ffffff))
            continue;
        r += (c >> 16) & 0xff;
        g += (c >> 8) & 0xff;
        b += c & 0xff;
        count++;
    }
    if (!count)
        return 0;
    *average = ((r / count) << 16) | ((g / count) << 8) | (b / count);
    return count;
}

static int channel_delta(DWORD a, DWORD b)
{
    int dr = (int)((a >> 16) & 0xff) - (int)((b >> 16) & 0xff);
    int dg = (int)((a >> 8) & 0xff) - (int)((b >> 8) & 0xff);
    int db = (int)(a & 0xff) - (int)(b & 0xff);

    if (dr < 0) dr = -dr;
    if (dg < 0) dg = -dg;
    if (db < 0) db = -db;
    return dr + dg + db;
}

int main(int argc, char **argv)
{
    struct cvertex verts[VIS_CUBE_VERTICES];
    DWORD sample[256];
    struct d7_scene s;
    struct vis_mat world, ry, tr;
    D3DDEVICEDESC7 caps;
    DWORD near_avg = 0, far_avg = 0;
    float fog_start = 7.0f, fog_end = 16.0f;
    int near_count = 0, far_count = 0;
    int have_fog = 0;
    int frame = 0, i, n;
    HWND hwnd;
    HRESULT hr;

    vis_parse_args(argc, argv);
    test_begin("d3d7vis_fog");

    hwnd = vis_create_window("Direct3D 7: linear fog");
    if (!d7_open(&s, hwnd))
    {
        skip_("no Direct3D 7 device could be created");
        goto done;
    }

    memset(&caps, 0, sizeof(caps));
    if (SUCCEEDED(IDirect3DDevice7_GetCaps(s.device, &caps)))
        have_fog = (caps.dpcTriCaps.dwRasterCaps & D3DPRASTERCAPS_FOGVERTEX) != 0;
    if (have_fog)
        ok_(1, "the device advertises D3DPRASTERCAPS_FOGVERTEX");
    else
        skip_("the device does not advertise vertex fog; the scene may render unfogged");

    for (i = 0; i < VIS_CUBE_VERTICES; i++)
    {
        verts[i].x = vis_cube[i].x;
        verts[i].y = vis_cube[i].y;
        verts[i].z = vis_cube[i].z;
        verts[i].colour = vis_cube[i].colour;
    }

    d7_setup_camera(&s, 6.0f);
    IDirect3DDevice7_SetRenderState(s.device, D3DRENDERSTATE_LIGHTING, FALSE);
    IDirect3DDevice7_SetRenderState(s.device, D3DRENDERSTATE_CULLMODE, D3DCULL_CCW);
    /* The scene helper attaches a z-buffer, so the clears below have to take
       it with them or the second frame draws nothing. */
    IDirect3DDevice7_SetRenderState(s.device, D3DRENDERSTATE_ZENABLE,
                                    s.depth ? D3DZB_TRUE : D3DZB_FALSE);

    hr = IDirect3DDevice7_SetRenderState(s.device, D3DRENDERSTATE_FOGENABLE, TRUE);
    ok_(SUCCEEDED(hr), "SetRenderState(FOGENABLE, TRUE) returned 0x%08lx", hr);
    IDirect3DDevice7_SetRenderState(s.device, D3DRENDERSTATE_FOGCOLOR, FOG_COLOUR);
    IDirect3DDevice7_SetRenderState(s.device, D3DRENDERSTATE_FOGTABLEMODE, D3DFOG_NONE);
    hr = IDirect3DDevice7_SetRenderState(s.device, D3DRENDERSTATE_FOGVERTEXMODE, D3DFOG_LINEAR);
    ok_(SUCCEEDED(hr), "SetRenderState(FOGVERTEXMODE, LINEAR) returned 0x%08lx", hr);
    IDirect3DDevice7_SetRenderState(s.device, D3DRENDERSTATE_RANGEFOGENABLE, FALSE);
    IDirect3DDevice7_SetRenderState(s.device, D3DRENDERSTATE_FOGSTART, *(DWORD *)&fog_start);
    IDirect3DDevice7_SetRenderState(s.device, D3DRENDERSTATE_FOGEND, *(DWORD *)&fog_end);

    while (vis_frame(frame++))
    {
        /* Swing the cube from just in front of the fog start out past the fog
           end and back, so the fog factor sweeps its whole range. */
        float depth = 6.0f + 8.0f * (1.0f + (float)sin(frame * 0.07f));

        vis_rotate_y(&ry, frame * 0.05f);
        vis_translate(&tr, 0.0f, 0.0f, depth);
        vis_mul(&world, &ry, &tr);
        IDirect3DDevice7_SetTransform(s.device, D3DTRANSFORMSTATE_WORLD, (D3DMATRIX *)&world);

        IDirect3DDevice7_Clear(s.device, 0, NULL,
                D3DCLEAR_TARGET | (s.depth ? D3DCLEAR_ZBUFFER : 0), FOG_CLEAR, 1.0f, 0);

        if (SUCCEEDED(IDirect3DDevice7_BeginScene(s.device)))
        {
            hr = IDirect3DDevice7_DrawIndexedPrimitive(s.device, D3DPT_TRIANGLELIST,
                    CFVF, verts, VIS_CUBE_VERTICES,
                    (WORD *)vis_cube_indices, VIS_CUBE_INDICES, 0);
            if (frame == 1)
                ok_(SUCCEEDED(hr), "DrawIndexedPrimitive returned 0x%08lx", hr);
            IDirect3DDevice7_EndScene(s.device);
        }
        d7_present(&s);
    }
    ok_(frame > 1, "rendered %d fogged frames", frame - 1);

    /* Two held frames, one in front of the fog and one well past its end. */
    draw_at_depth(&s, verts, 0.0f);
    near_count = cube_average(&s, &near_avg);
    draw_at_depth(&s, verts, 14.0f);
    far_count = cube_average(&s, &far_avg);

    n = d7_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
        vis_check_rendered(sample, n, FOG_CLEAR);
    else
        skip_("could not sample the render target");

    if (!near_count || !far_count)
    {
        skip_("the cube covered %d near and %d far pixels of the sampled column",
              near_count, far_count);
    }
    else
    {
        info_("cube averages 0x%06lx near and 0x%06lx far",
              (unsigned long)near_avg, (unsigned long)far_avg);
        if (have_fog)
            ok_(channel_delta(near_avg, far_avg) > 24,
                "fog shifted the cube's colour with distance (delta %d)",
                channel_delta(near_avg, far_avg));
        else
            info_("distance changed the cube's colour by %d",
                  channel_delta(near_avg, far_avg));
    }

    IDirect3DDevice7_SetRenderState(s.device, D3DRENDERSTATE_FOGENABLE, FALSE);
    vis_wait_if_held();
done:
    d7_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
