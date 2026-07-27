/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DXGI: swap chain buffer resizing
 */


#include "d3dtest.h"
#include <d3d11.h>

static D3DTEST_UNUSED HRESULT make_d3d11(ID3D11Device **device, ID3D11DeviceContext **context,
                                         D3D_FEATURE_LEVEL *level)
{
    static const D3D_DRIVER_TYPE types[] =
    {
        D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_REFERENCE,
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

/* Make a render target plus a staging copy so results can be inspected. */
static D3DTEST_UNUSED HRESULT make_rt(ID3D11Device *device, UINT size,
                                      ID3D11Texture2D **rt, ID3D11RenderTargetView **rtv,
                                      ID3D11Texture2D **staging)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    memset(&td, 0, sizeof(td));
    td.Width = size;
    td.Height = size;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(hr = ID3D11Device_CreateTexture2D(device, &td, NULL, rt)))
        return hr;

    if (FAILED(hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)*rt, NULL, rtv)))
        return hr;

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return ID3D11Device_CreateTexture2D(device, &td, NULL, staging);
}

static D3DTEST_UNUSED BOOL read_back(ID3D11DeviceContext *context, ID3D11Texture2D *rt,
                                     ID3D11Texture2D *staging, UINT x, UINT y, DWORD *out)
{
    D3D11_MAPPED_SUBRESOURCE m;

    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)rt);
    memset(&m, 0, sizeof(m));
    if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                       D3D11_MAP_READ, 0, &m)))
        return FALSE;
    *out = *(DWORD *)((BYTE *)m.pData + y * m.RowPitch + x * 4);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    return TRUE;
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
    D3D11_TEXTURE2D_DESC td;
    D3D_FEATURE_LEVEL level;
    HRESULT hr;
    HWND hwnd;

    test_begin("dxgi_resize");

    hwnd = test_create_window("dxgi_resize", 320, 240);
    hr = make_d3d11(&device, &context, &level);
    if (FAILED(hr)) { skip_("no Direct3D 11 device (0x%08lx)", hr); goto done; }

    if (FAILED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device))
        || FAILED(IDXGIDevice_GetAdapter(dxgi_device, &adapter))
        || FAILED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory)))
    {
        skip_("could not reach the DXGI factory");
        goto cleanup;
    }

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
    if (FAILED(hr)) { skip_("CreateSwapChain returned 0x%08lx", hr); goto cleanup; }
    ok_(SUCCEEDED(hr), "created a 256x256 swap chain");

    /* Every reference to a back buffer must be released before resizing. */
    hr = IDXGISwapChain_ResizeBuffers(swapchain, 1, 128, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    ok_(SUCCEEDED(hr), "ResizeBuffers(128x128) returned 0x%08lx", hr);

    if (SUCCEEDED(hr))
    {
        memset(&got, 0, sizeof(got));
        IDXGISwapChain_GetDesc(swapchain, &got);
        ok_(got.BufferDesc.Width == 128 && got.BufferDesc.Height == 128,
            "swap chain is %ux%u after the resize, expected 128x128",
            got.BufferDesc.Width, got.BufferDesc.Height);

        hr = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&bb);
        ok_(SUCCEEDED(hr) && bb != NULL, "GetBuffer after the resize returned 0x%08lx", hr);
        if (bb)
        {
            memset(&td, 0, sizeof(td));
            ID3D11Texture2D_GetDesc(bb, &td);
            ok_(td.Width == 128 && td.Height == 128,
                "back buffer texture is %ux%u after the resize", td.Width, td.Height);
            D3DTEST_RELEASE(bb);
        }
    }

    /* Resizing while a back buffer reference is outstanding must be refused. */
    hr = IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&bb);
    if (SUCCEEDED(hr))
    {
        hr = IDXGISwapChain_ResizeBuffers(swapchain, 1, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        ok_(FAILED(hr),
            "ResizeBuffers with an outstanding buffer returned 0x%08lx, expected failure", hr);
        D3DTEST_RELEASE(bb);
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
