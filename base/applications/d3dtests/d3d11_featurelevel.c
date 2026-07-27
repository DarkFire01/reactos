/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: explicit feature level requests
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
    static const D3D_FEATURE_LEVEL wanted[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    };
    ID3D11DeviceContext *context = NULL;
    ID3D11Device *device = NULL;
    D3D_FEATURE_LEVEL level = 0;
    HRESULT hr;
    unsigned int i;

    test_begin("d3d11_featurelevel");

    /* Asking for the whole list should land on the best the driver supports. */
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, wanted,
                           ARRAYSIZE(wanted), D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, wanted,
                               ARRAYSIZE(wanted), D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr))
    {
        skip_("no Direct3D 11 device available (0x%08lx)", hr);
        return test_end();
    }

    ok_(SUCCEEDED(hr), "created a device from the full feature level list");
    info_("granted feature level 0x%04x", (unsigned)level);

    for (i = 0; i < ARRAYSIZE(wanted); i++)
    {
        if (wanted[i] == level)
            break;
    }
    ok_(i < ARRAYSIZE(wanted), "granted level 0x%04x appears in the requested list",
        (unsigned)level);

    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);

    /* Requesting exactly 9_1 must give back exactly 9_1. */
    {
        static const D3D_FEATURE_LEVEL only_91[] = { D3D_FEATURE_LEVEL_9_1 };

        level = 0;
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, only_91, 1,
                               D3D11_SDK_VERSION, &device, &level, &context);
        if (FAILED(hr))
            hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, only_91, 1,
                                   D3D11_SDK_VERSION, &device, &level, &context);
        if (SUCCEEDED(hr))
        {
            ok_(level == D3D_FEATURE_LEVEL_9_1,
                "asked for 9_1 only and got 0x%04x", (unsigned)level);
            D3DTEST_RELEASE(context);
            D3DTEST_RELEASE(device);
        }
        else
        {
            skip_("could not create a 9_1-only device (0x%08lx)", hr);
        }
    }

    /* A NULL device pointer is the documented way to probe support. */
    level = 0;
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, wanted,
                           ARRAYSIZE(wanted), D3D11_SDK_VERSION, NULL, &level, NULL);
    info_("probe-only D3D11CreateDevice returned 0x%08lx, level 0x%04x", hr, (unsigned)level);

    return test_end();
}
