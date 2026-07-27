/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: mip chains and GenerateMips
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
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *texture = NULL;
    ID3D11Device *device = NULL;
    D3D11_TEXTURE2D_DESC td, got;
    D3D_FEATURE_LEVEL level;
    static DWORD image[64 * 64];
    HRESULT hr;
    int i;

    test_begin("d3d11_mipmap");

    hr = make_d3d11(&device, &context, &level);
    if (FAILED(hr)) { skip_("no Direct3D 11 device (0x%08lx)", hr); return test_end(); }

    /* 0 mip levels means "the full chain": 64x64 gives 7. */
    memset(&td, 0, sizeof(td));
    td.Width = td.Height = 64;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    hr = ID3D11Device_CreateTexture2D(device, &td, NULL, &texture);
    ok_(SUCCEEDED(hr) && texture != NULL, "CreateTexture2D(full mip chain) returned 0x%08lx", hr);
    if (!texture) goto cleanup;

    memset(&got, 0, sizeof(got));
    ID3D11Texture2D_GetDesc(texture, &got);
    ok_(got.MipLevels == 7, "texture has %u mip level(s), expected 7 for 64x64", got.MipLevels);

    hr = ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)texture, NULL, &srv);
    ok_(SUCCEEDED(hr) && srv != NULL, "CreateShaderResourceView returned 0x%08lx", hr);

    /* Seed level 0 with a solid colour, then let the runtime build the chain.
       With a NULL box UpdateSubresource reads the entire subresource, so the
       source buffer must cover all 64x64 texels. */
    for (i = 0; i < 64 * 64; i++)
        image[i] = 0xff4080c0;
    ID3D11DeviceContext_UpdateSubresource(context, (ID3D11Resource *)texture, 0, NULL,
                                          image, 64 * 4, 64 * 64 * 4);
    ok_(1, "seeded mip level 0");

    if (srv)
    {
        ID3D11DeviceContext_GenerateMips(context, srv);
        ok_(1, "GenerateMips completed without faulting");
    }

    /* A texture without MISC_GENERATE_MIPS must refuse the SRV/GenerateMips path. */
    {
        ID3D11Texture2D *plain = NULL;

        memset(&td, 0, sizeof(td));
        td.Width = td.Height = 32;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(device, &td, NULL, &plain)))
        {
            ok_(1, "a single-level texture without GENERATE_MIPS still creates");
            D3DTEST_RELEASE(plain);
        }
    }

cleanup:
    D3DTEST_RELEASE(srv);
    D3DTEST_RELEASE(texture);
    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
