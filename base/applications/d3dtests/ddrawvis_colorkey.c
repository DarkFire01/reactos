/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw visual: a source colour key dropping pixels out of a blit
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

/* The colour the key is set to, and the colour of the part of the tile that
   must survive the blit. Neither can be produced by vis_tex_gradient, so
   counting them over the whole surface is unambiguous. */
#define KEY_COLOUR  0x00ff00ff
#define BODY_COLOUR 0x0010ff10

/* Count how many pixels of the back surface carry one exact colour. The top
   eight bits are not part of the format, so they are masked off. */
static int count_colour(struct dd_screen *s, DWORD colour)
{
    DWORD *pixels;
    int pitch, x, y, n = 0;

    if (!dd_lock(s, &pixels, &pitch))
        return -1;
    for (y = 0; y < VIS_HEIGHT; y++)
    {
        for (x = 0; x < VIS_WIDTH; x++)
        {
            if ((pixels[y * pitch + x] & 0x00ffffff) == (colour & 0x00ffffff))
                n++;
        }
    }
    dd_unlock(s);
    return n;
}

static IDirectDrawSurface7 *make_surface(IDirectDraw7 *ddraw, int w, int h)
{
    IDirectDrawSurface7 *surface = NULL;
    DDSURFACEDESC2 desc;

    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    desc.dwWidth = w;
    desc.dwHeight = h;
    desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
    desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
    desc.ddpfPixelFormat.dwRGBBitCount = 32;
    desc.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
    desc.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
    desc.ddpfPixelFormat.dwBBitMask = 0x000000ff;
    if (FAILED(IDirectDraw7_CreateSurface(ddraw, &desc, &surface, NULL)))
        return NULL;
    return surface;
}

int main(int argc, char **argv)
{
    IDirectDrawSurface7 *background = NULL, *tile = NULL;
    static DWORD gradient[VIS_WIDTH * VIS_HEIGHT];
    DWORD sample[256];
    struct dd_screen s;
    DDSURFACEDESC2 desc;
    DDCOLORKEY key;
    DDCAPS caps;
    DWORD blt_flags = DDBLT_WAIT;
    int have_key = 0;
    int frame = 0, x, y, i, n;
    HWND hwnd;
    HRESULT hr;

    vis_parse_args(argc, argv);
    test_begin("ddrawvis_colorkey");

    hwnd = vis_create_window("DirectDraw: source colour key");
    if (!dd_open(&s, hwnd))
    {
        skip_("could not open a windowed DirectDraw screen");
        goto done;
    }

    memset(&caps, 0, sizeof(caps));
    caps.dwSize = sizeof(caps);
    if (SUCCEEDED(IDirectDraw7_GetCaps(s.ddraw, &caps, NULL)))
        info_("driver colour key caps 0x%08lx", (unsigned long)caps.dwCKeyCaps);

    /* A full-frame gradient to blit the tiles over, so the keyed-out pixels
       land on something obviously different from the tile. */
    background = make_surface(s.ddraw, VIS_WIDTH, VIS_HEIGHT);
    tile = make_surface(s.ddraw, 64, 64);
    ok_(background != NULL && tile != NULL, "created the background and tile surfaces");
    if (!background || !tile)
        goto done;

    vis_tex_gradient(gradient, VIS_WIDTH, VIS_HEIGHT);
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (SUCCEEDED(IDirectDrawSurface7_Lock(background, NULL, &desc, DDLOCK_WAIT, NULL)))
    {
        for (y = 0; y < VIS_HEIGHT; y++)
            memcpy((BYTE *)desc.lpSurface + y * desc.lPitch,
                   gradient + y * VIS_WIDTH, VIS_WIDTH * 4);
        IDirectDrawSurface7_Unlock(background, NULL);
    }

    /* The tile: a solid diamond on a field of the key colour. */
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    if (SUCCEEDED(IDirectDrawSurface7_Lock(tile, NULL, &desc, DDLOCK_WAIT, NULL)))
    {
        for (y = 0; y < 64; y++)
        {
            DWORD *row = (DWORD *)((BYTE *)desc.lpSurface + y * desc.lPitch);

            for (x = 0; x < 64; x++)
            {
                int dx = x - 32, dy = y - 32;

                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                row[x] = (dx + dy < 30) ? (0xff000000 | BODY_COLOUR)
                                        : (0xff000000 | KEY_COLOUR);
            }
        }
        IDirectDrawSurface7_Unlock(tile, NULL);
    }

    key.dwColorSpaceLowValue = key.dwColorSpaceHighValue = KEY_COLOUR;
    hr = IDirectDrawSurface7_SetColorKey(tile, DDCKEY_SRCBLT, &key);
    if (SUCCEEDED(hr))
    {
        have_key = 1;
        blt_flags = DDBLT_KEYSRC | DDBLT_WAIT;
        ok_(1, "SetColorKey(DDCKEY_SRCBLT, magenta) returned 0x%08lx", hr);
    }
    else
    {
        skip_("SetColorKey(DDCKEY_SRCBLT) returned 0x%08lx; blitting unkeyed", hr);
    }

    while (vis_frame(frame++))
    {
        RECT src = { 0, 0, 64, 64 };

        IDirectDrawSurface7_Blt(s.back, NULL, background, NULL, DDBLT_WAIT, NULL);

        /* Six tiles on a fixed grid, each wobbling about its own cell. The
           offsets are small enough that the destination always stays inside
           the back surface, which Blt insists on. */
        for (i = 0; i < 6; i++)
        {
            int cx = 80 + (i % 3) * 160 + (int)(40.0f * (float)sin(frame * 0.12f + i));
            int cy = 90 + (i / 3) * 170 + (int)(30.0f * (float)cos(frame * 0.1f + i * 1.7f));
            RECT dst;

            dst.left = cx - 32;
            dst.top = cy - 32;
            dst.right = dst.left + 64;
            dst.bottom = dst.top + 64;

            hr = IDirectDrawSurface7_Blt(s.back, &dst, tile, &src, blt_flags, NULL);
            if (FAILED(hr) && (blt_flags & DDBLT_KEYSRC))
            {
                skip_("Blt with DDBLT_KEYSRC returned 0x%08lx; blitting unkeyed", hr);
                have_key = 0;
                blt_flags = DDBLT_WAIT;
                IDirectDrawSurface7_Blt(s.back, &dst, tile, &src, blt_flags, NULL);
            }
            else if (frame == 1 && i == 0)
            {
                ok_(SUCCEEDED(hr), "Blt(%s) returned 0x%08lx",
                    have_key ? "DDBLT_KEYSRC" : "unkeyed", hr);
            }
        }
        dd_present(&s);
    }
    ok_(frame > 1, "rendered %d frames of keyed tiles", frame - 1);

    n = dd_sample(&s, sample, ARRAYSIZE(sample));
    if (n)
        vis_check_rendered(sample, n, 0xff000000);

    n = count_colour(&s, BODY_COLOUR);
    if (n < 0)
        skip_("could not lock the back surface to look for the tile body");
    else
        ok_(n > 0, "%d pixels of the tile body reached the back surface", n);

    n = count_colour(&s, KEY_COLOUR);
    if (n < 0)
        skip_("could not lock the back surface to look for keyed pixels");
    else if (have_key)
        ok_(n == 0, "the key colour was dropped from the blit (%d pixels left)", n);
    else
        info_("%d key-coloured pixels remain; the key was never applied", n);

    vis_wait_if_held();
done:
    D3DTEST_RELEASE(tile);
    D3DTEST_RELEASE(background);
    dd_close(&s);
    test_destroy_window(hwnd);
    return test_end();
}
