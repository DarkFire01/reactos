/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: queries and predicates
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

int main(void)
{
    ID3D11DeviceContext *context = NULL;
    ID3D11Device *device = NULL;
    ID3D11Query *query = NULL;
    D3D11_QUERY_DESC desc;
    D3D_FEATURE_LEVEL level;
    UINT64 result = 0;
    DWORD spins;
    HRESULT hr;

    test_begin("d3d11_query");

    hr = create_d3d11_device(&device, &context, &level);
    if (FAILED(hr))
    {
        skip_("no Direct3D 11 device available (0x%08lx)", hr);
        return test_end();
    }

    memset(&desc, 0, sizeof(desc));
    desc.Query = D3D11_QUERY_EVENT;
    hr = ID3D11Device_CreateQuery(device, &desc, &query);
    ok_(SUCCEEDED(hr) && query != NULL, "CreateQuery(EVENT) returned 0x%08lx", hr);

    if (query && context)
    {
        D3D11_QUERY_DESC got;
        BOOL done = FALSE;

        memset(&got, 0, sizeof(got));
        ID3D11Query_GetDesc(query, &got);
        ok_(got.Query == D3D11_QUERY_EVENT, "query reports type %u, expected EVENT", got.Query);

        ID3D11DeviceContext_End(context, (ID3D11Asynchronous *)query);

        for (spins = 0; spins < 10000; spins++)
        {
            hr = ID3D11DeviceContext_GetData(context, (ID3D11Asynchronous *)query,
                                             &done, sizeof(done), 0);
            if (hr != S_FALSE)
                break;
        }
        ok_(hr == S_OK, "event query completed with 0x%08lx after %lu poll(s)", hr, spins);
        D3DTEST_RELEASE(query);
    }

    memset(&desc, 0, sizeof(desc));
    desc.Query = D3D11_QUERY_OCCLUSION;
    hr = ID3D11Device_CreateQuery(device, &desc, &query);
    if (FAILED(hr))
    {
        skip_("no occlusion query support (0x%08lx)", hr);
    }
    else if (context)
    {
        ok_(SUCCEEDED(hr), "CreateQuery(OCCLUSION) returned 0x%08lx", hr);

        ID3D11DeviceContext_Begin(context, (ID3D11Asynchronous *)query);
        ID3D11DeviceContext_End(context, (ID3D11Asynchronous *)query);

        for (spins = 0; spins < 10000; spins++)
        {
            hr = ID3D11DeviceContext_GetData(context, (ID3D11Asynchronous *)query,
                                             &result, sizeof(result), 0);
            if (hr != S_FALSE)
                break;
        }
        ok_(hr == S_OK, "occlusion query completed with 0x%08lx", hr);
        if (hr == S_OK)
            ok_(result == 0, "occlusion query counted %llu pixel(s) with nothing drawn",
                (unsigned long long)result);
    }

    D3DTEST_RELEASE(query);
    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
