/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8: automatic depth-stencil buffers
 */


#include "d3dtest.h"
#include <d3d8.h>

static D3DTEST_UNUSED IDirect3DDevice8 *create_device8(IDirect3D8 *d3d, HWND hwnd, BOOL depth)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice8 *device = NULL;
    D3DDISPLAYMODE mode;
    HRESULT hr;

    /* d3d8 will not take D3DFMT_UNKNOWN for a windowed back buffer. */
    memset(&mode, 0, sizeof(mode));
    if (FAILED(IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode)))
        return NULL;

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 256;
    if (depth)
    {
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D16;
    }

    hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
        hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_REF, hwnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
        return NULL;
    return device;
}

int main(void)
{
    IDirect3DSurface8 *ds = NULL;
    IDirect3DDevice8 *device = NULL;
    IDirect3D8 *d3d = NULL;
    D3DSURFACE_DESC desc;
    DWORD value;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d8_depthstencil");

    hwnd = test_create_window("d3d8_depthstencil", 320, 240);
    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d) goto done;
    device = create_device8(d3d, hwnd, TRUE);
    if (!device) { skip_("no Direct3D 8 device with a depth buffer"); goto cleanup; }
    ok_(device != NULL, "created a device with an automatic depth-stencil");

    hr = IDirect3DDevice8_GetDepthStencilSurface(device, &ds);
    ok_(SUCCEEDED(hr) && ds != NULL, "GetDepthStencilSurface returned 0x%08lx", hr);

    if (ds)
    {
        memset(&desc, 0, sizeof(desc));
        hr = IDirect3DSurface8_GetDesc(ds, &desc);
        ok_(SUCCEEDED(hr), "GetDesc on the depth surface returned 0x%08lx", hr);
        ok_(desc.Width == 256 && desc.Height == 256,
            "depth surface is %ux%u, expected 256x256", desc.Width, desc.Height);
        info_("depth format %u, usage 0x%08lx", desc.Format, (unsigned long)desc.Usage);
    }

    /* Now a depth clear must succeed. */
    hr = IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                0xff000000, 1.0f, 0);
    ok_(SUCCEEDED(hr), "Clear(TARGET|ZBUFFER) returned 0x%08lx", hr);

    hr = IDirect3DDevice8_SetRenderState(device, D3DRS_ZENABLE, D3DZB_TRUE);
    ok_(SUCCEEDED(hr), "SetRenderState(ZENABLE, TRUE) returned 0x%08lx", hr);
    value = 0;
    IDirect3DDevice8_GetRenderState(device, D3DRS_ZENABLE, &value);
    ok_(value == D3DZB_TRUE, "ZENABLE reads back as %lu", value);

    hr = IDirect3DDevice8_SetRenderState(device, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    ok_(SUCCEEDED(hr), "SetRenderState(ZFUNC, LESSEQUAL) returned 0x%08lx", hr);

cleanup:
    D3DTEST_RELEASE(ds);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
