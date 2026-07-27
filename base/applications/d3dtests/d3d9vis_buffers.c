/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9 visual: a cube drawn from real vertex and index buffers
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
    IDirect3DVertexBuffer9 *vb = NULL;
    IDirect3DIndexBuffer9 *ib = NULL;
    struct d9_cvertex verts[VIS_CUBE_VERTICES];
    DWORD sample[256];
    struct d9_scene s;
    struct vis_mat world, ry, rx;
    void *data = NULL;
    int frame = 0, n;
    HWND hwnd;
    HRESULT hr;

    vis_parse_args(argc, argv);
    test_begin("d3d9vis_buffers");

    hwnd = vis_create_window("Direct3D 9: cube from buffers");
    if (!d9_open(&s, hwnd)) { skip_("no Direct3D 9 device"); goto done; }

    d9_fill_colour(verts, vis_cube, VIS_CUBE_VERTICES);

    hr = IDirect3DDevice9_CreateVertexBuffer(s.device, sizeof(verts), D3DUSAGE_WRITEONLY,
            D9_CFVF, D3DPOOL_MANAGED, &vb, NULL);
    ok_(SUCCEEDED(hr) && vb != NULL, "CreateVertexBuffer returned 0x%08lx", hr);
    hr = IDirect3DDevice9_CreateIndexBuffer(s.device, sizeof(vis_cube_indices),
            D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL);
    ok_(SUCCEEDED(hr) && ib != NULL, "CreateIndexBuffer returned 0x%08lx", hr);
    if (!vb || !ib) goto done;

    if (SUCCEEDED(IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(verts), &data, 0)))
    {
        memcpy(data, verts, sizeof(verts));
        IDirect3DVertexBuffer9_Unlock(vb);
    }
    data = NULL;
    if (SUCCEEDED(IDirect3DIndexBuffer9_Lock(ib, 0, sizeof(vis_cube_indices), &data, 0)))
    {
        memcpy(data, vis_cube_indices, sizeof(vis_cube_indices));
        IDirect3DIndexBuffer9_Unlock(ib);
    }

    d9_camera(&s, 6.0f);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice9_SetFVF(s.device, D9_CFVF);
    IDirect3DDevice9_SetStreamSource(s.device, 0, vb, 0, sizeof(verts[0]));
    IDirect3DDevice9_SetIndices(s.device, ib);

    while (vis_frame(frame++))
    {
        float t = frame * 0.05f;

        vis_rotate_y(&ry, t);
        vis_rotate_x(&rx, t * 0.7f);
        vis_mul(&world, &rx, &ry);
        IDirect3DDevice9_SetTransform(s.device, D3DTS_WORLD, (D3DMATRIX *)&world);

        IDirect3DDevice9_Clear(s.device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                               0xff283040, 1.0f, 0);
        if (SUCCEEDED(IDirect3DDevice9_BeginScene(s.device)))
        {
            IDirect3DDevice9_DrawIndexedPrimitive(s.device, D3DPT_TRIANGLELIST,
                    0, 0, VIS_CUBE_VERTICES, 0, VIS_CUBE_INDICES / 3);
            IDirect3DDevice9_EndScene(s.device);
        }
        IDirect3DDevice9_Present(s.device, NULL, NULL, NULL, NULL);
    }
    ok_(frame > 1, "rendered %d frames from buffer-resident geometry", frame - 1);

    n = d9_sample(&s, sample, ARRAYSIZE(sample));
    if (n) vis_check_rendered(sample, n, 0xff283040);

    vis_wait_if_held();
done:
    D3DTEST_RELEASE(ib);
    D3DTEST_RELEASE(vb);
    d9_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
