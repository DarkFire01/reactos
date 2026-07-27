/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8: render target switching
 */


#include "d3dtest.h"
#include <d3d8.h>

static D3DTEST_UNUSED IDirect3DDevice8 *create_device8(IDirect3D8 *d3d, HWND hwnd, BOOL depth)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice8 *device = NULL;
    D3DDISPLAYMODE mode;
    HRESULT hr;

    /* d3d8 will not take D3DFMT_UNKNOWN for a windowed back buffer. */
    memset(&mode, 0, sizeof(mode));
    if (FAILED(IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode)))
        return NULL;

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 256;
    if (depth)
    {
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D16;
    }

    hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
        hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
        return NULL;
    return device;
}

int main(void)
{
    IDirect3DSurface8 *rt = NULL, *original = NULL, *ds = NULL;
    IDirect3DTexture8 *texture = NULL;
    IDirect3DDevice8 *device = NULL;
    IDirect3D8 *d3d = NULL;
    D3DSURFACE_DESC desc;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d8_rendertarget");

    hwnd = test_create_window("d3d8_rendertarget", 320, 240);
    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d) goto done;
    device = create_device8(d3d, hwnd, FALSE);
    if (!device) { skip_("no Direct3D 8 device"); goto cleanup; }

    hr = IDirect3DDevice8_GetRenderTarget(device, &original);
    ok_(SUCCEEDED(hr) && original != NULL, "GetRenderTarget returned 0x%08lx", hr);

    hr = IDirect3DDevice8_CreateTexture(device, 128, 128, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &texture);
    if (FAILED(hr))
    {
        skip_("no render target texture support (0x%08lx)", hr);
        goto cleanup;
    }
    ok_(SUCCEEDED(hr), "created a 128x128 render target texture");

    hr = IDirect3DTexture8_GetSurfaceLevel(texture, 0, &rt);
    ok_(SUCCEEDED(hr) && rt != NULL, "GetSurfaceLevel(0) returned 0x%08lx", hr);
    if (!rt) goto cleanup;

    memset(&desc, 0, sizeof(desc));
    IDirect3DSurface8_GetDesc(rt, &desc);
    ok_(desc.Usage & D3DUSAGE_RENDERTARGET,
        "surface usage 0x%08lx includes RENDERTARGET", (unsigned long)desc.Usage);

    hr = IDirect3DDevice8_SetRenderTarget(device, rt, NULL);
    ok_(SUCCEEDED(hr), "SetRenderTarget(offscreen) returned 0x%08lx", hr);

    hr = IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff00ff00, 1.0f, 0);
    ok_(SUCCEEDED(hr), "Clear on the offscreen target returned 0x%08lx", hr);

    hr = IDirect3DDevice8_GetDepthStencilSurface(device, &ds);
    ok_(hr == D3DERR_NOTFOUND || FAILED(hr),
        "GetDepthStencilSurface with none bound returned 0x%08lx", hr);
    D3DTEST_RELEASE(ds);

    hr = IDirect3DDevice8_SetRenderTarget(device, original, NULL);
    ok_(SUCCEEDED(hr), "restoring the original render target returned 0x%08lx", hr);

cleanup:
    D3DTEST_RELEASE(rt);
    D3DTEST_RELEASE(texture);
    D3DTEST_RELEASE(original);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
