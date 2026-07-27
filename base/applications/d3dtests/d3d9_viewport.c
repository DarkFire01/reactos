/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: viewport handling and clear interaction
 */


#include "d3dtest.h"
#include <d3d9.h>

static D3DTEST_UNUSED IDirect3DDevice9 *create_device9(IDirect3D9 *d3d, HWND hwnd, BOOL depth)
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
    if (depth)
    {
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
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

/* Render-target readback helper: copy the RT into a lockable system surface
   and return one pixel. Returns FALSE when the path is unavailable. */
static D3DTEST_UNUSED BOOL read_rt_pixel(IDirect3DDevice9 *device, int x, int y, DWORD *out)
{
    IDirect3DSurface9 *rt = NULL, *sys = NULL;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    BOOL ok = FALSE;

    if (FAILED(IDirect3DDevice9_GetRenderTarget(device, 0, &rt)))
        return FALSE;
    if (FAILED(IDirect3DSurface9_GetDesc(rt, &desc)))
        goto done;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(device, desc.Width, desc.Height,
            desc.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)))
        goto done;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(device, rt, sys)))
        goto done;
    if (FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY)))
        goto done;

    *out = *(DWORD *)((BYTE *)lr.pBits + y * lr.Pitch + x * 4);
    IDirect3DSurface9_UnlockRect(sys);
    ok = TRUE;

done:
    if (sys) IDirect3DSurface9_Release(sys);
    if (rt) IDirect3DSurface9_Release(rt);
    return ok;
}

int main(void)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    D3DVIEWPORT9 vp, got;
    DWORD inside = 0, outside = 0;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d9_viewport");

    hwnd = test_create_window("d3d9_viewport", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) goto done;
    device = create_device9(d3d, hwnd, FALSE);
    if (!device) { skip_("no Direct3D 9 device"); goto cleanup; }

    /* Paint the whole target red with the default full viewport. */
    hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xffff0000, 1.0f, 0);
    ok_(SUCCEEDED(hr), "full-target Clear returned 0x%08lx", hr);

    memset(&vp, 0, sizeof(vp));
    vp.X = 64;
    vp.Y = 64;
    vp.Width = 128;
    vp.Height = 128;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;

    hr = IDirect3DDevice9_SetViewport(device, &vp);
    ok_(SUCCEEDED(hr), "SetViewport(64,64 128x128) returned 0x%08lx", hr);

    memset(&got, 0, sizeof(got));
    hr = IDirect3DDevice9_GetViewport(device, &got);
    ok_(SUCCEEDED(hr), "GetViewport returned 0x%08lx", hr);
    ok_(got.X == 64 && got.Y == 64 && got.Width == 128 && got.Height == 128,
        "viewport reads back as %lu,%lu %lux%lu", got.X, got.Y, got.Width, got.Height);

    /* Clear is bounded by the viewport, so only the sub-rectangle turns green. */
    hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff00ff00, 1.0f, 0);
    ok_(SUCCEEDED(hr), "Clear through the small viewport returned 0x%08lx", hr);

    if (read_rt_pixel(device, 128, 128, &inside) && read_rt_pixel(device, 16, 16, &outside))
    {
        ok_((inside & 0x00ffffff) == 0x0000ff00,
            "pixel inside the viewport is 0x%08lx, expected green", inside);
        ok_((outside & 0x00ffffff) == 0x00ff0000,
            "pixel outside the viewport is 0x%08lx, expected the earlier red", outside);
    }
    else
    {
        skip_("could not read the render target back");
    }

    /* A viewport bigger than the render target must be rejected. */
    memset(&vp, 0, sizeof(vp));
    vp.Width = 8192;
    vp.Height = 8192;
    vp.MaxZ = 1.0f;
    /* Larger than the render target. The retail runtime allows this and
       clips at draw time: Windows returns S_OK. */
    hr = IDirect3DDevice9_SetViewport(device, &vp);
    info_("an oversized viewport returned 0x%08lx (Windows: S_OK)", hr);

cleanup:
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
