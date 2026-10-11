/*
 * PROJECT:     FreeLoader
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Screen print routine handed to the kernel for early logging
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <freeldr.h>

/* BIOS Data Area: cursor position of video page 0 */
#define BDA_CURSOR_COLUMN   0x450
#define BDA_CURSOR_ROW      0x451

static ULONG ScreenLogColumn;
static ULONG ScreenLogRow;
static BOOLEAN ScreenLogStarted;

/**
 * @brief
 * Blanks one row of the screen.
 *
 * @param[in] Row
 * The row to blank.
 *
 * @param[in] Width
 * The screen width in characters.
 */
static
VOID
NTAPI
ScreenLogClearRow(
    _In_ ULONG Row,
    _In_ ULONG Width)
{
    ULONG Column;

    for (Column = 0; Column < Width; Column++)
        MachVideoPutChar(' ', ATTR(COLOR_GRAY, COLOR_BLACK), Column, Row);
}

/**
 * @brief
 * Moves to the start of the next row, wrapping to the top of the screen.
 *
 * @param[in] Width
 * The screen width in characters.
 *
 * @param[in] Height
 * The screen height in characters.
 */
static
VOID
NTAPI
ScreenLogNewLine(
    _In_ ULONG Width,
    _In_ ULONG Height)
{
    ScreenLogColumn = 0;
    ScreenLogRow = (ScreenLogRow + 1 < Height) ? (ScreenLogRow + 1) : 0;
    ScreenLogClearRow(ScreenLogRow, Width);
}

/**
 * @brief
 * printf to the screen without any firmware call.
 *
 * Characters go straight into video memory through MachVideoPutChar, so the
 * kernel can call this before it sets up its own display, for as long as the
 * loader image and video memory stay mapped. Output wraps to the top of the
 * screen instead of scrolling.
 *
 * @param[in] Format
 * A printf-style format string.
 *
 * @return
 * How many characters were formatted.
 */
ULONG
FrLdrScreenPrint(
    _In_ PCSTR Format,
    ...)
{
    CHAR Buffer[256];
    va_list Arguments;
    ULONG Width;
    ULONG Height;
    ULONG Depth;
    INT Length;
    INT Index;

    va_start(Arguments, Format);
    Length = _vsnprintf(Buffer, sizeof(Buffer), Format, Arguments);
    va_end(Arguments);

    if (Length < 0 || Length > (INT)sizeof(Buffer))
        Length = sizeof(Buffer);

    MachVideoGetDisplaySize(&Width, &Height, &Depth);
    if (Width == 0 || Height == 0)
        return 0;

    /* Carry on below whatever the loader printed last */
    if (!ScreenLogStarted)
    {
#ifndef UEFIBOOT
        ScreenLogColumn = *(PUCHAR)BDA_CURSOR_COLUMN;
        ScreenLogRow = *(PUCHAR)BDA_CURSOR_ROW;
#endif
        if (ScreenLogColumn >= Width || ScreenLogRow >= Height)
        {
            ScreenLogColumn = 0;
            ScreenLogRow = 0;
        }

        ScreenLogStarted = TRUE;
    }

    for (Index = 0; Index < Length; Index++)
    {
        if (Buffer[Index] == '\n')
        {
            ScreenLogNewLine(Width, Height);
            continue;
        }

        if (Buffer[Index] == '\r')
        {
            ScreenLogColumn = 0;
            continue;
        }

        MachVideoPutChar(Buffer[Index], ATTR(COLOR_WHITE, COLOR_BLACK), ScreenLogColumn, ScreenLogRow);
        if (++ScreenLogColumn >= Width)
            ScreenLogNewLine(Width, Height);
    }

    return (ULONG)Length;
}
