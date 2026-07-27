/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 7: device creation on an offscreen render target
 */


#include "d3dtest.h"
#include <ddraw.h>
#include <d3d.h>

int main(void)
{
    IDirectDrawSurface7 *target = NULL;
    IDirect3DDevice7 *device = NULL;
    IDirectDraw7 *ddraw = NULL;
    IDirect3D7 *d3d = NULL;
    D3DDEVICEDESC7 caps;
    DDSURFACEDESC2 desc;
    D3DVIEWPORT7 vp;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d7_device");

    hwnd = test_create_window("d3d7_device", 320, 240);
    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        goto done;

    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);

    hr = IDirectDraw7_QueryInterface(ddraw, &IID_IDirect3D7, (void **)&d3d);
    if (FAILED(hr))
    {
        skip_("no IDirect3D7 available (0x%08lx)", hr);
        goto cleanup;
    }

    /* A 3D-capable render target surface. */
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
    ok_(SUCCEEDED(hr), "created 256x256 3D render target (0x%08lx)", hr);

    hr = IDirect3D7_CreateDevice(d3d, &IID_IDirect3DHALDevice, target, &device);
    if (FAILED(hr))
    {
        info_("no HAL device (0x%08lx), falling back to RGB", hr);
        hr = IDirect3D7_CreateDevice(d3d, &IID_IDirect3DRGBDevice, target, &device);
    }
    if (FAILED(hr) || !device)
    {
        skip_("CreateDevice failed (0x%08lx)", hr);
        goto cleanup;
    }
    ok_(SUCCEEDED(hr), "CreateDevice returned 0x%08lx", hr);

    memset(&caps, 0, sizeof(caps));
    hr = IDirect3DDevice7_GetCaps(device, &caps);
    ok_(SUCCEEDED(hr), "GetCaps returned 0x%08lx", hr);
    info_("device caps 0x%08lx, max texture %lux%lu",
          caps.dwDevCaps, caps.dwMaxTextureWidth, caps.dwMaxTextureHeight);

    memset(&vp, 0, sizeof(vp));
    vp.dwWidth = 256;
    vp.dwHeight = 256;
    vp.dvMaxZ = 1.0f;
    hr = IDirect3DDevice7_SetViewport(device, &vp);
    ok_(SUCCEEDED(hr), "SetViewport returned 0x%08lx", hr);

    memset(&vp, 0, sizeof(vp));
    hr = IDirect3DDevice7_GetViewport(device, &vp);
    ok_(SUCCEEDED(hr), "GetViewport returned 0x%08lx", hr);
    ok_(vp.dwWidth == 256 && vp.dwHeight == 256,
        "viewport reads back as %lux%lu, expected 256x256", vp.dwWidth, vp.dwHeight);

cleanup:
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(target);
    D3DTEST_RELEASE(d3d);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
