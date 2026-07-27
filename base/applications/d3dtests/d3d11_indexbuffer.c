/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: index buffers and DrawIndexed
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
    "float4 main() : SV_TARGET { return float4(0.0f, 1.0f, 0.0f, 1.0f); }\n";

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
    ID3D11Buffer *vb = NULL, *ib = NULL;
    D3D11_INPUT_ELEMENT_DESC elements[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    static const float corners[] =
    {
        -0.9f,  0.9f, 0.0f, 1.0f,
         0.9f,  0.9f, 0.0f, 1.0f,
         0.9f, -0.9f, 0.0f, 1.0f,
        -0.9f, -0.9f, 0.0f, 1.0f,
    };
    static const WORD indices[] = { 0, 1, 2, 0, 2, 3 };
    D3D11_SUBRESOURCE_DATA data;
    D3D11_BUFFER_DESC bd, got;
    D3D_FEATURE_LEVEL level;
    D3D11_VIEWPORT vp;
    UINT stride = 16, offset = 0;
    float clear[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    DWORD pixel = 0;
    HRESULT hr;

    test_begin("d3d11_indexbuffer");

    hr = make_d3d11(&device, &context, &level);
    if (FAILED(hr)) { skip_("no Direct3D 11 device (0x%08lx)", hr); return test_end(); }

    hr = D3DCompile(vs_source, sizeof(vs_source) - 1, NULL, NULL, NULL,
                    "main", "vs_4_0", 0, 0, &vsb, &errors);
    if (FAILED(hr)) { skip_("D3DCompile(vs_4_0) returned 0x%08lx", hr); goto cleanup; }
    D3DTEST_RELEASE(errors);
    hr = D3DCompile(ps_source, sizeof(ps_source) - 1, NULL, NULL, NULL,
                    "main", "ps_4_0", 0, 0, &psb, &errors);
    if (FAILED(hr)) { skip_("D3DCompile(ps_4_0) returned 0x%08lx", hr); goto cleanup; }
    D3DTEST_RELEASE(errors);

    ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vsb),
            ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(psb),
            ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    ID3D11Device_CreateInputLayout(device, elements, 1, ID3D10Blob_GetBufferPointer(vsb),
            ID3D10Blob_GetBufferSize(vsb), &layout);

    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(corners);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&data, 0, sizeof(data));
    data.pSysMem = corners;
    ID3D11Device_CreateBuffer(device, &bd, &data, &vb);

    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(indices);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    memset(&data, 0, sizeof(data));
    data.pSysMem = indices;
    hr = ID3D11Device_CreateBuffer(device, &bd, &data, &ib);
    ok_(SUCCEEDED(hr) && ib != NULL, "CreateBuffer(INDEX_BUFFER) returned 0x%08lx", hr);

    if (ib)
    {
        memset(&got, 0, sizeof(got));
        ID3D11Buffer_GetDesc(ib, &got);
        ok_(got.BindFlags == D3D11_BIND_INDEX_BUFFER,
            "index buffer bind flags are 0x%08lx", (unsigned long)got.BindFlags);
        ok_(got.ByteWidth == sizeof(indices), "index buffer is %u bytes", got.ByteWidth);
    }

    if (FAILED(make_rt(device, 64, &target, &rtv, &staging)))
    {
        skip_("could not build a render target");
        goto cleanup;
    }
    if (!vs || !ps || !layout || !vb || !ib) goto cleanup;

    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(context, rtv, clear);

    memset(&vp, 0, sizeof(vp));
    vp.Width = 64.0f; vp.Height = 64.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(context, 1, &vp);

    ID3D11DeviceContext_IASetInputLayout(context, layout);
    ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(context, ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(context, ps, NULL, 0);

    ID3D11DeviceContext_DrawIndexed(context, 6, 0, 0);
    ok_(1, "DrawIndexed(6) completed without faulting");

    if (read_back(context, target, staging, 32, 32, &pixel))
        ok_(pixel == 0xff00ff00,
            "centre pixel is 0x%08lx, expected the green the shader emits", pixel);
    else
        skip_("could not read the render target back");

cleanup:
    D3DTEST_RELEASE(staging);
    D3DTEST_RELEASE(rtv);
    D3DTEST_RELEASE(target);
    D3DTEST_RELEASE(ib);
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
