#include "precomp.h"

#define NDEBUG
#include <debug.h>


/* PRIVATE FUNCTIONS *********************************************************/

VOID
NTAPI
DisplayCharacter(
    _In_ CHAR Character,
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ ULONG TextColor,
    _In_ ULONG BackColor)
{

}


VOID
PrepareForSetPixel(VOID)
{

}

VOID
NTAPI
DoScroll(
    _In_ ULONG Scroll)
{

}

VOID
NTAPI
PreserveRow(
    _In_ ULONG CurrentTop,
    _In_ ULONG TopDelta,
    _In_ BOOLEAN Restore)
{

}

VOID
NTAPI
InitPaletteWithTable(
    _In_ PULONG Table,
    _In_ ULONG Count)
{
    UNIMPLEMENTED;
}

/* PUBLIC FUNCTIONS **********************************************************/

/*
 * @implemented
 */
BOOLEAN
NTAPI
VidInitialize(
    _In_ BOOLEAN SetMode)
{

    //
    // We are done!
    //
    return TRUE;
}

/*
 * @implemented
 */
VOID
NTAPI
VidResetDisplay(
    _In_ BOOLEAN HalReset)
{

}

/*
 * @implemented
 */
VOID
NTAPI
VidCleanUp(VOID)
{
    UNIMPLEMENTED;
    while (TRUE);
}

/*
 * @implemented
 */
VOID
NTAPI
VidScreenToBufferBlt(
    _Out_writes_bytes_(Delta * Height) PUCHAR Buffer,
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ ULONG Width,
    _In_ ULONG Height,
    _In_ ULONG Delta)
{
    UNIMPLEMENTED;
    while (TRUE);
}

/*
 * @implemented
 */
VOID
NTAPI
VidSolidColorFill(
    _In_ ULONG Left,
    _In_ ULONG Top,
    _In_ ULONG Right,
    _In_ ULONG Bottom,
    _In_ UCHAR Color)
{

}
