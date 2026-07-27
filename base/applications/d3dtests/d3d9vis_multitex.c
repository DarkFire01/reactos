/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9 visual: two texture stages modulated together
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

static D3DTEST_UNUSED int d9_sample(struct d9_scene *s, DWORD *out, int max)
{
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    int i, n = 0;

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

    for (i = 0; i < max; i++)
    {
        int x = (i * 7) % (int)desc.Width;
        int y = (i * 13) % (int)desc.Height;
        out[n++] = *(DWORD *)((BYTE *)lr.pBits + y * lr.Pitch + x * 4);
    }
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

struct mtvertex { float x, y, z; float u1, v1; float u2, v2; };
#define MT_FVF (D3DFVF_XYZ | D3DFVF_TEX2)

int main(int argc, char **argv)
{
    IDirect3DTexture9 *base = NULL, *detail = NULL;
    static DWORD pixels[64 * 64];
    struct mtvertex quad[6];
    DWORD sample[256];
    struct d9_scene s;
    struct vis_mat world;
    D3DCAPS9 caps;
    int frame = 0, n, i;
    HWND hwnd;

    vis_parse_args(argc, argv);
    test_begin("d3d9vis_multitex");

    hwnd = vis_create_window("Direct3D 9: multitexturing");
    if (!d9_open(&s, hwnd)) { skip_("no Direct3D 9 device"); goto done; }

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice9_GetDeviceCaps(s.device, &caps);
    if (caps.MaxTextureBlendStages < 2 || caps.MaxSimultaneousTextures < 2)
    {
        skip_("device supports only %lu blend stage(s)", (unsigned long)caps.MaxTextureBlendStages);
        goto done;
    }

    vis_tex_gradient(pixels, 64, 64);
    base = d9_make_texture(s.device, pixels, 64);
    vis_tex_checker(pixels, 64, 64, 4, 0xffffffff, 0xff404040);
    detail = d9_make_texture(s.device, pixels, 64);
    ok_(base != NULL && detail != NULL, "created a gradient and a checker texture");
    if (!base || !detail) goto done;

    {
        static const float px[6] = { -2, 2, -2, 2, 2, -2 };
        static const float py[6] = {  2, 2, -2, 2, -2, -2 };
        for (i = 0; i < 6; i++)
        {
            quad[i].x = px[i]; quad[i].y = py[i]; quad[i].z = 0.0f;
            quad[i].u1 = (px[i] + 2) * 0.25f;
            quad[i].v1 = (2 - py[i]) * 0.25f;
            quad[i].u2 = quad[i].u1 * 3.0f;
            quad[i].v2 = quad[i].v1 * 3.0f;
        }
    }

    d9_camera(&s, 6.0f);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetFVF(s.device, MT_FVF);

    IDirect3DDevice9_SetTexture(s.device, 0, (IDirect3DBaseTexture9 *)base);
    IDirect3DDevice9_SetTextureStageState(s.device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(s.device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(s.device, 0, D3DTSS_TEXCOORDINDEX, 0);

    IDirect3DDevice9_SetTexture(s.device, 1, (IDirect3DBaseTexture9 *)detail);
    IDirect3DDevice9_SetTextureStageState(s.device, 1, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(s.device, 1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(s.device, 1, D3DTSS_COLORARG2, D3DTA_CURRENT);
    IDirect3DDevice9_SetTextureStageState(s.device, 1, D3DTSS_TEXCOORDINDEX, 1);
    ok_(1, "stage 0 selects the gradient, stage 1 modulates the checker over it");

    while (vis_frame(frame++))
    {
        vis_rotate_y(&world, (float)sin(frame * 0.04f) * 0.6f);
        IDirect3DDevice9_SetTransform(s.device, D3DTS_WORLD, (D3DMATRIX *)&world);

        IDirect3DDevice9_Clear(s.device, 0, NULL, D3DCLEAR_TARGET, 0xff101010, 1.0f, 0);
        if (SUCCEEDED(IDirect3DDevice9_BeginScene(s.device)))
        {
            IDirect3DDevice9_DrawPrimitiveUP(s.device, D3DPT_TRIANGLELIST, 2,
                                             quad, sizeof(quad[0]));
            IDirect3DDevice9_EndScene(s.device);
        }
        IDirect3DDevice9_Present(s.device, NULL, NULL, NULL, NULL);
    }
    ok_(frame > 1, "rendered %d multitextured frames", frame - 1);

    n = d9_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
    {
        vis_check_rendered(sample, n, 0xff101010);
        ok_(vis_count_distinct(sample, n, 0x00ffffff) > 4,
            "the modulated result shows %d distinct colours",
            vis_count_distinct(sample, n, 0x00ffffff));
    }

    IDirect3DDevice9_SetTexture(s.device, 0, NULL);
    IDirect3DDevice9_SetTexture(s.device, 1, NULL);
    vis_wait_if_held();
done:
    D3DTEST_RELEASE(base);
    D3DTEST_RELEASE(detail);
    d9_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
