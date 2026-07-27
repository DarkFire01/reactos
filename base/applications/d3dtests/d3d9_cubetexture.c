/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 9: cube and volume textures
 */


#include "d3dtest.h"
#include <d3d9.h>

static D3DTEST_UNUSED IDirect3DDevice9 *create_device9(IDirect3D9 *d3d, HWND hwnd, BOOL depth)
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
    if (depth)
    {
        pp.EnableAutoDepthStencil = TRUE;
        pp.AutoDepthStencilFormat = D3DFMT_D24S8;
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

/* Render-target readback helper: copy the RT into a lockable system surface
   and return one pixel. Returns FALSE when the path is unavailable. */
static D3DTEST_UNUSED BOOL read_rt_pixel(IDirect3DDevice9 *device, int x, int y, DWORD *out)
{
    IDirect3DSurface9 *rt = NULL, *sys = NULL;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    BOOL ok = FALSE;

    if (FAILED(IDirect3DDevice9_GetRenderTarget(device, 0, &rt)))
        return FALSE;
    if (FAILED(IDirect3DSurface9_GetDesc(rt, &desc)))
        goto done;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(device, desc.Width, desc.Height,
            desc.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)))
        goto done;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(device, rt, sys)))
        goto done;
    if (FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY)))
        goto done;

    *out = *(DWORD *)((BYTE *)lr.pBits + y * lr.Pitch + x * 4);
    IDirect3DSurface9_UnlockRect(sys);
    ok = TRUE;

done:
    if (sys) IDirect3DSurface9_Release(sys);
    if (rt) IDirect3DSurface9_Release(rt);
    return ok;
}

int main(void)
{
    IDirect3DCubeTexture9 *cube = NULL;
    IDirect3DVolumeTexture9 *volume = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d = NULL;
    D3DSURFACE_DESC desc;
    D3DVOLUME_DESC vdesc;
    D3DLOCKED_BOX box;
    D3DCAPS9 caps;
    HRESULT hr;
    HWND hwnd;
    int face;

    test_begin("d3d9_cubetexture");

    hwnd = test_create_window("d3d9_cubetexture", 320, 240);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) goto done;
    device = create_device9(d3d, hwnd, FALSE);
    if (!device) { skip_("no Direct3D 9 device"); goto cleanup; }

    memset(&caps, 0, sizeof(caps));
    IDirect3DDevice9_GetDeviceCaps(device, &caps);

    if (!(caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP))
    {
        skip_("device advertises no cube map support");
    }
    else
    {
        hr = IDirect3DDevice9_CreateCubeTexture(device, 32, 1, 0, D3DFMT_A8R8G8B8,
                                                D3DPOOL_MANAGED, &cube, NULL);
        ok_(SUCCEEDED(hr) && cube != NULL, "CreateCubeTexture(32) returned 0x%08lx", hr);

        if (cube)
        {
            memset(&desc, 0, sizeof(desc));
            hr = IDirect3DCubeTexture9_GetLevelDesc(cube, 0, &desc);
            ok_(SUCCEEDED(hr), "GetLevelDesc(0) returned 0x%08lx", hr);
            ok_(desc.Width == 32 && desc.Height == 32,
                "cube face is %ux%u, expected 32x32", desc.Width, desc.Height);

            /* Every one of the six faces must be reachable and lockable. */
            for (face = 0; face < 6; face++)
            {
                D3DLOCKED_RECT lr;
                hr = IDirect3DCubeTexture9_LockRect(cube, (D3DCUBEMAP_FACES)face, 0, &lr, NULL, 0);
                if (FAILED(hr))
                {
                    ok_(0, "LockRect on face %d returned 0x%08lx", face, hr);
                    break;
                }
                IDirect3DCubeTexture9_UnlockRect(cube, (D3DCUBEMAP_FACES)face, 0);
            }
            if (face == 6)
                ok_(1, "all six cube faces locked and unlocked");
        }
    }

    if (!(caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP))
    {
        skip_("device advertises no volume texture support");
    }
    else
    {
        hr = IDirect3DDevice9_CreateVolumeTexture(device, 16, 16, 16, 1, 0,
                D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &volume, NULL);
        ok_(SUCCEEDED(hr) && volume != NULL, "CreateVolumeTexture(16^3) returned 0x%08lx", hr);

        if (volume)
        {
            memset(&vdesc, 0, sizeof(vdesc));
            hr = IDirect3DVolumeTexture9_GetLevelDesc(volume, 0, &vdesc);
            ok_(SUCCEEDED(hr), "volume GetLevelDesc(0) returned 0x%08lx", hr);
            ok_(vdesc.Width == 16 && vdesc.Height == 16 && vdesc.Depth == 16,
                "volume is %ux%ux%u", vdesc.Width, vdesc.Height, vdesc.Depth);

            memset(&box, 0, sizeof(box));
            hr = IDirect3DVolumeTexture9_LockBox(volume, 0, &box, NULL, 0);
            ok_(SUCCEEDED(hr), "LockBox returned 0x%08lx", hr);
            if (SUCCEEDED(hr))
            {
                ok_(box.pBits != NULL, "LockBox produced a pointer");
                ok_(box.SlicePitch >= box.RowPitch * 16,
                    "slice pitch %d covers 16 rows of %d", box.SlicePitch, box.RowPitch);
                IDirect3DVolumeTexture9_UnlockBox(volume, 0);
            }
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
