#include <precomp.h>

#define NDEBUG
#include <debug.h>

/*
 * @unimplemented
 */
BOOL
WINAPI
GetLogColorSpaceA(
    HCOLORSPACE		a0,
    LPLOGCOLORSPACEA	a1,
    DWORD			a2
)
{
    UNIMPLEMENTED;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}


/*
 * @unimplemented
 */
BOOL
WINAPI
GetLogColorSpaceW(
    HCOLORSPACE		a0,
    LPLOGCOLORSPACEW	a1,
    DWORD			a2
)
{
    UNIMPLEMENTED;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
CheckColorsInGamut(
    HDC	a0,
    LPVOID	a1,
    LPVOID	a2,
    DWORD	a3
)
{
    UNIMPLEMENTED;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}


typedef BOOL (WINAPI *PFN_GAMMA_CAPABILITY)(HDC);
typedef BOOL (WINAPI *PFN_GAMMA_RAMP)(HDC, LPVOID);

/* Displays with a color transform, as WDDM monitors have, take gamma ramps through mscms */
static
FARPROC
IntGetMscmsGammaProc(
    _In_ PCSTR Name)
{
    static HMODULE Mscms;

    if (!Mscms)
        Mscms = LoadLibraryW(L"mscms.dll");
    return Mscms ? GetProcAddress(Mscms, Name) : NULL;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetDeviceGammaRamp( HDC hdc,
                    LPVOID lpGammaRamp)
{
    PFN_GAMMA_CAPABILITY Capability;
    PFN_GAMMA_RAMP GetRamp;
    BOOL retValue = FALSE;
    if (lpGammaRamp == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
    }
    else
    {
        Capability = (PFN_GAMMA_CAPABILITY)IntGetMscmsGammaProc("InternalGetDeviceGammaCapability");
        GetRamp = (PFN_GAMMA_RAMP)IntGetMscmsGammaProc("InternalGetAppliedGDIGammaRamp");
        if (Capability && GetRamp && Capability(hdc))
            retValue = GetRamp(hdc, lpGammaRamp);
        else
            retValue = NtGdiGetDeviceGammaRamp(hdc,lpGammaRamp);
    }

    return retValue;
}

/*
 * @implemented
 */
BOOL
WINAPI
SetDeviceGammaRamp(HDC hdc,
                   LPVOID lpGammaRamp)
{
    PFN_GAMMA_RAMP SetRamp;
    BOOL retValue = FALSE;

    if (lpGammaRamp)
    {
        retValue = NtGdiSetDeviceGammaRamp(hdc, lpGammaRamp);
        if (!retValue)
        {
            SetRamp = (PFN_GAMMA_RAMP)IntGetMscmsGammaProc("InternalSetDeviceGDIGammaRamp");
            if (SetRamp)
                retValue = SetRamp(hdc, lpGammaRamp);
        }
    }
    else
    {
        SetLastError(ERROR_INVALID_PARAMETER);
    }

    return  retValue;
}


/*
 * @unimplemented
 */
BOOL
WINAPI
ColorMatchToTarget(
    HDC	a0,
    HDC	a1,
    DWORD	a2
)
{
    UNIMPLEMENTED;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
SetColorAdjustment(
    HDC			hdc,
    CONST COLORADJUSTMENT	*a1
)
{
    UNIMPLEMENTED;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}
