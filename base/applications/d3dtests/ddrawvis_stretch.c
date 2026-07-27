/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw visual: a ring texture stretched by a hardware blit
 */


#include "d3dvis.h"
#include <ddraw.h>

/* A windowed DirectDraw presenter: primary + clipper, plus a system-memory
   back surface the test draws into with plain pixel writes. */
struct dd_screen
{
    IDirectDraw7 *ddraw;
    IDirectDrawSurface7 *primary;
    IDirectDrawSurface7 *back;
    IDirectDrawClipper *clipper;
    HWND hwnd;
};

static D3DTEST_UNUSED BOOL dd_open(struct dd_screen *s, HWND hwnd)
{
    DDSURFACEDESC2 desc;

    memset(s, 0, sizeof(*s));
    s->hwnd = hwnd;

    if (FAILED(DirectDrawCreateEx(NULL, (void **)&s->ddraw, &IID_IDirectDraw7, NULL)))
        return FALSE;
    if (FAILED(IDirectDraw7_SetCooperativeLevel(s->ddraw, hwnd, DDSCL_NORMAL)))
        return FALSE;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS;
    desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    if (FAILED(IDirectDraw7_CreateSurface(s->ddraw, &desc, &s->primary, NULL)))
        return FALSE;

    if (SUCCEEDED(IDirectDraw7_CreateClipper(s->ddraw, 0, &s->clipper, NULL)))
    {
        IDirectDrawClipper_SetHWnd(s->clipper, 0, hwnd);
        IDirectDrawSurface7_SetClipper(s->primary, s->clipper);
    }

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    desc.dwWidth = VIS_WIDTH;
    desc.dwHeight = VIS_HEIGHT;
    desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
    desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
    desc.ddpfPixelFormat.dwRGBBitCount = 32;
    desc.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
    desc.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
    desc.ddpfPixelFormat.dwBBitMask = 0x000000ff;
    if (FAILED(IDirectDraw7_CreateSurface(s->ddraw, &desc, &s->back, NULL)))
        return FALSE;

    return TRUE;
}

static D3DTEST_UNUSED void dd_close(struct dd_screen *s)
{
    if (s->back) IDirectDrawSurface7_Release(s->back);
    if (s->clipper) IDirectDrawClipper_Release(s->clipper);
    if (s->primary) IDirectDrawSurface7_Release(s->primary);
    if (s->ddraw) IDirectDraw7_Release(s->ddraw);
    memset(s, 0, sizeof(*s));
}

/* Hand the caller a pointer to the back surface pixels. */
static D3DTEST_UNUSED BOOL dd_lock(struct dd_screen *s, DWORD **pixels, int *pitch_dwords)
{
    DDSURFACEDESC2 desc;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (FAILED(IDirectDrawSurface7_Lock(s->back, NULL, &desc, DDLOCK_WAIT, NULL)))
        return FALSE;
    *pixels = (DWORD *)desc.lpSurface;
    *pitch_dwords = desc.lPitch / 4;
    return TRUE;
}

static D3DTEST_UNUSED void dd_unlock(struct dd_screen *s)
{
    IDirectDrawSurface7_Unlock(s->back, NULL);
}

/* Blit the back surface to wherever the window currently sits. */
static D3DTEST_UNUSED void dd_present(struct dd_screen *s)
{
    RECT dst;
    POINT tl = { 0, 0 };

    GetClientRect(s->hwnd, &dst);
    ClientToScreen(s->hwnd, &tl);
    OffsetRect(&dst, tl.x, tl.y);
    IDirectDrawSurface7_Blt(s->primary, &dst, s->back, NULL, DDBLT_WAIT, NULL);
}

/* Copy a strip out of the back surface for the end-of-run verification. */
static D3DTEST_UNUSED int dd_sample(struct dd_screen *s, DWORD *out, int max)
{
    DWORD *pixels;
    int pitch, i, n = 0;

    if (!dd_lock(s, &pixels, &pitch))
        return 0;
    for (i = 0; i < max && n < max; i++)
    {
        int x = (i * 7) % VIS_WIDTH;
        int y = (i * 13) % VIS_HEIGHT;
        out[n++] = pixels[y * pitch + x];
    }
    dd_unlock(s);
    return n;
}

int main(int argc, char **argv)
{
    IDirectDrawSurface7 *tile = NULL;
    static DWORD rings[128 * 128];
    DWORD sample[256];
    struct dd_screen s;
    DDSURFACEDESC2 desc;
    DDBLTFX fx;
    DWORD sam[1];
    int y, n;
    int frame = 0;
    HWND hwnd;
    HRESULT hr;

    (void)sam;
    vis_parse_args(argc, argv);
    test_begin("ddrawvis_stretch");

    hwnd = vis_create_window("DirectDraw: stretched blit");
    if (!dd_open(&s, hwnd))
    {
        skip_("could not open a windowed DirectDraw screen");
        goto done;
    }

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    desc.dwWidth = desc.dwHeight = 128;
    desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
    desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
    desc.ddpfPixelFormat.dwRGBBitCount = 32;
    desc.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
    desc.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
    desc.ddpfPixelFormat.dwBBitMask = 0x000000ff;
    hr = IDirectDraw7_CreateSurface(s.ddraw, &desc, &tile, NULL);
    ok_(SUCCEEDED(hr) && tile != NULL, "created a 128x128 source tile (0x%08lx)", hr);
    if (!tile)
        goto done;

    vis_tex_rings(rings, 128, 128);
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (SUCCEEDED(IDirectDrawSurface7_Lock(tile, NULL, &desc, DDLOCK_WAIT, NULL)))
    {
        for (y = 0; y < 128; y++)
            memcpy((BYTE *)desc.lpSurface + y * desc.lPitch, rings + y * 128, 128 * 4);
        IDirectDrawSurface7_Unlock(tile, NULL);
    }

    memset(&fx, 0, sizeof(fx));
    fx.dwSize = sizeof(fx);

    while (vis_frame(frame++))
    {
        /* Pulse the destination rectangle so the stretch factor changes. */
        float t = frame * 0.1f;
        int w = (int)(VIS_WIDTH * (0.35f + 0.3f * (1.0f + (float)sin(t))));
        int h = (int)(VIS_HEIGHT * (0.35f + 0.3f * (1.0f + (float)cos(t * 0.8f))));
        RECT dst;

        if (w > VIS_WIDTH) w = VIS_WIDTH;
        if (h > VIS_HEIGHT) h = VIS_HEIGHT;
        dst.left = (VIS_WIDTH - w) / 2;
        dst.top = (VIS_HEIGHT - h) / 2;
        dst.right = dst.left + w;
        dst.bottom = dst.top + h;

        fx.dwFillColor = 0xff101018;
        IDirectDrawSurface7_Blt(s.back, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        IDirectDrawSurface7_Blt(s.back, &dst, tile, NULL, DDBLT_WAIT, NULL);
        dd_present(&s);
    }
    ok_(frame > 1, "rendered %d stretched frames", frame - 1);

    n = dd_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
        vis_check_rendered(sample, n, 0xff101018);

    vis_wait_if_held();
done:
    D3DTEST_RELEASE(tile);
    dd_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
