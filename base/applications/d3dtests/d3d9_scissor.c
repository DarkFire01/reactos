/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: scissor rectangle clipping, verified by readback
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
    D3DCAPS9 caps;
    DWORD inside = 0, outside = 0;
    RECT scissor, got;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d9_scissor");

    hwnd = test_create_window("d3d9_scissor", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) goto done;
    device = create_device9(d3d, hwnd, FALSE);
    if (!device) { skip_("no Direct3D 9 device"); goto cleanup; }

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice9_GetDeviceCaps(device, &caps);
    if (!(caps.RasterCaps & D3DPRASTERCAPS_SCISSORTEST))
    {
        skip_("device advertises no scissor test support");
        goto cleanup;
    }

    /* Clear everything to red first, with scissoring off. */
    IDirect3DDevice9_SetRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
    hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xffff0000, 1.0f, 0);
    ok_(SUCCEEDED(hr), "initial Clear returned 0x%08lx", hr);

    scissor.left = 64; scissor.top = 64; scissor.right = 192; scissor.bottom = 192;
    hr = IDirect3DDevice9_SetScissorRect(device, &scissor);
    ok_(SUCCEEDED(hr), "SetScissorRect returned 0x%08lx", hr);

    memset(&got, 0, sizeof(got));
    hr = IDirect3DDevice9_GetScissorRect(device, &got);
    ok_(SUCCEEDED(hr), "GetScissorRect returned 0x%08lx", hr);
    ok_(got.left == 64 && got.top == 64 && got.right == 192 && got.bottom == 192,
        "scissor reads back as %ld,%ld..%ld,%ld", got.left, got.top, got.right, got.bottom);

    /* With scissoring on, a full-target clear must only touch the rectangle. */
    IDirect3DDevice9_SetRenderState(device, D3DRS_SCISSORTESTENABLE, TRUE);
    hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff00ff00, 1.0f, 0);
    ok_(SUCCEEDED(hr), "scissored Clear returned 0x%08lx", hr);

    if (read_rt_pixel(device, 128, 128, &inside) && read_rt_pixel(device, 16, 16, &outside))
    {
        ok_((inside & 0x00ffffff) == 0x0000ff00,
            "pixel inside the scissor is 0x%08lx, expected the green clear", inside);
        ok_((outside & 0x00ffffff) == 0x00ff0000,
            "pixel outside the scissor is 0x%08lx, expected the original red", outside);
    }
    else
    {
        skip_("could not read the render target back");
    }

    IDirect3DDevice9_SetRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);

cleanup:
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
