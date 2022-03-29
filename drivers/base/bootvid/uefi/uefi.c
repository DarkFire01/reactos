
/* PROJECT:     ReactOS Boot Video Driver for UEFI/Framebuffer devices
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     UEFI/Framebuffer-specific source file
 * COPYRIGHT:   Copyright 2022 Justin Miller <justinmiller100@gmail.com>
 */


BOOLEAN
NTAPI
VidInitializeUefi(
    _In_ BOOLEAN SetMode)
{
    return TRUE;
}

VOID
NTAPI
InitPaletteWithTableUefi(
    _In_ PULONG Table,
    _In_ ULONG Count)
{

}

VOID
PrepareForSetPixelUefi(VOID)
{
    NOTHING;
}

VOID
SetPixelUefi(
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ UCHAR Color)
{

}

VOID
NTAPI
PreserveRowUefi(
    _In_ ULONG CurrentTop,
    _In_ ULONG TopDelta,
    _In_ BOOLEAN Restore)
{

}

VOID
NTAPI
DoScrollUefi(
    _In_ ULONG Scroll)
{

}

VOID
NTAPI
DisplayCharacterUefi(
    _In_ CHAR Character,
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ ULONG TextColor,
    _In_ ULONG BackColor)
{

}
