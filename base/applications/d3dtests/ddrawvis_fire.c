/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw visual: a rising flame effect written pixel by pixel
 * COPYRIGHT:   Copyright 2026 The ReactOS Project
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

/* The heat field runs at half the screen resolution: each cell covers a 2x2
   block of pixels, which both halves the work and lets the flame front climb
   twice as fast in the frames this test has to play with. */
#define FIRE_W (VIS_WIDTH / 2)
#define FIRE_H (VIS_HEIGHT / 2)

/* A private generator, so the flicker is the same on every run and a bad frame
   can be reproduced. */
static unsigned int fire_seed = 0x13579bdf;

static unsigned int fire_rand(void)
{
    fire_seed = fire_seed * 1103515245u + 12345u;
    return (fire_seed >> 16) & 0x7fff;
}

int main(int argc, char **argv)
{
    static BYTE heat[FIRE_W * FIRE_H];
    static DWORD palette[256];
    DWORD sample[256];
    struct dd_screen s;
    DWORD *pixels;
    int pitch, x, y, i, n;
    int frame = 0;
    HWND hwnd;

    vis_parse_args(argc, argv);
    test_begin("ddrawvis_fire");

    hwnd = vis_create_window("DirectDraw: fire");
    if (!dd_open(&s, hwnd))
    {
        skip_("could not open a windowed DirectDraw screen");
        dd_close(&s);
        test_destroy_window(hwnd);
        return test_end();
    }
    ok_(1, "opened a %dx%d windowed DirectDraw screen", VIS_WIDTH, VIS_HEIGHT);

    /* The classic ramp: black to red to orange to yellow to white. Entry 0 is
       black, which is also what the cold part of the field maps to and what the
       verification below treats as the background. */
    for (i = 0; i < 256; i++)
    {
        int r = i * 3, g = (i - 85) * 3, b = (i - 170) * 3;

        if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;
        palette[i] = 0xff000000 | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
    }
    ok_(palette[0] == 0xff000000 && palette[255] == 0xffffffff,
        "built a 256-entry fire palette");

    memset(heat, 0, sizeof(heat));

    while (vis_frame(frame++))
    {
        float t = frame * 0.15f;

        /* Every cell becomes the average of the three cells below it and the
           one below that, minus a little; heat therefore drifts upwards and
           dies out, which is the whole effect. Rows are written top down so
           the rows being read still hold the previous frame. */
        for (y = 0; y < FIRE_H - 2; y++)
        {
            for (x = 0; x < FIRE_W; x++)
            {
                int l = x > 0 ? x - 1 : 0;
                int r = x < FIRE_W - 1 ? x + 1 : FIRE_W - 1;
                int v = (heat[(y + 1) * FIRE_W + l]
                       + heat[(y + 1) * FIRE_W + x]
                       + heat[(y + 1) * FIRE_W + r]
                       + heat[(y + 2) * FIRE_W + x]) / 4 - 1;

                heat[y * FIRE_W + x] = (BYTE)(v > 0 ? v : 0);
            }
        }

        /* Seed the bottom two rows with a hot, flickering, slowly moving band. */
        for (x = 0; x < FIRE_W; x++)
        {
            int v = 205 + (int)(50.0f * (float)sin(x * 0.035f + t))
                  - (int)(fire_rand() % 90);

            if (v < 0) v = 0;
            if (v > 255) v = 255;
            heat[(FIRE_H - 2) * FIRE_W + x] = (BYTE)v;
            heat[(FIRE_H - 1) * FIRE_W + x] = (BYTE)v;
        }

        if (dd_lock(&s, &pixels, &pitch))
        {
            for (y = 0; y < VIS_HEIGHT; y++)
            {
                const BYTE *row = heat + (y / 2) * FIRE_W;

                for (x = 0; x < VIS_WIDTH; x++)
                    pixels[y * pitch + x] = palette[row[x / 2]];
            }
            dd_unlock(&s);
        }
        dd_present(&s);
    }
    ok_(frame > 1, "rendered %d frames of fire", frame - 1);

    n = dd_sample(&s, sample, ARRAYSIZE(sample));
    ok_(n > 0, "sampled %d pixels from the back surface", n);
    if (n)
        vis_check_rendered(sample, n, 0xff000000);

    vis_wait_if_held();
    dd_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
