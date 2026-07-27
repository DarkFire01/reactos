/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8 visual: an octahedron stamped into the stencil buffer
 */


#include "d3dvis.h"
#include <d3d8.h>

struct d8_scene
{
    IDirect3D8 *d3d;
    IDirect3DDevice8 *device;
    HWND hwnd;
    D3DFORMAT stencil_format;
};

/* The other d3d8 visual tests ask for a plain D16 depth buffer, which has no
   stencil bits at all. This one walks the depth-stencil formats that do until
   a device takes one, and records which; if none of them work the test skips
   rather than failing. */
static const D3DFORMAT d8_stencil_formats[] =
{
    D3DFMT_D24S8, D3DFMT_D24X4S4, D3DFMT_D15S1
};

static D3DTEST_UNUSED BOOL d8_open(struct d8_scene *s, HWND hwnd)
{
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;
    int i;

    memset(s, 0, sizeof(*s));
    s->hwnd = hwnd;

    if (!(s->d3d = Direct3DCreate8(D3D_SDK_VERSION)))
        return FALSE;
    if (FAILED(IDirect3D8_GetAdapterDisplayMode(s->d3d, D3DADAPTER_DEFAULT, &mode)))
        return FALSE;

    for (i = 0; i < (int)ARRAYSIZE(d8_stencil_formats); i++)
    {
        /* d3d8 needs a real back buffer format even when windowed. */
        memset(&pp, 0, sizeof(pp));
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = mode.Format;
        pp.BackBufferWidth = VIS_WIDTH;
        pp.BackBufferHeight = VIS_HEIGHT;
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = d8_stencil_formats[i];

        if (SUCCEEDED(IDirect3D8_CreateDevice(s->d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &s->device))
            || SUCCEEDED(IDirect3D8_CreateDevice(s->d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &s->device)))
        {
            s->stencil_format = d8_stencil_formats[i];
            return TRUE;
        }
    }
    return FALSE;
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

/* Already-transformed vertices, so the full-screen pass covers exactly the
   window whatever the camera is doing. */
struct d8_svertex { float x, y, z, rhw; DWORD colour; };
#define D8_SFVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

int main(int argc, char **argv)
{
    struct d8_cvertex verts[VIS_MODEL_VERTICES];
    struct d8_svertex quad[6];
    DWORD sample[256];
    struct d8_scene s;
    struct vis_mat world, ry, rx;
    D3DCAPS8 caps;
    int frame = 0, n, i, distinct;
    HWND hwnd;
    HRESULT hr;

    vis_parse_args(argc, argv);
    test_begin("d3d8vis_stencil");

    hwnd = vis_create_window("Direct3D 8: stencil mask");
    if (!d8_open(&s, hwnd))
    {
        skip_("no Direct3D 8 device with a stencil-carrying depth buffer");
        goto done;
    }
    ok_(1, "created a Direct3D 8 device with depth-stencil format %d",
        (int)s.stencil_format);

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice8_GetDeviceCaps(s.device, &caps);
    if (!(caps.StencilCaps & D3DSTENCILCAPS_REPLACE))
    {
        skip_("device advertises no stencil REPLACE support");
        goto done;
    }
    if (!(caps.PrimitiveMiscCaps & D3DPMISCCAPS_COLORWRITEENABLE))
        info_("no COLORWRITEENABLE cap; the stencil pass may also touch the colour buffer");

    d8_fill_colour(verts, vis_model, VIS_MODEL_VERTICES);

    /* The payload: a screen-filling gouraud quad. Only the part of it that
       lands on the stencilled octahedron silhouette should survive. */
    {
        static const float qx[6] = { 0.0f, (float)VIS_WIDTH, 0.0f,
                                     (float)VIS_WIDTH, (float)VIS_WIDTH, 0.0f };
        static const float qy[6] = { 0.0f, 0.0f, (float)VIS_HEIGHT,
                                     0.0f, (float)VIS_HEIGHT, (float)VIS_HEIGHT };
        static const DWORD qc[6] = { 0xffff6030, 0xff30ff60, 0xff3060ff,
                                     0xff30ff60, 0xffffe040, 0xff3060ff };
        for (i = 0; i < 6; i++)
        {
            quad[i].x = qx[i]; quad[i].y = qy[i];
            quad[i].z = 0.5f;  quad[i].rhw = 1.0f;
            quad[i].colour = qc[i];
        }
    }

    /* Close in: the surviving pixels are only the octahedron's silhouette, so
       the bigger it is on screen the more the readback below has to work with. */
    d8_camera(&s, 4.0f);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_CULLMODE, D3DCULL_NONE);
    ok_(1, "masking a full-screen quad through a rotating octahedron");

    while (vis_frame(frame++))
    {
        float t = frame * 0.05f;

        IDirect3DDevice8_Clear(s.device, 0, NULL,
                D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff181818, 1.0f, 0);

        if (SUCCEEDED(IDirect3DDevice8_BeginScene(s.device)))
        {
            /* Pass one: stamp 1 into the stencil wherever the model covers,
               writing neither colour nor depth. */
            hr = IDirect3DDevice8_SetRenderState(s.device, D3DRS_STENCILENABLE, TRUE);
            if (frame == 1)
                ok_(SUCCEEDED(hr), "SetRenderState(STENCILENABLE) returned 0x%08lx", hr);
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_STENCILREF, 1);
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_STENCILMASK, 0xff);
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_STENCILWRITEMASK, 0xff);
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_COLORWRITEENABLE, 0);
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_ZWRITEENABLE, FALSE);
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_ZENABLE, D3DZB_TRUE);
            IDirect3DDevice8_SetVertexShader(s.device, D8_CFVF);

            vis_rotate_y(&ry, t);
            vis_rotate_x(&rx, t * 0.6f);
            vis_mul(&world, &rx, &ry);
            IDirect3DDevice8_SetTransform(s.device, D3DTS_WORLD, (D3DMATRIX *)&world);
            IDirect3DDevice8_DrawIndexedPrimitiveUP(s.device, D3DPT_TRIANGLELIST,
                    0, VIS_MODEL_VERTICES, VIS_MODEL_INDICES / 3, vis_model_indices,
                    D3DFMT_INDEX16, verts, sizeof(verts[0]));

            /* Pass two: the coloured quad, only where the stencil holds 1. The
               quad is already transformed, so depth testing is off for it. */
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_COLORWRITEENABLE, 0x0f);
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_ZWRITEENABLE, TRUE);
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_ZENABLE, D3DZB_FALSE);
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_STENCILFUNC, D3DCMP_EQUAL);
            IDirect3DDevice8_SetRenderState(s.device, D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
            IDirect3DDevice8_SetVertexShader(s.device, D8_SFVF);
            hr = IDirect3DDevice8_DrawPrimitiveUP(s.device, D3DPT_TRIANGLELIST, 2,
                                                  quad, sizeof(quad[0]));
            if (frame == 1)
                ok_(SUCCEEDED(hr), "the masked full-screen draw returned 0x%08lx", hr);

            IDirect3DDevice8_SetRenderState(s.device, D3DRS_STENCILENABLE, FALSE);
            IDirect3DDevice8_EndScene(s.device);
        }
        IDirect3DDevice8_Present(s.device, NULL, NULL, NULL, NULL);
    }
    ok_(frame > 1, "rendered %d stencil-masked frames", frame - 1);

    n = d8_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
    {
        vis_check_rendered(sample, n, 0xff181818);
        distinct = vis_count_distinct(sample, n, 0x00ffffff);
        ok_(distinct >= 3, "the masked gradient shows %d distinct colour(s)", distinct);
    }
    else skip_("could not read the back buffer");

    IDirect3DDevice8_SetRenderState(s.device, D3DRS_STENCILENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_ZENABLE, D3DZB_TRUE);
    vis_wait_if_held();
done:
    d8_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
