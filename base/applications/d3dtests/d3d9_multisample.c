/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: multisample type checking and format support
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
    IDirect3D9 *d3d = NULL;
    DWORD quality = 0;
    int supported = 0;
    HRESULT hr;
    int i;

    static const D3DMULTISAMPLE_TYPE types[] =
    {
        D3DMULTISAMPLE_NONE,
        D3DMULTISAMPLE_2_SAMPLES,
        D3DMULTISAMPLE_4_SAMPLES,
        D3DMULTISAMPLE_8_SAMPLES,
        D3DMULTISAMPLE_16_SAMPLES,
    };

    test_begin("d3d9_multisample");

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    ok_(d3d != NULL, "Direct3DCreate9 returned an object");
    if (!d3d)
        return test_end();

    for (i = 0; i < (int)ARRAYSIZE(types); i++)
    {
        quality = 0;
        hr = IDirect3D9_CheckDeviceMultiSampleType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                D3DFMT_X8R8G8B8, TRUE, types[i], &quality);
        if (SUCCEEDED(hr))
        {
            supported++;
            info_("%u sample(s): supported, %lu quality level(s)",
                  (unsigned)types[i], quality);
        }
        else
        {
            info_("%u sample(s): not supported (0x%08lx)", (unsigned)types[i], hr);
        }
    }

    /* NONE must always be available -- it means "no multisampling". */
    hr = IDirect3D9_CheckDeviceMultiSampleType(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            D3DFMT_X8R8G8B8, TRUE, D3DMULTISAMPLE_NONE, &quality);
    ok_(SUCCEEDED(hr), "MULTISAMPLE_NONE reported as supported (0x%08lx)", hr);
    ok_(supported >= 1, "%d multisample type(s) supported in total", supported);

    /* Depth and colour formats must be checkable for matching. */
    hr = IDirect3D9_CheckDepthStencilMatch(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_D24S8);
    info_("CheckDepthStencilMatch(X8R8G8B8 + D24S8) returned 0x%08lx", hr);

    hr = IDirect3D9_CheckDeviceFormatConversion(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8);
    info_("CheckDeviceFormatConversion(X8R8G8B8 -> X8R8G8B8) returned 0x%08lx", hr);

    D3DTEST_RELEASE(d3d);
    return test_end();
}
