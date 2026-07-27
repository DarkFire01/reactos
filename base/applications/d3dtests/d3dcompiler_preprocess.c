/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     d3dcompiler: the HLSL preprocessor and macro handling
 */


#include "d3dtest.h"
#include <d3dcompiler.h>

static const char source[] =
    "#define GREETING 42\n"
    "#ifdef FROM_CALLER\n"
    "int marker_from_caller;\n"
    "#endif\n"
    "int value = GREETING;\n";

static const char bad_source[] =
    "#if 1\n"
    "int unterminated;\n";

int main(void)
{
    D3D_SHADER_MACRO macros[] =
    {
        { "FROM_CALLER", "1" },
        { NULL, NULL },
    };
    ID3D10Blob *text = NULL, *errors = NULL;
    const char *out;
    HRESULT hr;

    test_begin("d3dcompiler_preprocess");

    hr = D3DPreprocess(source, sizeof(source) - 1, NULL, NULL, NULL, &text, &errors);
    if (FAILED(hr))
    {
        if (errors)
            info_("preprocessor said: %s", (const char *)ID3D10Blob_GetBufferPointer(errors));
        skip_("D3DPreprocess returned 0x%08lx", hr);
        D3DTEST_RELEASE(errors);
        return test_end();
    }
    ok_(text != NULL, "D3DPreprocess produced a blob");
    D3DTEST_RELEASE(errors);

    if (text)
    {
        out = (const char *)ID3D10Blob_GetBufferPointer(text);
        ok_(strstr(out, "42") != NULL, "GREETING expanded to its value");
        ok_(strstr(out, "marker_from_caller") == NULL,
            "the FROM_CALLER branch stayed out without the macro defined");
        D3DTEST_RELEASE(text);
    }

    /* Now with the macro supplied by the caller. */
    hr = D3DPreprocess(source, sizeof(source) - 1, NULL, macros, NULL, &text, &errors);
    ok_(SUCCEEDED(hr), "D3DPreprocess with a caller macro returned 0x%08lx", hr);
    D3DTEST_RELEASE(errors);

    if (SUCCEEDED(hr) && text)
    {
        out = (const char *)ID3D10Blob_GetBufferPointer(text);
        ok_(strstr(out, "marker_from_caller") != NULL,
            "the FROM_CALLER branch was taken once the macro was defined");
        D3DTEST_RELEASE(text);
    }

    /* An unterminated #if must be reported, not accepted. */
    hr = D3DPreprocess(bad_source, sizeof(bad_source) - 1, NULL, NULL, NULL, &text, &errors);
    /* The reference preprocessor tolerates this, treating end-of-input as
       closing the block: Windows returns S_OK. */
    info_("unterminated #if returned 0x%08lx (Windows: S_OK)", hr);
    if (errors)
        info_("reported: %s", (const char *)ID3D10Blob_GetBufferPointer(errors));
    D3DTEST_RELEASE(text);
    D3DTEST_RELEASE(errors);

    return test_end();
}
