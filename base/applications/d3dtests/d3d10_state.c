/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 10: rasteriser, blend and depth-stencil state objects
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
    ID3D10DepthStencilState *ds = NULL;
    ID3D10RasterizerState *rs = NULL;
    ID3D10BlendState *bs = NULL;
    ID3D10SamplerState *ss = NULL;
    ID3D10Device *device = NULL;
    D3D10_DEPTH_STENCIL_DESC dsd;
    D3D10_RASTERIZER_DESC rsd, got_rsd;
    D3D10_SAMPLER_DESC sd;
    D3D10_BLEND_DESC bd;
    HRESULT hr;

    test_begin("d3d10_state");

    hr = create_d3d10_device(&device);
    if (FAILED(hr))
    {
        skip_("no Direct3D 10 device available (0x%08lx)", hr);
        return test_end();
    }

    memset(&rsd, 0, sizeof(rsd));
    rsd.FillMode = D3D10_FILL_SOLID;
    rsd.CullMode = D3D10_CULL_BACK;
    rsd.DepthClipEnable = TRUE;
    hr = ID3D10Device_CreateRasterizerState(device, &rsd, &rs);
    ok_(SUCCEEDED(hr) && rs != NULL, "CreateRasterizerState returned 0x%08lx", hr);

    if (rs)
    {
        memset(&got_rsd, 0, sizeof(got_rsd));
        ID3D10RasterizerState_GetDesc(rs, &got_rsd);
        ok_(got_rsd.FillMode == D3D10_FILL_SOLID && got_rsd.CullMode == D3D10_CULL_BACK,
            "rasteriser state round-tripped (fill %u, cull %u)",
            got_rsd.FillMode, got_rsd.CullMode);

        ID3D10Device_RSSetState(device, rs);
        ok_(1, "RSSetState completed");
    }

    memset(&bd, 0, sizeof(bd));
    bd.BlendEnable[0] = TRUE;
    bd.SrcBlend = D3D10_BLEND_SRC_ALPHA;
    bd.DestBlend = D3D10_BLEND_INV_SRC_ALPHA;
    bd.BlendOp = D3D10_BLEND_OP_ADD;
    bd.SrcBlendAlpha = D3D10_BLEND_ONE;
    bd.DestBlendAlpha = D3D10_BLEND_ZERO;
    bd.BlendOpAlpha = D3D10_BLEND_OP_ADD;
    bd.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
    hr = ID3D10Device_CreateBlendState(device, &bd, &bs);
    ok_(SUCCEEDED(hr) && bs != NULL, "CreateBlendState returned 0x%08lx", hr);

    memset(&dsd, 0, sizeof(dsd));
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D10_COMPARISON_LESS;
    hr = ID3D10Device_CreateDepthStencilState(device, &dsd, &ds);
    ok_(SUCCEEDED(hr) && ds != NULL, "CreateDepthStencilState returned 0x%08lx", hr);

    memset(&sd, 0, sizeof(sd));
    sd.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D10_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D10_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D10_COMPARISON_NEVER;
    /* The macro is a double literal; MaxLOD is a FLOAT. */
    sd.MaxLOD = (FLOAT)D3D10_FLOAT32_MAX;
    hr = ID3D10Device_CreateSamplerState(device, &sd, &ss);
    ok_(SUCCEEDED(hr) && ss != NULL, "CreateSamplerState returned 0x%08lx", hr);

    D3DTEST_RELEASE(ss);
    D3DTEST_RELEASE(ds);
    D3DTEST_RELEASE(bs);
    D3DTEST_RELEASE(rs);
    D3DTEST_RELEASE(device);
    return test_end();
}
