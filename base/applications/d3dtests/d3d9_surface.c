/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: plain surfaces, GetDC and StretchRect
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
    IDirect3DSurface9 *a = NULL, *b = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    D3DLOCKED_RECT lr;
    HDC dc = NULL;
    HRESULT hr;
    HWND hwnd;
    DWORD px;

    test_begin("d3d9_surface");

    hwnd = test_create_window("d3d9_surface", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) goto done;
    device = create_device9(d3d, hwnd, FALSE);
    if (!device) { skip_("no Direct3D 9 device"); goto cleanup; }

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 64, 64, D3DFMT_X8R8G8B8,
            D3DPOOL_SYSTEMMEM, &a, NULL);
    ok_(SUCCEEDED(hr) && a != NULL, "CreateOffscreenPlainSurface(SYSTEMMEM) returned 0x%08lx", hr);
    if (!a) goto cleanup;

    /* Write a known pixel through a lock. */
    memset(&lr, 0, sizeof(lr));
    hr = IDirect3DSurface9_LockRect(a, &lr, NULL, 0);
    ok_(SUCCEEDED(hr), "LockRect returned 0x%08lx", hr);
    if (SUCCEEDED(hr))
    {
        *(DWORD *)lr.pBits = 0x00abcdef;
        IDirect3DSurface9_UnlockRect(a);
    }

    /* And read it back through GDI. */
    hr = IDirect3DSurface9_GetDC(a, &dc);
    if (FAILED(hr))
    {
        skip_("GetDC on a plain surface returned 0x%08lx", hr);
    }
    else
    {
        COLORREF got = GetPixel(dc, 0, 0);
        /* GDI is BGR-ordered relative to the D3D ARGB value. */
        ok_(got == RGB(0xab, 0xcd, 0xef),
            "GDI sees 0x%06lx where the lock wrote 0x00abcdef", (unsigned long)got);
        IDirect3DSurface9_ReleaseDC(a, dc);
        dc = NULL;
    }

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 64, 64, D3DFMT_X8R8G8B8,
            D3DPOOL_SYSTEMMEM, &b, NULL);
    ok_(SUCCEEDED(hr) && b != NULL, "created a second surface");

    if (b)
    {
        /* System-memory to system-memory goes through UpdateSurface. */
        hr = IDirect3DDevice9_UpdateSurface(device, a, NULL, b, NULL);
        info_("UpdateSurface between SYSTEMMEM surfaces returned 0x%08lx", hr);

        hr = IDirect3DDevice9_StretchRect(device, a, NULL, b, NULL, D3DTEXF_NONE);
        info_("StretchRect between SYSTEMMEM surfaces returned 0x%08lx", hr);

        memset(&lr, 0, sizeof(lr));
        if (SUCCEEDED(IDirect3DSurface9_LockRect(b, &lr, NULL, D3DLOCK_READONLY)))
        {
            px = *(DWORD *)lr.pBits & 0x00ffffff;
            info_("destination pixel after the copy attempts is 0x%06lx", px);
            IDirect3DSurface9_UnlockRect(b);
        }
    }

cleanup:
    if (dc) IDirect3DSurface9_ReleaseDC(a, dc);
    D3DTEST_RELEASE(a);
    D3DTEST_RELEASE(b);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
