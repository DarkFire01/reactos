/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw visual: colour-keyed sprites bouncing over a background
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
    IDirectDrawSurface7 *sprite = NULL;
    static DWORD bg[VIS_WIDTH * VIS_HEIGHT];
    DWORD sample[256];
    struct dd_screen s;
    DDSURFACEDESC2 desc;
    DDCOLORKEY key;
    DWORD *pixels;
    int pitch, x, y, n;
    int frame = 0;
    HWND hwnd;
    HRESULT hr;

    vis_parse_args(argc, argv);
    test_begin("ddrawvis_sprite");

    hwnd = vis_create_window("DirectDraw: colour-keyed sprite");
    if (!dd_open(&s, hwnd))
    {
        skip_("could not open a windowed DirectDraw screen");
        goto done;
    }

    /* A 48x48 sprite: a filled disc on a magenta key colour. */
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    desc.dwWidth = desc.dwHeight = 48;
    desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
    desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
    desc.ddpfPixelFormat.dwRGBBitCount = 32;
    desc.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
    desc.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
    desc.ddpfPixelFormat.dwBBitMask = 0x000000ff;
    hr = IDirectDraw7_CreateSurface(s.ddraw, &desc, &sprite, NULL);
    ok_(SUCCEEDED(hr) && sprite != NULL, "created a 48x48 sprite surface (0x%08lx)", hr);
    if (!sprite)
        goto done;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (SUCCEEDED(IDirectDrawSurface7_Lock(sprite, NULL, &desc, DDLOCK_WAIT, NULL)))
    {
        for (y = 0; y < 48; y++)
        {
            DWORD *row = (DWORD *)((BYTE *)desc.lpSurface + y * desc.lPitch);
            for (x = 0; x < 48; x++)
            {
                int dx = x - 24, dy = y - 24;
                row[x] = (dx * dx + dy * dy < 22 * 22) ? 0xffffc020 : 0xffff00ff;
            }
        }
        IDirectDrawSurface7_Unlock(sprite, NULL);
    }

    key.dwColorSpaceLowValue = key.dwColorSpaceHighValue = 0x00ff00ff;
    hr = IDirectDrawSurface7_SetColorKey(sprite, DDCKEY_SRCBLT, &key);
    ok_(SUCCEEDED(hr), "SetColorKey(SRCBLT, magenta) returned 0x%08lx", hr);

    vis_tex_gradient(bg, VIS_WIDTH, VIS_HEIGHT);

    while (vis_frame(frame++))
    {
        if (dd_lock(&s, &pixels, &pitch))
        {
            for (y = 0; y < VIS_HEIGHT; y++)
                memcpy(pixels + y * pitch, bg + y * VIS_WIDTH, VIS_WIDTH * 4);
            dd_unlock(&s);
        }

        /* Three discs on different Lissajous paths. */
        for (n = 0; n < 3; n++)
        {
            float t = frame * 0.09f + n * 2.0f;
            int px = (int)((VIS_WIDTH / 2 - 24) * (1.0 + 0.85 * sin(t)));
            int py = (int)((VIS_HEIGHT / 2 - 24) * (1.0 + 0.85 * cos(t * 1.3f)));
            RECT src = { 0, 0, 48, 48 };

            IDirectDrawSurface7_BltFast(s.back, px, py, sprite, &src,
                                        DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT);
        }
        dd_present(&s);
    }
    ok_(frame > 1, "rendered %d frames with three keyed sprites", frame - 1);

    n = dd_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
        vis_check_rendered(sample, n, 0xff000000);

    vis_wait_if_held();
done:
    D3DTEST_RELEASE(sprite);
    dd_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
