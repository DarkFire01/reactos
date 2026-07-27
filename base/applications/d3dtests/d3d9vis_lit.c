/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9 visual: an octahedron under a moving directional light
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
    struct d9_vertex verts[VIS_MODEL_VERTICES];
    DWORD sample[256];
    struct d9_scene s;
    struct vis_mat world;
    D3DMATERIAL9 material;
    D3DLIGHT9 light;
    int frame = 0, n, distinct;
    HWND hwnd;
    HRESULT hr;

    vis_parse_args(argc, argv);
    test_begin("d3d9vis_lit");

    hwnd = vis_create_window("Direct3D 9: fixed-function lighting");
    if (!d9_open(&s, hwnd)) { skip_("no Direct3D 9 device"); goto done; }

    /* The octahedron carries a real normal per face, so a working transform
       and lighting unit has to shade the eight faces differently. */
    d9_fill(verts, vis_model, VIS_MODEL_VERTICES);
    d9_camera(&s, 5.5f);

    /* A near-neutral material: every colour on screen comes from the light and
       the ambient term, not from the vertex data, which the FVF omits. */
    memset(&material, 0, sizeof(material));
    material.Diffuse.r = 0.9f; material.Diffuse.g = 0.88f; material.Diffuse.b = 0.8f;
    material.Diffuse.a = 1.0f;
    material.Ambient.r = 0.35f; material.Ambient.g = 0.35f; material.Ambient.b = 0.45f;
    material.Ambient.a = 1.0f;
    material.Specular.r = material.Specular.g = material.Specular.b = 0.7f;
    material.Specular.a = 1.0f;
    material.Power = 16.0f;
    hr = IDirect3DDevice9_SetMaterial(s.device, &material);
    ok_(SUCCEEDED(hr), "SetMaterial returned 0x%08lx", hr);

    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse.r = 1.0f; light.Diffuse.g = 0.92f; light.Diffuse.b = 0.75f;
    light.Diffuse.a = 1.0f;
    light.Specular.r = light.Specular.g = light.Specular.b = 1.0f;
    light.Specular.a = 1.0f;
    light.Direction.x = 0.0f; light.Direction.y = -0.5f; light.Direction.z = 1.0f;
    hr = IDirect3DDevice9_SetLight(s.device, 0, &light);
    ok_(SUCCEEDED(hr), "SetLight returned 0x%08lx", hr);
    hr = IDirect3DDevice9_LightEnable(s.device, 0, TRUE);
    ok_(SUCCEEDED(hr), "LightEnable returned 0x%08lx", hr);

    hr = IDirect3DDevice9_SetRenderState(s.device, D3DRS_LIGHTING, TRUE);
    ok_(SUCCEEDED(hr), "SetRenderState(LIGHTING, TRUE) returned 0x%08lx", hr);
    /* A deliberately cool ambient, so the unlit side of the model still shows
       up and reads as a different colour from the lit side. */
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_AMBIENT, 0xff283040);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_SPECULARENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_NORMALIZENORMALS, TRUE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice9_SetFVF(s.device, D9_FVF);

    while (vis_frame(frame++))
    {
        float t = frame * 0.05f;

        /* Walk the light around the model. A pipeline that quietly ignores
           SetLight would hold one fixed shading for the whole run. */
        light.Direction.x = (float)sin(t);
        light.Direction.y = -0.3f + 0.6f * (float)sin(t * 0.5f);
        light.Direction.z = (float)cos(t);
        IDirect3DDevice9_SetLight(s.device, 0, &light);

        vis_rotate_y(&world, t * 0.7f);
        IDirect3DDevice9_SetTransform(s.device, D3DTS_WORLD, (D3DMATRIX *)&world);

        IDirect3DDevice9_Clear(s.device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                               0xff101820, 1.0f, 0);
        if (SUCCEEDED(IDirect3DDevice9_BeginScene(s.device)))
        {
            hr = IDirect3DDevice9_DrawIndexedPrimitiveUP(s.device, D3DPT_TRIANGLELIST,
                    0, VIS_MODEL_VERTICES, VIS_MODEL_INDICES / 3, vis_model_indices,
                    D3DFMT_INDEX16, verts, sizeof(verts[0]));
            if (frame == 1)
                ok_(SUCCEEDED(hr), "DrawIndexedPrimitiveUP returned 0x%08lx", hr);
            IDirect3DDevice9_EndScene(s.device);
        }
        IDirect3DDevice9_Present(s.device, NULL, NULL, NULL, NULL);
    }
    ok_(frame > 1, "rendered %d lit frames", frame - 1);

    n = d9_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
    {
        vis_check_rendered(sample, n, 0xff101820);
        distinct = vis_count_distinct(sample, n, 0x00ffffff);
        ok_(distinct >= 3, "the shaded faces show %d distinct colour(s)", distinct);
    }
    else skip_("could not read the back buffer");

    IDirect3DDevice9_LightEnable(s.device, 0, FALSE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_SPECULARENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_LIGHTING, FALSE);
    vis_wait_if_held();
done:
    d9_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
