/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: depth-stencil surfaces and stencil state
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
    IDirect3DSurface9 *ds = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    D3DSURFACE_DESC desc;
    DWORD value;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d9_depthstencil");

    hwnd = test_create_window("d3d9_depthstencil", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) goto done;
    device = create_device9(d3d, hwnd, TRUE);
    if (!device) { skip_("no Direct3D 9 device with a depth-stencil"); goto cleanup; }
    ok_(device != NULL, "created a device with a D24S8 depth-stencil");

    hr = IDirect3DDevice9_GetDepthStencilSurface(device, &ds);
    ok_(SUCCEEDED(hr) && ds != NULL, "GetDepthStencilSurface returned 0x%08lx", hr);

    if (ds)
    {
        memset(&desc, 0, sizeof(desc));
        IDirect3DSurface9_GetDesc(ds, &desc);
        ok_(desc.Width == 256 && desc.Height == 256,
            "depth surface is %ux%u", desc.Width, desc.Height);
        ok_(desc.Usage & D3DUSAGE_DEPTHSTENCIL,
            "usage 0x%08lx includes DEPTHSTENCIL", (unsigned long)desc.Usage);
    }

    hr = IDirect3DDevice9_Clear(device, 0, NULL,
            D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xff000000, 1.0f, 0x7f);
    ok_(SUCCEEDED(hr), "Clear(TARGET|ZBUFFER|STENCIL) returned 0x%08lx", hr);

    hr = IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILENABLE, TRUE);
    ok_(SUCCEEDED(hr), "SetRenderState(STENCILENABLE) returned 0x%08lx", hr);

    hr = IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILFUNC, D3DCMP_EQUAL);
    ok_(SUCCEEDED(hr), "SetRenderState(STENCILFUNC, EQUAL) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice9_GetRenderState(device, D3DRS_STENCILFUNC, &value);
    ok_(value == D3DCMP_EQUAL, "STENCILFUNC reads back as %lu", value);

    hr = IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILREF, 0x7f);
    ok_(SUCCEEDED(hr), "SetRenderState(STENCILREF) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice9_GetRenderState(device, D3DRS_STENCILREF, &value);
    ok_(value == 0x7f, "STENCILREF reads back as 0x%lx", value);

    hr = IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILPASS, D3DSTENCILOP_INCR);
    ok_(SUCCEEDED(hr), "SetRenderState(STENCILPASS, INCR) returned 0x%08lx", hr);

    IDirect3DDevice9_SetRenderState(device, D3DRS_STENCILENABLE, FALSE);

cleanup:
    D3DTEST_RELEASE(ds);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
