/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: deferred contexts and command lists
 */


#include "d3dtest.h"
#include <d3d11.h>

static D3DTEST_UNUSED HRESULT make_d3d11(ID3D11Device **device, ID3D11DeviceContext **context,
                                         D3D_FEATURE_LEVEL *level)
{
    static const D3D_DRIVER_TYPE types[] =
    {
        D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_REFERENCE,
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

/* Make a render target plus a staging copy so results can be inspected. */
static D3DTEST_UNUSED HRESULT make_rt(ID3D11Device *device, UINT size,
                                      ID3D11Texture2D **rt, ID3D11RenderTargetView **rtv,
                                      ID3D11Texture2D **staging)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    memset(&td, 0, sizeof(td));
    td.Width = size;
    td.Height = size;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(hr = ID3D11Device_CreateTexture2D(device, &td, NULL, rt)))
        return hr;

    if (FAILED(hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)*rt, NULL, rtv)))
        return hr;

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return ID3D11Device_CreateTexture2D(device, &td, NULL, staging);
}

static D3DTEST_UNUSED BOOL read_back(ID3D11DeviceContext *context, ID3D11Texture2D *rt,
                                     ID3D11Texture2D *staging, UINT x, UINT y, DWORD *out)
{
    D3D11_MAPPED_SUBRESOURCE m;

    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)rt);
    memset(&m, 0, sizeof(m));
    if (FAILED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                       D3D11_MAP_READ, 0, &m)))
        return FALSE;
    *out = *(DWORD *)((BYTE *)m.pData + y * m.RowPitch + x * 4);
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    return TRUE;
}

int main(void)
{
    ID3D11DeviceContext *context = NULL, *deferred = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Texture2D *target = NULL, *staging = NULL;
    ID3D11CommandList *list = NULL;
    ID3D11Device *device = NULL;
    D3D_FEATURE_LEVEL level;
    float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    DWORD pixel = 0;
    HRESULT hr;

    test_begin("d3d11_deferred");

    hr = make_d3d11(&device, &context, &level);
    if (FAILED(hr)) { skip_("no Direct3D 11 device (0x%08lx)", hr); return test_end(); }

    hr = ID3D11Device_CreateDeferredContext(device, 0, &deferred);
    if (FAILED(hr))
    {
        skip_("no deferred context support (0x%08lx)", hr);
        goto cleanup;
    }
    ok_(SUCCEEDED(hr) && deferred != NULL, "CreateDeferredContext returned 0x%08lx", hr);

    ok_(ID3D11DeviceContext_GetType(deferred) == D3D11_DEVICE_CONTEXT_DEFERRED,
        "the deferred context reports type DEFERRED");

    if (FAILED(make_rt(device, 64, &target, &rtv, &staging)))
    {
        skip_("could not build a render target");
        goto cleanup;
    }

    /* Paint red on the immediate context first. */
    ID3D11DeviceContext_ClearRenderTargetView(context, rtv, red);

    /* Record a green clear on the deferred context; nothing should happen yet. */
    ID3D11DeviceContext_ClearRenderTargetView(deferred, rtv, green);

    hr = ID3D11DeviceContext_FinishCommandList(deferred, FALSE, &list);
    ok_(SUCCEEDED(hr) && list != NULL, "FinishCommandList returned 0x%08lx", hr);

    if (read_back(context, target, staging, 32, 32, &pixel))
        ok_(pixel == 0xff0000ff,
            "before ExecuteCommandList the target is 0x%08lx, expected the red clear", pixel);

    if (list)
    {
        ID3D11DeviceContext_ExecuteCommandList(context, list, FALSE);
        ok_(1, "ExecuteCommandList completed");

        if (read_back(context, target, staging, 32, 32, &pixel))
            ok_(pixel == 0xff00ff00,
                "after ExecuteCommandList the target is 0x%08lx, expected the green clear",
                pixel);
        else
            skip_("could not read the render target back");
    }

cleanup:
    D3DTEST_RELEASE(list);
    D3DTEST_RELEASE(staging);
    D3DTEST_RELEASE(rtv);
    D3DTEST_RELEASE(target);
    D3DTEST_RELEASE(deferred);
    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
