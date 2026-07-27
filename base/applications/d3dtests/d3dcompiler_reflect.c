/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     d3dcompiler: shader reflection over a compiled blob
 */


#include "d3dtest.h"
#include <d3dcompiler.h>

static const char source[] =
    "cbuffer constants : register(b0)\n"
    "{\n"
    "    float4 tint;\n"
    "    float  scale;\n"
    "};\n"
    "struct vs_in { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
    "struct vs_out { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "vs_out main(vs_in v)\n"
    "{\n"
    "    vs_out o;\n"
    "    o.pos = v.pos * scale + tint;\n"
    "    o.uv = v.uv;\n"
    "    return o;\n"
    "}\n";

int main(void)
{
    ID3D11ShaderReflection *reflection = NULL;
    ID3D10Blob *code = NULL, *errors = NULL;
    D3D11_SHADER_DESC desc;
    HRESULT hr;

    test_begin("d3dcompiler_reflect");

    hr = D3DCompile(source, sizeof(source) - 1, NULL, NULL, NULL,
                    "main", "vs_4_0", 0, 0, &code, &errors);
    if (FAILED(hr))
    {
        if (errors)
            info_("compiler said: %s", (const char *)ID3D10Blob_GetBufferPointer(errors));
        skip_("D3DCompile(vs_4_0) returned 0x%08lx", hr);
        D3DTEST_RELEASE(errors);
        return test_end();
    }
    ok_(code != NULL, "compiled the reflection sample");
    D3DTEST_RELEASE(errors);

    hr = D3DReflect(ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
                    &IID_ID3D11ShaderReflection, (void **)&reflection);
    if (FAILED(hr))
    {
        skip_("D3DReflect returned 0x%08lx", hr);
        D3DTEST_RELEASE(code);
        return test_end();
    }
    ok_(reflection != NULL, "D3DReflect produced a reflection object");

    memset(&desc, 0, sizeof(desc));
    /* d3d11shader.h declares these with DECLARE_INTERFACE_, which emits no
       Iface_Method() macros, so call through the vtable directly. */
    hr = reflection->lpVtbl->GetDesc(reflection, &desc);
    ok_(SUCCEEDED(hr), "GetDesc returned 0x%08lx", hr);

    if (SUCCEEDED(hr))
    {
        info_("%u input(s), %u output(s), %u constant buffer(s), %u bound resource(s)",
              desc.InputParameters, desc.OutputParameters,
              desc.ConstantBuffers, desc.BoundResources);

        ok_(desc.InputParameters == 2,
            "shader reports %u input parameter(s), expected POSITION and TEXCOORD",
            desc.InputParameters);
        ok_(desc.OutputParameters == 2,
            "shader reports %u output parameter(s), expected SV_POSITION and TEXCOORD",
            desc.OutputParameters);
        ok_(desc.ConstantBuffers == 1,
            "shader reports %u constant buffer(s), expected 1", desc.ConstantBuffers);
    }

    /* Walk the constant buffer and confirm the two members are described. */
    if (desc.ConstantBuffers >= 1)
    {
        ID3D11ShaderReflectionConstantBuffer *cb;
        D3D11_SHADER_BUFFER_DESC cbd;

        cb = reflection->lpVtbl->GetConstantBufferByIndex(reflection, 0);
        ok_(cb != NULL, "GetConstantBufferByIndex(0) returned an object");

        if (cb)
        {
            memset(&cbd, 0, sizeof(cbd));
            hr = cb->lpVtbl->GetDesc(cb, &cbd);
            ok_(SUCCEEDED(hr), "constant buffer GetDesc returned 0x%08lx", hr);
            if (SUCCEEDED(hr))
            {
                info_("cbuffer '%s': %u variable(s), %u bytes",
                      cbd.Name ? cbd.Name : "?", cbd.Variables, cbd.Size);
                ok_(cbd.Variables == 2, "cbuffer holds %u variable(s), expected 2",
                    cbd.Variables);
            }
        }
    }

    D3DTEST_RELEASE(reflection);
    D3DTEST_RELEASE(code);
    return test_end();
}
