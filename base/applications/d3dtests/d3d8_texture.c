/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8: texture creation, locking and stage binding
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
    IDirect3DTexture8 *texture = NULL;
    IDirect3DBaseTexture8 *got = NULL;
    IDirect3DDevice8 *device = NULL;
    IDirect3D8 *d3d = NULL;
    D3DLOCKED_RECT locked;
    D3DSURFACE_DESC desc;
    HRESULT hr;
    HWND hwnd;
    DWORD *row;
    int x, y;

    test_begin("d3d8_texture");

    hwnd = test_create_window("d3d8_texture", 320, 240);
    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d)
        goto done;

    device = create_device(d3d, hwnd);
    if (!device)
    {
        skip_("no Direct3D 8 device could be created on this adapter");
        goto cleanup;
    }

    hr = IDirect3DDevice8_CreateTexture(device, 32, 32, 1, 0, D3DFMT_A8R8G8B8,
                                        D3DPOOL_MANAGED, &texture);
    if (FAILED(hr))
    {
        skip_("CreateTexture(A8R8G8B8) returned 0x%08lx", hr);
        goto cleanup;
    }
    ok_(SUCCEEDED(hr) && texture != NULL, "created a 32x32 A8R8G8B8 texture");

    ok_(IDirect3DTexture8_GetLevelCount(texture) == 1,
        "texture has %u level(s), expected 1", IDirect3DTexture8_GetLevelCount(texture));

    memset(&desc, 0, sizeof(desc));
    hr = IDirect3DTexture8_GetLevelDesc(texture, 0, &desc);
    ok_(SUCCEEDED(hr), "GetLevelDesc(0) returned 0x%08lx", hr);
    ok_(desc.Width == 32 && desc.Height == 32,
        "level 0 is %ux%u, expected 32x32", desc.Width, desc.Height);

    memset(&locked, 0, sizeof(locked));
    hr = IDirect3DTexture8_LockRect(texture, 0, &locked, NULL, 0);
    ok_(SUCCEEDED(hr), "LockRect returned 0x%08lx", hr);
    if (SUCCEEDED(hr))
    {
        ok_(locked.pBits != NULL, "LockRect produced a pointer");
        ok_(locked.Pitch >= 32 * 4, "pitch %d covers a 32bpp row", locked.Pitch);

        /* A simple checkerboard, so a broken pitch shows up as a smear. */
        for (y = 0; y < 32; y++)
        {
            row = (DWORD *)((BYTE *)locked.pBits + y * locked.Pitch);
            for (x = 0; x < 32; x++)
                row[x] = ((x ^ y) & 4) ? 0xffffffff : 0xff000000;
        }

        hr = IDirect3DTexture8_UnlockRect(texture, 0);
        ok_(SUCCEEDED(hr), "UnlockRect returned 0x%08lx", hr);
    }

    hr = IDirect3DDevice8_SetTexture(device, 0, (IDirect3DBaseTexture8 *)texture);
    ok_(SUCCEEDED(hr), "SetTexture(stage 0) returned 0x%08lx", hr);

    hr = IDirect3DDevice8_GetTexture(device, 0, &got);
    ok_(SUCCEEDED(hr), "GetTexture(stage 0) returned 0x%08lx", hr);
    ok_((void *)got == (void *)texture, "stage 0 holds %p, expected %p", got, texture);
    D3DTEST_RELEASE(got);

    IDirect3DDevice8_SetTexture(device, 0, NULL);

cleanup:
    D3DTEST_RELEASE(texture);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
