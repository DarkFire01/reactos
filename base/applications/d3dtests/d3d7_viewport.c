/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 7: viewport clipping and validation
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

int main(void)
{
    IDirectDrawSurface7 *target = NULL;
    IDirect3DDevice7 *device = NULL;
    IDirectDraw7 *ddraw = NULL;
    IDirect3D7 *d3d = NULL;
    D3DVIEWPORT7 vp, got;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d7_viewport");

    hwnd = test_create_window("d3d7_viewport", 320, 240);
    if (!d3d7_setup(hwnd, &ddraw, &d3d, &target, &device))
    {
        skip_("no Direct3D 7 device could be created");
        goto done;
    }

    /* A sub-rectangle of the 256x256 target. */
    memset(&vp, 0, sizeof(vp));
    vp.dwX = 32;
    vp.dwY = 32;
    vp.dwWidth = 128;
    vp.dwHeight = 128;
    vp.dvMinZ = 0.0f;
    vp.dvMaxZ = 1.0f;

    hr = IDirect3DDevice7_SetViewport(device, &vp);
    ok_(SUCCEEDED(hr), "SetViewport(32,32 128x128) returned 0x%08lx", hr);

    memset(&got, 0, sizeof(got));
    hr = IDirect3DDevice7_GetViewport(device, &got);
    ok_(SUCCEEDED(hr), "GetViewport returned 0x%08lx", hr);
    ok_(got.dwX == 32 && got.dwY == 32 && got.dwWidth == 128 && got.dwHeight == 128,
        "viewport reads back as %lu,%lu %lux%lu", got.dwX, got.dwY, got.dwWidth, got.dwHeight);
    ok_(got.dvMinZ == 0.0f && got.dvMaxZ == 1.0f,
        "depth range reads back as %.1f..%.1f", got.dvMinZ, got.dvMaxZ);

    /* A viewport larger than the render target should be rejected. */
    memset(&vp, 0, sizeof(vp));
    vp.dwWidth = 4096;
    vp.dwHeight = 4096;
    vp.dvMaxZ = 1.0f;
    hr = IDirect3DDevice7_SetViewport(device, &vp);
    info_("oversized viewport returned 0x%08lx", hr);

    /* Restore something sane and clear through it. */
    memset(&vp, 0, sizeof(vp));
    vp.dwWidth = 256;
    vp.dwHeight = 256;
    vp.dvMaxZ = 1.0f;
    hr = IDirect3DDevice7_SetViewport(device, &vp);
    ok_(SUCCEEDED(hr), "restoring the full viewport returned 0x%08lx", hr);

    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0x00808080, 1.0f, 0);
    ok_(SUCCEEDED(hr), "Clear through the restored viewport returned 0x%08lx", hr);

    d3d7_teardown(ddraw, d3d, target, device);
done:
    test_destroy_window(hwnd);
    return test_end();
}
