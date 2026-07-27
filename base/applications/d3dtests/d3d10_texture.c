/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 10: 2D texture creation and views
 */


#include "d3dtest.h"
#include <d3d10_1.h>

static HRESULT D3DTEST_UNUSED create_d3d10_device(ID3D10Device **device)
{
    static const D3D10_DRIVER_TYPE types[] =
    {
        D3D10_DRIVER_TYPE_HARDWARE,
        D3D10_DRIVER_TYPE_WARP,
        D3D10_DRIVER_TYPE_REFERENCE,
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
    ID3D10ShaderResourceView *srv = NULL;
    ID3D10RenderTargetView *rtv = NULL;
    ID3D10Texture2D *texture = NULL;
    ID3D10Device *device = NULL;
    D3D10_TEXTURE2D_DESC desc, got;
    HRESULT hr;

    test_begin("d3d10_texture");

    hr = create_d3d10_device(&device);
    if (FAILED(hr))
    {
        skip_("no Direct3D 10 device available (0x%08lx)", hr);
        return test_end();
    }

    memset(&desc, 0, sizeof(desc));
    desc.Width = 64;
    desc.Height = 64;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D10_USAGE_DEFAULT;
    desc.BindFlags = D3D10_BIND_SHADER_RESOURCE | D3D10_BIND_RENDER_TARGET;

    hr = ID3D10Device_CreateTexture2D(device, &desc, NULL, &texture);
    ok_(SUCCEEDED(hr) && texture != NULL, "CreateTexture2D(64x64 RGBA8) returned 0x%08lx", hr);
    if (!texture)
        goto cleanup;

    memset(&got, 0, sizeof(got));
    ID3D10Texture2D_GetDesc(texture, &got);
    ok_(got.Width == 64 && got.Height == 64, "texture is %ux%u, expected 64x64",
        got.Width, got.Height);
    ok_(got.MipLevels == 1, "texture has %u mip level(s), expected 1", got.MipLevels);
    ok_(got.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "texture format is %u", got.Format);

    hr = ID3D10Device_CreateShaderResourceView(device, (ID3D10Resource *)texture, NULL, &srv);
    ok_(SUCCEEDED(hr) && srv != NULL, "CreateShaderResourceView returned 0x%08lx", hr);

    hr = ID3D10Device_CreateRenderTargetView(device, (ID3D10Resource *)texture, NULL, &rtv);
    ok_(SUCCEEDED(hr) && rtv != NULL, "CreateRenderTargetView returned 0x%08lx", hr);

    if (rtv)
    {
        float colour[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
        ID3D10Device_ClearRenderTargetView(device, rtv, colour);
        ok_(1, "ClearRenderTargetView completed without faulting");
    }

    /* A render target view over a texture not bound as one must fail. */
    {
        ID3D10Texture2D *plain = NULL;
        ID3D10RenderTargetView *bad = NULL;

        desc.BindFlags = D3D10_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(ID3D10Device_CreateTexture2D(device, &desc, NULL, &plain)))
        {
            hr = ID3D10Device_CreateRenderTargetView(device, (ID3D10Resource *)plain, NULL, &bad);
            ok_(FAILED(hr),
                "RTV over a non-render-target texture returned 0x%08lx, expected failure", hr);
            D3DTEST_RELEASE(bad);
            D3DTEST_RELEASE(plain);
        }
    }

cleanup:
    D3DTEST_RELEASE(srv);
    D3DTEST_RELEASE(rtv);
    D3DTEST_RELEASE(texture);
    D3DTEST_RELEASE(device);
    return test_end();
}
