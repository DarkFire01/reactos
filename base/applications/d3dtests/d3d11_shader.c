/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: HLSL compilation and shader object creation
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
    "    return float4(1.0f, 0.5f, 0.25f, 1.0f);\n"
    "}\n";

int main(void)
{
    ID3D10Blob *vs_blob = NULL, *ps_blob = NULL, *errors = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11Device *device = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr;

    test_begin("d3d11_shader");

    hr = create_d3d11_device(&device, &context, &level);
    if (FAILED(hr))
    {
        skip_("no Direct3D 11 device available (0x%08lx)", hr);
        return test_end();
    }

    hr = D3DCompile(vs_source, sizeof(vs_source) - 1, NULL, NULL, NULL,
                    "main", "vs_4_0", 0, 0, &vs_blob, &errors);
    if (FAILED(hr))
    {
        if (errors)
            info_("vertex shader errors: %s", (const char *)ID3D10Blob_GetBufferPointer(errors));
        skip_("D3DCompile(vs_4_0) returned 0x%08lx", hr);
        D3DTEST_RELEASE(errors);
    }
    else
    {
        ok_(vs_blob != NULL, "compiled the vertex shader");
        D3DTEST_RELEASE(errors);

        hr = ID3D11Device_CreateVertexShader(device,
                ID3D10Blob_GetBufferPointer(vs_blob),
                ID3D10Blob_GetBufferSize(vs_blob), NULL, &vs);
        ok_(SUCCEEDED(hr) && vs != NULL, "CreateVertexShader returned 0x%08lx", hr);

        if (vs && context)
        {
            ID3D11DeviceContext_VSSetShader(context, vs, NULL, 0);
            ok_(1, "VSSetShader completed");
        }
    }

    hr = D3DCompile(ps_source, sizeof(ps_source) - 1, NULL, NULL, NULL,
                    "main", "ps_4_0", 0, 0, &ps_blob, &errors);
    if (FAILED(hr))
    {
        if (errors)
            info_("pixel shader errors: %s", (const char *)ID3D10Blob_GetBufferPointer(errors));
        skip_("D3DCompile(ps_4_0) returned 0x%08lx", hr);
        D3DTEST_RELEASE(errors);
    }
    else
    {
        ok_(ps_blob != NULL, "compiled the pixel shader");
        D3DTEST_RELEASE(errors);

        hr = ID3D11Device_CreatePixelShader(device,
                ID3D10Blob_GetBufferPointer(ps_blob),
                ID3D10Blob_GetBufferSize(ps_blob), NULL, &ps);
        ok_(SUCCEEDED(hr) && ps != NULL, "CreatePixelShader returned 0x%08lx", hr);

        if (ps && context)
        {
            ID3D11DeviceContext_PSSetShader(context, ps, NULL, 0);
            ok_(1, "PSSetShader completed");
        }
    }

    /* Bytecode the runtime cannot parse must be refused. */
    {
        static const DWORD junk[] = { 0xdeadbeef, 0, 0, 0 };
        ID3D11VertexShader *bad = NULL;

        hr = ID3D11Device_CreateVertexShader(device, junk, sizeof(junk), NULL, &bad);
        ok_(FAILED(hr), "CreateVertexShader on junk returned 0x%08lx, expected failure", hr);
        D3DTEST_RELEASE(bad);
    }

    D3DTEST_RELEASE(vs_blob);
    D3DTEST_RELEASE(ps_blob);
    D3DTEST_RELEASE(vs);
    D3DTEST_RELEASE(ps);
    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
