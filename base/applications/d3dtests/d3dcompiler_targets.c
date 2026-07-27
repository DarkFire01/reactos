/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     d3dcompiler: shader model targets and compile flags
 */


#include "d3dtest.h"
#include <d3dcompiler.h>

struct target { const char *profile; const char *source; };

int main(void)
{
    static const char vs[] = "float4 main(float4 p : POSITION) : SV_POSITION { return p; }\n";
    static const char ps[] = "float4 main() : SV_TARGET { return 1.0f; }\n";
    static const char cs[] =
        "RWStructuredBuffer<uint> o : register(u0);\n"
        "[numthreads(1,1,1)] void main(uint3 t : SV_DispatchThreadID) { o[t.x] = 1; }\n";
    static const char gs[] =
        "struct gs_in { float4 pos : SV_POSITION; };\n"
        "[maxvertexcount(3)]\n"
        "void main(triangle gs_in i[3], inout TriangleStream<gs_in> s)\n"
        "{ for (int n = 0; n < 3; n++) s.Append(i[n]); }\n";

    static const struct target targets[] =
    {
        { "vs_4_0", vs }, { "ps_4_0", ps },
        { "vs_4_1", vs }, { "ps_4_1", ps },
        { "vs_5_0", vs }, { "ps_5_0", ps },
        { "gs_4_0", gs }, { "cs_5_0", cs },
    };
    ID3D10Blob *code = NULL, *errors = NULL;
    ID3D10Blob *opt = NULL, *dbg = NULL;
    int ok_count = 0;
    unsigned int i;
    HRESULT hr;

    test_begin("d3dcompiler_targets");

    for (i = 0; i < ARRAYSIZE(targets); i++)
    {
        hr = D3DCompile(targets[i].source, strlen(targets[i].source), NULL, NULL, NULL,
                        "main", targets[i].profile, 0, 0, &code, &errors);
        if (SUCCEEDED(hr))
        {
            ok_count++;
            info_("%-8s compiled, %u bytes", targets[i].profile,
                  (unsigned)ID3D10Blob_GetBufferSize(code));
        }
        else
        {
            info_("%-8s not available (0x%08lx)", targets[i].profile, hr);
        }
        D3DTEST_RELEASE(code);
        D3DTEST_RELEASE(errors);
    }

    ok_(ok_count >= 2, "%d of %u shader model targets compiled",
        ok_count, (unsigned)ARRAYSIZE(targets));

    /* Optimisation and debug flags must both be accepted. */
    hr = D3DCompile(vs, sizeof(vs) - 1, NULL, NULL, NULL, "main", "vs_4_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &opt, &errors);
    ok_(SUCCEEDED(hr), "compiling with OPTIMIZATION_LEVEL3 returned 0x%08lx", hr);
    D3DTEST_RELEASE(errors);

    hr = D3DCompile(vs, sizeof(vs) - 1, NULL, NULL, NULL, "main", "vs_4_0",
                    D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION, 0, &dbg, &errors);
    ok_(SUCCEEDED(hr), "compiling with DEBUG|SKIP_OPTIMIZATION returned 0x%08lx", hr);
    D3DTEST_RELEASE(errors);

    if (opt && dbg)
        info_("optimised blob %u bytes, debug blob %u bytes",
              (unsigned)ID3D10Blob_GetBufferSize(opt), (unsigned)ID3D10Blob_GetBufferSize(dbg));

    /* A profile that does not exist must be rejected cleanly. */
    hr = D3DCompile(vs, sizeof(vs) - 1, NULL, NULL, NULL, "main", "xs_9_9", 0, 0, &code, &errors);
    ok_(FAILED(hr), "an invalid profile returned 0x%08lx, expected failure", hr);
    D3DTEST_RELEASE(code);
    D3DTEST_RELEASE(errors);

    D3DTEST_RELEASE(opt);
    D3DTEST_RELEASE(dbg);
    return test_end();
}
