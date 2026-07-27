/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: alpha blending verified by reading the result back
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

struct vertex { float x, y, z, rhw; DWORD colour; };

int main(void)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    DWORD pixel = 0;
    HRESULT hr;
    HWND hwnd;

    /* A half-transparent white quad over the whole 256x256 target. */
    struct vertex quad[] =
    {
        {   0.0f,   0.0f, 0.5f, 1.0f, 0x80ffffff },
        { 256.0f,   0.0f, 0.5f, 1.0f, 0x80ffffff },
        { 256.0f, 256.0f, 0.5f, 1.0f, 0x80ffffff },
        {   0.0f,   0.0f, 0.5f, 1.0f, 0x80ffffff },
        { 256.0f, 256.0f, 0.5f, 1.0f, 0x80ffffff },
        {   0.0f, 256.0f, 0.5f, 1.0f, 0x80ffffff },
    };

    test_begin("d3d9_blend");

    hwnd = test_create_window("d3d9_blend", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) goto done;
    device = create_device9(d3d, hwnd, FALSE);
    if (!device) { skip_("no Direct3D 9 device"); goto cleanup; }

    /* Start from black. */
    hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff000000, 1.0f, 0);
    ok_(SUCCEEDED(hr), "Clear to black returned 0x%08lx", hr);

    IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);

    hr = IDirect3DDevice9_BeginScene(device);
    ok_(SUCCEEDED(hr), "BeginScene returned 0x%08lx", hr);
    if (SUCCEEDED(hr))
    {
        IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 2,
                quad, sizeof(struct vertex));
        ok_(SUCCEEDED(hr), "DrawPrimitiveUP returned 0x%08lx", hr);
        IDirect3DDevice9_EndScene(device);
    }

    /* 0x80/255 of white over black should land near mid grey. Allow a wide
       tolerance: the exact rounding differs between implementations. */
    if (read_rt_pixel(device, 128, 128, &pixel))
    {
        int r = (pixel >> 16) & 0xff;
        int g = (pixel >> 8) & 0xff;
        int b = pixel & 0xff;

        info_("blended pixel is 0x%08lx (r=%d g=%d b=%d)", pixel, r, g, b);
        ok_(r > 100 && r < 160, "red channel %d is around half intensity", r);
        ok_(g > 100 && g < 160, "green channel %d is around half intensity", g);
        ok_(b > 100 && b < 160, "blue channel %d is around half intensity", b);
    }
    else
    {
        skip_("could not read the render target back");
    }

    IDirect3DDevice9_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);

cleanup:
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
