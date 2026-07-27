/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: compute shaders and unordered access views
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

/* Fill a UAV with a recognisable pattern derived from the thread index. */
static const char cs_source[] =
    "RWStructuredBuffer<uint> output : register(u0);\n"
    "[numthreads(16, 1, 1)]\n"
    "void main(uint3 tid : SV_DispatchThreadID)\n"
    "{\n"
    "    output[tid.x] = tid.x * 3 + 1;\n"
    "}\n";

int main(void)
{
    ID3D11UnorderedAccessView *uav = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11ComputeShader *cs = NULL;
    ID3D11Buffer *buffer = NULL, *staging = NULL;
    ID3D10Blob *csb = NULL, *errors = NULL;
    ID3D11Device *device = NULL;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavd;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_BUFFER_DESC bd;
    D3D_FEATURE_LEVEL level;
    int correct = 1;
    HRESULT hr;
    int i;

    test_begin("d3d11_compute");

    hr = make_d3d11(&device, &context, &level);
    if (FAILED(hr)) { skip_("no Direct3D 11 device (0x%08lx)", hr); return test_end(); }

    if (level < D3D_FEATURE_LEVEL_11_0)
    {
        skip_("feature level 0x%04x is below 11_0, no compute shaders", (unsigned)level);
        goto cleanup;
    }

    hr = D3DCompile(cs_source, sizeof(cs_source) - 1, NULL, NULL, NULL,
                    "main", "cs_5_0", 0, 0, &csb, &errors);
    if (FAILED(hr))
    {
        if (errors) info_("compiler said: %s", (const char *)ID3D10Blob_GetBufferPointer(errors));
        skip_("D3DCompile(cs_5_0) returned 0x%08lx", hr);
        goto cleanup;
    }
    ok_(csb != NULL, "compiled a cs_5_0 compute shader");
    D3DTEST_RELEASE(errors);

    hr = ID3D11Device_CreateComputeShader(device, ID3D10Blob_GetBufferPointer(csb),
            ID3D10Blob_GetBufferSize(csb), NULL, &cs);
    ok_(SUCCEEDED(hr) && cs != NULL, "CreateComputeShader returned 0x%08lx", hr);
    if (!cs) goto cleanup;

    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = 16 * sizeof(UINT);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(UINT);
    hr = ID3D11Device_CreateBuffer(device, &bd, NULL, &buffer);
    ok_(SUCCEEDED(hr) && buffer != NULL, "CreateBuffer(structured UAV) returned 0x%08lx", hr);
    if (!buffer) goto cleanup;

    memset(&uavd, 0, sizeof(uavd));
    uavd.Format = DXGI_FORMAT_UNKNOWN;
    uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavd.Buffer.NumElements = 16;
    hr = ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource *)buffer, &uavd, &uav);
    ok_(SUCCEEDED(hr) && uav != NULL, "CreateUnorderedAccessView returned 0x%08lx", hr);
    if (!uav) goto cleanup;

    ID3D11DeviceContext_CSSetShader(context, cs, NULL, 0);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(context, 0, 1, &uav, NULL);
    ID3D11DeviceContext_Dispatch(context, 1, 1, 1);
    ok_(1, "Dispatch(1,1,1) completed without faulting");

    /* Read the buffer back and check the shader actually ran. */
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = 16 * sizeof(UINT);
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(UINT);
    if (SUCCEEDED(ID3D11Device_CreateBuffer(device, &bd, NULL, &staging)))
    {
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                         (ID3D11Resource *)buffer);
        memset(&m, 0, sizeof(m));
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                     D3D11_MAP_READ, 0, &m);
        ok_(SUCCEEDED(hr), "Map on the staging buffer returned 0x%08lx", hr);
        if (SUCCEEDED(hr))
        {
            const UINT *values = (const UINT *)m.pData;
            for (i = 0; i < 16; i++)
            {
                if (values[i] != (UINT)(i * 3 + 1))
                {
                    correct = 0;
                    info_("element %d is %u, expected %u", i, values[i], i * 3 + 1);
                    break;
                }
            }
            ok_(correct, "all 16 elements hold the value the compute shader wrote");
            ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
        }
    }

cleanup:
    D3DTEST_RELEASE(staging);
    D3DTEST_RELEASE(uav);
    D3DTEST_RELEASE(buffer);
    D3DTEST_RELEASE(cs);
    D3DTEST_RELEASE(csb);
    D3DTEST_RELEASE(errors);
    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
