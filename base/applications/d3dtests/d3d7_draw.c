/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 7: clearing and DrawPrimitive inside a scene
 */


#include "d3dtest.h"
#include <ddraw.h>
#include <d3d.h>

struct vertex
{
    float x, y, z, rhw;
    DWORD colour;
};

int main(void)
{
    IDirectDrawSurface7 *target = NULL;
    IDirect3DDevice7 *device = NULL;
    IDirectDraw7 *ddraw = NULL;
    IDirect3D7 *d3d = NULL;
    DDSURFACEDESC2 desc;
    D3DVIEWPORT7 vp;
    HRESULT hr;
    HWND hwnd;

    struct vertex tri[] =
    {
        { 128.0f,  32.0f, 0.5f, 1.0f, 0x00ff0000 },
        { 224.0f, 224.0f, 0.5f, 1.0f, 0x0000ff00 },
        {  32.0f, 224.0f, 0.5f, 1.0f, 0x000000ff },
    };

    test_begin("d3d7_draw");

    hwnd = test_create_window("d3d7_draw", 320, 240);
    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    if (FAILED(hr))
        goto done;
    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);

    hr = IDirectDraw7_QueryInterface(ddraw, &IID_IDirect3D7, (void **)&d3d);
    if (FAILED(hr))
    {
        skip_("no IDirect3D7 available (0x%08lx)", hr);
        goto cleanup;
    }

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
    desc.dwWidth = 256;
    desc.dwHeight = 256;
    hr = IDirectDraw7_CreateSurface(ddraw, &desc, &target, NULL);
    if (FAILED(hr))
    {
        skip_("cannot create a 3DDEVICE surface (0x%08lx)", hr);
        goto cleanup;
    }

    hr = IDirect3D7_CreateDevice(d3d, &IID_IDirect3DHALDevice, target, &device);
    if (FAILED(hr))
        hr = IDirect3D7_CreateDevice(d3d, &IID_IDirect3DRGBDevice, target, &device);
    if (FAILED(hr) || !device)
    {
        skip_("CreateDevice failed (0x%08lx)", hr);
        goto cleanup;
    }
    ok_(SUCCEEDED(hr), "created a Direct3D 7 device");

    memset(&vp, 0, sizeof(vp));
    vp.dwWidth = 256;
    vp.dwHeight = 256;
    vp.dvMaxZ = 1.0f;
    IDirect3DDevice7_SetViewport(device, &vp);

    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0x00202020, 1.0f, 0);
    ok_(SUCCEEDED(hr), "Clear returned 0x%08lx", hr);

    hr = IDirect3DDevice7_BeginScene(device);
    ok_(SUCCEEDED(hr), "BeginScene returned 0x%08lx", hr);

    if (SUCCEEDED(hr))
    {
        IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_LIGHTING, FALSE);
        IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);

        hr = IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLELIST,
                D3DFVF_XYZRHW | D3DFVF_DIFFUSE, tri, 3, 0);
        ok_(SUCCEEDED(hr), "DrawPrimitive(TRIANGLELIST) returned 0x%08lx", hr);

        hr = IDirect3DDevice7_EndScene(device);
        ok_(SUCCEEDED(hr), "EndScene returned 0x%08lx", hr);
    }

    /* Drawing outside a scene must be rejected. */
    hr = IDirect3DDevice7_DrawPrimitive(device, D3DPT_TRIANGLELIST,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE, tri, 3, 0);
    /* Documented as invalid, but the retail runtime does not enforce it:
       Windows returns S_OK. Record the result instead of asserting. */
    info_("DrawPrimitive outside BeginScene returned 0x%08lx (Windows: S_OK)", hr);

cleanup:
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(target);
    D3DTEST_RELEASE(d3d);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
