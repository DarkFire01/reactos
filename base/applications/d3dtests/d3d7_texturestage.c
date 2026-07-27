/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 7: texture stage state and multitexturing limits
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
    D3DDEVICEDESC7 caps;
    DWORD value;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d7_texturestage");

    hwnd = test_create_window("d3d7_texturestage", 320, 240);
    if (!d3d7_setup(hwnd, &ddraw, &d3d, &target, &device))
    {
        skip_("no Direct3D 7 device could be created");
        goto done;
    }

    memset(&caps, 0, sizeof(caps));
    hr = IDirect3DDevice7_GetCaps(device, &caps);
    ok_(SUCCEEDED(hr), "GetCaps returned 0x%08lx", hr);
    info_("device supports %lu texture stage(s)", caps.wMaxSimultaneousTextures);

    hr = IDirect3DDevice7_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    ok_(SUCCEEDED(hr), "SetTextureStageState(0, COLOROP, MODULATE) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice7_GetTextureStageState(device, 0, D3DTSS_COLOROP, &value);
    ok_(value == D3DTOP_MODULATE, "COLOROP reads back as %lu, expected MODULATE", value);

    hr = IDirect3DDevice7_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    ok_(SUCCEEDED(hr), "SetTextureStageState(COLORARG1, TEXTURE) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice7_GetTextureStageState(device, 0, D3DTSS_COLORARG1, &value);
    ok_(value == D3DTA_TEXTURE, "COLORARG1 reads back as %lu", value);

    hr = IDirect3DDevice7_SetTextureStageState(device, 0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
    ok_(SUCCEEDED(hr), "SetTextureStageState(MINFILTER, LINEAR) returned 0x%08lx", hr);

    hr = IDirect3DDevice7_SetTextureStageState(device, 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    ok_(SUCCEEDED(hr), "SetTextureStageState(ADDRESSU, CLAMP) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice7_GetTextureStageState(device, 0, D3DTSS_ADDRESSU, &value);
    ok_(value == D3DTADDRESS_CLAMP, "ADDRESSU reads back as %lu", value);

    /* Stage 1 must be independent of stage 0. */
    hr = IDirect3DDevice7_SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    ok_(SUCCEEDED(hr), "SetTextureStageState(1, COLOROP, DISABLE) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice7_GetTextureStageState(device, 0, D3DTSS_COLOROP, &value);
    ok_(value == D3DTOP_MODULATE, "stage 0 still reads MODULATE after touching stage 1");

    d3d7_teardown(ddraw, d3d, target, device);
done:
    test_destroy_window(hwnd);
    return test_end();
}
