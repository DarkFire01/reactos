/*
 * PROJECT:     ReactOS DirectX test applications
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D 10: blob objects
 */


#include "d3dtest.h"
#include <d3d10.h>

int main(void)
{
    ID3D10Blob *blob = NULL;
    SIZE_T size;
    void *ptr;
    HRESULT hr;

    test_begin("d3d10_blob");

    hr = D3D10CreateBlob(256, &blob);
    ok_(SUCCEEDED(hr) && blob != NULL, "D3D10CreateBlob(256) returned 0x%08lx", hr);
    if (!blob)
        return test_end();

    size = ID3D10Blob_GetBufferSize(blob);
    ok_(size == 256, "blob reports %u bytes, expected 256", (unsigned)size);

    ptr = ID3D10Blob_GetBufferPointer(blob);
    ok_(ptr != NULL, "blob has a buffer pointer");

    if (ptr)
    {
        /* The buffer must be real, writable memory of the requested size. */
        memset(ptr, 0xab, size);
        ok_(*((unsigned char *)ptr) == 0xab && *((unsigned char *)ptr + size - 1) == 0xab,
            "blob storage is writable across its whole length");
    }

    D3DTEST_RELEASE(blob);

    hr = D3D10CreateBlob(0, &blob);
    info_("D3D10CreateBlob(0) returned 0x%08lx", hr);
    D3DTEST_RELEASE(blob);

    return test_end();
}
