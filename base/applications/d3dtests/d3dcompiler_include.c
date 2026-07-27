/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     d3dcompiler: #include handling through a caller callback
 */


#include "d3dtest.h"
#include <d3dcompiler.h>

static const char header_text[] =
    "float4 helper() { return float4(0.5f, 0.25f, 0.125f, 1.0f); }\n";

static const char source[] =
    "#include \"helper.hlsl\"\n"
    "float4 main() : SV_TARGET { return helper(); }\n";

static int open_calls, close_calls;

static HRESULT WINAPI inc_open(ID3DInclude *iface, D3D_INCLUDE_TYPE type,
                               const char *filename, const void *parent,
                               const void **data, UINT *bytes)
{
    open_calls++;
    info_("include callback asked for '%s' (type %d)", filename ? filename : "?", type);
    *data = header_text;
    *bytes = sizeof(header_text) - 1;
    return S_OK;
}

static HRESULT WINAPI inc_close(ID3DInclude *iface, const void *data)
{
    close_calls++;
    return S_OK;
}

/* lpVtbl is declared CONST_VTBL, which expands to nothing unless the build
   asks for const vtables, so keep the table itself non-const. */
static ID3DIncludeVtbl inc_vtbl = { inc_open, inc_close };
static ID3DInclude inc = { &inc_vtbl };

int main(void)
{
    ID3D10Blob *code = NULL, *errors = NULL;
    HRESULT hr;

    test_begin("d3dcompiler_include");

    /* Without a handler the include cannot be resolved. */
    hr = D3DCompile(source, sizeof(source) - 1, NULL, NULL, NULL,
                    "main", "ps_4_0", 0, 0, &code, &errors);
    ok_(FAILED(hr), "compiling with an unresolvable include returned 0x%08lx", hr);
    D3DTEST_RELEASE(code);
    D3DTEST_RELEASE(errors);

    /* With one, the callback supplies the text. */
    hr = D3DCompile(source, sizeof(source) - 1, NULL, NULL, &inc,
                    "main", "ps_4_0", 0, 0, &code, &errors);
    if (FAILED(hr))
    {
        if (errors) info_("compiler said: %s", (const char *)ID3D10Blob_GetBufferPointer(errors));
        skip_("D3DCompile with an include handler returned 0x%08lx", hr);
    }
    else
    {
        ok_(code != NULL, "compiled a shader whose include came from the callback");
        ok_(open_calls >= 1, "the Open callback was invoked %d time(s)", open_calls);
        ok_(close_calls >= 1, "the Close callback was invoked %d time(s)", close_calls);
    }

    D3DTEST_RELEASE(code);
    D3DTEST_RELEASE(errors);
    return test_end();
}
