/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9 visual: a receding plane showing mip level selection
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

int main(int argc, char **argv)
{
    IDirect3DTexture9 *texture = NULL;
    struct d9_vertex plane[6];
    static DWORD level[64 * 64];
    DWORD sample[256];
    struct d9_scene s;
    struct vis_mat world, tr, rx;
    D3DLOCKED_RECT lr;
    int frame = 0, n, i, size, mip;
    HWND hwnd;
    HRESULT hr;

    /* One flat colour per mip level, so whichever level the sampler picks is
       obvious on screen. */
    static const DWORD mip_colours[7] =
    {
        0xffff4040, 0xff40ff40, 0xff4040ff, 0xffffff40,
        0xff40ffff, 0xffff40ff, 0xffffffff,
    };

    vis_parse_args(argc, argv);
    test_begin("d3d9vis_mipmap");

    hwnd = vis_create_window("Direct3D 9: mip level selection");
    if (!d9_open(&s, hwnd)) { skip_("no Direct3D 9 device"); goto done; }

    hr = IDirect3DDevice9_CreateTexture(s.device, 64, 64, 0, 0, D3DFMT_A8R8G8B8,
                                        D3DPOOL_MANAGED, &texture, NULL);
    ok_(SUCCEEDED(hr) && texture != NULL, "CreateTexture(full mip chain) returned 0x%08lx", hr);
    if (!texture) goto done;

    ok_(IDirect3DTexture9_GetLevelCount(texture) == 7,
        "texture has %u level(s)", IDirect3DTexture9_GetLevelCount(texture));

    for (mip = 0, size = 64; size >= 1; mip++, size /= 2)
    {
        vis_tex_solid(level, size, size, mip_colours[mip % 7]);
        if (SUCCEEDED(IDirect3DTexture9_LockRect(texture, mip, &lr, NULL, 0)))
        {
            for (i = 0; i < size; i++)
                memcpy((BYTE *)lr.pBits + i * lr.Pitch, level + i * size, size * 4);
            IDirect3DTexture9_UnlockRect(texture, mip);
        }
    }
    ok_(mip == 7, "filled %d mip levels, one flat colour each", mip);

    /* A large quad in the XZ plane, tilted towards the viewer. */
    {
        static const float px[6] = { -8, 8, -8, 8, 8, -8 };
        static const float pz[6] = {  8, 8, -8, 8, -8, -8 };
        for (i = 0; i < 6; i++)
        {
            plane[i].x = px[i]; plane[i].y = 0.0f; plane[i].z = pz[i];
            plane[i].nx = 0; plane[i].ny = 1; plane[i].nz = 0;
            plane[i].u = (px[i] + 8) * 0.5f;
            plane[i].v = (pz[i] + 8) * 0.5f;
        }
    }

    d9_camera(&s, 4.0f);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetFVF(s.device, D9_FVF);
    IDirect3DDevice9_SetTexture(s.device, 0, (IDirect3DBaseTexture9 *)texture);
    IDirect3DDevice9_SetTextureStageState(s.device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(s.device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetSamplerState(s.device, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(s.device, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(s.device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    IDirect3DDevice9_SetSamplerState(s.device, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

    while (vis_frame(frame++))
    {
        vis_rotate_x(&rx, 0.35f);
        vis_translate(&tr, 0.0f, -1.0f, frame * 0.08f);
        vis_mul(&world, &rx, &tr);
        IDirect3DDevice9_SetTransform(s.device, D3DTS_WORLD, (D3DMATRIX *)&world);

        IDirect3DDevice9_Clear(s.device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                               0xff101010, 1.0f, 0);
        if (SUCCEEDED(IDirect3DDevice9_BeginScene(s.device)))
        {
            IDirect3DDevice9_DrawPrimitiveUP(s.device, D3DPT_TRIANGLELIST, 2,
                                             plane, sizeof(plane[0]));
            IDirect3DDevice9_EndScene(s.device);
        }
        IDirect3DDevice9_Present(s.device, NULL, NULL, NULL, NULL);
    }
    ok_(frame > 1, "rendered %d frames of the receding plane", frame - 1);

    n = d9_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
    {
        vis_check_rendered(sample, n, 0xff101010);
        ok_(vis_count_distinct(sample, n, 0x00ffffff) >= 2,
            "the plane shows %d distinct colours, so more than one mip level is in use",
            vis_count_distinct(sample, n, 0x00ffffff));
    }

    IDirect3DDevice9_SetTexture(s.device, 0, NULL);
    vis_wait_if_held();
done:
    D3DTEST_RELEASE(texture);
    d9_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
