/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: Clear, Present and rectangle clears
 */


#include "d3dtest.h"
#include <d3d9.h>

static D3DTEST_UNUSED IDirect3DDevice9 *create_device_ex(IDirect3D9 *d3d, HWND hwnd, BOOL want_depth)
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
    if (want_depth)
    {
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D16;
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

static D3DTEST_UNUSED IDirect3DDevice9 *create_device(IDirect3D9 *d3d, HWND hwnd)
{
    return create_device_ex(d3d, hwnd, FALSE);
}

int main(void)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    D3DRECT rect;
    HRESULT hr;
    HWND hwnd;
    int i;

    test_begin("d3d9_clear");

    hwnd = test_create_window("d3d9_clear", 320, 240);
    ShowWindow(hwnd, SW_SHOW);
    test_pump();

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
        goto done;

    device = create_device_ex(d3d, hwnd, TRUE);
    if (!device)
    {
        skip_("no Direct3D 9 device could be created on this adapter");
        goto cleanup;
    }

    for (i = 0; i < 3; i++)
    {
        D3DCOLOR colour = (i == 0) ? 0xffff0000 : (i == 1) ? 0xff00ff00 : 0xff0000ff;

        hr = IDirect3DDevice9_Clear(device, 0, NULL,
                D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, colour, 1.0f, 0);
        ok_(SUCCEEDED(hr), "frame %d: Clear(TARGET|ZBUFFER) returned 0x%08lx", i, hr);

        hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
        ok_(SUCCEEDED(hr), "frame %d: Present returned 0x%08lx", i, hr);
        test_pump();
    }

    /* A partial clear through an explicit rectangle. */
    rect.x1 = 64; rect.y1 = 64; rect.x2 = 192; rect.y2 = 192;
    hr = IDirect3DDevice9_Clear(device, 1, &rect, D3DCLEAR_TARGET, 0xffffff00, 1.0f, 0);
    ok_(SUCCEEDED(hr), "Clear with one rectangle returned 0x%08lx", hr);
    IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);

    /* A zero count with a non-NULL rect pointer is invalid. */
    hr = IDirect3DDevice9_Clear(device, 0, &rect, D3DCLEAR_TARGET, 0, 1.0f, 0);
    /* Documented as invalid, but the retail runtime accepts it: Windows
       returns S_OK. */
    info_("Clear(0 rects, non-NULL pointer) returned 0x%08lx (Windows: S_OK)", hr);

cleanup:
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
