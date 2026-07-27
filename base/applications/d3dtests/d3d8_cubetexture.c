/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 8: cube and volume textures
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
    IDirect3DCubeTexture8 *cube = NULL;
    IDirect3DVolumeTexture8 *volume = NULL;
    IDirect3DDevice8 *device = NULL;
    IDirect3D8 *d3d = NULL;
    D3DSURFACE_DESC desc;
    D3DVOLUME_DESC vdesc;
    D3DCAPS8 caps;
    HRESULT hr;
    HWND hwnd;

    test_begin("d3d8_cubetexture");

    hwnd = test_create_window("d3d8_cubetexture", 320, 240);
    d3d = Direct3DCreate8(D3D_SDK_VERSION);
    if (!d3d) goto done;
    device = create_device8(d3d, hwnd, FALSE);
    if (!device) { skip_("no Direct3D 8 device"); goto cleanup; }

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice8_GetDeviceCaps(device, &caps);

    if (!(caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP))
    {
        skip_("device advertises no cube map support");
    }
    else
    {
        hr = IDirect3DDevice8_CreateCubeTexture(device, 32, 1, 0, D3DFMT_A8R8G8B8,
                                                D3DPOOL_MANAGED, &cube);
        ok_(SUCCEEDED(hr) && cube != NULL, "CreateCubeTexture(32) returned 0x%08lx", hr);

        if (cube)
        {
            memset(&desc, 0, sizeof(desc));
            hr = IDirect3DCubeTexture8_GetLevelDesc(cube, 0, &desc);
            ok_(SUCCEEDED(hr), "GetLevelDesc(0) returned 0x%08lx", hr);
            ok_(desc.Width == 32 && desc.Height == 32,
                "cube face is %ux%u, expected 32x32", desc.Width, desc.Height);

            /* All six faces must be addressable. */
            {
                IDirect3DSurface8 *face = NULL;
                hr = IDirect3DCubeTexture8_GetCubeMapSurface(cube,
                        D3DCUBEMAP_FACE_NEGATIVE_Z, 0, &face);
                ok_(SUCCEEDED(hr) && face != NULL,
                    "GetCubeMapSurface(NEGATIVE_Z) returned 0x%08lx", hr);
                D3DTEST_RELEASE(face);
            }
        }
    }

    if (!(caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP))
    {
        skip_("device advertises no volume texture support");
    }
    else
    {
        hr = IDirect3DDevice8_CreateVolumeTexture(device, 16, 16, 16, 1, 0,
                D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &volume);
        ok_(SUCCEEDED(hr) && volume != NULL, "CreateVolumeTexture(16^3) returned 0x%08lx", hr);

        if (volume)
        {
            memset(&vdesc, 0, sizeof(vdesc));
            hr = IDirect3DVolumeTexture8_GetLevelDesc(volume, 0, &vdesc);
            ok_(SUCCEEDED(hr), "volume GetLevelDesc(0) returned 0x%08lx", hr);
            ok_(vdesc.Width == 16 && vdesc.Height == 16 && vdesc.Depth == 16,
                "volume is %ux%ux%u, expected 16x16x16", vdesc.Width, vdesc.Height, vdesc.Depth);
        }
    }

cleanup:
    D3DTEST_RELEASE(cube);
    D3DTEST_RELEASE(volume);
    D3DTEST_RELEASE(device);
    D3DTEST_RELEASE(d3d);
done:
    test_destroy_window(hwnd);
    return test_end();
}
