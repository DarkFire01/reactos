/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8 visual: two texture stages combined with MODULATE and ADD
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

/* Two independent texture coordinate sets, so each stage can walk its own
   texture at its own rate. */
struct d8_mtvertex { float x, y, z; float u1, v1; float u2, v2; };
#define D8_MTFVF (D3DFVF_XYZ | D3DFVF_TEX2)

int main(int argc, char **argv)
{
    IDirect3DTexture8 *checker = NULL, *rings = NULL;
    static DWORD pixels[64 * 64];
    struct d8_mtvertex quad[6];
    DWORD sample[256];
    struct d8_scene s;
    struct vis_mat world;
    D3DCAPS8 caps;
    int frame = 0, n, i, distinct, op_is_add = 0;
    HWND hwnd;
    HRESULT hr;

    vis_parse_args(argc, argv);
    test_begin("d3d8vis_multitex");

    hwnd = vis_create_window("Direct3D 8: multitexturing");
    if (!d8_open(&s, hwnd)) { skip_("no Direct3D 8 device"); goto done; }

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice8_GetDeviceCaps(s.device, &caps);
    if (caps.MaxSimultaneousTextures < 2 || caps.MaxTextureBlendStages < 2)
    {
        skip_("device offers %lu simultaneous texture(s) and %lu blend stage(s)",
              (unsigned long)caps.MaxSimultaneousTextures,
              (unsigned long)caps.MaxTextureBlendStages);
        goto done;
    }
    ok_(1, "device offers %lu simultaneous textures over %lu blend stages",
        (unsigned long)caps.MaxSimultaneousTextures,
        (unsigned long)caps.MaxTextureBlendStages);

    vis_tex_checker(pixels, 64, 64, 8, 0xffffa030, 0xff203080);
    checker = d8_make_texture(s.device, pixels, 64);
    vis_tex_rings(pixels, 64, 64);
    rings = d8_make_texture(s.device, pixels, 64);
    ok_(checker != NULL && rings != NULL, "created a checker and a rings texture");
    if (!checker || !rings) goto done;

    {
        static const float px[6] = { -2.4f, 2.4f, -2.4f, 2.4f, 2.4f, -2.4f };
        static const float py[6] = {  2.4f, 2.4f, -2.4f, 2.4f, -2.4f, -2.4f };
        for (i = 0; i < 6; i++)
        {
            quad[i].x = px[i]; quad[i].y = py[i]; quad[i].z = 0.0f;
            quad[i].u1 = (px[i] + 2.4f) / 4.8f;
            quad[i].v1 = (2.4f - py[i]) / 4.8f;
            quad[i].u2 = quad[i].u1 * 2.0f;
            quad[i].v2 = quad[i].v1 * 2.0f;
        }
    }

    d8_camera(&s, 6.0f);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_LIGHTING, FALSE);
    /* Flat single quad: no depth wanted, so turn the z test off rather than
       leaving an uncleared z-buffer to reject the geometry. */
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetVertexShader(s.device, D8_MTFVF);

    IDirect3DDevice8_SetTexture(s.device, 0, (IDirect3DBaseTexture8 *)checker);
    IDirect3DDevice8_SetTextureStageState(s.device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice8_SetTextureStageState(s.device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(s.device, 0, D3DTSS_TEXCOORDINDEX, 0);
    IDirect3DDevice8_SetTextureStageState(s.device, 0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice8_SetTextureStageState(s.device, 0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);

    IDirect3DDevice8_SetTexture(s.device, 1, (IDirect3DBaseTexture8 *)rings);
    hr = IDirect3DDevice8_SetTextureStageState(s.device, 1, D3DTSS_COLOROP, D3DTOP_MODULATE);
    ok_(SUCCEEDED(hr), "SetTextureStageState(1, COLOROP, MODULATE) returned 0x%08lx", hr);
    IDirect3DDevice8_SetTextureStageState(s.device, 1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice8_SetTextureStageState(s.device, 1, D3DTSS_COLORARG2, D3DTA_CURRENT);
    IDirect3DDevice8_SetTextureStageState(s.device, 1, D3DTSS_TEXCOORDINDEX, 1);
    IDirect3DDevice8_SetTextureStageState(s.device, 1, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice8_SetTextureStageState(s.device, 1, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);

    while (vis_frame(frame++))
    {
        /* Halfway through, swap the second stage from MODULATE to ADD: the two
           operators have to produce visibly different frames from the same
           pair of textures. */
        if (!op_is_add && frame > VIS_FRAMES / 2)
        {
            hr = IDirect3DDevice8_SetTextureStageState(s.device, 1, D3DTSS_COLOROP, D3DTOP_ADD);
            ok_(SUCCEEDED(hr), "SetTextureStageState(1, COLOROP, ADD) returned 0x%08lx", hr);
            op_is_add = 1;
        }

        vis_rotate_y(&world, (float)sin(frame * 0.04f) * 0.6f);
        IDirect3DDevice8_SetTransform(s.device, D3DTS_WORLD, (D3DMATRIX *)&world);

        IDirect3DDevice8_Clear(s.device, 0, NULL, D3DCLEAR_TARGET, 0xff101010, 1.0f, 0);
        if (SUCCEEDED(IDirect3DDevice8_BeginScene(s.device)))
        {
            hr = IDirect3DDevice8_DrawPrimitiveUP(s.device, D3DPT_TRIANGLELIST, 2,
                                                  quad, sizeof(quad[0]));
            if (frame == 1)
                ok_(SUCCEEDED(hr), "DrawPrimitiveUP returned 0x%08lx", hr);
            IDirect3DDevice8_EndScene(s.device);
        }
        IDirect3DDevice8_Present(s.device, NULL, NULL, NULL, NULL);
    }
    ok_(frame > 1, "rendered %d multitextured frames", frame - 1);

    n = d8_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
    {
        vis_check_rendered(sample, n, 0xff101010);
        distinct = vis_count_distinct(sample, n, 0x00ffffff);
        ok_(distinct > 4, "the combined stages show %d distinct colours", distinct);
    }
    else skip_("could not read the back buffer");

    IDirect3DDevice8_SetTextureStageState(s.device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice8_SetTexture(s.device, 0, NULL);
    IDirect3DDevice8_SetTexture(s.device, 1, NULL);
    vis_wait_if_held();
done:
    D3DTEST_RELEASE(checker);
    D3DTEST_RELEASE(rings);
    d8_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
