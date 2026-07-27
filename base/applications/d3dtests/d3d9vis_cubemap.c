/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9 visual: an octahedron reflecting a six-faced cube map
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

/* The reflection vector is built from the vertex normal, and the generated
   coordinates replace whatever set 0 held, so these vertices carry no uv. */
struct d9_nvertex { float x, y, z, nx, ny, nz; };
#define D9_NFVF (D3DFVF_XYZ | D3DFVF_NORMAL)

static BOOL d9_fill_cube_face(IDirect3DCubeTexture9 *cube, D3DCUBEMAP_FACES face,
                              const DWORD *pixels, int size)
{
    D3DLOCKED_RECT lr;
    int y;

    if (FAILED(IDirect3DCubeTexture9_LockRect(cube, face, 0, &lr, NULL, 0)))
        return FALSE;
    for (y = 0; y < size; y++)
        memcpy((BYTE *)lr.pBits + y * lr.Pitch, pixels + y * size, size * 4);
    IDirect3DCubeTexture9_UnlockRect(cube, face, 0);
    return TRUE;
}

int main(int argc, char **argv)
{
    IDirect3DCubeTexture9 *cube = NULL;
    static DWORD pixels[64 * 64];
    struct d9_nvertex verts[VIS_MODEL_VERTICES];
    DWORD sample[256];
    struct d9_scene s;
    struct vis_mat world, ry, rx;
    D3DCAPS9 caps;
    int frame = 0, n, i, faces = 0, distinct;
    HWND hwnd;
    HRESULT hr;

    vis_parse_args(argc, argv);
    test_begin("d3d9vis_cubemap");

    hwnd = vis_create_window("Direct3D 9: cube map reflection");
    if (!d9_open(&s, hwnd)) { skip_("no Direct3D 9 device"); goto done; }

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice9_GetDeviceCaps(s.device, &caps);
    if (!(caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP))
    {
        skip_("device advertises no cube map support");
        goto done;
    }

    hr = IDirect3DDevice9_CreateCubeTexture(s.device, 64, 1, 0, D3DFMT_A8R8G8B8,
                                            D3DPOOL_MANAGED, &cube, NULL);
    if (FAILED(hr) || !cube)
    {
        skip_("CreateCubeTexture returned 0x%08lx", hr);
        goto done;
    }
    ok_(SUCCEEDED(hr), "created a 64x64 cube texture");

    /* Six visibly different faces, so which face a reflection lands on is
       obvious from the colour alone. */
    vis_tex_checker(pixels, 64, 64, 8, 0xffff5030, 0xff802000);
    faces += d9_fill_cube_face(cube, D3DCUBEMAP_FACE_POSITIVE_X, pixels, 64) ? 1 : 0;
    vis_tex_checker(pixels, 64, 64, 8, 0xff30ff50, 0xff006020);
    faces += d9_fill_cube_face(cube, D3DCUBEMAP_FACE_NEGATIVE_X, pixels, 64) ? 1 : 0;
    vis_tex_gradient(pixels, 64, 64);
    faces += d9_fill_cube_face(cube, D3DCUBEMAP_FACE_POSITIVE_Y, pixels, 64) ? 1 : 0;
    vis_tex_rings(pixels, 64, 64);
    faces += d9_fill_cube_face(cube, D3DCUBEMAP_FACE_NEGATIVE_Y, pixels, 64) ? 1 : 0;
    vis_tex_plasma(pixels, 64, 64, 0.0f);
    faces += d9_fill_cube_face(cube, D3DCUBEMAP_FACE_POSITIVE_Z, pixels, 64) ? 1 : 0;
    vis_tex_solid(pixels, 64, 64, 0xffc040ff);
    faces += d9_fill_cube_face(cube, D3DCUBEMAP_FACE_NEGATIVE_Z, pixels, 64) ? 1 : 0;
    ok_(faces == 6, "filled %d of the 6 cube map faces", faces);
    if (!faces) goto done;

    for (i = 0; i < VIS_MODEL_VERTICES; i++)
    {
        verts[i].x = vis_model[i].x;   verts[i].y = vis_model[i].y;
        verts[i].z = vis_model[i].z;
        verts[i].nx = vis_model[i].nx; verts[i].ny = vis_model[i].ny;
        verts[i].nz = vis_model[i].nz;
    }

    d9_camera(&s, 5.0f);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice9_SetRenderState(s.device, D3DRS_NORMALIZENORMALS, TRUE);
    IDirect3DDevice9_SetFVF(s.device, D9_NFVF);

    IDirect3DDevice9_SetTexture(s.device, 0, (IDirect3DBaseTexture9 *)cube);
    IDirect3DDevice9_SetTextureStageState(s.device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(s.device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    hr = IDirect3DDevice9_SetTextureStageState(s.device, 0, D3DTSS_TEXCOORDINDEX,
            D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR | 0);
    ok_(SUCCEEDED(hr), "SetTextureStageState(TEXCOORDINDEX, reflection) returned 0x%08lx", hr);
    /* Three generated components, which is what a cube map lookup needs. */
    hr = IDirect3DDevice9_SetTextureStageState(s.device, 0, D3DTSS_TEXTURETRANSFORMFLAGS,
                                               D3DTTFF_COUNT3);
    ok_(SUCCEEDED(hr), "SetTextureStageState(TEXTURETRANSFORMFLAGS, COUNT3) returned 0x%08lx", hr);
    IDirect3DDevice9_SetSamplerState(s.device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(s.device, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(s.device, 0, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(s.device, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(s.device, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

    while (vis_frame(frame++))
    {
        float t = frame * 0.05f;

        /* Tumbling the model sweeps its face normals right around the cube, so
           the reflection has to walk from one face of the map to the next. */
        vis_rotate_y(&ry, t);
        vis_rotate_x(&rx, t * 0.7f);
        vis_mul(&world, &rx, &ry);
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
    ok_(frame > 1, "rendered %d environment-mapped frames", frame - 1);

    n = d9_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
    {
        vis_check_rendered(sample, n, 0xff101820);
        distinct = vis_count_distinct(sample, n, 0x00ffffff);
        ok_(distinct >= 3, "the reflected faces show %d distinct colour(s)", distinct);
    }
    else skip_("could not read the back buffer");

    IDirect3DDevice9_SetTextureStageState(s.device, 0, D3DTSS_TEXTURETRANSFORMFLAGS,
                                          D3DTTFF_DISABLE);
    IDirect3DDevice9_SetTextureStageState(s.device, 0, D3DTSS_TEXCOORDINDEX, 0);
    IDirect3DDevice9_SetTexture(s.device, 0, NULL);
    vis_wait_if_held();
done:
    D3DTEST_RELEASE(cube);
    d9_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
