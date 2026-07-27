/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: rasteriser, blend, depth-stencil and sampler state
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
    ID3D11DepthStencilState *ds = NULL;
    ID3D11RasterizerState *rs = NULL, *got_rs = NULL;
    ID3D11BlendState *bs = NULL;
    ID3D11SamplerState *ss = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Device *device = NULL;
    D3D11_DEPTH_STENCIL_DESC dsd;
    D3D11_RASTERIZER_DESC rsd, got_rsd;
    D3D11_SAMPLER_DESC sd;
    D3D11_BLEND_DESC bd;
    D3D_FEATURE_LEVEL level;
    HRESULT hr;

    test_begin("d3d11_state");

    hr = create_d3d11_device(&device, &context, &level);
    if (FAILED(hr))
    {
        skip_("no Direct3D 11 device available (0x%08lx)", hr);
        return test_end();
    }

    memset(&rsd, 0, sizeof(rsd));
    rsd.FillMode = D3D11_FILL_SOLID;
    rsd.CullMode = D3D11_CULL_BACK;
    rsd.DepthClipEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(device, &rsd, &rs);
    ok_(SUCCEEDED(hr) && rs != NULL, "CreateRasterizerState returned 0x%08lx", hr);

    if (rs && context)
    {
        memset(&got_rsd, 0, sizeof(got_rsd));
        ID3D11RasterizerState_GetDesc(rs, &got_rsd);
        ok_(got_rsd.FillMode == D3D11_FILL_SOLID && got_rsd.CullMode == D3D11_CULL_BACK,
            "rasteriser state round-tripped (fill %u, cull %u)",
            got_rsd.FillMode, got_rsd.CullMode);

        ID3D11DeviceContext_RSSetState(context, rs);
        ID3D11DeviceContext_RSGetState(context, &got_rs);
        ok_(got_rs == rs, "context holds %p, expected %p", got_rs, rs);
        D3DTEST_RELEASE(got_rs);
    }

    memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = ID3D11Device_CreateBlendState(device, &bd, &bs);
    ok_(SUCCEEDED(hr) && bs != NULL, "CreateBlendState returned 0x%08lx", hr);

    memset(&dsd, 0, sizeof(dsd));
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    hr = ID3D11Device_CreateDepthStencilState(device, &dsd, &ds);
    ok_(SUCCEEDED(hr) && ds != NULL, "CreateDepthStencilState returned 0x%08lx", hr);

    memset(&sd, 0, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    /* The macro is a double literal; MaxLOD is a FLOAT. */
    sd.MaxLOD = (FLOAT)D3D11_FLOAT32_MAX;
    hr = ID3D11Device_CreateSamplerState(device, &sd, &ss);
    ok_(SUCCEEDED(hr) && ss != NULL, "CreateSamplerState returned 0x%08lx", hr);

    /* An out-of-range filter must be rejected. */
    {
        ID3D11SamplerState *bad = NULL;
        sd.Filter = (D3D11_FILTER)0x7fffffff;
        hr = ID3D11Device_CreateSamplerState(device, &sd, &bad);
        ok_(FAILED(hr), "CreateSamplerState with a bogus filter returned 0x%08lx", hr);
        D3DTEST_RELEASE(bad);
    }

    D3DTEST_RELEASE(ss);
    D3DTEST_RELEASE(ds);
    D3DTEST_RELEASE(bs);
    D3DTEST_RELEASE(rs);
    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
