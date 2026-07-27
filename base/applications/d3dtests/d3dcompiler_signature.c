/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     d3dcompiler: input and output signature blobs
 */


#include "d3dtest.h"
#include <d3dcompiler.h>

static const char source[] =
    "struct vs_in { float4 pos : POSITION; float4 col : COLOR; };\n"
    "struct vs_out { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
    "vs_out main(vs_in v) { vs_out o; o.pos = v.pos; o.col = v.col; return o; }\n";

int main(void)
{
    ID3D10Blob *code = NULL, *errors = NULL;
    ID3D10Blob *input = NULL, *output = NULL, *both = NULL;
    HRESULT hr;

    test_begin("d3dcompiler_signature");

    hr = D3DCompile(source, sizeof(source) - 1, NULL, NULL, NULL,
                    "main", "vs_4_0", 0, 0, &code, &errors);
    if (FAILED(hr))
    {
        if (errors) info_("compiler said: %s", (const char *)ID3D10Blob_GetBufferPointer(errors));
        skip_("D3DCompile(vs_4_0) returned 0x%08lx", hr);
        D3DTEST_RELEASE(errors);
        return test_end();
    }
    ok_(code != NULL, "compiled the signature sample");
    D3DTEST_RELEASE(errors);

    hr = D3DGetInputSignatureBlob(ID3D10Blob_GetBufferPointer(code),
                                  ID3D10Blob_GetBufferSize(code), &input);
    if (FAILED(hr))
    {
        skip_("D3DGetInputSignatureBlob returned 0x%08lx", hr);
    }
    else
    {
        ok_(input != NULL, "extracted the input signature");
        ok_(ID3D10Blob_GetBufferSize(input) > 0, "input signature is %u bytes",
            (unsigned)ID3D10Blob_GetBufferSize(input));
    }

    hr = D3DGetOutputSignatureBlob(ID3D10Blob_GetBufferPointer(code),
                                   ID3D10Blob_GetBufferSize(code), &output);
    if (FAILED(hr))
    {
        skip_("D3DGetOutputSignatureBlob returned 0x%08lx", hr);
    }
    else
    {
        ok_(output != NULL, "extracted the output signature");
        ok_(ID3D10Blob_GetBufferSize(output) > 0, "output signature is %u bytes",
            (unsigned)ID3D10Blob_GetBufferSize(output));
    }

    hr = D3DGetInputAndOutputSignatureBlob(ID3D10Blob_GetBufferPointer(code),
                                           ID3D10Blob_GetBufferSize(code), &both);
    if (FAILED(hr))
    {
        skip_("D3DGetInputAndOutputSignatureBlob returned 0x%08lx", hr);
    }
    else
    {
        ok_(both != NULL, "extracted the combined signature");
        if (input && output)
            ok_(ID3D10Blob_GetBufferSize(both) >= ID3D10Blob_GetBufferSize(input),
                "the combined blob (%u bytes) is at least as large as the input one (%u)",
                (unsigned)ID3D10Blob_GetBufferSize(both),
                (unsigned)ID3D10Blob_GetBufferSize(input));
    }

    /* Asking for a signature from something that is not a shader must fail. */
    {
        static const DWORD junk[] = { 0xdeadbeef, 0, 0, 0 };
        ID3D10Blob *bad = NULL;

        hr = D3DGetInputSignatureBlob(junk, sizeof(junk), &bad);
        ok_(FAILED(hr), "signature extraction from junk returned 0x%08lx", hr);
        D3DTEST_RELEASE(bad);
    }

    D3DTEST_RELEASE(input);
    D3DTEST_RELEASE(output);
    D3DTEST_RELEASE(both);
    D3DTEST_RELEASE(code);
    return test_end();
}
