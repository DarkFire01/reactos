/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DXGI: adapter descriptions and memory reporting
 */


#include "d3dtest.h"
#include <dxgi.h>

int main(void)
{
    IDXGIFactory *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIAdapter1 *adapter1 = NULL;
    DXGI_ADAPTER_DESC desc;
    DXGI_ADAPTER_DESC1 desc1;
    LARGE_INTEGER version;
    HRESULT hr;

    test_begin("dxgi_adapter");

    hr = CreateDXGIFactory(&IID_IDXGIFactory, (void **)&factory);
    ok_(SUCCEEDED(hr) && factory != NULL, "CreateDXGIFactory returned 0x%08lx", hr);
    if (!factory)
        return test_end();

    hr = IDXGIFactory_EnumAdapters(factory, 0, &adapter);
    if (FAILED(hr))
    {
        skip_("no adapter 0 (0x%08lx)", hr);
        D3DTEST_RELEASE(factory);
        return test_end();
    }
    ok_(SUCCEEDED(hr), "EnumAdapters(0) returned 0x%08lx", hr);

    memset(&desc, 0, sizeof(desc));
    hr = IDXGIAdapter_GetDesc(adapter, &desc);
    ok_(SUCCEEDED(hr), "GetDesc returned 0x%08lx", hr);
    if (SUCCEEDED(hr))
    {
        info_("dedicated video memory: %lu MB",
              (unsigned long)(desc.DedicatedVideoMemory / (1024 * 1024)));
        info_("dedicated system memory: %lu MB",
              (unsigned long)(desc.DedicatedSystemMemory / (1024 * 1024)));
        info_("shared system memory: %lu MB",
              (unsigned long)(desc.SharedSystemMemory / (1024 * 1024)));
        ok_(desc.Description[0] != 0, "adapter has a non-empty description");
        ok_(desc.AdapterLuid.LowPart != 0 || desc.AdapterLuid.HighPart != 0,
            "adapter has a non-zero LUID");
    }

    /* CheckInterfaceSupport tells us whether a runtime is present at all. */
    hr = IDXGIAdapter_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &version);
    info_("CheckInterfaceSupport(IDXGIDevice) returned 0x%08lx", hr);

    hr = IDXGIAdapter_QueryInterface(adapter, &IID_IDXGIAdapter1, (void **)&adapter1);
    if (SUCCEEDED(hr) && adapter1)
    {
        memset(&desc1, 0, sizeof(desc1));
        hr = IDXGIAdapter1_GetDesc1(adapter1, &desc1);
        ok_(SUCCEEDED(hr), "GetDesc1 returned 0x%08lx", hr);
        if (SUCCEEDED(hr))
            info_("adapter1 flags 0x%08lx%s", (unsigned long)desc1.Flags,
                  (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
        D3DTEST_RELEASE(adapter1);
    }
    else
    {
        skip_("no IDXGIAdapter1 (0x%08lx)", hr);
    }

    D3DTEST_RELEASE(adapter);
    D3DTEST_RELEASE(factory);
    return test_end();
}
