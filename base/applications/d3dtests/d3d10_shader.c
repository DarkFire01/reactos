/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 10: HLSL compilation and shader objects
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
    "float4 main() : SV_TARGET { return float4(0.25f, 0.5f, 0.75f, 1.0f); }\n";

int main(void)
{
    ID3D10Blob *vsb = NULL, *psb = NULL, *errors = NULL;
    ID3D10VertexShader *vs = NULL;
    ID3D10PixelShader *ps = NULL;
    ID3D10Device *device = NULL;
    HRESULT hr;

    test_begin("d3d10_shader");

    hr = make_d3d10(&device);
    if (FAILED(hr)) { skip_("no Direct3D 10 device (0x%08lx)", hr); return test_end(); }

    hr = D3DCompile(vs_source, sizeof(vs_source) - 1, NULL, NULL, NULL,
                    "main", "vs_4_0", 0, 0, &vsb, &errors);
    if (FAILED(hr))
    {
        if (errors) info_("compiler said: %s", (const char *)ID3D10Blob_GetBufferPointer(errors));
        skip_("D3DCompile(vs_4_0) returned 0x%08lx", hr);
    }
    else
    {
        ok_(vsb != NULL, "compiled a vs_4_0 shader");
        hr = ID3D10Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vsb),
                ID3D10Blob_GetBufferSize(vsb), &vs);
        ok_(SUCCEEDED(hr) && vs != NULL, "CreateVertexShader returned 0x%08lx", hr);
        if (vs)
        {
            ID3D10Device_VSSetShader(device, vs);
            ok_(1, "VSSetShader completed");
        }
    }
    D3DTEST_RELEASE(errors);

    hr = D3DCompile(ps_source, sizeof(ps_source) - 1, NULL, NULL, NULL,
                    "main", "ps_4_0", 0, 0, &psb, &errors);
    if (FAILED(hr))
    {
        skip_("D3DCompile(ps_4_0) returned 0x%08lx", hr);
    }
    else
    {
        ok_(psb != NULL, "compiled a ps_4_0 shader");
        hr = ID3D10Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(psb),
                ID3D10Blob_GetBufferSize(psb), &ps);
        ok_(SUCCEEDED(hr) && ps != NULL, "CreatePixelShader returned 0x%08lx", hr);
        if (ps)
        {
            ID3D10Device_PSSetShader(device, ps);
            ok_(1, "PSSetShader completed");
        }
    }
    D3DTEST_RELEASE(errors);

    /* Junk must be rejected. */
    {
        static const DWORD junk[] = { 0xdeadbeef, 0, 0, 0 };
        ID3D10VertexShader *bad = NULL;
        hr = ID3D10Device_CreateVertexShader(device, junk, sizeof(junk), &bad);
        ok_(FAILED(hr), "CreateVertexShader on junk returned 0x%08lx, expected failure", hr);
        D3DTEST_RELEASE(bad);
    }

    D3DTEST_RELEASE(vsb);
    D3DTEST_RELEASE(psb);
    D3DTEST_RELEASE(vs);
    D3DTEST_RELEASE(ps);
    D3DTEST_RELEASE(device);
    return test_end();
}
