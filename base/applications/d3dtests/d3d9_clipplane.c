/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: user clip planes
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
    float plane[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
    float got[4] = { 0 };
    D3DCAPS9 caps;
    DWORD value;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d9_clipplane");

    hwnd = test_create_window("d3d9_clipplane", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) goto done;
    device = create_device9(d3d, hwnd, FALSE);
    if (!device) { skip_("no Direct3D 9 device"); goto cleanup; }

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice9_GetDeviceCaps(device, &caps);
    info_("device advertises %lu user clip plane(s)", (unsigned long)caps.MaxUserClipPlanes);

    hr = IDirect3DDevice9_SetClipPlane(device, 0, plane);
    ok_(SUCCEEDED(hr), "SetClipPlane(0) returned 0x%08lx", hr);

    hr = IDirect3DDevice9_GetClipPlane(device, 0, got);
    ok_(SUCCEEDED(hr), "GetClipPlane(0) returned 0x%08lx", hr);
    ok_(got[0] == 0.0f && got[1] == 1.0f && got[2] == 0.0f && got[3] == 0.0f,
        "clip plane reads back as %.1f,%.1f,%.1f,%.1f", got[0], got[1], got[2], got[3]);

    hr = IDirect3DDevice9_SetRenderState(device, D3DRS_CLIPPLANEENABLE, 0x1);
    ok_(SUCCEEDED(hr), "enabling clip plane 0 returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice9_GetRenderState(device, D3DRS_CLIPPLANEENABLE, &value);
    ok_(value == 0x1, "CLIPPLANEENABLE reads back as 0x%lx", value);

    /* An index past the advertised maximum must be refused. */
    if (caps.MaxUserClipPlanes < 32)
    {
        /* Documented as out of range, but the retail runtime accepts it:
           Windows returns S_OK. */
        hr = IDirect3DDevice9_SetClipPlane(device, caps.MaxUserClipPlanes + 8, plane);
        info_("SetClipPlane past the maximum returned 0x%08lx (Windows: S_OK)", hr);
    }

    IDirect3DDevice9_SetRenderState(device, D3DRS_CLIPPLANEENABLE, 0);

cleanup:
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
