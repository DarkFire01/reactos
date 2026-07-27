/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: 8-bit palette creation and entry round-trip
 */


#include "d3dtest.h"
#include <ddraw.h>

int main(void)
{
    IDirectDrawPalette *palette = NULL;
    IDirectDraw7 *ddraw = NULL;
    PALETTEENTRY entries[256];
    PALETTEENTRY readback[256];
    int match = 1;
    HRESULT hr;
    HWND hwnd;
    int i;

    test_begin("ddraw_palette");

    hwnd = test_create_window("ddraw_palette", 320, 240);
    hr = DirectDrawCreateEx(NULL, (void **)&ddraw, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    if (!ddraw)
        goto done;

    IDirectDraw7_SetCooperativeLevel(ddraw, hwnd, DDSCL_NORMAL);

    /* A simple greyscale ramp, so a wrong entry is obvious. */
    for (i = 0; i < 256; i++)
    {
        entries[i].peRed = (BYTE)i;
        entries[i].peGreen = (BYTE)i;
        entries[i].peBlue = (BYTE)i;
        entries[i].peFlags = 0;
    }

    hr = IDirectDraw7_CreatePalette(ddraw, DDPCAPS_8BIT | DDPCAPS_ALLOW256,
                                    entries, &palette, NULL);
    ok_(SUCCEEDED(hr) && palette != NULL, "CreatePalette(8BIT) returned 0x%08lx", hr);
    if (!palette)
        goto cleanup;

    memset(readback, 0, sizeof(readback));
    hr = IDirectDrawPalette_GetEntries(palette, 0, 0, 256, readback);
    ok_(SUCCEEDED(hr), "GetEntries returned 0x%08lx", hr);

    for (i = 0; i < 256; i++)
    {
        if (readback[i].peRed != (BYTE)i || readback[i].peGreen != (BYTE)i ||
            readback[i].peBlue != (BYTE)i)
        {
            match = 0;
            info_("entry %d is %u,%u,%u", i, readback[i].peRed,
                  readback[i].peGreen, readback[i].peBlue);
            break;
        }
    }
    ok_(match, "all 256 palette entries round-tripped unchanged");

    /* Change a single entry and confirm only that one moved. */
    entries[7].peRed = 0xfe;
    entries[7].peGreen = 0xed;
    entries[7].peBlue = 0xfa;
    hr = IDirectDrawPalette_SetEntries(palette, 0, 7, 1, &entries[7]);
    ok_(SUCCEEDED(hr), "SetEntries for a single index returned 0x%08lx", hr);

    hr = IDirectDrawPalette_GetEntries(palette, 0, 7, 1, readback);
    ok_(SUCCEEDED(hr), "GetEntries after update returned 0x%08lx", hr);
    ok_(readback[0].peRed == 0xfe && readback[0].peGreen == 0xed && readback[0].peBlue == 0xfa,
        "updated entry reads back as %u,%u,%u", readback[0].peRed,
        readback[0].peGreen, readback[0].peBlue);

cleanup:
    D3DTEST_RELEASE(palette);
    D3DTEST_RELEASE(ddraw);
done:
    test_destroy_window(hwnd);
    return test_end();
}
