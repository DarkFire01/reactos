/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: depth testing verified by reading the result back
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

#include <d3dcompiler.h>

static const char vs_source[] =
    "float4 main(float4 pos : POSITION) : SV_POSITION { return pos; }\n";
static const char ps_red[] =
    "float4 main() : SV_TARGET { return float4(1.0f, 0.0f, 0.0f, 1.0f); }\n";
static const char ps_green[] =
    "float4 main() : SV_TARGET { return float4(0.0f, 1.0f, 0.0f, 1.0f); }\n";

/* Two full-screen quads: the near one at z=0.2, the far one at z=0.8. */
static const float near_quad[] =
{
    -1.0f,  1.0f, 0.2f, 1.0f,   1.0f,  1.0f, 0.2f, 1.0f,  -1.0f, -1.0f, 0.2f, 1.0f,
     1.0f,  1.0f, 0.2f, 1.0f,   1.0f, -1.0f, 0.2f, 1.0f,  -1.0f, -1.0f, 0.2f, 1.0f,
};
static const float far_quad[] =
{
    -1.0f,  1.0f, 0.8f, 1.0f,   1.0f,  1.0f, 0.8f, 1.0f,  -1.0f, -1.0f, 0.8f, 1.0f,
     1.0f,  1.0f, 0.8f, 1.0f,   1.0f, -1.0f, 0.8f, 1.0f,  -1.0f, -1.0f, 0.8f, 1.0f,
};

int main(void)
{
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Texture2D *target = NULL, *staging = NULL, *depth = NULL;
    ID3D10Blob *vsb = NULL, *psb_r = NULL, *psb_g = NULL, *errors = NULL;
    ID3D11DepthStencilState *dss = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11InputLayout *layout = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps_r = NULL, *ps_g = NULL;
    ID3D11Device *device = NULL;
    ID3D11Buffer *vb_near = NULL, *vb_far = NULL;
    D3D11_INPUT_ELEMENT_DESC elements[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    D3D11_DEPTH_STENCIL_DESC dsd;
    D3D11_SUBRESOURCE_DATA data;
    D3D11_TEXTURE2D_DESC td;
    D3D11_BUFFER_DESC bd;
    D3D_FEATURE_LEVEL level;
    D3D11_VIEWPORT vp;
    UINT stride = 16, offset = 0;
    float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    DWORD pixel = 0;
    HRESULT hr;

    test_begin("d3d11_depth");

    hr = make_d3d11(&device, &context, &level);
    if (FAILED(hr)) { skip_("no Direct3D 11 device (0x%08lx)", hr); return test_end(); }

    hr = D3DCompile(vs_source, sizeof(vs_source) - 1, NULL, NULL, NULL,
                    "main", "vs_4_0", 0, 0, &vsb, &errors);
    if (FAILED(hr)) { skip_("D3DCompile(vs_4_0) returned 0x%08lx", hr); goto cleanup; }
    D3DTEST_RELEASE(errors);
    hr = D3DCompile(ps_red, sizeof(ps_red) - 1, NULL, NULL, NULL,
                    "main", "ps_4_0", 0, 0, &psb_r, &errors);
    if (FAILED(hr)) { skip_("D3DCompile(red) returned 0x%08lx", hr); goto cleanup; }
    D3DTEST_RELEASE(errors);
    hr = D3DCompile(ps_green, sizeof(ps_green) - 1, NULL, NULL, NULL,
                    "main", "ps_4_0", 0, 0, &psb_g, &errors);
    if (FAILED(hr)) { skip_("D3DCompile(green) returned 0x%08lx", hr); goto cleanup; }
    D3DTEST_RELEASE(errors);

    ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vsb),
            ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(psb_r),
            ID3D10Blob_GetBufferSize(psb_r), NULL, &ps_r);
    ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(psb_g),
            ID3D10Blob_GetBufferSize(psb_g), NULL, &ps_g);
    ID3D11Device_CreateInputLayout(device, elements, 1, ID3D10Blob_GetBufferPointer(vsb),
            ID3D10Blob_GetBufferSize(vsb), &layout);

    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(near_quad);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&data, 0, sizeof(data));
    data.pSysMem = near_quad;
    ID3D11Device_CreateBuffer(device, &bd, &data, &vb_near);
    data.pSysMem = far_quad;
    ID3D11Device_CreateBuffer(device, &bd, &data, &vb_far);

    if (FAILED(make_rt(device, 64, &target, &rtv, &staging)))
    {
        skip_("could not build a render target");
        goto cleanup;
    }

    memset(&td, 0, sizeof(td));
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = ID3D11Device_CreateTexture2D(device, &td, NULL, &depth);
    ok_(SUCCEEDED(hr) && depth != NULL, "CreateTexture2D(D32_FLOAT) returned 0x%08lx", hr);
    if (!depth) goto cleanup;

    hr = ID3D11Device_CreateDepthStencilView(device, (ID3D11Resource *)depth, NULL, &dsv);
    ok_(SUCCEEDED(hr) && dsv != NULL, "CreateDepthStencilView returned 0x%08lx", hr);

    memset(&dsd, 0, sizeof(dsd));
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    hr = ID3D11Device_CreateDepthStencilState(device, &dsd, &dss);
    ok_(SUCCEEDED(hr) && dss != NULL, "CreateDepthStencilState(LESS) returned 0x%08lx", hr);
    if (!vs || !ps_r || !ps_g || !layout || !vb_near || !vb_far || !dsv || !dss) goto cleanup;

    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, dsv);
    ID3D11DeviceContext_ClearRenderTargetView(context, rtv, clear);
    ID3D11DeviceContext_ClearDepthStencilView(context, dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11DeviceContext_OMSetDepthStencilState(context, dss, 0);

    memset(&vp, 0, sizeof(vp));
    vp.Width = 64.0f; vp.Height = 64.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(context, 1, &vp);

    ID3D11DeviceContext_IASetInputLayout(context, layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);

    /* Near green quad first, then a far red one: LESS must reject the red. */
    ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1, &vb_near, &stride, &offset);
    ID3D11DeviceContext_PSSetShader(context, ps_g, NULL, 0);
    ID3D11DeviceContext_Draw(context, 6, 0);

    ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1, &vb_far, &stride, &offset);
    ID3D11DeviceContext_PSSetShader(context, ps_r, NULL, 0);
    ID3D11DeviceContext_Draw(context, 6, 0);

    if (read_back(context, target, staging, 32, 32, &pixel))
        ok_(pixel == 0xff00ff00,
            "pixel is 0x%08lx, expected the near green quad to survive the depth test", pixel);
    else
        skip_("could not read the render target back");

cleanup:
    D3DTEST_RELEASE(dss);
    D3DTEST_RELEASE(dsv);
    D3DTEST_RELEASE(depth);
    D3DTEST_RELEASE(staging);
    D3DTEST_RELEASE(rtv);
    D3DTEST_RELEASE(target);
    D3DTEST_RELEASE(vb_near);
    D3DTEST_RELEASE(vb_far);
    D3DTEST_RELEASE(layout);
    D3DTEST_RELEASE(vs);
    D3DTEST_RELEASE(ps_r);
    D3DTEST_RELEASE(ps_g);
    D3DTEST_RELEASE(vsb);
    D3DTEST_RELEASE(psb_r);
    D3DTEST_RELEASE(psb_g);
    D3DTEST_RELEASE(errors);
    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
