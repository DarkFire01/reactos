/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: vertex buffers, declarations and drawing
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

struct vertex
{
    float x, y, z, rhw;
    DWORD colour;
};

int main(void)
{
    IDirect3DVertexBuffer9 *vb = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    D3DVERTEXBUFFER_DESC desc;
    void *data = NULL;
    HRESULT hr;
    HWND hwnd;

    struct vertex tri[] =
    {
        { 128.0f,  32.0f, 0.5f, 1.0f, 0xffff0000 },
        { 224.0f, 224.0f, 0.5f, 1.0f, 0xff00ff00 },
        {  32.0f, 224.0f, 0.5f, 1.0f, 0xff0000ff },
    };

    test_begin("d3d9_vertexbuffer");

    hwnd = test_create_window("d3d9_vertexbuffer", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 9 device could be created on this adapter");
        goto cleanup;
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(tri), D3DUSAGE_WRITEONLY,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_DEFAULT, &vb, NULL);
    ok_(SUCCEEDED(hr) && vb != NULL, "CreateVertexBuffer returned 0x%08lx", hr);
    if (!vb)
        goto cleanup;

    memset(&desc, 0, sizeof(desc));
    hr = IDirect3DVertexBuffer9_GetDesc(vb, &desc);
    ok_(SUCCEEDED(hr), "GetDesc returned 0x%08lx", hr);
    ok_(desc.Size == sizeof(tri), "buffer is %u bytes, expected %u",
        desc.Size, (unsigned)sizeof(tri));
    ok_(desc.Pool == D3DPOOL_DEFAULT, "buffer pool is %u, expected DEFAULT", desc.Pool);

    hr = IDirect3DVertexBuffer9_Lock(vb, 0, sizeof(tri), &data, 0);
    ok_(SUCCEEDED(hr) && data != NULL, "Lock returned 0x%08lx", hr);
    if (data)
    {
        memcpy(data, tri, sizeof(tri));
        hr = IDirect3DVertexBuffer9_Unlock(vb);
        ok_(SUCCEEDED(hr), "Unlock returned 0x%08lx", hr);
    }

    IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff202020, 1.0f, 0);

    hr = IDirect3DDevice9_BeginScene(device);
    ok_(SUCCEEDED(hr), "BeginScene returned 0x%08lx", hr);
    if (SUCCEEDED(hr))
    {
        hr = IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        ok_(SUCCEEDED(hr), "SetFVF returned 0x%08lx", hr);

        hr = IDirect3DDevice9_SetStreamSource(device, 0, vb, 0, sizeof(struct vertex));
        ok_(SUCCEEDED(hr), "SetStreamSource returned 0x%08lx", hr);

        hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 1);
        ok_(SUCCEEDED(hr), "DrawPrimitive returned 0x%08lx", hr);

        /* The UP variant needs no buffer at all. */
        hr = IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1,
                tri, sizeof(struct vertex));
        ok_(SUCCEEDED(hr), "DrawPrimitiveUP returned 0x%08lx", hr);

        hr = IDirect3DDevice9_EndScene(device);
        ok_(SUCCEEDED(hr), "EndScene returned 0x%08lx", hr);
    }

    IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);

cleanup:
    D3DTEST_RELEASE(vb);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
