/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DXGI: format support reporting across the common formats
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

#include <dxgi.h>

struct fmt_entry { DXGI_FORMAT fmt; const char *name; };

int main(void)
{
    static const struct fmt_entry formats[] =
    {
        { DXGI_FORMAT_R8G8B8A8_UNORM,      "R8G8B8A8_UNORM" },
        { DXGI_FORMAT_B8G8R8A8_UNORM,      "B8G8R8A8_UNORM" },
        { DXGI_FORMAT_R16G16B16A16_FLOAT,  "R16G16B16A16_FLOAT" },
        { DXGI_FORMAT_R32G32B32A32_FLOAT,  "R32G32B32A32_FLOAT" },
        { DXGI_FORMAT_D24_UNORM_S8_UINT,   "D24_UNORM_S8_UINT" },
        { DXGI_FORMAT_D32_FLOAT,           "D32_FLOAT" },
        { DXGI_FORMAT_BC1_UNORM,           "BC1_UNORM" },
        { DXGI_FORMAT_BC3_UNORM,           "BC3_UNORM" },
        { DXGI_FORMAT_R32_UINT,            "R32_UINT" },
    };
    ID3D11DeviceContext *context = NULL;
    ID3D11Device *device = NULL;
    D3D_FEATURE_LEVEL level;
    int usable = 0;
    UINT support;
    HRESULT hr;
    unsigned int i;

    test_begin("dxgi_format");

    hr = make_d3d11(&device, &context, &level);
    if (FAILED(hr)) { skip_("no Direct3D 11 device (0x%08lx)", hr); return test_end(); }

    for (i = 0; i < ARRAYSIZE(formats); i++)
    {
        support = 0;
        hr = ID3D11Device_CheckFormatSupport(device, formats[i].fmt, &support);
        if (SUCCEEDED(hr))
        {
            usable++;
            info_("%-20s support 0x%08lx%s%s%s", formats[i].name, (unsigned long)support,
                  (support & D3D11_FORMAT_SUPPORT_TEXTURE2D) ? " tex2d" : "",
                  (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ? " rt" : "",
                  (support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL) ? " ds" : "");
        }
        else
        {
            info_("%-20s not supported (0x%08lx)", formats[i].name, hr);
        }
    }

    ok_(usable >= 4, "%d of %u formats report support information",
        usable, (unsigned)ARRAYSIZE(formats));

    /* R8G8B8A8_UNORM is mandatory for every feature level worth the name. */
    support = 0;
    ID3D11Device_CheckFormatSupport(device, DXGI_FORMAT_R8G8B8A8_UNORM, &support);
    ok_(support & D3D11_FORMAT_SUPPORT_TEXTURE2D,
        "R8G8B8A8_UNORM works as a 2D texture");
    ok_(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET,
        "R8G8B8A8_UNORM works as a render target");

    /* A depth format must not claim to be a render target. */
    support = 0;
    if (SUCCEEDED(ID3D11Device_CheckFormatSupport(device, DXGI_FORMAT_D24_UNORM_S8_UINT, &support)))
        ok_(support & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL,
            "D24_UNORM_S8_UINT reports depth-stencil support");

    D3DTEST_RELEASE(context);
    D3DTEST_RELEASE(device);
    return test_end();
}
