/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: device creation, back buffer and viewport defaults
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
    IDirect3DSurface9 *backbuffer = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    D3DSURFACE_DESC desc;
    D3DVIEWPORT9 vp;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d9_device");

    hwnd = test_create_window("d3d9_device", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    ok_(d3d != NULL, "Direct3DCreate9 returned an object");
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 9 device could be created on this adapter");
        goto cleanup;
    }
    ok_(device != NULL, "created a Direct3D 9 device");

    hr = IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    ok_(SUCCEEDED(hr) && backbuffer != NULL, "GetBackBuffer returned 0x%08lx", hr);

    if (backbuffer)
    {
        memset(&desc, 0, sizeof(desc));
        hr = IDirect3DSurface9_GetDesc(backbuffer, &desc);
        ok_(SUCCEEDED(hr), "GetDesc on the back buffer returned 0x%08lx", hr);
        ok_(desc.Width == 256 && desc.Height == 256,
            "back buffer is %ux%u, expected 256x256", desc.Width, desc.Height);
        ok_(desc.Usage & D3DUSAGE_RENDERTARGET,
            "back buffer usage 0x%08lx includes RENDERTARGET", (unsigned long)desc.Usage);
    }

    memset(&vp, 0, sizeof(vp));
    hr = IDirect3DDevice9_GetViewport(device, &vp);
    ok_(SUCCEEDED(hr), "GetViewport returned 0x%08lx", hr);
    ok_(vp.Width == 256 && vp.Height == 256,
        "default viewport is %ux%u, expected the back buffer size", vp.Width, vp.Height);
    ok_(vp.MinZ == 0.0f && vp.MaxZ == 1.0f, "default depth range is %f..%f", vp.MinZ, vp.MaxZ);

    /* The device should report itself available while nothing has been lost. */
    hr = IDirect3DDevice9_TestCooperativeLevel(device);
    ok_(hr == D3D_OK, "TestCooperativeLevel returned 0x%08lx on a healthy device", hr);

cleanup:
    D3DTEST_RELEASE(backbuffer);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
