/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 10: queries and predicates
 */


#include "d3dtest.h"
#include <d3d10_1.h>

static D3DTEST_UNUSED HRESULT make_d3d10(ID3D10Device **device)
{
    static const D3D10_DRIVER_TYPE types[] =
    {
        D3D10_DRIVER_TYPE_HARDWARE, D3D10_DRIVER_TYPE_WARP, D3D10_DRIVER_TYPE_REFERENCE,
    };
    HRESULT hr = E_FAIL;
    unsigned int i;

    for (i = 0; i < ARRAYSIZE(types); i++)
    {
        hr = D3D10CreateDevice(NULL, types[i], NULL, 0, D3D10_SDK_VERSION, device);
        if (SUCCEEDED(hr))
            return hr;
    }
    return hr;
}

int main(void)
{
    ID3D10Query *query = NULL;
    ID3D10Device *device = NULL;
    D3D10_QUERY_DESC desc, got;
    UINT64 result = 0;
    DWORD spins;
    BOOL done = FALSE;
    HRESULT hr;

    test_begin("d3d10_query");

    hr = make_d3d10(&device);
    if (FAILED(hr)) { skip_("no Direct3D 10 device (0x%08lx)", hr); return test_end(); }

    memset(&desc, 0, sizeof(desc));
    desc.Query = D3D10_QUERY_EVENT;
    hr = ID3D10Device_CreateQuery(device, &desc, &query);
    ok_(SUCCEEDED(hr) && query != NULL, "CreateQuery(EVENT) returned 0x%08lx", hr);

    if (query)
    {
        memset(&got, 0, sizeof(got));
        ID3D10Query_GetDesc(query, &got);
        ok_(got.Query == D3D10_QUERY_EVENT, "query reports type %u, expected EVENT", got.Query);

        ID3D10Query_End(query);
        for (spins = 0; spins < 10000; spins++)
        {
            hr = ID3D10Query_GetData(query, &done, sizeof(done), 0);
            if (hr != S_FALSE) break;
        }
        ok_(hr == S_OK, "event query completed with 0x%08lx after %lu poll(s)", hr, spins);
        D3DTEST_RELEASE(query);
    }

    memset(&desc, 0, sizeof(desc));
    desc.Query = D3D10_QUERY_OCCLUSION;
    hr = ID3D10Device_CreateQuery(device, &desc, &query);
    if (FAILED(hr))
    {
        skip_("no occlusion query support (0x%08lx)", hr);
    }
    else
    {
        ID3D10Query_Begin(query);
        ID3D10Query_End(query);
        for (spins = 0; spins < 10000; spins++)
        {
            hr = ID3D10Query_GetData(query, &result, sizeof(result), 0);
            if (hr != S_FALSE) break;
        }
        ok_(hr == S_OK, "occlusion query completed with 0x%08lx", hr);
        if (hr == S_OK)
            ok_(result == 0, "occlusion query counted %llu pixel(s) with nothing drawn",
                (unsigned long long)result);
    }

    D3DTEST_RELEASE(query);
    D3DTEST_RELEASE(device);
    return test_end();
}
