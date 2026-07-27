/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: device creation and feature level selection
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

int main(void)
{
    ID3D11DeviceContext *context = NULL;
    ID3D11Device *device = NULL;
    D3D_FEATURE_LEVEL level = 0;
    UINT support = 0;
    HRESULT hr;

    test_begin("d3d11_device");

    hr = create_d3d11_device(&device, &context, &level);
    if (FAILED(hr))
    {
        skip_("no Direct3D 11 device available (0x%08lx)", hr);
        return test_end();
    }
    ok_(SUCCEEDED(hr) && device != NULL, "created a Direct3D 11 device");
    ok_(context != NULL, "device came with an immediate context");

    info_("feature level 0x%04x", (unsigned)level);
    ok_(level >= D3D_FEATURE_LEVEL_9_1,
        "reported feature level 0x%04x is at least 9_1", (unsigned)level);
    ok_(ID3D11Device_GetFeatureLevel(device) == level,
        "GetFeatureLevel agrees with the creation output");

    hr = ID3D11Device_GetDeviceRemovedReason(device);
    ok_(hr == S_OK, "GetDeviceRemovedReason returned 0x%08lx on a healthy device", hr);

    hr = ID3D11Device_CheckFormatSupport(device, DXGI_FORMAT_R8G8B8A8_UNORM, &support);
    ok_(SUCCEEDED(hr), "CheckFormatSupport(R8G8B8A8_UNORM) returned 0x%08lx", hr);
    if (SUCCEEDED(hr))
        ok_(support & D3D11_FORMAT_SUPPORT_TEXTURE2D,
            "R8G8B8A8_UNORM is usable as a 2D texture (mask 0x%08lx)", (unsigned long)support);

    /* The immediate context must report itself as such. */
    if (context)
        ok_(ID3D11DeviceContext_GetType(context) == D3D11_DEVICE_CONTEXT_IMMEDIATE,
            "context reports type IMMEDIATE");

    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
