/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: input layout validation against a shader signature
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
    "struct vs_in { float4 pos : POSITION; float4 col : COLOR; };\n"
    "float4 main(vs_in v) : SV_POSITION\n"
    "{\n"
    "    return v.pos + v.col * 0.0f;\n"
    "}\n";

int main(void)
{
    D3D11_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11DeviceContext *context = NULL;
    ID3D11InputLayout *input = NULL;
    ID3D10Blob *blob = NULL, *errors = NULL;
    ID3D11Device *device = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr;

    test_begin("d3d11_inputlayout");

    hr = create_d3d11_device(&device, &context, &level);
    if (FAILED(hr))
    {
        skip_("no Direct3D 11 device available (0x%08lx)", hr);
        return test_end();
    }

    hr = D3DCompile(vs_source, sizeof(vs_source) - 1, NULL, NULL, NULL,
                    "main", "vs_4_0", 0, 0, &blob, &errors);
    if (FAILED(hr))
    {
        if (errors)
            info_("compiler said: %s", (const char *)ID3D10Blob_GetBufferPointer(errors));
        skip_("D3DCompile(vs_4_0) returned 0x%08lx", hr);
        goto cleanup;
    }
    ok_(blob != NULL, "compiled the vertex shader");

    hr = ID3D11Device_CreateInputLayout(device, layout, ARRAYSIZE(layout),
            ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &input);
    ok_(SUCCEEDED(hr) && input != NULL, "CreateInputLayout returned 0x%08lx", hr);

    if (input && context)
    {
        ID3D11InputLayout *got = NULL;

        ID3D11DeviceContext_IASetInputLayout(context, input);
        ID3D11DeviceContext_IAGetInputLayout(context, &got);
        ok_(got == input, "context holds %p, expected %p", got, input);
        D3DTEST_RELEASE(got);

        ID3D11DeviceContext_IASetPrimitiveTopology(context,
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ok_(1, "IASetPrimitiveTopology completed");
    }

    /* A layout missing a semantic the shader consumes must be rejected. */
    {
        ID3D11InputLayout *bad = NULL;

        hr = ID3D11Device_CreateInputLayout(device, layout, 1,
                ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &bad);
        ok_(FAILED(hr),
            "layout without COLOR returned 0x%08lx, expected the shader signature to reject it",
            hr);
        D3DTEST_RELEASE(bad);
    }

cleanup:
    D3DTEST_RELEASE(errors);
    D3DTEST_RELEASE(blob);
    D3DTEST_RELEASE(input);
    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
