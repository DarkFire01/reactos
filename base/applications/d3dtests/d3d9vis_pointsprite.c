/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9 visual: a cloud of textured point sprites
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

/* The shared readback walks a fixed scatter, ((i*7)%w, (i*13)%h), which is fine
   for a solid object filling a good part of the frame but wrong here: point
   sprites are a few pixels across and the stride steps cleanly between them,
   the same way on every run. Read a contiguous vertical run through the middle
   of the window instead. The scene below anchors a column of sprites on the
   world x=0, z=0 line, which this camera projects exactly onto the centre
   column, so the run is guaranteed to cross them. A column and not a row
   because the camera sits above the origin looking down, and anything with
   depth drifts off the middle row. */
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

/* The first PS_SPINE points sit on the world x=0, z=0 line so the readback
   column always has sprites to find; the rest swirl around them. */
#define PS_COUNT 192
#define PS_SPINE 13

int main(int argc, char **argv)
{
    IDirect3DTexture9 *sprite = NULL;
    static DWORD pixels[32 * 32];
    static struct d9_cvertex points[PS_COUNT];
    DWORD sample[256];
    struct d9_scene s;
    struct vis_mat world;
    D3DCAPS9 caps;
    float pt_size = 0.25f, pt_min = 8.0f;
    int frame = 0, n, i, distinct;
    HWND hwnd;
    HRESULT hr;

    vis_parse_args(argc, argv);
    test_begin("d3d9vis_pointsprite");

    hwnd = vis_create_window("Direct3D 9: point sprites");
    if (!d9_open(&s, hwnd)) { skip_("no Direct3D 9 device"); goto done; }

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice9_GetDeviceCaps(s.device, &caps);
    if (caps.MaxPointSize <= 1.0f)
    {
        skip_("device caps a point at %.1f pixel(s); no point sprites here",
              caps.MaxPointSize);
        goto done;
    }
    ok_(1, "device allows points up to %.1f pixels across", caps.MaxPointSize);
    if (pt_min > caps.MaxPointSize)
        pt_min = caps.MaxPointSize;

    vis_tex_rings(pixels, 32, 32);
    sprite = d9_make_texture(s.device, pixels, 32);
    ok_(sprite != NULL, "created a 32x32 sprite texture");
    if (!sprite) goto done;

    d9_camera(&s, 6.0f);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetFVF(s.device, D9_CFVF);

    hr = IDirect3DDevice9_SetRenderState(s.device, D3DRS_POINTSPRITEENABLE, TRUE);
    if (FAILED(hr))
    {
        skip_("SetRenderState(POINTSPRITEENABLE) returned 0x%08lx", hr);
        goto done;
    }
    ok_(SUCCEEDED(hr), "SetRenderState(POINTSPRITEENABLE, TRUE) returned 0x%08lx", hr);

    hr = IDirect3DDevice9_SetRenderState(s.device, D3DRS_POINTSCALEENABLE, TRUE);
    ok_(SUCCEEDED(hr), "SetRenderState(POINTSCALEENABLE, TRUE) returned 0x%08lx", hr);

    /* With scaling on, POINTSIZE is a view-space size and the distance terms
       shrink it; POINTSIZE_MIN keeps the sprites readable if a driver ignores
       the scaling altogether. */
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_POINTSIZE, *(DWORD *)&pt_size);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_POINTSIZE_MIN, *(DWORD *)&pt_min);
    {
        float a = 0.0f, b = 0.0f, c = 1.0f;
        IDirect3DDevice9_SetRenderState(s.device, D3DRS_POINTSCALE_A, *(DWORD *)&a);
        IDirect3DDevice9_SetRenderState(s.device, D3DRS_POINTSCALE_B, *(DWORD *)&b);
        IDirect3DDevice9_SetRenderState(s.device, D3DRS_POINTSCALE_C, *(DWORD *)&c);
    }

    IDirect3DDevice9_SetTexture(s.device, 0, (IDirect3DBaseTexture9 *)sprite);
    IDirect3DDevice9_SetTextureStageState(s.device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(s.device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(s.device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetSamplerState(s.device, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(s.device, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

    vis_identity(&world);
    IDirect3DDevice9_SetTransform(s.device, D3DTS_WORLD, (D3DMATRIX *)&world);

    while (vis_frame(frame++))
    {
        float t = frame * 0.06f;

        for (i = 0; i < PS_COUNT; i++)
        {
            if (i < PS_SPINE)
            {
                points[i].x = 0.0f;
                points[i].y = -1.8f + i * (3.6f / (PS_SPINE - 1))
                            + 0.15f * (float)sin(t + i * 0.4f);
                points[i].z = 0.0f;
            }
            else
            {
                float a = i * 0.37f + t;
                float r = 0.4f + 1.8f * ((i % 19) / 18.0f);

                points[i].x = r * (float)sin(a);
                points[i].y = 1.7f * (float)sin(i * 0.13f + t * 0.8f);
                points[i].z = r * (float)cos(a);
            }
            points[i].colour = 0xff000000
                    | ((DWORD)(BYTE)(128 + 120 * sin(i * 0.21f + t)) << 16)
                    | ((DWORD)(BYTE)(128 + 120 * sin(i * 0.17f + t + 2.0f)) << 8)
                    |  (DWORD)(BYTE)(128 + 120 * sin(i * 0.11f + t + 4.0f));
        }

        IDirect3DDevice9_Clear(s.device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                               0xff101018, 1.0f, 0);
        if (SUCCEEDED(IDirect3DDevice9_BeginScene(s.device)))
        {
            hr = IDirect3DDevice9_DrawPrimitiveUP(s.device, D3DPT_POINTLIST, PS_COUNT,
                                                  points, sizeof(points[0]));
            if (frame == 1)
                ok_(SUCCEEDED(hr), "DrawPrimitiveUP(POINTLIST) returned 0x%08lx", hr);
            IDirect3DDevice9_EndScene(s.device);
        }
        IDirect3DDevice9_Present(s.device, NULL, NULL, NULL, NULL);
    }
    ok_(frame > 1, "rendered %d frames of %d point sprites", frame - 1, PS_COUNT);

    n = d9_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
    {
        vis_check_rendered(sample, n, 0xff101018);
        distinct = vis_count_distinct(sample, n, 0x00ffffff);
        ok_(distinct >= 3, "the centre column crosses %d distinct colour(s)", distinct);
    }
    else skip_("could not read the back buffer");

    IDirect3DDevice9_SetTexture(s.device, 0, NULL);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_POINTSCALEENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_POINTSPRITEENABLE, FALSE);
    vis_wait_if_held();
done:
    D3DTEST_RELEASE(sprite);
    d9_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
