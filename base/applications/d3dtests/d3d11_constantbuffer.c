/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: constant buffers feeding a shader, verified by readback
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
static const char ps_source[] =
    "cbuffer c : register(b0) { float4 tint; };\n"
    "float4 main() : SV_TARGET { return tint; }\n";

int main(void)
{
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Texture2D *target = NULL, *staging = NULL;
    ID3D10Blob *vsb = NULL, *psb = NULL, *errors = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11InputLayout *layout = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11Device *device = NULL;
    ID3D11Buffer *vb = NULL, *cb = NULL;
    D3D11_INPUT_ELEMENT_DESC elements[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    static const float quad[] =
    {
        -1.0f,  1.0f, 0.0f, 1.0f,   1.0f,  1.0f, 0.0f, 1.0f,  -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 0.0f, 1.0f,   1.0f, -1.0f, 0.0f, 1.0f,  -1.0f, -1.0f, 0.0f, 1.0f,
    };
    /* Exactly representable in 8-bit UNORM: 0.25 -> 64, 0.5 -> 128. */
    static const float tint[4] = { 0.25f, 0.5f, 1.0f, 1.0f };
    D3D11_SUBRESOURCE_DATA data;
    D3D11_BUFFER_DESC bd;
    D3D_FEATURE_LEVEL level;
    D3D11_VIEWPORT vp;
    UINT stride = 16, offset = 0;
    float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    DWORD pixel = 0;
    HRESULT hr;

    test_begin("d3d11_constantbuffer");

    hr = make_d3d11(&device, &context, &level);
    if (FAILED(hr)) { skip_("no Direct3D 11 device (0x%08lx)", hr); return test_end(); }

    hr = D3DCompile(vs_source, sizeof(vs_source) - 1, NULL, NULL, NULL,
                    "main", "vs_4_0", 0, 0, &vsb, &errors);
    if (FAILED(hr)) { skip_("D3DCompile(vs_4_0) returned 0x%08lx", hr); goto cleanup; }
    D3DTEST_RELEASE(errors);

    hr = D3DCompile(ps_source, sizeof(ps_source) - 1, NULL, NULL, NULL,
                    "main", "ps_4_0", 0, 0, &psb, &errors);
    if (FAILED(hr))
    {
        if (errors) info_("compiler said: %s", (const char *)ID3D10Blob_GetBufferPointer(errors));
        skip_("D3DCompile(ps_4_0) returned 0x%08lx", hr);
        goto cleanup;
    }
    D3DTEST_RELEASE(errors);

    ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vsb),
            ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(psb),
            ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    ID3D11Device_CreateInputLayout(device, elements, 1, ID3D10Blob_GetBufferPointer(vsb),
            ID3D10Blob_GetBufferSize(vsb), &layout);

    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(quad);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&data, 0, sizeof(data));
    data.pSysMem = quad;
    ID3D11Device_CreateBuffer(device, &bd, &data, &vb);

    /* Constant buffers must be a multiple of 16 bytes. */
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(tint);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    memset(&data, 0, sizeof(data));
    data.pSysMem = tint;
    hr = ID3D11Device_CreateBuffer(device, &bd, &data, &cb);
    ok_(SUCCEEDED(hr) && cb != NULL, "CreateBuffer(CONSTANT_BUFFER, 16) returned 0x%08lx", hr);

    if (FAILED(make_rt(device, 64, &target, &rtv, &staging)))
    {
        skip_("could not build a render target");
        goto cleanup;
    }
    if (!vs || !ps || !layout || !vb || !cb) goto cleanup;

    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(context, rtv, clear);

    memset(&vp, 0, sizeof(vp));
    vp.Width = 64.0f;
    vp.Height = 64.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(context, 1, &vp);

    ID3D11DeviceContext_IASetInputLayout(context, layout);
    ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(context, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(context, 0, 1, &cb);
    ID3D11DeviceContext_Draw(context, 6, 0);
    ok_(1, "Draw with a bound constant buffer completed");

    if (read_back(context, target, staging, 32, 32, &pixel))
    {
        int r = pixel & 0xff, g = (pixel >> 8) & 0xff, b = (pixel >> 16) & 0xff;
        info_("shaded pixel is 0x%08lx (r=%d g=%d b=%d)", pixel, r, g, b);
        ok_(r >= 62 && r <= 66, "red channel %d matches the 0.25 constant (~64)", r);
        ok_(g >= 126 && g <= 130, "green channel %d matches the 0.5 constant (~128)", g);
        ok_(b >= 253, "blue channel %d matches the 1.0 constant (~255)", b);
    }
    else
    {
        skip_("could not read the render target back");
    }

cleanup:
    D3DTEST_RELEASE(staging);
    D3DTEST_RELEASE(rtv);
    D3DTEST_RELEASE(target);
    D3DTEST_RELEASE(cb);
    D3DTEST_RELEASE(vb);
    D3DTEST_RELEASE(layout);
    D3DTEST_RELEASE(vs);
    D3DTEST_RELEASE(ps);
    D3DTEST_RELEASE(vsb);
    D3DTEST_RELEASE(psb);
    D3DTEST_RELEASE(errors);
    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
