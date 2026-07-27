/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: vertex declarations
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
    IDirect3DVertexDeclaration9 *decl = NULL, *got = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    D3DVERTEXELEMENT9 elements[16];
    UINT count = 0;
    HRESULT hr;
    HWND hwnd;

    static const D3DVERTEXELEMENT9 layout[] =
    {
        { 0,  0, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
        { 0, 16, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };

    test_begin("d3d9_vertexdecl");

    hwnd = test_create_window("d3d9_vertexdecl", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 9 device could be created on this adapter");
        goto cleanup;
    }

    hr = IDirect3DDevice9_CreateVertexDeclaration(device, layout, &decl);
    ok_(SUCCEEDED(hr) && decl != NULL, "CreateVertexDeclaration returned 0x%08lx", hr);
    if (!decl)
        goto cleanup;

    memset(elements, 0, sizeof(elements));
    hr = IDirect3DVertexDeclaration9_GetDeclaration(decl, elements, &count);
    ok_(SUCCEEDED(hr), "GetDeclaration returned 0x%08lx", hr);
    /* The count includes the terminating D3DDECL_END element. */
    ok_(count == 4, "declaration has %u element(s), expected 3 plus the terminator", count);

    if (count >= 3)
    {
        ok_(elements[0].Usage == D3DDECLUSAGE_POSITION && elements[0].Offset == 0,
            "element 0 is usage %u at offset %u", elements[0].Usage, elements[0].Offset);
        ok_(elements[1].Usage == D3DDECLUSAGE_COLOR && elements[1].Offset == 12,
            "element 1 is usage %u at offset %u", elements[1].Usage, elements[1].Offset);
        ok_(elements[2].Usage == D3DDECLUSAGE_TEXCOORD && elements[2].Type == D3DDECLTYPE_FLOAT2,
            "element 2 is usage %u type %u", elements[2].Usage, elements[2].Type);
    }

    hr = IDirect3DDevice9_SetVertexDeclaration(device, decl);
    ok_(SUCCEEDED(hr), "SetVertexDeclaration returned 0x%08lx", hr);

    hr = IDirect3DDevice9_GetVertexDeclaration(device, &got);
    ok_(SUCCEEDED(hr), "GetVertexDeclaration returned 0x%08lx", hr);
    ok_(got == decl, "device holds %p, expected %p", got, decl);
    D3DTEST_RELEASE(got);

    IDirect3DDevice9_SetVertexDeclaration(device, NULL);

cleanup:
    D3DTEST_RELEASE(decl);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
