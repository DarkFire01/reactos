/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8: state block capture and apply
 */


#include "d3dtest.h"
#include <d3d8.h>

static D3DTEST_UNUSED IDirect3DDevice8 *create_device(IDirect3D8 *d3d, HWND hwnd)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice8 *device = NULL;
    D3DDISPLAYMODE mode;
    HRESULT hr;

    /* Unlike d3d9, d3d8 will not take D3DFMT_UNKNOWN for a windowed back
       buffer: it has to be given the real format, so use the desktop's. */
    memset(&mode, 0, sizeof(mode));
    if (FAILED(IDirect3D8_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode)))
        return NULL;

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = mode.Format;
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 256;

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
    IDirect3DDevice8 *device = NULL;
    IDirect3D8 *d3d = NULL;
    DWORD token = 0;
    DWORD value;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d8_stateblock");

    hwnd = test_create_window("d3d8_stateblock", 320, 240);
    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 8 device could be created on this adapter");
        goto cleanup;
    }

    /* Establish a known state, record a block over a change, then verify that
       applying the block puts the recorded value back. */
    hr = IDirect3DDevice8_SetRenderState(device, D3DRS_FILLMODE, D3DFILL_SOLID);
    ok_(SUCCEEDED(hr), "SetRenderState(FILLMODE, SOLID) returned 0x%08lx", hr);

    hr = IDirect3DDevice8_BeginStateBlock(device);
    ok_(SUCCEEDED(hr), "BeginStateBlock returned 0x%08lx", hr);
    if (FAILED(hr))
        goto cleanup;

    IDirect3DDevice8_SetRenderState(device, D3DRS_FILLMODE, D3DFILL_WIREFRAME);

    hr = IDirect3DDevice8_EndStateBlock(device, &token);
    ok_(SUCCEEDED(hr) && token != 0, "EndStateBlock returned 0x%08lx (token %lu)", hr, token);
    if (!token)
        goto cleanup;

    /* Put the device back to SOLID by hand ... */
    IDirect3DDevice8_SetRenderState(device, D3DRS_FILLMODE, D3DFILL_SOLID);
    value = 0;
    IDirect3DDevice8_GetRenderState(device, D3DRS_FILLMODE, &value);
    ok_(value == D3DFILL_SOLID, "FILLMODE is %lu before Apply, expected SOLID", value);

    /* ... then let the block restore WIREFRAME. */
    hr = IDirect3DDevice8_ApplyStateBlock(device, token);
    ok_(SUCCEEDED(hr), "ApplyStateBlock returned 0x%08lx", hr);

    value = 0;
    IDirect3DDevice8_GetRenderState(device, D3DRS_FILLMODE, &value);
    ok_(value == D3DFILL_WIREFRAME, "FILLMODE is %lu after Apply, expected WIREFRAME", value);

    hr = IDirect3DDevice8_CaptureStateBlock(device, token);
    ok_(SUCCEEDED(hr), "CaptureStateBlock returned 0x%08lx", hr);

    hr = IDirect3DDevice8_DeleteStateBlock(device, token);
    ok_(SUCCEEDED(hr), "DeleteStateBlock returned 0x%08lx", hr);

cleanup:
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
