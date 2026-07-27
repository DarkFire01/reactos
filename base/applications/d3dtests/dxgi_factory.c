/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DXGI: factory creation and adapter enumeration
 */


#include "d3dtest.h"
#include <dxgi.h>

int main(void)
{
    IDXGIFactory *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC desc;
    UINT count = 0;
    HRESULT hr;

    test_begin("dxgi_factory");

    hr = CreateDXGIFactory(&IID_IDXGIFactory, (void **)&factory);
    ok_(SUCCEEDED(hr) && factory != NULL, "CreateDXGIFactory returned 0x%08lx", hr);
    if (!factory)
        return test_end();

    while (IDXGIFactory_EnumAdapters(factory, count, &adapter) != DXGI_ERROR_NOT_FOUND)
    {
        memset(&desc, 0, sizeof(desc));
        hr = IDXGIAdapter_GetDesc(adapter, &desc);
        ok_(SUCCEEDED(hr), "adapter %u: GetDesc returned 0x%08lx", count, hr);
        if (SUCCEEDED(hr))
            info_("adapter %u: '%ls' vendor 0x%04x device 0x%04x",
                  count, desc.Description, desc.VendorId, desc.DeviceId);
        D3DTEST_RELEASE(adapter);
        count++;
        if (count > 16)
            break;
    }
    ok_(count >= 1, "enumerated %u adapter(s)", count);

    /* Past the end must report NOT_FOUND rather than an arbitrary failure. */
    hr = IDXGIFactory_EnumAdapters(factory, count, &adapter);
    ok_(hr == DXGI_ERROR_NOT_FOUND,
        "EnumAdapters past the end returned 0x%08lx, expected DXGI_ERROR_NOT_FOUND", hr);

    D3DTEST_RELEASE(factory);
    return test_end();
}
