/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9 visual: a cube swinging in and out of linear vertex fog
 */


#include "d3dvis.h"
#include <d3d9.h>

struct d9_scene
{
    IDirect3D9 *d3d;
    IDirect3DDevice9 *device;
    HWND hwnd;
};

static D3DTEST_UNUSED BOOL d9_open(struct d9_scene *s, HWND hwnd)
{
    D3DPRESENT_PARAMETERS pp;

    memset(s, 0, sizeof(*s));
    s->hwnd = hwnd;

    if (!(s->d3d = Direct3DCreate9(D3D_SDK_VERSION)))
        return FALSE;

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = VIS_WIDTH;
    pp.BackBufferHeight = VIS_HEIGHT;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;

    if (FAILED(IDirect3D9_CreateDevice(s->d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &s->device))
        && FAILED(IDirect3D9_CreateDevice(s->d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &s->device)))
        return FALSE;

    return TRUE;
}

static D3DTEST_UNUSED void d9_close(struct d9_scene *s)
{
    if (s->device) IDirect3DDevice9_Release(s->device);
    if (s->d3d) IDirect3D9_Release(s->d3d);
    memset(s, 0, sizeof(*s));
}

static D3DTEST_UNUSED void d9_camera(struct d9_scene *s, float dist)
{
    struct vis_mat proj, view;

    vis_perspective(&proj, 1.05f, (float)VIS_WIDTH / VIS_HEIGHT, 1.0f, 100.0f);
    vis_lookat(&view, 0.0f, 1.2f, -dist, 0.0f, 0.0f, 0.0f);
    IDirect3DDevice9_SetTransform(s->device, D3DTS_PROJECTION, (D3DMATRIX *)&proj);
    IDirect3DDevice9_SetTransform(s->device, D3DTS_VIEW, (D3DMATRIX *)&view);
}

/* The shared readback walks a fixed scatter, ((i*7)%w, (i*13)%h). That is fine
   for a cube parked at the origin, but this one is metres away by the time the
   animation ends: it shrinks to a couple of percent of the frame and the fixed
   stride then steps around it identically on every run. Read a contiguous run
   of pixels instead. It has to be the centre column and not the centre row,
   because d9_camera puts the eye above the origin looking down at it, so an
   object translated off in +z climbs toward the horizon and leaves the middle
   row entirely -- while its world x stays 0, which projects exactly onto the
   middle column. */
static D3DTEST_UNUSED int d9_sample(struct d9_scene *s, DWORD *out, int max)
{
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    int i, n = 0, x, y0;

    if (FAILED(IDirect3DDevice9_GetRenderTarget(s->device, 0, &bb)))
        return 0;
    if (FAILED(IDirect3DSurface9_GetDesc(bb, &desc)))
        goto done;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(s->device, desc.Width, desc.Height,
            desc.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)))
        goto done;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(s->device, bb, sys)))
        goto done;
    if (FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY)))
        goto done;

    x = (int)desc.Width / 2;
    y0 = ((int)desc.Height - max) / 2;
    if (y0 < 0)
        y0 = 0;
    for (i = 0; i < max && y0 + i < (int)desc.Height; i++)
        out[n++] = *(DWORD *)((BYTE *)lr.pBits + (y0 + i) * lr.Pitch + x * 4);
    IDirect3DSurface9_UnlockRect(sys);

done:
    if (sys) IDirect3DSurface9_Release(sys);
    if (bb) IDirect3DSurface9_Release(bb);
    return n;
}

struct d9_vertex { float x, y, z, nx, ny, nz, u, v; };
#define D9_FVF (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)
struct d9_cvertex { float x, y, z; DWORD colour; };
#define D9_CFVF (D3DFVF_XYZ | D3DFVF_DIFFUSE)

static D3DTEST_UNUSED void d9_fill(struct d9_vertex *dst, const struct vis_vertex *src, int n)
{
    int i;
    for (i = 0; i < n; i++)
    {
        dst[i].x = src[i].x;   dst[i].y = src[i].y;   dst[i].z = src[i].z;
        dst[i].nx = src[i].nx; dst[i].ny = src[i].ny; dst[i].nz = src[i].nz;
        dst[i].u = src[i].u;   dst[i].v = src[i].v;
    }
}

static D3DTEST_UNUSED void d9_fill_colour(struct d9_cvertex *dst,
                                          const struct vis_vertex *src, int n)
{
    int i;
    for (i = 0; i < n; i++)
    {
        dst[i].x = src[i].x; dst[i].y = src[i].y; dst[i].z = src[i].z;
        dst[i].colour = src[i].colour;
    }
}

static D3DTEST_UNUSED IDirect3DTexture9 *d9_make_texture(IDirect3DDevice9 *device,
                                                         const DWORD *pixels, int size)
{
    IDirect3DTexture9 *texture = NULL;
    D3DLOCKED_RECT lr;
    int y;

    if (FAILED(IDirect3DDevice9_CreateTexture(device, size, size, 1, 0, D3DFMT_A8R8G8B8,
                                              D3DPOOL_MANAGED, &texture, NULL)))
        return NULL;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(texture, 0, &lr, NULL, 0)))
    {
        for (y = 0; y < size; y++)
            memcpy((BYTE *)lr.pBits + y * lr.Pitch, pixels + y * size, size * 4);
        IDirect3DTexture9_UnlockRect(texture, 0);
    }
    return texture;
}

int main(int argc, char **argv)
{
    struct d9_cvertex verts[VIS_CUBE_VERTICES];
    DWORD sample[256];
    struct d9_scene s;
    struct vis_mat world, ry, tr;
    float fog_start = 4.0f, fog_end = 26.0f;
    D3DCAPS9 caps;
    int frame = 0, n;
    HWND hwnd;
    HRESULT hr;

    vis_parse_args(argc, argv);
    test_begin("d3d9vis_fog");

    hwnd = vis_create_window("Direct3D 9: linear fog");
    if (!d9_open(&s, hwnd)) { skip_("no Direct3D 9 device"); goto done; }

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice9_GetDeviceCaps(s.device, &caps);
    if (!(caps.RasterCaps & D3DPRASTERCAPS_FOGVERTEX))
        info_("device does not advertise vertex fog; the scene may render unfogged");

    d9_fill_colour(verts, vis_cube, VIS_CUBE_VERTICES);
    d9_camera(&s, 6.0f);

    IDirect3DDevice9_SetRenderState(s.device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice9_SetFVF(s.device, D9_CFVF);

    hr = IDirect3DDevice9_SetRenderState(s.device, D3DRS_FOGENABLE, TRUE);
    ok_(SUCCEEDED(hr), "SetRenderState(FOGENABLE) returned 0x%08lx", hr);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_FOGCOLOR, 0xff405060);
    hr = IDirect3DDevice9_SetRenderState(s.device, D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    ok_(SUCCEEDED(hr), "SetRenderState(FOGVERTEXMODE, LINEAR) returned 0x%08lx", hr);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_FOGSTART, *(DWORD *)&fog_start);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_FOGEND, *(DWORD *)&fog_end);

    while (vis_frame(frame++))
    {
        /* Swing the cube between near and far so the fog factor changes. */
        float depth = 6.0f + 8.0f * (1.0f + (float)sin(frame * 0.07f));

        vis_rotate_y(&ry, frame * 0.05f);
        vis_translate(&tr, 0.0f, 0.0f, depth);
        vis_mul(&world, &ry, &tr);
        IDirect3DDevice9_SetTransform(s.device, D3DTS_WORLD, (D3DMATRIX *)&world);

        /* Deliberately not the fog colour: a fully fogged cube would then be
           indistinguishable from the background and the readback below could
           not tell "drew nothing" from "drew something and fogged it out". */
        IDirect3DDevice9_Clear(s.device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                               0xff101018, 1.0f, 0);
        if (SUCCEEDED(IDirect3DDevice9_BeginScene(s.device)))
        {
            hr = IDirect3DDevice9_DrawIndexedPrimitiveUP(s.device, D3DPT_TRIANGLELIST,
                    0, VIS_CUBE_VERTICES, VIS_CUBE_INDICES / 3, vis_cube_indices,
                    D3DFMT_INDEX16, verts, sizeof(verts[0]));
            if (frame == 1)
                ok_(SUCCEEDED(hr), "DrawIndexedPrimitiveUP returned 0x%08lx", hr);
            IDirect3DDevice9_EndScene(s.device);
        }
        IDirect3DDevice9_Present(s.device, NULL, NULL, NULL, NULL);
    }
    ok_(frame > 1, "rendered %d fogged frames", frame - 1);

    n = d9_sample(&s, sample, ARRAYSIZE(sample));
    if (n) vis_check_rendered(sample, n, 0xff101018);
    else skip_("could not read the back buffer");

    IDirect3DDevice9_SetRenderState(s.device, D3DRS_FOGENABLE, FALSE);
    vis_wait_if_held();
done:
    d9_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
