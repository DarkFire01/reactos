/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8: vertex buffer creation, locking and drawing
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

struct vertex
{
    float x, y, z, rhw;
    DWORD colour;
};

int main(void)
{
    IDirect3DVertexBuffer8 *vb = NULL;
    IDirect3DDevice8 *device = NULL;
    IDirect3D8 *d3d = NULL;
    D3DVERTEXBUFFER_DESC desc;
    BYTE *data = NULL;
    HRESULT hr;
    HWND hwnd;

    struct vertex tri[] =
    {
        { 128.0f,  32.0f, 0.5f, 1.0f, 0xffff0000 },
        { 224.0f, 224.0f, 0.5f, 1.0f, 0xff00ff00 },
        {  32.0f, 224.0f, 0.5f, 1.0f, 0xff0000ff },
    };

    test_begin("d3d8_vertexbuffer");

    hwnd = test_create_window("d3d8_vertexbuffer", 320, 240);
    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 8 device could be created on this adapter");
        goto cleanup;
    }

    hr = IDirect3DDevice8_CreateVertexBuffer(device, sizeof(tri), D3DUSAGE_WRITEONLY,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_DEFAULT, &vb);
    ok_(SUCCEEDED(hr) && vb != NULL, "CreateVertexBuffer returned 0x%08lx", hr);
    if (!vb)
        goto cleanup;

    memset(&desc, 0, sizeof(desc));
    hr = IDirect3DVertexBuffer8_GetDesc(vb, &desc);
    ok_(SUCCEEDED(hr), "GetDesc returned 0x%08lx", hr);
    ok_(desc.Size == sizeof(tri), "buffer is %u bytes, expected %u",
        desc.Size, (unsigned)sizeof(tri));
    ok_(desc.FVF == (D3DFVF_XYZRHW | D3DFVF_DIFFUSE), "buffer FVF is 0x%08lx",
        (unsigned long)desc.FVF);

    hr = IDirect3DVertexBuffer8_Lock(vb, 0, sizeof(tri), &data, 0);
    ok_(SUCCEEDED(hr) && data != NULL, "Lock returned 0x%08lx", hr);
    if (data)
    {
        memcpy(data, tri, sizeof(tri));
        hr = IDirect3DVertexBuffer8_Unlock(vb);
        ok_(SUCCEEDED(hr), "Unlock returned 0x%08lx", hr);
    }

    IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff202020, 1.0f, 0);

    hr = IDirect3DDevice8_BeginScene(device);
    ok_(SUCCEEDED(hr), "BeginScene returned 0x%08lx", hr);
    if (SUCCEEDED(hr))
    {
        hr = IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        ok_(SUCCEEDED(hr), "SetVertexShader(FVF) returned 0x%08lx", hr);

        hr = IDirect3DDevice8_SetStreamSource(device, 0, vb, sizeof(struct vertex));
        ok_(SUCCEEDED(hr), "SetStreamSource returned 0x%08lx", hr);

        hr = IDirect3DDevice8_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 1);
        ok_(SUCCEEDED(hr), "DrawPrimitive returned 0x%08lx", hr);

        hr = IDirect3DDevice8_EndScene(device);
        ok_(SUCCEEDED(hr), "EndScene returned 0x%08lx", hr);
    }

    IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);

cleanup:
    D3DTEST_RELEASE(vb);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
