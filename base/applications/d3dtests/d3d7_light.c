/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 7: lights and materials
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
    D3DMATERIAL7 material, got_material;
    D3DLIGHT7 light, got_light;
    BOOL enabled = FALSE;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d7_light");

    hwnd = test_create_window("d3d7_light", 320, 240);
    if (!d3d7_setup(hwnd, &ddraw, &d3d, &target, &device))
    {
        skip_("no Direct3D 7 device could be created");
        goto done;
    }

    memset(&material, 0, sizeof(material));
    material.dcvDiffuse.r = 1.0f;
    material.dcvDiffuse.g = 0.5f;
    material.dcvDiffuse.b = 0.25f;
    material.dcvDiffuse.a = 1.0f;
    material.dcvAmbient.r = 0.1f;
    material.power = 16.0f;

    hr = IDirect3DDevice7_SetMaterial(device, &material);
    ok_(SUCCEEDED(hr), "SetMaterial returned 0x%08lx", hr);

    memset(&got_material, 0, sizeof(got_material));
    hr = IDirect3DDevice7_GetMaterial(device, &got_material);
    ok_(SUCCEEDED(hr), "GetMaterial returned 0x%08lx", hr);
    ok_(got_material.dcvDiffuse.r == 1.0f && got_material.dcvDiffuse.g == 0.5f
        && got_material.power == 16.0f,
        "material round-tripped (diffuse %.2f,%.2f power %.1f)",
        got_material.dcvDiffuse.r, got_material.dcvDiffuse.g, got_material.power);

    memset(&light, 0, sizeof(light));
    light.dltType = D3DLIGHT_DIRECTIONAL;
    light.dcvDiffuse.r = 1.0f;
    light.dcvDiffuse.g = 1.0f;
    light.dcvDiffuse.b = 1.0f;
    light.dcvDiffuse.a = 1.0f;
    light.dvDirection.x = 0.0f;
    light.dvDirection.y = 0.0f;
    light.dvDirection.z = 1.0f;

    hr = IDirect3DDevice7_SetLight(device, 0, &light);
    ok_(SUCCEEDED(hr), "SetLight(0, directional) returned 0x%08lx", hr);

    memset(&got_light, 0, sizeof(got_light));
    hr = IDirect3DDevice7_GetLight(device, 0, &got_light);
    ok_(SUCCEEDED(hr), "GetLight(0) returned 0x%08lx", hr);
    ok_(got_light.dltType == D3DLIGHT_DIRECTIONAL && got_light.dvDirection.z == 1.0f,
        "light round-tripped (type %u, direction z %.1f)",
        got_light.dltType, got_light.dvDirection.z);

    hr = IDirect3DDevice7_LightEnable(device, 0, TRUE);
    ok_(SUCCEEDED(hr), "LightEnable(0, TRUE) returned 0x%08lx", hr);

    hr = IDirect3DDevice7_GetLightEnable(device, 0, &enabled);
    ok_(SUCCEEDED(hr), "GetLightEnable(0) returned 0x%08lx", hr);
    ok_(enabled, "light 0 reports itself enabled");

    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_LIGHTING, TRUE);
    ok_(SUCCEEDED(hr), "enabling the lighting pipeline returned 0x%08lx", hr);

    hr = IDirect3DDevice7_LightEnable(device, 0, FALSE);
    ok_(SUCCEEDED(hr), "LightEnable(0, FALSE) returned 0x%08lx", hr);

    d3d7_teardown(ddraw, d3d, target, device);
done:
    test_destroy_window(hwnd);
    return test_end();
}
