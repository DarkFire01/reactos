/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     d3dcompiler: disassembly, stripping and blob part access
 */


#include "d3dtest.h"
#include <d3dcompiler.h>

static const char source[] =
    "float4 main(float4 pos : POSITION) : SV_POSITION\n"
    "{\n"
    "    return pos;\n"
    "}\n";

int main(void)
{
    ID3D10Blob *code = NULL, *errors = NULL, *text = NULL, *stripped = NULL, *part = NULL;
    HRESULT hr;

    test_begin("d3dcompiler_disasm");

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
    ok_(code != NULL, "compiled the sample shader");
    D3DTEST_RELEASE(errors);

    hr = D3DDisassemble(ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
                        0, NULL, &text);
    if (FAILED(hr))
    {
        skip_("D3DDisassemble returned 0x%08lx", hr);
    }
    else
    {
        ok_(text != NULL, "D3DDisassemble produced a blob");
        if (text && ID3D10Blob_GetBufferSize(text))
        {
            ok_(ID3D10Blob_GetBufferSize(text) > 0, "disassembly is %u bytes",
                (unsigned)ID3D10Blob_GetBufferSize(text));
            info_("disassembly begins: %.60s", (const char *)ID3D10Blob_GetBufferPointer(text));
        }
        D3DTEST_RELEASE(text);
    }

    /* The input signature is a named part of the container. */
    hr = D3DGetBlobPart(ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
                        D3D_BLOB_INPUT_SIGNATURE_BLOB, 0, &part);
    if (FAILED(hr))
    {
        skip_("D3DGetBlobPart(INPUT_SIGNATURE) returned 0x%08lx", hr);
    }
    else
    {
        ok_(part != NULL, "extracted the input signature blob");
        ok_(ID3D10Blob_GetBufferSize(part) > 0, "input signature is %u bytes",
            (unsigned)ID3D10Blob_GetBufferSize(part));
        D3DTEST_RELEASE(part);
    }

    /* Stripping debug data should never grow the container. */
    hr = D3DStripShader(ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
                        D3DCOMPILER_STRIP_DEBUG_INFO | D3DCOMPILER_STRIP_REFLECTION_DATA,
                        &stripped);
    if (FAILED(hr))
    {
        skip_("D3DStripShader returned 0x%08lx", hr);
    }
    else
    {
        ok_(stripped != NULL, "D3DStripShader produced a blob");
        if (stripped)
            ok_(ID3D10Blob_GetBufferSize(stripped) <= ID3D10Blob_GetBufferSize(code),
                "stripped blob is %u bytes against the original %u",
                (unsigned)ID3D10Blob_GetBufferSize(stripped),
                (unsigned)ID3D10Blob_GetBufferSize(code));
        D3DTEST_RELEASE(stripped);
    }

    D3DTEST_RELEASE(code);
    return test_end();
}
