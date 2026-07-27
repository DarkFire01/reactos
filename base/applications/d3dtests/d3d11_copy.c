/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: UpdateSubresource and CopySubresourceRegion
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
    ID3D11Texture2D *src = NULL, *dst = NULL, *staging = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Device *device = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_TEXTURE2D_DESC td;
    D3D_FEATURE_LEVEL level;
    D3D11_BOX box;
    DWORD pattern[16 * 16];
    HRESULT hr;
    int i;

    test_begin("d3d11_copy");

    hr = make_d3d11(&device, &context, &level);
    if (FAILED(hr)) { skip_("no Direct3D 11 device (0x%08lx)", hr); return test_end(); }

    for (i = 0; i < 16 * 16; i++)
        pattern[i] = 0xff000000 | (DWORD)i;

    memset(&td, 0, sizeof(td));
    td.Width = td.Height = 16;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    hr = ID3D11Device_CreateTexture2D(device, &td, NULL, &src);
    ok_(SUCCEEDED(hr) && src != NULL, "created a 16x16 source texture (0x%08lx)", hr);

    td.Width = td.Height = 32;
    hr = ID3D11Device_CreateTexture2D(device, &td, NULL, &dst);
    ok_(SUCCEEDED(hr) && dst != NULL, "created a 32x32 destination texture (0x%08lx)", hr);
    if (!src || !dst) goto cleanup;

    /* Push CPU data into a DEFAULT texture. */
    ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)src, 0, NULL,
                                          pattern, 16 * 4, 0);
    ok_(1, "UpdateSubresource completed without faulting");

    /* Copy the 16x16 source into the middle of the 32x32 destination. */
    memset(&box, 0, sizeof(box));
    box.right = 16;
    box.bottom = 16;
    box.back = 1;
    ID3D11DeviceContext_CopySubresourceRegion(context, (ID3D11Resource *)dst, 0, 8, 8, 0,
                                              (ID3D11Resource *)src, 0, &box);
    ok_(1, "CopySubresourceRegion completed without faulting");

    /* Read the destination back and check the copy landed at the right offset. */
    td.Width = td.Height = 32;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(device, &td, NULL, &staging);
    if (FAILED(hr))
    {
        skip_("could not create a staging texture (0x%08lx)", hr);
        goto cleanup;
    }

    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)dst);
    memset(&m, 0, sizeof(m));
    hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m);
    ok_(SUCCEEDED(hr), "Map on the staging texture returned 0x%08lx", hr);

    if (SUCCEEDED(hr))
    {
        /* Source pixel (0,0) should now sit at destination (8,8). */
        DWORD at88 = *(DWORD *)((BYTE *)m.pData + 8 * m.RowPitch + 8 * 4);
        DWORD at99 = *(DWORD *)((BYTE *)m.pData + 9 * m.RowPitch + 9 * 4);

        ok_(at88 == pattern[0],
            "destination (8,8) is 0x%08lx, expected the source origin 0x%08lx",
            at88, pattern[0]);
        ok_(at99 == pattern[16 + 1],
            "destination (9,9) is 0x%08lx, expected source (1,1) 0x%08lx",
            at99, pattern[16 + 1]);

        ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    }

cleanup:
    D3DTEST_RELEASE(staging);
    D3DTEST_RELEASE(dst);
    D3DTEST_RELEASE(src);
    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
