/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 7 visual: a textured rotating cube
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
        for (i = 0; i < max; i++)
        {
            int x = (i * 7) % VIS_WIDTH;
            int y = (i * 13) % VIS_HEIGHT;
            out[n++] = *(DWORD *)((BYTE *)desc.lpSurface + y * desc.lPitch + x * 4);
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

static IDirectDrawSurface7 *make_texture(IDirectDraw7 *ddraw, const DWORD *pixels, int size)
{
    IDirectDrawSurface7 *texture = NULL;
    DDSURFACEDESC2 desc;
    int y;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    desc.dwWidth = desc.dwHeight = size;
    desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
    desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
    desc.ddpfPixelFormat.dwRGBBitCount = 32;
    desc.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
    desc.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
    desc.ddpfPixelFormat.dwBBitMask = 0x000000ff;
    if (FAILED(IDirectDraw7_CreateSurface(ddraw, &desc, &texture, NULL)))
        return NULL;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (SUCCEEDED(IDirectDrawSurface7_Lock(texture, NULL, &desc, DDLOCK_WAIT, NULL)))
    {
        for (y = 0; y < size; y++)
            memcpy((BYTE *)desc.lpSurface + y * desc.lPitch, pixels + y * size, size * 4);
        IDirectDrawSurface7_Unlock(texture, NULL);
    }
    return texture;
}

int main(int argc, char **argv)
{
    IDirectDrawSurface7 *texture = NULL;
    struct d7_vertex verts[VIS_CUBE_VERTICES];
    static DWORD pixels[64 * 64];
    DWORD sample[256];
    struct d7_scene s;
    struct vis_mat world, ry, rx;
    int frame = 0, n;
    HWND hwnd;

    vis_parse_args(argc, argv);
    test_begin("d3d7vis_cubetex");

    hwnd = vis_create_window("Direct3D 7: textured cube");
    if (!d7_open(&s, hwnd))
    {
        skip_("no Direct3D 7 device could be created");
        goto done;
    }

    vis_tex_checker(pixels, 64, 64, 8, 0xffff8020, 0xff2040a0);
    texture = make_texture(s.ddraw, pixels, 64);
    if (!texture)
    {
        skip_("could not create a 64x64 texture surface");
        goto done;
    }
    ok_(texture != NULL, "created a 64x64 checkerboard texture");

    d7_fill_vertices(verts, vis_cube, VIS_CUBE_VERTICES);
    d7_setup_camera(&s, 6.0f);

    IDirect3DDevice7_SetRenderState(s.device, D3DRENDERSTATE_LIGHTING, FALSE);
    IDirect3DDevice7_SetRenderState(s.device, D3DRENDERSTATE_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice7_SetRenderState(s.device, D3DRENDERSTATE_ZENABLE,
                                    s.depth ? D3DZB_TRUE : D3DZB_FALSE);
    IDirect3DDevice7_SetTexture(s.device, 0, texture);
    IDirect3DDevice7_SetTextureStageState(s.device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice7_SetTextureStageState(s.device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice7_SetTextureStageState(s.device, 0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
    IDirect3DDevice7_SetTextureStageState(s.device, 0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);

    while (vis_frame(frame++))
    {
        float t = frame * 0.045f;

        vis_rotate_y(&ry, t);
        vis_rotate_x(&rx, t * 0.4f);
        vis_mul(&world, &rx, &ry);
        IDirect3DDevice7_SetTransform(s.device, D3DTRANSFORMSTATE_WORLD, (D3DMATRIX *)&world);

        IDirect3DDevice7_Clear(s.device, 0, NULL,
                D3DCLEAR_TARGET | (s.depth ? D3DCLEAR_ZBUFFER : 0), 0x00181c28, 1.0f, 0);

        if (SUCCEEDED(IDirect3DDevice7_BeginScene(s.device)))
        {
            IDirect3DDevice7_DrawIndexedPrimitive(s.device, D3DPT_TRIANGLELIST,
                    D7_FVF, verts, VIS_CUBE_VERTICES,
                    (WORD *)vis_cube_indices, VIS_CUBE_INDICES, 0);
            IDirect3DDevice7_EndScene(s.device);
        }
        d7_present(&s);
    }
    ok_(frame > 1, "rendered %d frames of the textured cube", frame - 1);

    n = d7_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
        vis_check_rendered(sample, n, 0x00181c28);

    vis_wait_if_held();
    IDirect3DDevice7_SetTexture(s.device, 0, NULL);
done:
    D3DTEST_RELEASE(texture);
    d7_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
