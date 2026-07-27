/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: textures, mip levels and sampler binding
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
    IDirect3DTexture9 *texture = NULL;
    IDirect3DBaseTexture9 *got = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    D3DLOCKED_RECT locked;
    D3DSURFACE_DESC desc;
    DWORD value = 0;
    HRESULT hr;
    HWND hwnd;
    DWORD *row;
    int x, y;

    test_begin("d3d9_texture");

    hwnd = test_create_window("d3d9_texture", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 9 device could be created on this adapter");
        goto cleanup;
    }

    /* 0 levels means "generate the full mip chain". 64x64 gives 7 levels. */
    hr = IDirect3DDevice9_CreateTexture(device, 64, 64, 0, 0, D3DFMT_A8R8G8B8,
                                        D3DPOOL_MANAGED, &texture, NULL);
    if (FAILED(hr))
    {
        skip_("CreateTexture(A8R8G8B8) returned 0x%08lx", hr);
        goto cleanup;
    }
    ok_(SUCCEEDED(hr), "created a 64x64 mipmapped A8R8G8B8 texture");

    ok_(IDirect3DTexture9_GetLevelCount(texture) == 7,
        "texture has %u level(s), expected 7 for 64x64",
        IDirect3DTexture9_GetLevelCount(texture));

    memset(&desc, 0, sizeof(desc));
    hr = IDirect3DTexture9_GetLevelDesc(texture, 1, &desc);
    ok_(SUCCEEDED(hr), "GetLevelDesc(1) returned 0x%08lx", hr);
    ok_(desc.Width == 32 && desc.Height == 32,
        "level 1 is %ux%u, expected 32x32", desc.Width, desc.Height);

    memset(&locked, 0, sizeof(locked));
    hr = IDirect3DTexture9_LockRect(texture, 0, &locked, NULL, 0);
    ok_(SUCCEEDED(hr), "LockRect(0) returned 0x%08lx", hr);
    if (SUCCEEDED(hr))
    {
        for (y = 0; y < 64; y++)
        {
            row = (DWORD *)((BYTE *)locked.pBits + y * locked.Pitch);
            for (x = 0; x < 64; x++)
                row[x] = 0xff000000 | (x << 16) | (y << 8);
        }
        hr = IDirect3DTexture9_UnlockRect(texture, 0);
        ok_(SUCCEEDED(hr), "UnlockRect(0) returned 0x%08lx", hr);
    }

    hr = IDirect3DDevice9_SetTexture(device, 0, (IDirect3DBaseTexture9 *)texture);
    ok_(SUCCEEDED(hr), "SetTexture(stage 0) returned 0x%08lx", hr);

    hr = IDirect3DDevice9_GetTexture(device, 0, &got);
    ok_(SUCCEEDED(hr), "GetTexture(stage 0) returned 0x%08lx", hr);
    ok_((void *)got == (void *)texture, "stage 0 holds %p, expected %p", got, texture);
    D3DTEST_RELEASE(got);

    hr = IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    ok_(SUCCEEDED(hr), "SetSamplerState(MINFILTER, LINEAR) returned 0x%08lx", hr);
    hr = IDirect3DDevice9_GetSamplerState(device, 0, D3DSAMP_MINFILTER, &value);
    ok_(SUCCEEDED(hr), "GetSamplerState(MINFILTER) returned 0x%08lx", hr);
    ok_(value == D3DTEXF_LINEAR, "MINFILTER reads back as %lu, expected LINEAR", value);

    hr = IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    ok_(SUCCEEDED(hr), "SetSamplerState(ADDRESSU, CLAMP) returned 0x%08lx", hr);

    IDirect3DDevice9_SetTexture(device, 0, NULL);

cleanup:
    D3DTEST_RELEASE(texture);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
