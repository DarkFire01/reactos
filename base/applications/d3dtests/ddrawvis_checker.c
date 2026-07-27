/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw visual: a scrolling checkerboard blitted from an offscreen surface
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
    static DWORD image[VIS_WIDTH * VIS_HEIGHT];
    DWORD sample[256];
    struct dd_screen s;
    DWORD *pixels;
    int pitch, x, y, n;
    int frame = 0;
    HWND hwnd;

    vis_parse_args(argc, argv);
    test_begin("ddrawvis_checker");

    hwnd = vis_create_window("DirectDraw: scrolling checkerboard");
    if (!dd_open(&s, hwnd))
    {
        skip_("could not open a windowed DirectDraw screen");
        dd_close(&s);
        test_destroy_window(hwnd);
        return test_end();
    }

    vis_tex_checker(image, VIS_WIDTH, VIS_HEIGHT, 32, 0xff203060, 0xffe0e0f0);
    ok_(1, "built a %dx%d checkerboard", VIS_WIDTH, VIS_HEIGHT);

    while (vis_frame(frame++))
    {
        int ox = (frame * 3) % VIS_WIDTH;
        int oy = (frame * 2) % VIS_HEIGHT;

        if (dd_lock(&s, &pixels, &pitch))
        {
            /* Scroll by sampling the source with a wrapping offset. */
            for (y = 0; y < VIS_HEIGHT; y++)
            {
                int sy = (y + oy) % VIS_HEIGHT;
                for (x = 0; x < VIS_WIDTH; x++)
                    pixels[y * pitch + x] = image[sy * VIS_WIDTH + (x + ox) % VIS_WIDTH];
            }
            dd_unlock(&s);
        }
        dd_present(&s);
    }
    ok_(frame > 1, "rendered %d frames", frame - 1);

    n = dd_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
    {
        vis_check_rendered(sample, n, 0xff000000);
        ok_(vis_count_distinct(sample, n, 0x00ffffff) == 2,
            "the checkerboard uses exactly two colours");
    }

    vis_wait_if_held();
    dd_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
