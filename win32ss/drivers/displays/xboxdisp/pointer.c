/*
 * PROJECT:     Xbox NV2A accelerated GDI display driver
 * LICENSE:     GPL-2.0-or-later
 * PURPOSE:     Software mouse pointer — delegated to Eng helpers
 */

#include "xboxdisp.h"

ULONG APIENTRY
DrvSetPointerShape(IN SURFOBJ *pso, IN SURFOBJ *psoMask, IN SURFOBJ *psoColor,
                   IN XLATEOBJ *pxlo, IN LONG xHot, IN LONG yHot,
                   IN LONG x, IN LONG y, IN RECTL *prcl, IN FLONG fl)
{
    return EngSetPointerShape(pso, psoMask, psoColor, pxlo,
                              xHot, yHot, x, y, prcl, fl);
}

VOID APIENTRY
DrvMovePointer(IN SURFOBJ *pso, IN LONG x, IN LONG y, IN RECTL *prcl)
{
    EngMovePointer(pso, x, y, prcl);
}
