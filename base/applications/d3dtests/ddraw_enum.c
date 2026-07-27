/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DirectDraw: driver enumeration and interface versions
 */

#include "d3dtest.h"
#include <ddraw.h>

static int enum_count;

static BOOL WINAPI enum_cb(GUID *guid, char *desc, char *name, void *ctx)
{
    enum_count++;
    info_("driver %d: name='%s' desc='%s' guid=%s",
          enum_count, name ? name : "(null)", desc ? desc : "(null)",
          guid ? "specific" : "primary");
    return DDENUMRET_OK;
}

int main(void)
{
    IDirectDraw *ddraw = NULL;
    IDirectDraw7 *ddraw7 = NULL;
    IDirectDraw4 *ddraw4 = NULL;
    HRESULT hr;

    test_begin("ddraw_enum");

    hr = DirectDrawEnumerateA(enum_cb, NULL);
    ok_(SUCCEEDED(hr), "DirectDrawEnumerateA returned 0x%08lx", hr);
    ok_(enum_count >= 1, "enumerated %d driver(s), expected at least the primary", enum_count);

    hr = DirectDrawCreate(NULL, &ddraw, NULL);
    ok_(SUCCEEDED(hr) && ddraw != NULL, "DirectDrawCreate returned 0x%08lx", hr);

    if (ddraw)
    {
        /* Every ddraw object should expose the later interfaces. */
        hr = IDirectDraw_QueryInterface(ddraw, &IID_IDirectDraw4, (void **)&ddraw4);
        ok_(SUCCEEDED(hr) && ddraw4 != NULL, "QueryInterface(IDirectDraw4) returned 0x%08lx", hr);
        D3DTEST_RELEASE(ddraw4);

        hr = IDirectDraw_QueryInterface(ddraw, &IID_IDirectDraw7, (void **)&ddraw7);
        ok_(SUCCEEDED(hr) && ddraw7 != NULL, "QueryInterface(IDirectDraw7) returned 0x%08lx", hr);
        D3DTEST_RELEASE(ddraw7);

        D3DTEST_RELEASE(ddraw);
    }

    /* DirectDrawCreateEx only hands out DirectDraw7. */
    hr = DirectDrawCreateEx(NULL, (void **)&ddraw7, &IID_IDirectDraw7, NULL);
    ok_(SUCCEEDED(hr) && ddraw7 != NULL, "DirectDrawCreateEx returned 0x%08lx", hr);
    D3DTEST_RELEASE(ddraw7);

    return test_end();
}
