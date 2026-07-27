/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: event and occlusion queries
 */


#include "d3dtest.h"
#include <d3d9.h>

static D3DTEST_UNUSED IDirect3DDevice9 *create_device_ex(IDirect3D9 *d3d, HWND hwnd, BOOL want_depth)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *device = NULL;
    HRESULT hr;

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 256;
    if (want_depth)
    {
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D16;
    }

    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
        hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
        return NULL;
    return device;
}

static D3DTEST_UNUSED IDirect3DDevice9 *create_device(IDirect3D9 *d3d, HWND hwnd)
{
    return create_device_ex(d3d, hwnd, FALSE);
}

int main(void)
{
    IDirect3DQuery9 *query = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    DWORD data = 0;
    DWORD spins;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d9_query");

    hwnd = test_create_window("d3d9_query", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 9 device could be created on this adapter");
        goto cleanup;
    }

    hr = IDirect3DDevice9_CreateQuery(device, D3DQUERYTYPE_EVENT, &query);
    if (FAILED(hr))
    {
        skip_("no event query support (0x%08lx)", hr);
    }
    else
    {
        ok_(SUCCEEDED(hr) && query != NULL, "CreateQuery(EVENT) returned 0x%08lx", hr);
        ok_(IDirect3DQuery9_GetType(query) == D3DQUERYTYPE_EVENT, "query reports type EVENT");
        ok_(IDirect3DQuery9_GetDataSize(query) == sizeof(BOOL),
            "event query data size is %u", IDirect3DQuery9_GetDataSize(query));

        hr = IDirect3DQuery9_Issue(query, D3DISSUE_END);
        ok_(SUCCEEDED(hr), "Issue(END) returned 0x%08lx", hr);

        /* Flush until the GPU catches up, but do not spin forever. */
        for (spins = 0; spins < 10000; spins++)
        {
            hr = IDirect3DQuery9_GetData(query, &data, sizeof(data), D3DGETDATA_FLUSH);
            if (hr != S_FALSE)
                break;
        }
        ok_(hr == S_OK, "event query completed with 0x%08lx after %lu poll(s)", hr, spins);

        D3DTEST_RELEASE(query);
    }

    hr = IDirect3DDevice9_CreateQuery(device, D3DQUERYTYPE_OCCLUSION, &query);
    if (FAILED(hr))
    {
        skip_("no occlusion query support (0x%08lx)", hr);
    }
    else
    {
        DWORD pixels = 0;

        ok_(SUCCEEDED(hr), "CreateQuery(OCCLUSION) returned 0x%08lx", hr);

        IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);

        hr = IDirect3DQuery9_Issue(query, D3DISSUE_BEGIN);
        ok_(SUCCEEDED(hr), "Issue(BEGIN) returned 0x%08lx", hr);

        if (SUCCEEDED(IDirect3DDevice9_BeginScene(device)))
            IDirect3DDevice9_EndScene(device);

        hr = IDirect3DQuery9_Issue(query, D3DISSUE_END);
        ok_(SUCCEEDED(hr), "Issue(END) returned 0x%08lx", hr);

        for (spins = 0; spins < 10000; spins++)
        {
            hr = IDirect3DQuery9_GetData(query, &pixels, sizeof(pixels), D3DGETDATA_FLUSH);
            if (hr != S_FALSE)
                break;
        }
        ok_(hr == S_OK, "occlusion query completed with 0x%08lx", hr);
        if (hr == S_OK)
            info_("occlusion query counted %lu pixel(s)", pixels);
    }

cleanup:
    D3DTEST_RELEASE(query);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
