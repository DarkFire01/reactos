/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: device Reset and resource pool behaviour
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
    IDirect3DVertexBuffer9 *managed = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    D3DPRESENT_PARAMETERS pp;
    IDirect3DSurface9 *bb = NULL;
    D3DSURFACE_DESC desc;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d9_reset");

    hwnd = test_create_window("d3d9_reset", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 9 device could be created on this adapter");
        goto cleanup;
    }

    /* Managed resources survive a Reset; DEFAULT pool ones would not. */
    hr = IDirect3DDevice9_CreateVertexBuffer(device, 256, 0, D3DFVF_XYZ,
            D3DPOOL_MANAGED, &managed, NULL);
    ok_(SUCCEEDED(hr) && managed != NULL, "created a MANAGED vertex buffer (0x%08lx)", hr);

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 128;
    pp.BackBufferHeight = 128;

    hr = IDirect3DDevice9_Reset(device, &pp);
    ok_(SUCCEEDED(hr), "Reset to 128x128 returned 0x%08lx", hr);

    if (SUCCEEDED(hr))
    {
        hr = IDirect3DDevice9_TestCooperativeLevel(device);
        ok_(hr == D3D_OK, "TestCooperativeLevel after Reset returned 0x%08lx", hr);

        hr = IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        ok_(SUCCEEDED(hr) && bb != NULL, "GetBackBuffer after Reset returned 0x%08lx", hr);

        if (bb)
        {
            memset(&desc, 0, sizeof(desc));
            IDirect3DSurface9_GetDesc(bb, &desc);
            ok_(desc.Width == 128 && desc.Height == 128,
                "back buffer is %ux%u after Reset, expected 128x128", desc.Width, desc.Height);
            D3DTEST_RELEASE(bb);
        }

        /* The managed buffer must still be usable. */
        if (managed)
        {
            void *data = NULL;
            hr = IDirect3DVertexBuffer9_Lock(managed, 0, 0, &data, 0);
            ok_(SUCCEEDED(hr) && data != NULL,
                "MANAGED buffer still lockable after Reset (0x%08lx)", hr);
            if (SUCCEEDED(hr))
                IDirect3DVertexBuffer9_Unlock(managed);
        }

        hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff004080, 1.0f, 0);
        ok_(SUCCEEDED(hr), "Clear after Reset returned 0x%08lx", hr);
        IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
    }

cleanup:
    D3DTEST_RELEASE(managed);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
