/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 7: render state and transform round-trips
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
    DDSURFACEDESC2 desc;
    D3DMATRIX identity =
    {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    D3DMATRIX got_matrix;
    DWORD value;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d7_renderstate");

    hwnd = test_create_window("d3d7_renderstate", 320, 240);
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

    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_LIGHTING, FALSE);
    ok_(SUCCEEDED(hr), "SetRenderState(LIGHTING, FALSE) returned 0x%08lx", hr);
    value = 0xdeadbeef;
    hr = IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_LIGHTING, &value);
    ok_(SUCCEEDED(hr), "GetRenderState(LIGHTING) returned 0x%08lx", hr);
    ok_(value == FALSE, "LIGHTING reads back as %lu, expected 0", value);

    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_CULLMODE, D3DCULL_CW);
    ok_(SUCCEEDED(hr), "SetRenderState(CULLMODE, CW) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice7_GetRenderState(device, D3DRENDERSTATE_CULLMODE, &value);
    ok_(value == D3DCULL_CW, "CULLMODE reads back as %lu, expected %u", value, D3DCULL_CW);

    hr = IDirect3DDevice7_SetRenderState(device, D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
    ok_(SUCCEEDED(hr), "SetRenderState(ZENABLE, FALSE) returned 0x%08lx", hr);

    hr = IDirect3DDevice7_SetTransform(device, D3DTRANSFORMSTATE_WORLD, &identity);
    ok_(SUCCEEDED(hr), "SetTransform(WORLD) returned 0x%08lx", hr);

    memset(&got_matrix, 0, sizeof(got_matrix));
    hr = IDirect3DDevice7_GetTransform(device, D3DTRANSFORMSTATE_WORLD, &got_matrix);
    ok_(SUCCEEDED(hr), "GetTransform(WORLD) returned 0x%08lx", hr);
    ok_(memcmp(&got_matrix, &identity, sizeof(identity)) == 0,
        "world transform round-tripped unchanged");

    /* Texture stage state lives on the device too. */
    hr = IDirect3DDevice7_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    ok_(SUCCEEDED(hr), "SetTextureStageState(COLOROP) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice7_GetTextureStageState(device, 0, D3DTSS_COLOROP, &value);
    ok_(value == D3DTOP_SELECTARG1, "COLOROP reads back as %lu", value);

cleanup:
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(target);
    D3DTEST_RELEASE(d3d);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
