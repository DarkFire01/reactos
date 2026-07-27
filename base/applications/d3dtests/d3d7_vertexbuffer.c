/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 7: vertex buffers and buffer-sourced draws
 */


#include "d3dtest.h"
#include <ddraw.h>
#include <d3d.h>

/* Bring up ddraw + IDirect3D7 + a device on an offscreen 3D target. Returns
   FALSE when this machine simply has no 3D, which the caller reports as a
   skip rather than a failure. */
static D3DTEST_UNUSED BOOL d3d7_setup(HWND hwnd, IDirectDraw7 **ddraw, IDirect3D7 **d3d,
                                      IDirectDrawSurface7 **target, IDirect3DDevice7 **device)
{
    DDSURFACEDESC2 desc;

    *ddraw = NULL; *d3d = NULL; *target = NULL; *device = NULL;

    if (FAILED(DirectDrawCreateEx(NULL, (void **)ddraw, &IID_IDirectDraw7, NULL)))
        return FALSE;
    IDirectDraw7_SetCooperativeLevel(*ddraw, hwnd, DDSCL_NORMAL);

    if (FAILED(IDirectDraw7_QueryInterface(*ddraw, &IID_IDirect3D7, (void **)d3d)))
        return FALSE;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
    desc.dwWidth = 256;
    desc.dwHeight = 256;
    if (FAILED(IDirectDraw7_CreateSurface(*ddraw, &desc, target, NULL)))
        return FALSE;

    if (FAILED(IDirect3D7_CreateDevice(*d3d, &IID_IDirect3DHALDevice, *target, device))
        && FAILED(IDirect3D7_CreateDevice(*d3d, &IID_IDirect3DRGBDevice, *target, device)))
        return FALSE;

    return TRUE;
}

static D3DTEST_UNUSED void d3d7_teardown(IDirectDraw7 *ddraw, IDirect3D7 *d3d,
                                         IDirectDrawSurface7 *target, IDirect3DDevice7 *device)
{
    if (device) IDirect3DDevice7_Release(device);
    if (target) IDirectDrawSurface7_Release(target);
    if (d3d) IDirect3D7_Release(d3d);
    if (ddraw) IDirectDraw7_Release(ddraw);
}

struct vertex
{
    float x, y, z, rhw;
    DWORD colour;
};

int main(void)
{
    IDirect3DVertexBuffer7 *vb = NULL;
    IDirectDrawSurface7 *target = NULL;
    IDirect3DDevice7 *device = NULL;
    IDirectDraw7 *ddraw = NULL;
    IDirect3D7 *d3d = NULL;
    D3DVERTEXBUFFERDESC desc;
    void *data = NULL;
    HRESULT hr;
    HWND hwnd;

    struct vertex tri[] =
    {
        { 128.0f,  32.0f, 0.5f, 1.0f, 0x00ff0000 },
        { 224.0f, 224.0f, 0.5f, 1.0f, 0x0000ff00 },
        {  32.0f, 224.0f, 0.5f, 1.0f, 0x000000ff },
    };

    test_begin("d3d7_vertexbuffer");

    hwnd = test_create_window("d3d7_vertexbuffer", 320, 240);
    if (!d3d7_setup(hwnd, &ddraw, &d3d, &target, &device))
    {
        skip_("no Direct3D 7 device could be created");
        goto done;
    }

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwCaps = D3DVBCAPS_SYSTEMMEMORY;
    desc.dwFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
    desc.dwNumVertices = 3;

    hr = IDirect3D7_CreateVertexBuffer(d3d, &desc, &vb, 0);
    ok_(SUCCEEDED(hr) && vb != NULL, "CreateVertexBuffer returned 0x%08lx", hr);
    if (!vb)
        goto cleanup;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    hr = IDirect3DVertexBuffer7_GetVertexBufferDesc(vb, &desc);
    ok_(SUCCEEDED(hr), "GetVertexBufferDesc returned 0x%08lx", hr);
    ok_(desc.dwNumVertices == 3, "buffer holds %lu vertices, expected 3", desc.dwNumVertices);
    ok_(desc.dwFVF == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE), "buffer FVF is 0x%08lx", desc.dwFVF);

    hr = IDirect3DVertexBuffer7_Lock(vb, DDLOCK_WAIT | DDLOCK_WRITEONLY, &data, NULL);
    ok_(SUCCEEDED(hr) && data != NULL, "Lock returned 0x%08lx", hr);
    if (data)
    {
        memcpy(data, tri, sizeof(tri));
        hr = IDirect3DVertexBuffer7_Unlock(vb);
        ok_(SUCCEEDED(hr), "Unlock returned 0x%08lx", hr);
    }

    IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0x00202020, 1.0f, 0);

    hr = IDirect3DDevice7_BeginScene(device);
    ok_(SUCCEEDED(hr), "BeginScene returned 0x%08lx", hr);
    if (SUCCEEDED(hr))
    {
        IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_LIGHTING, FALSE);

        hr = IDirect3DDevice7_DrawPrimitiveVB(device, D3DPT_TRIANGLELIST, vb, 0, 3, 0);
        ok_(SUCCEEDED(hr), "DrawPrimitiveVB returned 0x%08lx", hr);

        IDirect3DDevice7_EndScene(device);
    }

cleanup:
    D3DTEST_RELEASE(vb);
    d3d7_teardown(ddraw, d3d, target, device);
done:
    test_destroy_window(hwnd);
    return test_end();
}
