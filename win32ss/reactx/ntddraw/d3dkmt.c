/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * PURPOSE:          Native DirectDraw implementation
 * FILE:             win32ss/reactx/ntddraw/d3dkmt.c
 * PROGRAMER:        Sebastian Gasiorek (sebastian.gasiorek@reactos.com)
 */

#include <win32k.h>

DWORD
APIENTRY
NtGdiDdDDICreateDCFromMemory(D3DKMT_CREATEDCFROMMEMORY *desc)
{
    D3DKMT_CREATEDCFROMMEMORY info;
    PALETTEENTRY aPalEntries[256];
    PPALETTE ppal = NULL;
    PSURFACE psurf = NULL;
    HANDLE hSecure = NULL;
    HDC hDC = NULL;
    HBITMAP hBitmap;
    ULONG cjBits;

    const struct d3dddi_format_info
    {
        D3DDDIFORMAT format;
        unsigned int bit_count;
        DWORD compression;
        unsigned int palette_size;
        DWORD mask_r, mask_g, mask_b;
    } *format = NULL;
    unsigned int i;

    static const struct d3dddi_format_info format_info[] =
    {
        { D3DDDIFMT_R8G8B8,   24, BI_RGB,       0,   0x00ff0000, 0x0000ff00, 0x000000ff },
        { D3DDDIFMT_A8R8G8B8, 32, BI_RGB,       0,   0x00ff0000, 0x0000ff00, 0x000000ff },
        { D3DDDIFMT_X8R8G8B8, 32, BI_RGB,       0,   0x00ff0000, 0x0000ff00, 0x000000ff },
        { D3DDDIFMT_R5G6B5,   16, BI_BITFIELDS, 0,   0x0000f800, 0x000007e0, 0x0000001f },
        { D3DDDIFMT_X1R5G5B5, 16, BI_BITFIELDS, 0,   0x00007c00, 0x000003e0, 0x0000001f },
        { D3DDDIFMT_A1R5G5B5, 16, BI_BITFIELDS, 0,   0x00007c00, 0x000003e0, 0x0000001f },
        { D3DDDIFMT_A4R4G4B4, 16, BI_BITFIELDS, 0,   0x00000f00, 0x000000f0, 0x0000000f },
        { D3DDDIFMT_X4R4G4B4, 16, BI_BITFIELDS, 0,   0x00000f00, 0x000000f0, 0x0000000f },
        { D3DDDIFMT_P8,       8,  BI_RGB,       256, 0x00000000, 0x00000000, 0x00000000 },
    };

    if (!desc)
        return STATUS_INVALID_PARAMETER;

    /* Capture the request from user mode */
    _SEH2_TRY
    {
        ProbeForWrite(desc, sizeof(*desc), 1);
        info = *desc;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return STATUS_INVALID_PARAMETER);
    }
    _SEH2_END;

    if (!info.pMemory)
        return STATUS_INVALID_PARAMETER;

    for (i = 0; i < sizeof(format_info) / sizeof(*format_info); ++i)
    {
        if (format_info[i].format == info.Format)
        {
            format = &format_info[i];
            break;
        }
    }

    if (!format)
        return STATUS_INVALID_PARAMETER;

    if (info.Width > (UINT_MAX & ~3) / (format->bit_count / 8) ||
        !info.Pitch || info.Pitch < (((info.Width * format->bit_count + 31) >> 3) & ~3) ||
        !info.Height || info.Height > UINT_MAX / info.Pitch)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Capture the color table, if the format is paletted and one was given */
    if (format->palette_size && info.pColorTable)
    {
        _SEH2_TRY
        {
            ProbeForRead(info.pColorTable, format->palette_size * sizeof(PALETTEENTRY), 1);
            RtlCopyMemory(aPalEntries, info.pColorTable, format->palette_size * sizeof(PALETTEENTRY));
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            _SEH2_YIELD(return STATUS_INVALID_PARAMETER);
        }
        _SEH2_END;
    }

    cjBits = info.Pitch * info.Height;
    hSecure = EngSecureMem(info.pMemory, cjBits);
    if (!hSecure)
        return STATUS_INVALID_PARAMETER;

    if (!info.hDeviceDc || !(hDC = NtGdiCreateCompatibleDC(info.hDeviceDc)))
    {
        EngUnsecureMem(hSecure);
        return STATUS_INVALID_PARAMETER;
    }

    psurf = SURFACE_AllocSurface(STYPE_BITMAP,
                                 info.Width,
                                 info.Height,
                                 BitmapFormat(format->bit_count, format->compression),
                                 BMF_TOPDOWN | BMF_NOZEROINIT,
                                 info.Pitch,
                                 cjBits,
                                 info.pMemory);
    if (!psurf)
    {
        NtGdiDeleteObjectApp(hDC);
        EngUnsecureMem(hSecure);
        return STATUS_NO_MEMORY;
    }

    /* Mark as API bitmap. The secured range is released when the DC pair is
     * destroyed, see NtGdiDdDDIDestroyDCFromMemory. */
    psurf->flags |= (DDB_SURFACE | API_BITMAP);
    psurf->hSecure = hSecure;

    hBitmap = (HBITMAP)psurf->SurfObj.hsurf;

    /* Give the surface a palette matching the format */
    if (format->palette_size)
        ppal = PALETTE_AllocPalette(PAL_INDEXED, format->palette_size,
                                    info.pColorTable ? aPalEntries : NULL, 0, 0, 0);
    else
        ppal = PALETTE_AllocPalette(PAL_BITFIELDS, 0, NULL,
                                    format->mask_r, format->mask_g, format->mask_b);
    if (ppal)
    {
        SURFACE_vSetPalette(psurf, ppal);
        PALETTE_ShareUnlockPalette(ppal);
    }

    /* Unlock the surface */
    SURFACE_UnlockSurface(psurf);

    if (!NtGdiSelectBitmap(hDC, hBitmap))
    {
        NtGdiDeleteObjectApp(hBitmap);
        NtGdiDeleteObjectApp(hDC);
        EngUnsecureMem(hSecure);
        return STATUS_INVALID_PARAMETER;
    }

    /* Return the handles to user mode */
    _SEH2_TRY
    {
        desc->hDc = hDC;
        desc->hBitmap = hBitmap;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        NtGdiDeleteObjectApp(hDC);
        NtGdiDeleteObjectApp(hBitmap);
        EngUnsecureMem(hSecure);
        _SEH2_YIELD(return STATUS_INVALID_PARAMETER);
    }
    _SEH2_END;

    return STATUS_SUCCESS;
}

DWORD
APIENTRY
NtGdiDdDDIDestroyDCFromMemory(const D3DKMT_DESTROYDCFROMMEMORY *desc)
{
    D3DKMT_DESTROYDCFROMMEMORY info;
    HANDLE hSecure = NULL;
    PSURFACE psurf;

    if (!desc)
        return STATUS_INVALID_PARAMETER;

    /* Capture the request from user mode */
    _SEH2_TRY
    {
        ProbeForRead(desc, sizeof(*desc), 1);
        info = *desc;
    }
    _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
    {
        _SEH2_YIELD(return STATUS_INVALID_PARAMETER);
    }
    _SEH2_END;

    if (GDI_HANDLE_GET_TYPE(info.hDc) != GDI_OBJECT_TYPE_DC ||
        GDI_HANDLE_GET_TYPE(info.hBitmap) != GDI_OBJECT_TYPE_BITMAP)
        return STATUS_INVALID_PARAMETER;

    /* Grab the secured memory handle before the surface goes away */
    psurf = SURFACE_ShareLockSurface(info.hBitmap);
    if (psurf)
    {
        hSecure = psurf->hSecure;
        psurf->hSecure = NULL;
        SURFACE_ShareUnlockSurface(psurf);
    }

    /* Delete the DC first, so the bitmap is no longer selected into it */
    NtGdiDeleteObjectApp(info.hDc);
    NtGdiDeleteObjectApp(info.hBitmap);

    if (hSecure)
        EngUnsecureMem(hSecure);

    return STATUS_SUCCESS;
}
