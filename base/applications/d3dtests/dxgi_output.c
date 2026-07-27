/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     DXGI: output enumeration and display mode lists
 */


#include "d3dtest.h"
#include <dxgi.h>

int main(void)
{
    IDXGIFactory *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIOutput *output = NULL;
    DXGI_OUTPUT_DESC desc;
    DXGI_MODE_DESC *modes = NULL;
    UINT count = 0, outputs = 0;
    HRESULT hr;
    UINT i;

    test_begin("dxgi_output");

    hr = CreateDXGIFactory(&IID_IDXGIFactory, (void **)&factory);
    ok_(SUCCEEDED(hr) && factory != NULL, "CreateDXGIFactory returned 0x%08lx", hr);
    if (!factory)
        return test_end();

    hr = IDXGIFactory_EnumAdapters(factory, 0, &adapter);
    if (FAILED(hr))
    {
        skip_("no adapter 0 (0x%08lx)", hr);
        goto cleanup;
    }

    while (IDXGIAdapter_EnumOutputs(adapter, outputs, &output) != DXGI_ERROR_NOT_FOUND)
    {
        memset(&desc, 0, sizeof(desc));
        hr = IDXGIOutput_GetDesc(output, &desc);
        ok_(SUCCEEDED(hr), "output %u: GetDesc returned 0x%08lx", outputs, hr);
        if (SUCCEEDED(hr))
            info_("output %u: '%ls' desktop %ldx%ld..%ldx%ld attached=%d",
                  outputs, desc.DeviceName,
                  desc.DesktopCoordinates.left, desc.DesktopCoordinates.top,
                  desc.DesktopCoordinates.right, desc.DesktopCoordinates.bottom,
                  desc.AttachedToDesktop);

        if (outputs == 0)
        {
            /* Two-call idiom: ask for the count, then for the data. */
            hr = IDXGIOutput_GetDisplayModeList(output, DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                                                &count, NULL);
            if (SUCCEEDED(hr) && count)
            {
                modes = (DXGI_MODE_DESC *)calloc(count, sizeof(*modes));
                if (modes)
                {
                    hr = IDXGIOutput_GetDisplayModeList(output, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                        0, &count, modes);
                    ok_(SUCCEEDED(hr), "GetDisplayModeList returned 0x%08lx", hr);
                    ok_(count > 0, "output 0 has %u R8G8B8A8 mode(s)", count);
                    for (i = 0; i < count && i < 5; i++)
                        info_("  mode %u: %ux%u @%u/%u Hz", i, modes[i].Width, modes[i].Height,
                              modes[i].RefreshRate.Numerator, modes[i].RefreshRate.Denominator);
                    free(modes);
                    modes = NULL;
                }
            }
            else
            {
                skip_("GetDisplayModeList returned 0x%08lx with %u modes", hr, count);
            }
        }

        D3DTEST_RELEASE(output);
        outputs++;
        if (outputs > 8)
            break;
    }

    if (!outputs)
        skip_("adapter 0 reports no outputs");
    else
        ok_(outputs >= 1, "enumerated %u output(s)", outputs);

cleanup:
    D3DTEST_RELEASE(adapter);
    D3DTEST_RELEASE(factory);
    return test_end();
}
