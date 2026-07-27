/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: offscreen render targets and StretchRect
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
    IDirect3DSurface9 *rt = NULL, *original = NULL, *offscreen = NULL;
    IDirect3DTexture9 *texture = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    D3DLOCKED_RECT locked;
    D3DSURFACE_DESC desc;
    DWORD pixel;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d9_rendertarget");

    hwnd = test_create_window("d3d9_rendertarget", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 9 device could be created on this adapter");
        goto cleanup;
    }

    hr = IDirect3DDevice9_GetRenderTarget(device, 0, &original);
    ok_(SUCCEEDED(hr) && original != NULL, "GetRenderTarget(0) returned 0x%08lx", hr);

    hr = IDirect3DDevice9_CreateTexture(device, 128, 128, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, NULL);
    if (FAILED(hr))
    {
        skip_("no render target texture support (0x%08lx)", hr);
        goto cleanup;
    }
    ok_(SUCCEEDED(hr), "created a 128x128 render target texture");

    hr = IDirect3DTexture9_GetSurfaceLevel(texture, 0, &rt);
    ok_(SUCCEEDED(hr) && rt != NULL, "GetSurfaceLevel(0) returned 0x%08lx", hr);
    if (!rt)
        goto cleanup;

    memset(&desc, 0, sizeof(desc));
    IDirect3DSurface9_GetDesc(rt, &desc);
    ok_(desc.Usage & D3DUSAGE_RENDERTARGET,
        "surface usage 0x%08lx includes RENDERTARGET", (unsigned long)desc.Usage);

    hr = IDirect3DDevice9_SetRenderTarget(device, 0, rt);
    ok_(SUCCEEDED(hr), "SetRenderTarget(0, offscreen) returned 0x%08lx", hr);

    hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff00ff00, 1.0f, 0);
    ok_(SUCCEEDED(hr), "Clear on the offscreen target returned 0x%08lx", hr);

    /* Read the result back through a lockable system-memory surface. */
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 128, 128, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &offscreen, NULL);
    ok_(SUCCEEDED(hr) && offscreen != NULL, "CreateOffscreenPlainSurface returned 0x%08lx", hr);

    if (offscreen)
    {
        hr = IDirect3DDevice9_GetRenderTargetData(device, rt, offscreen);
        if (SUCCEEDED(hr))
        {
            memset(&locked, 0, sizeof(locked));
            hr = IDirect3DSurface9_LockRect(offscreen, &locked, NULL, D3DLOCK_READONLY);
            ok_(SUCCEEDED(hr), "LockRect on the readback surface returned 0x%08lx", hr);
            if (SUCCEEDED(hr))
            {
                pixel = *(DWORD *)locked.pBits;
                ok_(pixel == 0xff00ff00,
                    "render target pixel is 0x%08lx, expected the cleared 0xff00ff00", pixel);
                IDirect3DSurface9_UnlockRect(offscreen);
            }
        }
        else
        {
            skip_("GetRenderTargetData returned 0x%08lx", hr);
        }
    }

    hr = IDirect3DDevice9_SetRenderTarget(device, 0, original);
    ok_(SUCCEEDED(hr), "restoring the original render target returned 0x%08lx", hr);

cleanup:
    D3DTEST_RELEASE(offscreen);
    D3DTEST_RELEASE(rt);
    D3DTEST_RELEASE(texture);
    D3DTEST_RELEASE(original);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
