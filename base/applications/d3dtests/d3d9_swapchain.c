/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: additional swap chains
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
    IDirect3DSwapChain9 *swapchain = NULL, *implicit = NULL;
    IDirect3DSurface9 *bb = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3DSURFACE_DESC desc;
    HWND hwnd, second;
    UINT count;
    HRESULT hr;

    test_begin("d3d9_swapchain");

    hwnd = test_create_window("d3d9_swapchain", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 9 device could be created on this adapter");
        goto cleanup;
    }

    count = IDirect3DDevice9_GetNumberOfSwapChains(device);
    ok_(count == 1, "device reports %u swap chain(s), expected the implicit one", count);

    hr = IDirect3DDevice9_GetSwapChain(device, 0, &implicit);
    ok_(SUCCEEDED(hr) && implicit != NULL, "GetSwapChain(0) returned 0x%08lx", hr);

    if (implicit)
    {
        memset(&pp, 0, sizeof(pp));
        hr = IDirect3DSwapChain9_GetPresentParameters(implicit, &pp);
        ok_(SUCCEEDED(hr), "GetPresentParameters returned 0x%08lx", hr);
        ok_(pp.BackBufferWidth == 256 && pp.BackBufferHeight == 256,
            "implicit chain is %ux%u, expected 256x256",
            pp.BackBufferWidth, pp.BackBufferHeight);
        D3DTEST_RELEASE(implicit);
    }

    /* An extra chain targeting a second window. */
    second = CreateWindowExA(0, "d3dtest_window", "d3d9_swapchain 2",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             200, 200, NULL, NULL, GetModuleHandleA(NULL), NULL);
    ok_(second != NULL, "created a second window");

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 128;
    pp.BackBufferHeight = 128;
    pp.hDeviceWindow = second;

    hr = IDirect3DDevice9_CreateAdditionalSwapChain(device, &pp, &swapchain);
    if (FAILED(hr))
    {
        skip_("CreateAdditionalSwapChain returned 0x%08lx", hr);
    }
    else
    {
        ok_(SUCCEEDED(hr) && swapchain != NULL, "created an additional swap chain");

        count = IDirect3DDevice9_GetNumberOfSwapChains(device);
        ok_(count == 1,
            "device still reports %u swap chain(s): additional chains are not counted", count);

        hr = IDirect3DSwapChain9_GetBackBuffer(swapchain, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
        ok_(SUCCEEDED(hr) && bb != NULL, "GetBackBuffer on the extra chain returned 0x%08lx", hr);

        if (bb)
        {
            memset(&desc, 0, sizeof(desc));
            IDirect3DSurface9_GetDesc(bb, &desc);
            ok_(desc.Width == 128 && desc.Height == 128,
                "extra chain back buffer is %ux%u, expected 128x128", desc.Width, desc.Height);
            D3DTEST_RELEASE(bb);
        }

        hr = IDirect3DSwapChain9_Present(swapchain, NULL, NULL, NULL, NULL, 0);
        ok_(SUCCEEDED(hr), "Present on the extra chain returned 0x%08lx", hr);
    }

    if (second)
        DestroyWindow(second);

cleanup:
    D3DTEST_RELEASE(swapchain);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
