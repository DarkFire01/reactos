/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 10: device creation and feature reporting
 */


#include "d3dtest.h"
#include <d3d10_1.h>

static HRESULT D3DTEST_UNUSED create_d3d10_device(ID3D10Device **device)
{
    static const D3D10_DRIVER_TYPE types[] =
    {
        D3D10_DRIVER_TYPE_HARDWARE,
        D3D10_DRIVER_TYPE_WARP,
        D3D10_DRIVER_TYPE_REFERENCE,
    };
    HRESULT hr = E_FAIL;
    unsigned int i;

    for (i = 0; i < ARRAYSIZE(types); i++)
    {
        hr = D3D10CreateDevice(NULL, types[i], NULL, 0, D3D10_SDK_VERSION, device);
        if (SUCCEEDED(hr))
            return hr;
    }
    return hr;
}

int main(void)
{
    ID3D10Device *device = NULL;
    UINT support = 0;
    HRESULT hr;

    test_begin("d3d10_device");

    hr = create_d3d10_device(&device);
    if (FAILED(hr))
    {
        skip_("no Direct3D 10 device available (0x%08lx)", hr);
        return test_end();
    }
    ok_(SUCCEEDED(hr) && device != NULL, "created a Direct3D 10 device");

    hr = ID3D10Device_CheckFormatSupport(device, DXGI_FORMAT_R8G8B8A8_UNORM, &support);
    ok_(SUCCEEDED(hr), "CheckFormatSupport(R8G8B8A8_UNORM) returned 0x%08lx", hr);
    if (SUCCEEDED(hr))
    {
        info_("R8G8B8A8_UNORM support mask 0x%08lx", (unsigned long)support);
        ok_(support & D3D10_FORMAT_SUPPORT_TEXTURE2D,
            "R8G8B8A8_UNORM is usable as a 2D texture");
    }

    hr = ID3D10Device_GetDeviceRemovedReason(device);
    ok_(hr == S_OK, "GetDeviceRemovedReason returned 0x%08lx on a healthy device", hr);

    info_("creation flags 0x%08lx", (unsigned long)ID3D10Device_GetCreationFlags(device));

    D3DTEST_RELEASE(device);
    return test_end();
}
