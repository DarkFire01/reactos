/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8: index buffers and DrawIndexedPrimitive
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

struct vertex { float x, y, z, rhw; DWORD colour; };

int main(void)
{
    IDirect3DVertexBuffer8 *vb = NULL;
    IDirect3DIndexBuffer8 *ib = NULL;
    IDirect3DDevice8 *device = NULL;
    IDirect3D8 *d3d = NULL;
    D3DINDEXBUFFER_DESC desc;
    BYTE *data = NULL;
    HRESULT hr;
    HWND hwnd;

    struct vertex quad[] =
    {
        {  32.0f,  32.0f, 0.5f, 1.0f, 0xffff0000 },
        { 224.0f,  32.0f, 0.5f, 1.0f, 0xff00ff00 },
        { 224.0f, 224.0f, 0.5f, 1.0f, 0xff0000ff },
        {  32.0f, 224.0f, 0.5f, 1.0f, 0xffffffff },
    };
    WORD indices[] = { 0, 1, 2, 0, 2, 3 };

    test_begin("d3d8_indexbuffer");

    hwnd = test_create_window("d3d8_indexbuffer", 320, 240);
    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d) goto done;
    device = create_device8(d3d, hwnd, FALSE);
    if (!device) { skip_("no Direct3D 8 device"); goto cleanup; }

    hr = IDirect3DDevice8_CreateVertexBuffer(device, sizeof(quad), D3DUSAGE_WRITEONLY,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_DEFAULT, &vb);
    ok_(SUCCEEDED(hr), "CreateVertexBuffer returned 0x%08lx", hr);

    hr = IDirect3DDevice8_CreateIndexBuffer(device, sizeof(indices), D3DUSAGE_WRITEONLY,
            D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib);
    ok_(SUCCEEDED(hr) && ib != NULL, "CreateIndexBuffer(INDEX16) returned 0x%08lx", hr);
    if (!vb || !ib) goto cleanup;

    memset(&desc, 0, sizeof(desc));
    hr = IDirect3DIndexBuffer8_GetDesc(ib, &desc);
    ok_(SUCCEEDED(hr), "GetDesc returned 0x%08lx", hr);
    ok_(desc.Format == D3DFMT_INDEX16, "index format is %u, expected INDEX16", desc.Format);

    if (SUCCEEDED(IDirect3DVertexBuffer8_Lock(vb, 0, sizeof(quad), &data, 0)))
    {
        memcpy(data, quad, sizeof(quad));
        IDirect3DVertexBuffer8_Unlock(vb);
    }
    data = NULL;
    hr = IDirect3DIndexBuffer8_Lock(ib, 0, sizeof(indices), &data, 0);
    ok_(SUCCEEDED(hr) && data != NULL, "index Lock returned 0x%08lx", hr);
    if (data)
    {
        memcpy(data, indices, sizeof(indices));
        IDirect3DIndexBuffer8_Unlock(ib);
    }

    IDirect3DDevice8_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff202020, 1.0f, 0);
    hr = IDirect3DDevice8_BeginScene(device);
    ok_(SUCCEEDED(hr), "BeginScene returned 0x%08lx", hr);
    if (SUCCEEDED(hr))
    {
        IDirect3DDevice8_SetVertexShader(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        IDirect3DDevice8_SetStreamSource(device, 0, vb, sizeof(struct vertex));

        hr = IDirect3DDevice8_SetIndices(device, ib, 0);
        ok_(SUCCEEDED(hr), "SetIndices returned 0x%08lx", hr);

        hr = IDirect3DDevice8_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 0, 4, 0, 2);
        ok_(SUCCEEDED(hr), "DrawIndexedPrimitive returned 0x%08lx", hr);

        IDirect3DDevice8_EndScene(device);
    }
    IDirect3DDevice8_Present(device, NULL, NULL, NULL, NULL);

cleanup:
    D3DTEST_RELEASE(ib);
    D3DTEST_RELEASE(vb);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
