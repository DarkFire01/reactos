/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     d3dcompiler: HLSL compilation, targets and error reporting
 */


#include "d3dtest.h"
#include <d3dcompiler.h>

static const char good_source[] =
    "float4 main(float4 pos : POSITION) : SV_POSITION\n"
    "{\n"
    "    return pos * 2.0f;\n"
    "}\n";

static const char broken_source[] =
    "float4 main() : SV_TARGET\n"
    "{\n"
    "    return this_symbol_does_not_exist;\n"
    "}\n";

static const char ps_source[] =
    "float4 main() : SV_TARGET\n"
    "{\n"
    "    return float4(0.5f, 0.5f, 0.5f, 1.0f);\n"
    "}\n";

int main(void)
{
    ID3D10Blob *code = NULL, *errors = NULL;
    HRESULT hr;

    test_begin("d3dcompiler_compile");

    hr = D3DCompile(good_source, sizeof(good_source) - 1, NULL, NULL, NULL,
                    "main", "vs_4_0", 0, 0, &code, &errors);
    if (FAILED(hr))
    {
        if (errors)
            info_("compiler said: %s", (const char *)ID3D10Blob_GetBufferPointer(errors));
        skip_("D3DCompile(vs_4_0) returned 0x%08lx", hr);
        D3DTEST_RELEASE(errors);
    }
    else
    {
        ok_(code != NULL, "vs_4_0 compilation produced a blob");
        ok_(ID3D10Blob_GetBufferSize(code) > 0, "blob is %u bytes",
            (unsigned)ID3D10Blob_GetBufferSize(code));
        /* Shader bytecode containers start with the DXBC fourcc. */
        if (code && ID3D10Blob_GetBufferSize(code) >= 4)
        {
            const char *magic = (const char *)ID3D10Blob_GetBufferPointer(code);
            ok_(magic[0] == 'D' && magic[1] == 'X' && magic[2] == 'B' && magic[3] == 'C',
                "blob starts with the DXBC signature");
        }
        D3DTEST_RELEASE(code);
        D3DTEST_RELEASE(errors);
    }

    hr = D3DCompile(ps_source, sizeof(ps_source) - 1, NULL, NULL, NULL,
                    "main", "ps_4_0", 0, 0, &code, &errors);
    if (SUCCEEDED(hr))
        ok_(code != NULL, "ps_4_0 compilation produced a blob");
    else
        skip_("D3DCompile(ps_4_0) returned 0x%08lx", hr);
    D3DTEST_RELEASE(code);
    D3DTEST_RELEASE(errors);

    /* A deliberate error must fail and say why, not silently succeed. */
    hr = D3DCompile(broken_source, sizeof(broken_source) - 1, NULL, NULL, NULL,
                    "main", "ps_4_0", 0, 0, &code, &errors);
    ok_(FAILED(hr), "compiling a broken shader returned 0x%08lx, expected failure", hr);
    ok_(errors != NULL, "compiler produced an error blob");
    if (errors)
    {
        info_("reported: %s", (const char *)ID3D10Blob_GetBufferPointer(errors));
        ok_(ID3D10Blob_GetBufferSize(errors) > 0, "error blob is not empty");
    }
    D3DTEST_RELEASE(code);
    D3DTEST_RELEASE(errors);

    /* An entry point that does not exist is also an error. */
    hr = D3DCompile(good_source, sizeof(good_source) - 1, NULL, NULL, NULL,
                    "no_such_entry", "vs_4_0", 0, 0, &code, &errors);
    ok_(FAILED(hr), "compiling with a missing entry point returned 0x%08lx", hr);
    D3DTEST_RELEASE(code);
    D3DTEST_RELEASE(errors);

    return test_end();
}
