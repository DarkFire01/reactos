/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * PURPOSE:          Native driver for dxg implementation
 * FILE:             win32ss/reactx/dxg/eng.c
 * PROGRAMER:        Magnus olsen (magnus@greatlord.com)
 * REVISION HISTORY:
 *       30/12-2007   Magnus Olsen
 */

#include <dxg_int.h>

PDD_SURFACE_LOCAL
NTAPI
DxDdLockDirectDrawSurface(HANDLE hDdSurface)
{
    PEDD_SURFACE SurfaceObj;
    PDD_SURFACE_LOCAL SurfaceLocal;

    SurfaceObj = (PEDD_SURFACE)DdHmgLock(hDdSurface, ObjType_DDSURFACE_TYPE, FALSE);
    if (SurfaceObj != NULL)
    {
        SurfaceLocal = &SurfaceObj->ddsSurfaceLocal;
    }
    else
    {
        SurfaceLocal = NULL;
    }

    return SurfaceLocal;
}

BOOL
NTAPI
DxDdUnlockDirectDrawSurface(PDD_SURFACE_LOCAL pSurface)
{
    PEDD_SURFACE SurfaceObj;

    if (!pSurface)
    {
        return FALSE;
    }

    /* Get base EDD_SURFACE from ddsSurfaceLocal member pointer */
    /* ddsSurfaceLocal is the second member after pobj, so offset is sizeof(DD_BASEOBJECT) */
    SurfaceObj = (PEDD_SURFACE)((PBYTE)pSurface - sizeof(DD_BASEOBJECT));

    /* Decrement lock count that was incremented by DdHmgLock */
    InterlockedDecrement((VOID*)&SurfaceObj->pobj.cExclusiveLock);

    return TRUE;
}
