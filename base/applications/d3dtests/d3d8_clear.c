/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8: Clear and Present
 */


#include "d3dtest.h"
#include <d3d8.h>

static D3DTEST_UNUSED IDirect3DDevice8 *create_device(IDirect3D8 *d3d, HWND hwnd)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice8 *device = NULL;
    D3DDISPLAYMODE mode;
    HRESULT hr;

    /* Unlike d3d9, d3d8 will not take D3DFMT_UNKNOWN for a windowed back
       buffer: it has to be given the real format, so use the desktop's. */
    memset(&mode, 0, sizeof(mode));
    if (FAILED(IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode)))
        return NULL;

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 256;

    hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
        hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
        return NULL;
    return device;
}

int main(void)
{
    IDirect3DDevice8 *device = NULL;
    IDirect3D8 *d3d = NULL;
    HRESULT hr;
    HWND hwnd;
    int i;

    test_begin("d3d8_clear");

    hwnd = test_create_window("d3d8_clear", 320, 240);
    ShowWindow(hwnd, SW_SHOW);
    test_pump();

    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 8 device could be created on this adapter");
        goto cleanup;
    }

    for (i = 0; i < 3; i++)
    {
        D3DCOLOR colour = (i == 0) ? 0xffff0000 : (i == 1) ? 0xff00ff00 : 0xff0000ff;

        hr = IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_TARGET, colour, 1.0f, 0);
        ok_(SUCCEEDED(hr), "frame %d: Clear returned 0x%08lx", i, hr);

        hr = IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);
        ok_(SUCCEEDED(hr), "frame %d: Present returned 0x%08lx", i, hr);
        test_pump();
    }

    /* Clearing the depth buffer when none was requested must be rejected. */
    hr = IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    ok_(FAILED(hr), "Clear(ZBUFFER) without a depth buffer returned 0x%08lx, expected failure", hr);

cleanup:
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
