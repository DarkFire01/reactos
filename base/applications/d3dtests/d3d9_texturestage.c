/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: texture stage state and stage independence
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
    DWORD value;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d9_texturestage");

    hwnd = test_create_window("d3d9_texturestage", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) goto done;
    device = create_device9(d3d, hwnd, FALSE);
    if (!device) { skip_("no Direct3D 9 device"); goto cleanup; }

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice9_GetDeviceCaps(device, &caps);
    info_("%lu blend stage(s), %lu simultaneous texture(s)",
          (unsigned long)caps.MaxTextureBlendStages,
          (unsigned long)caps.MaxSimultaneousTextures);

    hr = IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    ok_(SUCCEEDED(hr), "SetTextureStageState(0, COLOROP, MODULATE) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice9_GetTextureStageState(device, 0, D3DTSS_COLOROP, &value);
    ok_(value == D3DTOP_MODULATE, "COLOROP reads back as %lu", value);

    hr = IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    ok_(SUCCEEDED(hr), "SetTextureStageState(ALPHAOP) returned 0x%08lx", hr);

    hr = IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_TEXCOORDINDEX, 1);
    ok_(SUCCEEDED(hr), "SetTextureStageState(TEXCOORDINDEX, 1) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice9_GetTextureStageState(device, 0, D3DTSS_TEXCOORDINDEX, &value);
    ok_(value == 1, "TEXCOORDINDEX reads back as %lu", value);

    /* Stage 1 must not disturb stage 0. */
    hr = IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    ok_(SUCCEEDED(hr), "SetTextureStageState(1, COLOROP, DISABLE) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice9_GetTextureStageState(device, 0, D3DTSS_COLOROP, &value);
    ok_(value == D3DTOP_MODULATE, "stage 0 unchanged after touching stage 1");

    /* Sampler state is separate from stage state in d3d9. */
    hr = IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    ok_(SUCCEEDED(hr), "SetSamplerState(MAGFILTER, POINT) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice9_GetSamplerState(device, 0, D3DSAMP_MAGFILTER, &value);
    ok_(value == D3DTEXF_POINT, "MAGFILTER reads back as %lu", value);

    hr = IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAXANISOTROPY, 4);
    info_("SetSamplerState(MAXANISOTROPY, 4) returned 0x%08lx", hr);

cleanup:
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
