/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: stream source frequency and hardware instancing
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
    IDirect3DVertexBuffer9 *vb = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    D3DCAPS9 caps;
    UINT divider = 0;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d9_streamfreq");

    hwnd = test_create_window("d3d9_streamfreq", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) goto done;
    device = create_device9(d3d, hwnd, FALSE);
    if (!device) { skip_("no Direct3D 9 device"); goto cleanup; }

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice9_GetDeviceCaps(device, &caps);
    info_("vertex shader version %u.%u",
          (unsigned)((caps.VertexShaderVersion >> 8) & 0xff),
          (unsigned)(caps.VertexShaderVersion & 0xff));

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 256, 0, D3DFVF_XYZ,
            D3DPOOL_MANAGED, &vb, NULL);
    ok_(SUCCEEDED(hr) && vb != NULL, "CreateVertexBuffer returned 0x%08lx", hr);
    if (!vb) goto cleanup;

    hr = IDirect3DDevice9_SetStreamSource(device, 0, vb, 0, 12);
    ok_(SUCCEEDED(hr), "SetStreamSource returned 0x%08lx", hr);

    /* The default divider is 1: one vertex per vertex. */
    hr = IDirect3DDevice9_GetStreamSourceFreq(device, 0, &divider);
    ok_(SUCCEEDED(hr), "GetStreamSourceFreq returned 0x%08lx", hr);
    ok_(divider == 1, "default stream frequency is %u, expected 1", divider);

    /* Instancing: stream 0 supplies geometry, stream 1 per-instance data. */
    hr = IDirect3DDevice9_SetStreamSourceFreq(device, 0,
            D3DSTREAMSOURCE_INDEXEDDATA | 4);
    if (FAILED(hr))
    {
        skip_("no hardware instancing support (0x%08lx)", hr);
    }
    else
    {
        ok_(SUCCEEDED(hr), "SetStreamSourceFreq(INDEXEDDATA | 4) returned 0x%08lx", hr);

        divider = 0;
        hr = IDirect3DDevice9_GetStreamSourceFreq(device, 0, &divider);
        ok_(SUCCEEDED(hr), "GetStreamSourceFreq after the change returned 0x%08lx", hr);
        ok_(divider == (D3DSTREAMSOURCE_INDEXEDDATA | 4),
            "frequency reads back as 0x%08x", divider);

        /* Put it back so nothing else is surprised. */
        IDirect3DDevice9_SetStreamSourceFreq(device, 0, 1);
    }

cleanup:
    D3DTEST_RELEASE(vb);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
