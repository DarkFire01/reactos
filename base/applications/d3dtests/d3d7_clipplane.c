/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 7: user clip planes
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
    D3DVALUE plane[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
    D3DVALUE got[4] = { 0 };
    D3DDEVICEDESC7 caps;
    DWORD value;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d7_clipplane");

    hwnd = test_create_window("d3d7_clipplane", 320, 240);
    if (!d3d7_setup(hwnd, &ddraw, &d3d, &target, &device))
    {
        skip_("no Direct3D 7 device could be created");
        goto done;
    }

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice7_GetCaps(device, &caps);
    info_("device advertises %lu user clip plane(s)", caps.wMaxUserClipPlanes);

    hr = IDirect3DDevice7_SetClipPlane(device, 0, plane);
    if (FAILED(hr))
    {
        skip_("SetClipPlane returned 0x%08lx", hr);
        goto cleanup;
    }
    ok_(SUCCEEDED(hr), "SetClipPlane(0) returned 0x%08lx", hr);

    hr = IDirect3DDevice7_GetClipPlane(device, 0, got);
    ok_(SUCCEEDED(hr), "GetClipPlane(0) returned 0x%08lx", hr);
    ok_(got[0] == 0.0f && got[1] == 1.0f && got[2] == 0.0f && got[3] == 0.0f,
        "clip plane reads back as %.1f,%.1f,%.1f,%.1f", got[0], got[1], got[2], got[3]);

    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_CLIPPLANEENABLE, 0x1);
    ok_(SUCCEEDED(hr), "enabling clip plane 0 returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_CLIPPLANEENABLE, &value);
    ok_(value == 0x1, "CLIPPLANEENABLE reads back as 0x%lx, expected 0x1", value);

    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_CLIPPLANEENABLE, 0);
    ok_(SUCCEEDED(hr), "disabling clip planes returned 0x%08lx", hr);

cleanup:
    d3d7_teardown(ddraw, d3d, target, device);
done:
    test_destroy_window(hwnd);
    return test_end();
}
