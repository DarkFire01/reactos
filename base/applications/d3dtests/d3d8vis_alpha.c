/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8 visual: overlapping alpha-blended quads
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
    struct d8_cvertex quad[6];
    DWORD sample[256];
    struct d8_scene s;
    struct vis_mat world, ry, tr;
    int frame = 0, n, i, layer;
    HWND hwnd;

    vis_parse_args(argc, argv);
    test_begin("d3d8vis_alpha");

    hwnd = vis_create_window("Direct3D 8: alpha blending");
    if (!d8_open(&s, hwnd)) { skip_("no Direct3D 8 device"); goto done; }

    d8_camera(&s, 6.0f);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice8_SetVertexShader(s.device, D8_CFVF);
    ok_(1, "enabled SRCALPHA/INVSRCALPHA blending");

    while (vis_frame(frame++))
    {
        IDirect3DDevice8_Clear(s.device, 0, NULL, D3DCLEAR_TARGET, 0xff101010, 1.0f, 0);

        if (SUCCEEDED(IDirect3DDevice8_BeginScene(s.device)))
        {
            static const DWORD tints[3] = { 0x80ff4040, 0x8040ff40, 0x804040ff };

            for (layer = 0; layer < 3; layer++)
            {
                float off = (float)sin(frame * 0.06f + layer * 2.1f) * 1.2f;

                vis_rotate_y(&ry, frame * 0.03f + layer * 0.7f);
                vis_translate(&tr, off, off * 0.5f, layer * 0.01f);
                vis_mul(&world, &ry, &tr);
                IDirect3DDevice8_SetTransform(s.device, D3DTS_WORLD, (D3DMATRIX *)&world);

                for (i = 0; i < 6; i++)
                {
                    static const float px[6] = { -1.4f, 1.4f, -1.4f, 1.4f, 1.4f, -1.4f };
                    static const float py[6] = {  1.4f, 1.4f, -1.4f, 1.4f, -1.4f, -1.4f };
                    quad[i].x = px[i];
                    quad[i].y = py[i];
                    quad[i].z = 0.0f;
                    quad[i].colour = tints[layer];
                }
                IDirect3DDevice8_DrawPrimitiveUP(s.device, D3DPT_TRIANGLELIST, 2,
                                                 quad, sizeof(quad[0]));
            }
            IDirect3DDevice8_EndScene(s.device);
        }
        IDirect3DDevice8_Present(s.device, NULL, NULL, NULL, NULL);
    }
    ok_(frame > 1, "rendered %d blended frames", frame - 1);

    n = d8_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
    {
        vis_check_rendered(sample, n, 0xff101010);
        ok_(vis_count_distinct(sample, n, 0x00ffffff) > 2,
            "blending produced %d distinct colours",
            vis_count_distinct(sample, n, 0x00ffffff));
    }

    IDirect3DDevice8_SetRenderState(s.device, D3DRS_ALPHABLENDENABLE, FALSE);
    vis_wait_if_held();
done:
    d8_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
