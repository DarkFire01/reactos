/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 10.1: device creation and feature levels
 */


#include "d3dtest.h"
#include <d3d10_1.h>

int main(void)
{
    static const D3D10_FEATURE_LEVEL1 levels[] =
    {
        D3D10_FEATURE_LEVEL_10_1,
        D3D10_FEATURE_LEVEL_10_0,
        D3D10_FEATURE_LEVEL_9_3,
        D3D10_FEATURE_LEVEL_9_2,
        D3D10_FEATURE_LEVEL_9_1,
    };
    ID3D10Device1 *device = NULL;
    D3D10_FEATURE_LEVEL1 got;
    unsigned int i;
    HRESULT hr = E_FAIL;

    test_begin("d3d10_1_device");

    /* Walk down from 10.1 until one is accepted. */
    for (i = 0; i < ARRAYSIZE(levels); i++)
    {
        hr = D3D10CreateDevice1(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, levels[i],
                                D3D10_1_SDK_VERSION, &device);
        if (SUCCEEDED(hr))
            break;
        hr = D3D10CreateDevice1(NULL, D3D10_DRIVER_TYPE_WARP, NULL, 0, levels[i],
                                D3D10_1_SDK_VERSION, &device);
        if (SUCCEEDED(hr))
            break;
    }

    if (FAILED(hr) || !device)
    {
        skip_("no Direct3D 10.1 device available (0x%08lx)", hr);
        return test_end();
    }
    ok_(SUCCEEDED(hr), "created a Direct3D 10.1 device at level index %u", i);

    got = ID3D10Device1_GetFeatureLevel(device);
    info_("device reports feature level 0x%04x", (unsigned)got);
    ok_(got == levels[i], "GetFeatureLevel agrees with what was requested");

    /* The 10.1 device must also be usable through the plain 10 interface. */
    {
        ID3D10Device *base = NULL;
        hr = ID3D10Device1_QueryInterface(device, &IID_ID3D10Device, (void **)&base);
        ok_(SUCCEEDED(hr) && base != NULL, "QueryInterface(ID3D10Device) returned 0x%08lx", hr);
        if (base)
        {
            UINT support = 0;
            hr = ID3D10Device_CheckFormatSupport(base, DXGI_FORMAT_R8G8B8A8_UNORM, &support);
            ok_(SUCCEEDED(hr), "CheckFormatSupport through the base interface returned 0x%08lx", hr);
            D3DTEST_RELEASE(base);
        }
    }

    /* A blend state description that only 10.1 accepts. */
    {
        ID3D10BlendState1 *bs = NULL;
        D3D10_BLEND_DESC1 bd;

        memset(&bd, 0, sizeof(bd));
        bd.IndependentBlendEnable = TRUE;
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D10_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D10_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D10_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D10_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D10_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D10_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D10_COLOR_WRITE_ENABLE_ALL;

        hr = ID3D10Device1_CreateBlendState1(device, &bd, &bs);
        ok_(SUCCEEDED(hr) && bs != NULL, "CreateBlendState1 returned 0x%08lx", hr);
        D3DTEST_RELEASE(bs);
    }

    D3DTEST_RELEASE(device);
    return test_end();
}
