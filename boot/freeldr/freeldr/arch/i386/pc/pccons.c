/*
 *  FreeLoader
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <freeldr.h>
#include "../../vidfb.h"

extern VIDEODISPLAYMODE DisplayMode;

UCHAR MachDefaultTextColor = COLOR_GRAY;

/*static*/ unsigned CurrentCursorX = 0;
/*static*/ unsigned CurrentCursorY = 0;
static UCHAR CurrentAttr = ATTR(COLOR_GRAY, COLOR_BLACK);

/**
* TODO Consider this for console support:
All of the following TTY functions must be supported when this bit is set:
01 Set Cursor Size
02 Set Cursor Position
06 Scroll TTY window up or Blank Window
07 Scroll TTY window down or Blank Window
09 Write character and attribute at cursor position
0A Write character only at cursor position
0E Write character and advance cursor
*
* It may be this in bit 2 of the VESA ModeAttributes that tells us whether
* we can use the INT 10h for TTY operations, or use our own implementations.
**/

VOID
PcConsPutChar(int Ch)
{
    REGS Regs;

    /* If we are displaying a CR '\n' then do a LF also */
    if (Ch == '\n')
    {
        /* Display the LF */
        PcConsPutChar('\r');
    }

    /* If we are displaying a TAB '\t' then display 8 spaces ' ' */
    if (Ch == '\t')
    {
        /* Display the 8 spaces ' ' */
        PcConsPutChar(' ');
        PcConsPutChar(' ');
        PcConsPutChar(' ');
        PcConsPutChar(' ');
        PcConsPutChar(' ');
        PcConsPutChar(' ');
        PcConsPutChar(' ');
        PcConsPutChar(' ');
        return;
    }

    if (DisplayMode == VideoGraphicsMode)
    {
        ULONG Width, Height, Unused;
        BOOLEAN NeedScroll;

        PcVideoGetDisplaySize(&Width, &Height, &Unused);

        NeedScroll = (CurrentCursorY >= Height);
        if (NeedScroll)
        {
            FbConsScrollUp(CurrentAttr);
            --CurrentCursorY;
        }

        if (Ch == '\r')
        {
            CurrentCursorX = 0;
        }
        else if (Ch == '\n')
        {
            CurrentCursorX = 0;
            if (!NeedScroll)
                ++CurrentCursorY;
        }
        else if (Ch == '\t')
        {
            CurrentCursorX = (CurrentCursorX + 8) & ~ 7;
        }
        else
        {
            PcVideoPutChar(Ch, CurrentAttr, CurrentCursorX, CurrentCursorY);
            CurrentCursorX++;
        }

        if (CurrentCursorX >= Width)
        {
            CurrentCursorX = 0;
            CurrentCursorY++;
        }
        return;
    }
    // else, VideoTextMode

    /* Int 10h AH=0Eh
     * VIDEO - TELETYPE OUTPUT
     *
     * AH = 0Eh
     * AL = character to write
     * BH = page number
     * BL = foreground color (graphics modes only)
     */
    Regs.b.ah = 0x0E;
    Regs.b.al = Ch;
    Regs.w.bx = 1;
    Int386(0x10, &Regs, &Regs);
}

BOOLEAN
PcConsKbHit(VOID)
{
    REGS Regs;

    /* Int 16h AH=01h
     * KEYBOARD - CHECK FOR KEYSTROKE
     *
     * AH = 01h
     * Return:
     * ZF set if no keystroke available
     * ZF clear if keystroke available
     * AH = BIOS scan code
     * AL = ASCII character
     */
    Regs.b.ah = 0x01;
    Int386(0x16, &Regs, &Regs);

    return !(Regs.x.eflags & EFLAGS_ZF);
}

int
PcConsGetCh(void)
{
    REGS Regs;
    static BOOLEAN ExtendedKey = FALSE;
    static char ExtendedScanCode = 0;

    /* If the last time we were called an
     * extended key was pressed then return
     * that keys scan code. */
    if (ExtendedKey)
    {
        ExtendedKey = FALSE;
        return ExtendedScanCode;
    }

    /* Int 16h AH=00h
     * KEYBOARD - GET KEYSTROKE
     *
     * AH = 00h
     * Return:
     * AH = BIOS scan code
     * AL = ASCII character
     */
    Regs.b.ah = 0x00;
    Int386(0x16, &Regs, &Regs);

    /* Check for an extended keystroke */
    if (Regs.b.al == 0)
    {
        ExtendedKey = TRUE;
        ExtendedScanCode = Regs.b.ah;
    }

    /* Return keystroke */
    return Regs.b.al;
}

/* EOF */
