/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8 visual: an octahedron under a sweeping directional light
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
    struct d8_vertex verts[VIS_MODEL_VERTICES];
    DWORD sample[256];
    struct d8_scene s;
    struct vis_mat world;
    D3DMATERIAL8 material;
    D3DLIGHT8 light;
    int frame = 0, n, distinct;
    HWND hwnd;
    HRESULT hr;

    vis_parse_args(argc, argv);
    test_begin("d3d8vis_lit");

    hwnd = vis_create_window("Direct3D 8: fixed-function lighting");
    if (!d8_open(&s, hwnd)) { skip_("no Direct3D 8 device"); goto done; }

    /* The octahedron carries a real normal per face, so a working transform
       and lighting unit has to shade the eight faces differently. */
    d8_fill(verts, vis_model, VIS_MODEL_VERTICES);
    d8_camera(&s, 5.5f);

    /* A near-neutral material: whatever colour ends up on screen came from the
       light, not from the vertex data, which the FVF does not even carry. */
    memset(&material, 0, sizeof(material));
    material.Diffuse.r = 0.85f; material.Diffuse.g = 0.85f; material.Diffuse.b = 0.90f;
    material.Diffuse.a = 1.0f;
    material.Ambient.r = material.Ambient.g = material.Ambient.b = 0.30f;
    material.Ambient.a = 1.0f;
    material.Specular.r = material.Specular.g = material.Specular.b = 0.6f;
    material.Specular.a = 1.0f;
    material.Power = 12.0f;
    hr = IDirect3DDevice8_SetMaterial(s.device, &material);
    ok_(SUCCEEDED(hr), "SetMaterial returned 0x%08lx", hr);

    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse.r = 1.0f; light.Diffuse.g = 0.95f; light.Diffuse.b = 0.8f;
    light.Diffuse.a = 1.0f;
    light.Specular.r = light.Specular.g = light.Specular.b = 1.0f;
    light.Specular.a = 1.0f;
    light.Direction.x = 0.0f; light.Direction.y = -0.4f; light.Direction.z = 1.0f;
    hr = IDirect3DDevice8_SetLight(s.device, 0, &light);
    ok_(SUCCEEDED(hr), "SetLight returned 0x%08lx", hr);
    hr = IDirect3DDevice8_LightEnable(s.device, 0, TRUE);
    ok_(SUCCEEDED(hr), "LightEnable returned 0x%08lx", hr);

    hr = IDirect3DDevice8_SetRenderState(s.device, D3DRS_LIGHTING, TRUE);
    ok_(SUCCEEDED(hr), "SetRenderState(LIGHTING, TRUE) returned 0x%08lx", hr);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_AMBIENT, 0xff303038);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_SPECULARENABLE, TRUE);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_NORMALIZENORMALS, TRUE);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice8_SetVertexShader(s.device, D8_FVF);

    while (vis_frame(frame++))
    {
        float t = frame * 0.05f;

        /* Swing the light right around the model. A pipeline that quietly
           ignores SetLight would hold a fixed shading instead. */
        light.Direction.x = (float)sin(t);
        light.Direction.y = -0.4f;
        light.Direction.z = (float)cos(t);
        IDirect3DDevice8_SetLight(s.device, 0, &light);

        vis_rotate_y(&world, t * 0.7f);
        IDirect3DDevice8_SetTransform(s.device, D3DTS_WORLD, (D3DMATRIX *)&world);

        IDirect3DDevice8_Clear(s.device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                               0xff101820, 1.0f, 0);
        if (SUCCEEDED(IDirect3DDevice8_BeginScene(s.device)))
        {
            hr = IDirect3DDevice8_DrawIndexedPrimitiveUP(s.device, D3DPT_TRIANGLELIST,
                    0, VIS_MODEL_VERTICES, VIS_MODEL_INDICES / 3, vis_model_indices,
                    D3DFMT_INDEX16, verts, sizeof(verts[0]));
            if (frame == 1)
                ok_(SUCCEEDED(hr), "DrawIndexedPrimitiveUP returned 0x%08lx", hr);
            IDirect3DDevice8_EndScene(s.device);
        }
        IDirect3DDevice8_Present(s.device, NULL, NULL, NULL, NULL);
    }
    ok_(frame > 1, "rendered %d lit frames", frame - 1);

    n = d8_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
    {
        vis_check_rendered(sample, n, 0xff101820);
        distinct = vis_count_distinct(sample, n, 0x00ffffff);
        ok_(distinct >= 3, "the shaded faces show %d distinct colour(s)", distinct);
    }
    else skip_("could not read the back buffer");

    IDirect3DDevice8_LightEnable(s.device, 0, FALSE);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_SPECULARENABLE, FALSE);
    IDirect3DDevice8_SetRenderState(s.device, D3DRS_LIGHTING, FALSE);
    vis_wait_if_held();
done:
    d8_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
