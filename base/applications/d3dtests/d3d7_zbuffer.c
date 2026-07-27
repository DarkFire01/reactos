/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 7: depth buffering and depth clears
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
    DWORD value;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d7_zbuffer");

    hwnd = test_create_window("d3d7_zbuffer", 320, 240);
    if (!d3d7_setup(hwnd, &ddraw, &d3d, &target, &device))
    {
        skip_("no Direct3D 7 device could be created");
        goto done;
    }
    ok_(device != NULL, "created a Direct3D 7 device");

    /* With no z-buffer attached, a depth clear must be refused. */
    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    ok_(FAILED(hr), "Clear(ZBUFFER) with no z-buffer returned 0x%08lx, expected failure", hr);

    /* Depth state should still be settable and readable. */
    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
    ok_(SUCCEEDED(hr), "SetRenderState(ZENABLE, TRUE) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_ZENABLE, &value);
    ok_(value == D3DZB_TRUE, "ZENABLE reads back as %lu, expected TRUE", value);

    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
    ok_(SUCCEEDED(hr), "SetRenderState(ZFUNC, LESSEQUAL) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_ZFUNC, &value);
    ok_(value == D3DCMP_LESSEQUAL, "ZFUNC reads back as %lu", value);

    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZWRITEENABLE, FALSE);
    ok_(SUCCEEDED(hr), "SetRenderState(ZWRITEENABLE, FALSE) returned 0x%08lx", hr);
    value = 0xffffffff;
    IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_ZWRITEENABLE, &value);
    ok_(value == FALSE, "ZWRITEENABLE reads back as %lu, expected 0", value);

    /* A colour clear still has to work. */
    hr = IDirect3DDevice7_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0x00404040, 1.0f, 0);
    ok_(SUCCEEDED(hr), "Clear(TARGET) returned 0x%08lx", hr);

    d3d7_teardown(ddraw, d3d, target, device);
done:
    test_destroy_window(hwnd);
    return test_end();
}
