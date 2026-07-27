/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DXGI: window association and Alt+Enter handling
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
    IDXGIFactory *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIDevice *dxgi_device = NULL;
    ID3D11Device *device = NULL;
    D3D_FEATURE_LEVEL level;
    HWND got = NULL;
    HRESULT hr;
    HWND hwnd;

    test_begin("dxgi_windowassoc");

    hwnd = test_create_window("dxgi_windowassoc", 320, 240);
    hr = make_d3d11(&device, &context, &level);
    if (FAILED(hr)) { skip_("no Direct3D 11 device (0x%08lx)", hr); goto done; }

    if (FAILED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device))
        || FAILED(IDXGIDevice_GetAdapter(dxgi_device, &adapter))
        || FAILED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory)))
    {
        skip_("could not reach the DXGI factory");
        goto cleanup;
    }
    ok_(factory != NULL, "reached the DXGI factory through device -> adapter -> parent");

    /* Nothing associated yet. */
    hr = IDXGIFactory_GetWindowAssociation(factory, &got);
    ok_(SUCCEEDED(hr), "GetWindowAssociation returned 0x%08lx", hr);

    hr = IDXGIFactory_MakeWindowAssociation(factory, hwnd, DXGI_MWA_NO_ALT_ENTER);
    ok_(SUCCEEDED(hr), "MakeWindowAssociation(NO_ALT_ENTER) returned 0x%08lx", hr);

    got = NULL;
    hr = IDXGIFactory_GetWindowAssociation(factory, &got);
    ok_(SUCCEEDED(hr), "GetWindowAssociation after the change returned 0x%08lx", hr);
    info_("factory reports associated window %p (ours is %p)", got, hwnd);

    /* Clearing the association is done by passing NULL. */
    hr = IDXGIFactory_MakeWindowAssociation(factory, NULL, 0);
    ok_(SUCCEEDED(hr), "clearing the association returned 0x%08lx", hr);

    /* An invalid flag combination should be rejected. */
    hr = IDXGIFactory_MakeWindowAssociation(factory, hwnd, 0xf0000000);
    ok_(FAILED(hr), "MakeWindowAssociation with bogus flags returned 0x%08lx", hr);

cleanup:
    D3DTEST_RELEASE(factory);
    D3DTEST_RELEASE(adapter);
    D3DTEST_RELEASE(dxgi_device);
    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
done:
    test_destroy_window(hwnd);
    return test_end();
}
