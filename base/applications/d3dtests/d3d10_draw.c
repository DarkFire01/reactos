/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 10: a complete draw with pixel readback
 */


#include "d3dtest.h"
#include <d3d10_1.h>

static D3DTEST_UNUSED HRESULT make_d3d10(ID3D10Device **device)
{
    static const D3D10_DRIVER_TYPE types[] =
    {
        D3D10_DRIVER_TYPE_HARDWARE, D3D10_DRIVER_TYPE_WARP, D3D10_DRIVER_TYPE_REFERENCE,
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

#include <d3dcompiler.h>

static const char vs_source[] =
    "float4 main(float4 pos : POSITION) : SV_POSITION { return pos; }\n";
static const char ps_source[] =
    "float4 main() : SV_TARGET { return float4(1.0f, 0.0f, 0.0f, 1.0f); }\n";

int main(void)
{
    ID3D10RenderTargetView *rtv = NULL;
    ID3D10Texture2D *target = NULL, *staging = NULL;
    ID3D10Blob *vsb = NULL, *psb = NULL, *errors = NULL;
    ID3D10InputLayout *layout = NULL;
    ID3D10VertexShader *vs = NULL;
    ID3D10PixelShader *ps = NULL;
    ID3D10Device *device = NULL;
    ID3D10Buffer *vb = NULL;
    D3D10_INPUT_ELEMENT_DESC elements[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0 },
    };
    static const float tri[] =
    {
         0.0f,  0.8f, 0.0f, 1.0f,
         0.8f, -0.8f, 0.0f, 1.0f,
        -0.8f, -0.8f, 0.0f, 1.0f,
    };
    D3D10_SUBRESOURCE_DATA data;
    D3D10_TEXTURE2D_DESC td;
    D3D10_BUFFER_DESC bd;
    D3D10_VIEWPORT vp;
    UINT stride = 16, offset = 0;
    float clear[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    HRESULT hr;

    test_begin("d3d10_draw");

    hr = make_d3d10(&device);
    if (FAILED(hr)) { skip_("no Direct3D 10 device (0x%08lx)", hr); return test_end(); }

    hr = D3DCompile(vs_source, sizeof(vs_source) - 1, NULL, NULL, NULL,
                    "main", "vs_4_0", 0, 0, &vsb, &errors);
    if (FAILED(hr)) { skip_("D3DCompile(vs_4_0) returned 0x%08lx", hr); goto cleanup; }
    D3DTEST_RELEASE(errors);

    hr = D3DCompile(ps_source, sizeof(ps_source) - 1, NULL, NULL, NULL,
                    "main", "ps_4_0", 0, 0, &psb, &errors);
    if (FAILED(hr)) { skip_("D3DCompile(ps_4_0) returned 0x%08lx", hr); goto cleanup; }
    D3DTEST_RELEASE(errors);

    hr = ID3D10Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vsb),
            ID3D10Blob_GetBufferSize(vsb), &vs);
    ok_(SUCCEEDED(hr), "CreateVertexShader returned 0x%08lx", hr);

    hr = ID3D10Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(psb),
            ID3D10Blob_GetBufferSize(psb), &ps);
    ok_(SUCCEEDED(hr), "CreatePixelShader returned 0x%08lx", hr);

    hr = ID3D10Device_CreateInputLayout(device, elements, 1,
            ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &layout);
    ok_(SUCCEEDED(hr), "CreateInputLayout returned 0x%08lx", hr);

    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = sizeof(tri);
    bd.Usage = D3D10_USAGE_DEFAULT;
    bd.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    memset(&data, 0, sizeof(data));
    data.pSysMem = tri;
    hr = ID3D10Device_CreateBuffer(device, &bd, &data, &vb);
    ok_(SUCCEEDED(hr), "CreateBuffer(vertices) returned 0x%08lx", hr);

    memset(&td, 0, sizeof(td));
    td.Width = td.Height = 64;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D10_USAGE_DEFAULT;
    td.BindFlags = D3D10_BIND_RENDER_TARGET;
    hr = ID3D10Device_CreateTexture2D(device, &td, NULL, &target);
    ok_(SUCCEEDED(hr), "CreateTexture2D(render target) returned 0x%08lx", hr);
    if (!vs || !ps || !layout || !vb || !target) goto cleanup;

    hr = ID3D10Device_CreateRenderTargetView(device, (ID3D10Resource *)target, NULL, &rtv);
    ok_(SUCCEEDED(hr) && rtv != NULL, "CreateRenderTargetView returned 0x%08lx", hr);
    if (!rtv) goto cleanup;

    ID3D10Device_OMSetRenderTargets(device, 1, &rtv, NULL);
    ID3D10Device_ClearRenderTargetView(device, rtv, clear);

    memset(&vp, 0, sizeof(vp));
    vp.Width = 64;
    vp.Height = 64;
    vp.MaxDepth = 1.0f;
    ID3D10Device_RSSetViewports(device, 1, &vp);

    ID3D10Device_IASetInputLayout(device, layout);
    ID3D10Device_IASetVertexBuffers(device, 0, 1, &vb, &stride, &offset);
    ID3D10Device_IASetPrimitiveTopology(device, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D10Device_VSSetShader(device, vs);
    ID3D10Device_PSSetShader(device, ps);
    ID3D10Device_Draw(device, 3, 0);
    ok_(1, "Draw(3) completed without faulting");

    /* Read the centre pixel back through a staging texture. */
    td.Usage = D3D10_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    if (SUCCEEDED(ID3D10Device_CreateTexture2D(device, &td, NULL, &staging)))
    {
        D3D10_MAPPED_TEXTURE2D m;

        ID3D10Device_CopyResource(device, (ID3D10Resource *)staging, (ID3D10Resource *)target);
        memset(&m, 0, sizeof(m));
        hr = ID3D10Texture2D_Map(staging, 0, D3D10_MAP_READ, 0, &m);
        if (SUCCEEDED(hr))
        {
            DWORD centre = *(DWORD *)((BYTE *)m.pData + 32 * m.RowPitch + 32 * 4);
            ok_(centre == 0xff0000ff,
                "centre pixel is 0x%08lx, expected the shader red 0xff0000ff", centre);
            ID3D10Texture2D_Unmap(staging, 0);
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
    D3DTEST_RELEASE(layout);
    D3DTEST_RELEASE(vs);
    D3DTEST_RELEASE(ps);
    D3DTEST_RELEASE(vsb);
    D3DTEST_RELEASE(psb);
    D3DTEST_RELEASE(errors);
    D3DTEST_RELEASE(device);
    return test_end();
}
