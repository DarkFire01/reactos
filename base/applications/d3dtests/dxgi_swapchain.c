/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DXGI: swap chain creation and Present through D3D11
 */


#include "d3dtest.h"
#include <d3d11.h>

/* Try the hardware driver first and fall back to WARP/reference, so the test
   still exercises the API surface on a machine with no 3D driver. */
static HRESULT D3DTEST_UNUSED create_d3d11_device(ID3D11Device **device, ID3D11DeviceContext **context,
                                   D3D_FEATURE_LEVEL *level)
{
    static const D3D_DRIVER_TYPE types[] =
    {
        D3D_DRIVER_TYPE_HARDWARE,
        D3D_DRIVER_TYPE_WARP,
        D3D_DRIVER_TYPE_REFERENCE,
    };
    HRESULT hr = E_FAIL;
    unsigned int i;

    for (i = 0; i < ARRAYSIZE(types); i++)
    {
        hr = D3D11CreateDevice(NULL, types[i], NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                               device, level, context);
        if (SUCCEEDED(hr))
            return hr;
    }
    return hr;
}

#include <dxgi.h>

int main(void)
{
    ID3D11DeviceContext *context = NULL;
    IDXGISwapChain *swapchain = NULL;
    IDXGIFactory *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIDevice *dxgi_device = NULL;
    ID3D11Device *device = NULL;
    ID3D11Texture2D *bb = NULL;
    DXGI_SWAP_CHAIN_DESC scd, got;
    D3D11_TEXTURE2D_DESC tex;
    D3D_FEATURE_LEVEL level;
    HRESULT hr;
    HWND hwnd;
    int i;

    test_begin("dxgi_swapchain");

    hwnd = test_create_window("dxgi_swapchain", 320, 240);
    ShowWindow(hwnd, SW_SHOW);
    test_pump();

    hr = create_d3d11_device(&device, &context, &level);
    if (FAILED(hr))
    {
        skip_("no Direct3D 11 device available (0x%08lx)", hr);
        goto done;
    }

    /* Walk device -> adapter -> factory, the documented route to the factory
       that owns this device. */
    hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device);
    ok_(SUCCEEDED(hr) && dxgi_device != NULL, "QueryInterface(IDXGIDevice) returned 0x%08lx", hr);
    if (!dxgi_device)
        goto cleanup;

    hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    ok_(SUCCEEDED(hr) && adapter != NULL, "IDXGIDevice::GetAdapter returned 0x%08lx", hr);
    if (!adapter)
        goto cleanup;

    hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory);
    ok_(SUCCEEDED(hr) && factory != NULL, "IDXGIAdapter::GetParent returned 0x%08lx", hr);
    if (!factory)
        goto cleanup;

    memset(&scd, 0, sizeof(scd));
    scd.BufferDesc.Width = 256;
    scd.BufferDesc.Height = 256;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 1;
    scd.OutputWindow = hwnd;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = IDXGIFactory_CreateSwapChain(factory, (IUnknown *)device, &scd, &swapchain);
    if (FAILED(hr))
    {
        skip_("CreateSwapChain returned 0x%08lx", hr);
        goto cleanup;
    }
    ok_(SUCCEEDED(hr) && swapchain != NULL, "created a swap chain");

    memset(&got, 0, sizeof(got));
    hr = IDXGISwapChain_GetDesc(swapchain, &got);
    ok_(SUCCEEDED(hr), "GetDesc returned 0x%08lx", hr);
    ok_(got.BufferDesc.Width == 256 && got.BufferDesc.Height == 256,
        "swap chain is %ux%u, expected 256x256", got.BufferDesc.Width, got.BufferDesc.Height);
    ok_(got.OutputWindow == hwnd, "swap chain targets %p, expected %p", got.OutputWindow, hwnd);

    hr = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ok_(SUCCEEDED(hr) && bb != NULL, "GetBuffer(0) returned 0x%08lx", hr);

    if (bb)
    {
        memset(&tex, 0, sizeof(tex));
        ID3D11Texture2D_GetDesc(bb, &tex);
        ok_(tex.Width == 256 && tex.Height == 256,
            "back buffer texture is %ux%u", tex.Width, tex.Height);
        ok_(tex.BindFlags & D3D11_BIND_RENDER_TARGET,
            "back buffer bind flags 0x%08lx include RENDER_TARGET", (unsigned long)tex.BindFlags);
    }

    for (i = 0; i < 3; i++)
    {
        hr = IDXGISwapChain_Present(swapchain, 0, 0);
        ok_(SUCCEEDED(hr), "frame %d: Present returned 0x%08lx", i, hr);
        test_pump();
    }

cleanup:
    D3DTEST_RELEASE(bb);
    D3DTEST_RELEASE(swapchain);
    D3DTEST_RELEASE(factory);
    D3DTEST_RELEASE(adapter);
    D3DTEST_RELEASE(dxgi_device);
    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
done:
    test_destroy_window(hwnd);
    return test_end();
}
