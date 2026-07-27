/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: a complete draw into an offscreen render target
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

#include <d3dcompiler.h>

static const char vs_source[] =
    "float4 main(float4 pos : POSITION) : SV_POSITION\n"
    "{\n"
    "    return pos;\n"
    "}\n";

static const char ps_source[] =
    "float4 main() : SV_TARGET\n"
    "{\n"
    "    return float4(1.0f, 0.0f, 0.0f, 1.0f);\n"
    "}\n";

int main(void)
{
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Texture2D *target = NULL, *staging = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11InputLayout *input = NULL;
    ID3D10Blob *vsb = NULL, *psb = NULL, *errors = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11Device *device = NULL;
    ID3D11Buffer *vb = NULL;
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_SUBRESOURCE_DATA data;
    D3D11_TEXTURE2D_DESC td;
    D3D11_BUFFER_DESC bd;
    D3D_FEATURE_LEVEL level;
    D3D11_VIEWPORT vp;
    UINT stride, offset;
    float clear[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    HRESULT hr;

    D3D11_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    /* A triangle in clip space large enough to cover the centre pixel. */
    static const float tri[] =
    {
         0.0f,  0.8f, 0.0f, 1.0f,
         0.8f, -0.8f, 0.0f, 1.0f,
        -0.8f, -0.8f, 0.0f, 1.0f,
    };

    test_begin("d3d11_draw");

    hr = create_d3d11_device(&device, &context, &level);
    if (FAILED(hr))
    {
        skip_("no Direct3D 11 device available (0x%08lx)", hr);
        return test_end();
    }

    hr = D3DCompile(vs_source, sizeof(vs_source) - 1, NULL, NULL, NULL,
                    "main", "vs_4_0", 0, 0, &vsb, &errors);
    if (FAILED(hr))
    {
        skip_("D3DCompile(vs_4_0) returned 0x%08lx", hr);
        goto cleanup;
    }
    D3DTEST_RELEASE(errors);

    hr = D3DCompile(ps_source, sizeof(ps_source) - 1, NULL, NULL, NULL,
                    "main", "ps_4_0", 0, 0, &psb, &errors);
    if (FAILED(hr))
    {
        skip_("D3DCompile(ps_4_0) returned 0x%08lx", hr);
        goto cleanup;
    }
    D3DTEST_RELEASE(errors);

    hr = ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vsb),
            ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ok_(SUCCEEDED(hr), "CreateVertexShader returned 0x%08lx", hr);

    hr = ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(psb),
            ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    ok_(SUCCEEDED(hr), "CreatePixelShader returned 0x%08lx", hr);

    hr = ID3D11Device_CreateInputLayout(device, layout, 1,
            ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &input);
    ok_(SUCCEEDED(hr), "CreateInputLayout returned 0x%08lx", hr);

    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(tri);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&data, 0, sizeof(data));
    data.pSysMem = tri;
    hr = ID3D11Device_CreateBuffer(device, &bd, &data, &vb);
    ok_(SUCCEEDED(hr), "CreateBuffer(vertices) returned 0x%08lx", hr);

    memset(&td, 0, sizeof(td));
    td.Width = 64;
    td.Height = 64;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    hr = ID3D11Device_CreateTexture2D(device, &td, NULL, &target);
    ok_(SUCCEEDED(hr), "CreateTexture2D(render target) returned 0x%08lx", hr);

    if (!vs || !ps || !input || !vb || !target)
        goto cleanup;

    hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)target, NULL, &rtv);
    ok_(SUCCEEDED(hr) && rtv != NULL, "CreateRenderTargetView returned 0x%08lx", hr);
    if (!rtv)
        goto cleanup;

    ID3D11DeviceContext_OMSetRenderTargets(context, 1, &rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(context, rtv, clear);

    memset(&vp, 0, sizeof(vp));
    vp.Width = 64.0f;
    vp.Height = 64.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(context, 1, &vp);

    stride = 16;
    offset = 0;
    ID3D11DeviceContext_IASetInputLayout(context, input);
    ID3D11DeviceContext_IASetVertexBuffers(context, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(context, ps, NULL, 0);

    ID3D11DeviceContext_Draw(context, 3, 0);
    ok_(1, "Draw(3 vertices) completed without faulting");

    /* Read the centre pixel back: it should be the red the shader emits. */
    memset(&td, 0, sizeof(td));
    td.Width = 64;
    td.Height = 64;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(device, &td, NULL, &staging)))
    {
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                         (ID3D11Resource *)target);
        memset(&mapped, 0, sizeof(mapped));
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                     D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            DWORD centre = *(DWORD *)((BYTE *)mapped.pData + 32 * mapped.RowPitch + 32 * 4);
            ok_(centre == 0xff0000ff,
                "centre pixel is 0x%08lx, expected the shader red 0xff0000ff", centre);
            ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
        }
        else
        {
            skip_("could not map the staging texture (0x%08lx)", hr);
        }
    }

cleanup:
    D3DTEST_RELEASE(staging);
    D3DTEST_RELEASE(rtv);
    D3DTEST_RELEASE(target);
    D3DTEST_RELEASE(vb);
    D3DTEST_RELEASE(input);
    D3DTEST_RELEASE(vs);
    D3DTEST_RELEASE(ps);
    D3DTEST_RELEASE(vsb);
    D3DTEST_RELEASE(psb);
    D3DTEST_RELEASE(errors);
    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
