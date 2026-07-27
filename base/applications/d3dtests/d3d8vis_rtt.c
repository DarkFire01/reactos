/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8 visual: an octahedron drawn into a texture, then mapped onto a cube
 */


#include "d3dvis.h"
#include <d3d8.h>

struct d8_scene
{
    IDirect3D8 *d3d;
    IDirect3DDevice8 *device;
    HWND hwnd;
};

static D3DTEST_UNUSED BOOL d8_open(struct d8_scene *s, HWND hwnd)
{
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;

    memset(s, 0, sizeof(*s));
    s->hwnd = hwnd;

    if (!(s->d3d = Direct3DCreate8(D3D_SDK_VERSION)))
        return FALSE;
    if (FAILED(IDirect3D8_GetAdapterDisplayMode(s->d3d, D3DADAPTER_DEFAULT, &mode)))
        return FALSE;

    /* d3d8 needs a real back buffer format even when windowed. */
    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferWidth = VIS_WIDTH;
    pp.BackBufferHeight = VIS_HEIGHT;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D16;

    if (FAILED(IDirect3D8_CreateDevice(s->d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &s->device))
        && FAILED(IDirect3D8_CreateDevice(s->d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &s->device)))
        return FALSE;

    return TRUE;
}

static D3DTEST_UNUSED void d8_close(struct d8_scene *s)
{
    if (s->device) IDirect3DDevice8_Release(s->device);
    if (s->d3d) IDirect3D8_Release(s->d3d);
    memset(s, 0, sizeof(*s));
}

static D3DTEST_UNUSED void d8_camera(struct d8_scene *s, float dist)
{
    struct vis_mat proj, view;

    vis_perspective(&proj, 1.05f, (float)VIS_WIDTH / VIS_HEIGHT, 1.0f, 100.0f);
    vis_lookat(&view, 0.0f, 1.2f, -dist, 0.0f, 0.0f, 0.0f);
    IDirect3DDevice8_SetTransform(s->device, D3DTS_PROJECTION, (D3DMATRIX *)&proj);
    IDirect3DDevice8_SetTransform(s->device, D3DTS_VIEW, (D3DMATRIX *)&view);
}

/* Read the back buffer through a system-memory copy for verification. */
static D3DTEST_UNUSED int d8_sample(struct d8_scene *s, DWORD *out, int max)
{
    IDirect3DSurface8 *bb = NULL, *sys = NULL;
    D3DLOCKED_RECT lr;
    D3DSURFACE_DESC desc;
    int i, n = 0;

    if (FAILED(IDirect3DDevice8_GetBackBuffer(s->device, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        return 0;
    if (FAILED(IDirect3DSurface8_GetDesc(bb, &desc)))
        goto done;
    if (FAILED(IDirect3DDevice8_CreateImageSurface(s->device, desc.Width, desc.Height,
                                                   desc.Format, &sys)))
        goto done;
    if (FAILED(IDirect3DDevice8_CopyRects(s->device, bb, NULL, 0, sys, NULL)))
        goto done;
    if (FAILED(IDirect3DSurface8_LockRect(sys, &lr, NULL, D3DLOCK_READONLY)))
        goto done;

    for (i = 0; i < max; i++)
    {
        int x = (i * 7) % (int)desc.Width;
        int y = (i * 13) % (int)desc.Height;
        out[n++] = *(DWORD *)((BYTE *)lr.pBits + y * lr.Pitch + x * 4);
    }
    IDirect3DSurface8_UnlockRect(sys);

done:
    if (sys) IDirect3DSurface8_Release(sys);
    if (bb) IDirect3DSurface8_Release(bb);
    return n;
}

struct d8_vertex { float x, y, z, nx, ny, nz, u, v; };
#define D8_FVF (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)
struct d8_cvertex { float x, y, z; DWORD colour; };
#define D8_CFVF (D3DFVF_XYZ | D3DFVF_DIFFUSE)

static D3DTEST_UNUSED void d8_fill(struct d8_vertex *dst, const struct vis_vertex *src, int n)
{
    int i;
    for (i = 0; i < n; i++)
    {
        dst[i].x = src[i].x;   dst[i].y = src[i].y;   dst[i].z = src[i].z;
        dst[i].nx = src[i].nx; dst[i].ny = src[i].ny; dst[i].nz = src[i].nz;
        dst[i].u = src[i].u;   dst[i].v = src[i].v;
    }
}

static D3DTEST_UNUSED void d8_fill_colour(struct d8_cvertex *dst,
                                          const struct vis_vertex *src, int n)
{
    int i;
    for (i = 0; i < n; i++)
    {
        dst[i].x = src[i].x; dst[i].y = src[i].y; dst[i].z = src[i].z;
        dst[i].colour = src[i].colour;
    }
}

static D3DTEST_UNUSED IDirect3DTexture8 *d8_make_texture(IDirect3DDevice8 *device,
                                                         const DWORD *pixels, int size)
{
    IDirect3DTexture8 *texture = NULL;
    D3DLOCKED_RECT lr;
    int y;

    if (FAILED(IDirect3DDevice8_CreateTexture(device, size, size, 1, 0, D3DFMT_A8R8G8B8,
                                              D3DPOOL_MANAGED, &texture)))
        return NULL;
    if (SUCCEEDED(IDirect3DTexture8_LockRect(texture, 0, &lr, NULL, 0)))
    {
        for (y = 0; y < size; y++)
            memcpy((BYTE *)lr.pBits + y * lr.Pitch, pixels + y * size, size * 4);
        IDirect3DTexture8_UnlockRect(texture, 0);
    }
    return texture;
}

int main(int argc, char **argv)
{
    IDirect3DTexture8 *rt_texture = NULL;
    IDirect3DSurface8 *rt_surface = NULL, *back = NULL, *depth = NULL, *rt_depth = NULL;
    struct d8_cvertex inner[VIS_MODEL_VERTICES];
    struct d8_vertex outer[VIS_CUBE_VERTICES];
    DWORD sample[256];
    DWORD rt_clear;
    struct d8_scene s;
    struct vis_mat world, ry, rx;
    int frame = 0, n;
    HWND hwnd;
    HRESULT hr;

    vis_parse_args(argc, argv);
    test_begin("d3d8vis_rtt");

    hwnd = vis_create_window("Direct3D 8: render to texture");
    if (!d8_open(&s, hwnd)) { skip_("no Direct3D 8 device"); goto done; }

    hr = IDirect3DDevice8_CreateTexture(s.device, 256, 256, 1, D3DUSAGE_RENDERTARGET,
                                        D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &rt_texture);
    if (FAILED(hr) || !rt_texture)
    {
        skip_("no render target texture support (0x%08lx)", hr);
        goto done;
    }
    ok_(SUCCEEDED(hr), "created a 256x256 render target texture");

    hr = IDirect3DTexture8_GetSurfaceLevel(rt_texture, 0, &rt_surface);
    ok_(SUCCEEDED(hr), "GetSurfaceLevel on the render target returned 0x%08lx", hr);
    IDirect3DDevice8_GetRenderTarget(s.device, &back);
    IDirect3DDevice8_GetDepthStencilSurface(s.device, &depth);
    if (!rt_surface || !back) { skip_("could not get at the render target surfaces"); goto done; }

    /* The offscreen pass needs its own z-buffer: the back buffer's is the wrong
       size for a 256x256 target. Without one we simply turn depth off there. */
    hr = IDirect3DDevice8_CreateDepthStencilSurface(s.device, 256, 256, D3DFMT_D16,
                                                    D3DMULTISAMPLE_NONE, &rt_depth);
    if (FAILED(hr))
        info_("no 256x256 depth surface (0x%08lx); the offscreen pass runs without depth", hr);
    rt_clear = D3DCLEAR_TARGET | (rt_depth ? D3DCLEAR_ZBUFFER : 0);

    d8_fill_colour(inner, vis_model, VIS_MODEL_VERTICES);
    d8_fill(outer, vis_cube, VIS_CUBE_VERTICES);

    IDirect3DDevice8_SetRenderState(s.device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_CULLMODE, D3DCULL_CCW);

    while (vis_frame(frame++))
    {
        float t = frame * 0.05f;

        /* Pass one: the octahedron straight into the texture. */
        IDirect3DDevice8_SetRenderTarget(s.device, rt_surface, rt_depth);
        IDirect3DDevice8_SetRenderState(s.device, D3DRS_ZENABLE,
                                        rt_depth ? D3DZB_TRUE : D3DZB_FALSE);
        d8_camera(&s, 5.0f);
        IDirect3DDevice8_SetVertexShader(s.device, D8_CFVF);
        IDirect3DDevice8_SetTexture(s.device, 0, NULL);
        IDirect3DDevice8_Clear(s.device, 0, NULL, rt_clear, 0xff802020, 1.0f, 0);
        vis_rotate_y(&ry, t * 2.0f);
        vis_rotate_x(&rx, t);
        vis_mul(&world, &rx, &ry);
        IDirect3DDevice8_SetTransform(s.device, D3DTS_WORLD, (D3DMATRIX *)&world);
        if (SUCCEEDED(IDirect3DDevice8_BeginScene(s.device)))
        {
            hr = IDirect3DDevice8_DrawIndexedPrimitiveUP(s.device, D3DPT_TRIANGLELIST,
                    0, VIS_MODEL_VERTICES, VIS_MODEL_INDICES / 3, vis_model_indices,
                    D3DFMT_INDEX16, inner, sizeof(inner[0]));
            if (frame == 1)
                ok_(SUCCEEDED(hr), "the offscreen draw returned 0x%08lx", hr);
            IDirect3DDevice8_EndScene(s.device);
        }

        /* Pass two: the back buffer again, with that texture on a cube. */
        IDirect3DDevice8_SetRenderTarget(s.device, back, depth);
        IDirect3DDevice8_SetRenderState(s.device, D3DRS_ZENABLE, D3DZB_TRUE);
        d8_camera(&s, 6.0f);
        IDirect3DDevice8_SetVertexShader(s.device, D8_FVF);
        IDirect3DDevice8_SetTexture(s.device, 0, (IDirect3DBaseTexture8 *)rt_texture);
        IDirect3DDevice8_SetTextureStageState(s.device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        IDirect3DDevice8_SetTextureStageState(s.device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        IDirect3DDevice8_SetTextureStageState(s.device, 0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice8_SetTextureStageState(s.device, 0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice8_Clear(s.device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                               0xff101820, 1.0f, 0);
        vis_rotate_y(&world, t * 0.6f);
        IDirect3DDevice8_SetTransform(s.device, D3DTS_WORLD, (D3DMATRIX *)&world);
        if (SUCCEEDED(IDirect3DDevice8_BeginScene(s.device)))
        {
            IDirect3DDevice8_DrawIndexedPrimitiveUP(s.device, D3DPT_TRIANGLELIST,
                    0, VIS_CUBE_VERTICES, VIS_CUBE_INDICES / 3, vis_cube_indices,
                    D3DFMT_INDEX16, outer, sizeof(outer[0]));
            IDirect3DDevice8_EndScene(s.device);
        }
        IDirect3DDevice8_SetTexture(s.device, 0, NULL);
        IDirect3DDevice8_Present(s.device, NULL, NULL, NULL, NULL);
    }
    ok_(frame > 1, "rendered %d two-pass frames", frame - 1);

    n = d8_sample(&s, sample, ARRAYSIZE(sample));
    if (n) vis_check_rendered(sample, n, 0xff101820);
    else skip_("could not read the back buffer");

    vis_wait_if_held();
done:
    /* Put the back buffer back before anything else goes away. */
    if (s.device && back)
        IDirect3DDevice8_SetRenderTarget(s.device, back, depth);
    D3DTEST_RELEASE(rt_depth);
    D3DTEST_RELEASE(depth);
    D3DTEST_RELEASE(back);
    D3DTEST_RELEASE(rt_surface);
    D3DTEST_RELEASE(rt_texture);
    d8_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
