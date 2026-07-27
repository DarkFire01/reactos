/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: fixed-function lights and materials
 */


#include "d3dtest.h"
#include <d3d9.h>

static D3DTEST_UNUSED IDirect3DDevice9 *create_device9(IDirect3D9 *d3d, HWND hwnd, BOOL depth)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *device = NULL;
    HRESULT hr;

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 256;
    if (depth)
    {
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    }

    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
        hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
        return NULL;
    return device;
}

/* Render-target readback helper: copy the RT into a lockable system surface
   and return one pixel. Returns FALSE when the path is unavailable. */
static D3DTEST_UNUSED BOOL read_rt_pixel(IDirect3DDevice9 *device, int x, int y, DWORD *out)
{
    IDirect3DSurface9 *rt = NULL, *sys = NULL;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    BOOL ok = FALSE;

    if (FAILED(IDirect3DDevice9_GetRenderTarget(device, 0, &rt)))
        return FALSE;
    if (FAILED(IDirect3DSurface9_GetDesc(rt, &desc)))
        goto done;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(device, desc.Width, desc.Height,
            desc.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)))
        goto done;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(device, rt, sys)))
        goto done;
    if (FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY)))
        goto done;

    *out = *(DWORD *)((BYTE *)lr.pBits + y * lr.Pitch + x * 4);
    IDirect3DSurface9_UnlockRect(sys);
    ok = TRUE;

done:
    if (sys) IDirect3DSurface9_Release(sys);
    if (rt) IDirect3DSurface9_Release(rt);
    return ok;
}

int main(void)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    D3DMATERIAL9 material, got_material;
    D3DLIGHT9 light, got_light;
    BOOL enabled = FALSE;
    D3DCAPS9 caps;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d9_light");

    hwnd = test_create_window("d3d9_light", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) goto done;
    device = create_device9(d3d, hwnd, FALSE);
    if (!device) { skip_("no Direct3D 9 device"); goto cleanup; }

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice9_GetDeviceCaps(device, &caps);
    info_("device supports %lu active light(s)", (unsigned long)caps.MaxActiveLights);

    memset(&material, 0, sizeof(material));
    material.Diffuse.r = 1.0f;
    material.Diffuse.g = 0.5f;
    material.Diffuse.b = 0.25f;
    material.Diffuse.a = 1.0f;
    material.Power = 32.0f;

    hr = IDirect3DDevice9_SetMaterial(device, &material);
    ok_(SUCCEEDED(hr), "SetMaterial returned 0x%08lx", hr);

    memset(&got_material, 0, sizeof(got_material));
    hr = IDirect3DDevice9_GetMaterial(device, &got_material);
    ok_(SUCCEEDED(hr), "GetMaterial returned 0x%08lx", hr);
    ok_(got_material.Diffuse.g == 0.5f && got_material.Power == 32.0f,
        "material round-tripped (diffuse g %.2f, power %.1f)",
        got_material.Diffuse.g, got_material.Power);

    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_POINT;
    light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = light.Diffuse.a = 1.0f;
    light.Position.z = -5.0f;
    light.Range = 100.0f;
    light.Attenuation0 = 1.0f;

    hr = IDirect3DDevice9_SetLight(device, 0, &light);
    ok_(SUCCEEDED(hr), "SetLight(0, point) returned 0x%08lx", hr);

    memset(&got_light, 0, sizeof(got_light));
    hr = IDirect3DDevice9_GetLight(device, 0, &got_light);
    ok_(SUCCEEDED(hr), "GetLight(0) returned 0x%08lx", hr);
    ok_(got_light.Type == D3DLIGHT_POINT && got_light.Range == 100.0f,
        "light round-tripped (type %u, range %.1f)", got_light.Type, got_light.Range);

    hr = IDirect3DDevice9_LightEnable(device, 0, TRUE);
    ok_(SUCCEEDED(hr), "LightEnable(0, TRUE) returned 0x%08lx", hr);

    hr = IDirect3DDevice9_GetLightEnable(device, 0, &enabled);
    ok_(SUCCEEDED(hr) && enabled, "GetLightEnable(0) reports enabled (0x%08lx)", hr);

    /* Querying a light that was never set must fail rather than return junk. */
    memset(&got_light, 0, sizeof(got_light));
    hr = IDirect3DDevice9_GetLight(device, 7, &got_light);
    ok_(FAILED(hr), "GetLight on an unset index returned 0x%08lx, expected failure", hr);

    IDirect3DDevice9_LightEnable(device, 0, FALSE);

cleanup:
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
