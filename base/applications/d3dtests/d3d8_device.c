/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8: device creation and back buffer inspection
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
    IDirect3DSurface8 *backbuffer = NULL;
    IDirect3DDevice8 *device = NULL;
    IDirect3D8 *d3d = NULL;
    D3DSURFACE_DESC desc;
    D3DVIEWPORT8 vp;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d8_device");

    hwnd = test_create_window("d3d8_device", 320, 240);
    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    ok_(d3d != NULL, "Direct3DCreate8 returned an object");
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 8 device could be created on this adapter");
        goto cleanup;
    }
    ok_(device != NULL, "created a Direct3D 8 device");

    hr = IDirect3DDevice8_GetBackBuffer(device, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    ok_(SUCCEEDED(hr) && backbuffer != NULL, "GetBackBuffer returned 0x%08lx", hr);

    if (backbuffer)
    {
        memset(&desc, 0, sizeof(desc));
        hr = IDirect3DSurface8_GetDesc(backbuffer, &desc);
        ok_(SUCCEEDED(hr), "GetDesc on the back buffer returned 0x%08lx", hr);
        ok_(desc.Width == 256 && desc.Height == 256,
            "back buffer is %ux%u, expected the requested 256x256", desc.Width, desc.Height);
        info_("back buffer format %u, usage 0x%08lx", desc.Format, (unsigned long)desc.Usage);
    }

    memset(&vp, 0, sizeof(vp));
    hr = IDirect3DDevice8_GetViewport(device, &vp);
    ok_(SUCCEEDED(hr), "GetViewport returned 0x%08lx", hr);
    ok_(vp.Width == 256 && vp.Height == 256,
        "default viewport is %ux%u, expected it to match the back buffer", vp.Width, vp.Height);

cleanup:
    D3DTEST_RELEASE(backbuffer);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
