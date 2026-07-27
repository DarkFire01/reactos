/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 11: 2D textures, staging readback and views
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
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Texture2D *texture = NULL, *staging = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Device *device = NULL;
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_TEXTURE2D_DESC desc, got;
    D3D_FEATURE_LEVEL level;
    float colour[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    HRESULT hr;

    test_begin("d3d11_texture");

    hr = create_d3d11_device(&device, &context, &level);
    if (FAILED(hr))
    {
        skip_("no Direct3D 11 device available (0x%08lx)", hr);
        return test_end();
    }

    memset(&desc, 0, sizeof(desc));
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &texture);
    ok_(SUCCEEDED(hr) && texture != NULL, "CreateTexture2D(64x64 RGBA8) returned 0x%08lx", hr);
    if (!texture)
        goto cleanup;

    memset(&got, 0, sizeof(got));
    ID3D11Texture2D_GetDesc(texture, &got);
    ok_(got.Width == 64 && got.Height == 64, "texture is %ux%u, expected 64x64",
        got.Width, got.Height);

    hr = ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)texture, NULL, &srv);
    ok_(SUCCEEDED(hr) && srv != NULL, "CreateShaderResourceView returned 0x%08lx", hr);

    hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)texture, NULL, &rtv);
    ok_(SUCCEEDED(hr) && rtv != NULL, "CreateRenderTargetView returned 0x%08lx", hr);

    if (rtv && context)
        ID3D11DeviceContext_ClearRenderTargetView(context, rtv, colour);

    /* Copy into a staging texture to read the cleared pixels back. */
    memset(&desc, 0, sizeof(desc));
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
    ok_(SUCCEEDED(hr) && staging != NULL, "CreateTexture2D(STAGING) returned 0x%08lx", hr);

    if (staging && context && rtv)
    {
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                                         (ID3D11Resource *)texture);

        memset(&mapped, 0, sizeof(mapped));
        hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                                     D3D11_MAP_READ, 0, &mapped);
        ok_(SUCCEEDED(hr), "Map(READ) on the staging texture returned 0x%08lx", hr);
        if (SUCCEEDED(hr))
        {
            DWORD pixel = *(DWORD *)mapped.pData;
            /* RGBA8: green is 0xff00ff00 little-endian as ABGR. */
            ok_(pixel == 0xff00ff00,
                "cleared pixel reads back as 0x%08lx, expected 0xff00ff00", pixel);
            ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
        }
    }

cleanup:
    D3DTEST_RELEASE(staging);
    D3DTEST_RELEASE(srv);
    D3DTEST_RELEASE(rtv);
    D3DTEST_RELEASE(texture);
    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
