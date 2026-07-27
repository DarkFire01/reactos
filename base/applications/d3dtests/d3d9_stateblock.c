/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: state block objects
 */


#include "d3dtest.h"
#include <d3d9.h>

static D3DTEST_UNUSED IDirect3DDevice9 *create_device_ex(IDirect3D9 *d3d, HWND hwnd, BOOL want_depth)
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
    if (want_depth)
    {
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D16;
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

static D3DTEST_UNUSED IDirect3DDevice9 *create_device(IDirect3D9 *d3d, HWND hwnd)
{
    return create_device_ex(d3d, hwnd, FALSE);
}

int main(void)
{
    IDirect3DStateBlock9 *block = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    DWORD value;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d9_stateblock");

    hwnd = test_create_window("d3d9_stateblock", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 9 device could be created on this adapter");
        goto cleanup;
    }

    IDirect3DDevice9_SetRenderState(device, D3DRS_FILLMODE, D3DFILL_SOLID);

    hr = IDirect3DDevice9_BeginStateBlock(device);
    ok_(SUCCEEDED(hr), "BeginStateBlock returned 0x%08lx", hr);
    if (FAILED(hr))
        goto cleanup;

    IDirect3DDevice9_SetRenderState(device, D3DRS_FILLMODE, D3DFILL_WIREFRAME);

    hr = IDirect3DDevice9_EndStateBlock(device, &block);
    ok_(SUCCEEDED(hr) && block != NULL, "EndStateBlock returned 0x%08lx", hr);
    if (!block)
        goto cleanup;

    IDirect3DDevice9_SetRenderState(device, D3DRS_FILLMODE, D3DFILL_SOLID);
    value = 0;
    IDirect3DDevice9_GetRenderState(device, D3DRS_FILLMODE, &value);
    ok_(value == D3DFILL_SOLID, "FILLMODE is %lu before Apply, expected SOLID", value);

    hr = IDirect3DStateBlock9_Apply(block);
    ok_(SUCCEEDED(hr), "Apply returned 0x%08lx", hr);

    value = 0;
    IDirect3DDevice9_GetRenderState(device, D3DRS_FILLMODE, &value);
    ok_(value == D3DFILL_WIREFRAME, "FILLMODE is %lu after Apply, expected WIREFRAME", value);

    /* Capture folds the device's current values back into the block. */
    IDirect3DDevice9_SetRenderState(device, D3DRS_FILLMODE, D3DFILL_POINT);
    hr = IDirect3DStateBlock9_Capture(block);
    ok_(SUCCEEDED(hr), "Capture returned 0x%08lx", hr);

    IDirect3DDevice9_SetRenderState(device, D3DRS_FILLMODE, D3DFILL_SOLID);
    IDirect3DStateBlock9_Apply(block);
    value = 0;
    IDirect3DDevice9_GetRenderState(device, D3DRS_FILLMODE, &value);
    ok_(value == D3DFILL_POINT, "FILLMODE is %lu after re-Capture, expected POINT", value);

    D3DTEST_RELEASE(block);

    /* A whole-device block should also be creatable in one call. */
    hr = IDirect3DDevice9_CreateStateBlock(device, D3DSBT_ALL, &block);
    ok_(SUCCEEDED(hr) && block != NULL, "CreateStateBlock(ALL) returned 0x%08lx", hr);

cleanup:
    D3DTEST_RELEASE(block);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
