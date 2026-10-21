/*
 * PROJECT:     ReactOS Color Management System
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Display gamma of night light and GDI, sent to each monitor's color transform
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <stdarg.h>
#include <math.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winreg.h"
#include "winuser.h"
#include "winioctl.h"
#include "setupapi.h"
#include "ntddvdeo.h"

/* Both layers are kept for the session; a reboot starts the displays at their defaults */
#define GAMMA_STATE_KEY         L"Software\\ReactOS\\DisplayColor"
#define GAMMA_TEMPERATURE_VALUE L"Temperature"
#define GAMMA_GDI_RAMP_VALUE    L"GdiRamp"
#define GAMMA_LOCK_NAME         L"Local\\ReactOSDisplayColor"

#define GAMMA_NEUTRAL_KELVIN    6500
#define GAMMA_LOWEST_KELVIN     1000

/* The planar layout GDI ramps come in */
typedef struct _GAMMA_GDI_RAMP
{
    WORD Channel[3][256];
} GAMMA_GDI_RAMP;

static
VOID
GammaIdentityRamp(
    _Out_ GAMMA_GDI_RAMP *Ramp)
{
    UINT Entry;

    for (Entry = 0; Entry < 256; Entry++)
        Ramp->Channel[0][Entry] = Ramp->Channel[1][Entry] = Ramp->Channel[2][Entry] = (WORD)(Entry * 257);
}

static
double
GammaEncodeSrgb(
    _In_ double Linear)
{
    double Encoded = (Linear > 0.0031308) ? 1.055 * pow(Linear, 1.0 / 2.4) - 0.055 : Linear * 12.92;

    if (Encoded < 0.0)
        return 0.0;
    return (Encoded > 1.0) ? 1.0 : Encoded;
}

/*
 * The scale of each channel that makes white look like a light of the given temperature. Warmer
 * light is also made dimmer, and 6500 K and above leave the display as it is.
 */
static
VOID
GammaTemperatureGains(
    _In_ DWORD Kelvin,
    _Out_writes_(3) double *Gains)
{
    double T, Denominator, Uc, Vc, Xc, Yc, X, Y, Z;

    if (Kelvin >= GAMMA_NEUTRAL_KELVIN)
    {
        Gains[0] = Gains[1] = Gains[2] = 1.0;
        return;
    }
    T = (Kelvin < GAMMA_LOWEST_KELVIN) ? GAMMA_LOWEST_KELVIN : Kelvin;

    /* Krystek's rational fit of the Planckian locus, in CIE 1960 UCS */
    Uc = (0.860117757 + 1.54118254e-4 * T + 1.28641212e-7 * T * T) /
         (1.0 + 8.42420235e-4 * T + 7.08145163e-7 * T * T);
    Vc = (0.317398726 + 4.22806245e-5 * T + 4.20481691e-8 * T * T) /
         (1.0 - 2.89741816e-5 * T + 1.61456053e-7 * T * T);

    Denominator = 2.0 * Uc - 8.0 * Vc + 4.0;
    Xc = 3.0 * Uc / Denominator;
    Yc = 2.0 * Vc / Denominator;

    Y = 0.1244 + 0.00013946 * T;
    X = Xc * Y / Yc;
    Z = (1.0 - Xc - Yc) * Y / Yc;

    Gains[0] = GammaEncodeSrgb(3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z);
    Gains[1] = GammaEncodeSrgb(-0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z);
    Gains[2] = GammaEncodeSrgb(0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z);
}

static
HANDLE
GammaLock(VOID)
{
    HANDLE Lock = CreateMutexW(NULL, FALSE, GAMMA_LOCK_NAME);

    if (Lock)
        WaitForSingleObject(Lock, INFINITE);
    return Lock;
}

static
VOID
GammaUnlock(
    _In_opt_ HANDLE Lock)
{
    if (!Lock)
        return;
    ReleaseMutex(Lock);
    CloseHandle(Lock);
}

static
HKEY
GammaOpenState(VOID)
{
    HKEY Key;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, GAMMA_STATE_KEY, 0, NULL, REG_OPTION_VOLATILE,
                        KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &Key, NULL) != ERROR_SUCCESS)
    {
        return NULL;
    }
    return Key;
}

static
VOID
GammaReadState(
    _In_opt_ HKEY Key,
    _Out_ DWORD *Kelvin,
    _Out_ GAMMA_GDI_RAMP *GdiRamp)
{
    DWORD Size = sizeof(*Kelvin);

    if (!Key || RegQueryValueExW(Key, GAMMA_TEMPERATURE_VALUE, NULL, NULL, (BYTE *)Kelvin, &Size) != ERROR_SUCCESS)
        *Kelvin = GAMMA_NEUTRAL_KELVIN;

    Size = sizeof(*GdiRamp);
    if (!Key || RegQueryValueExW(Key, GAMMA_GDI_RAMP_VALUE, NULL, NULL, (BYTE *)GdiRamp, &Size) != ERROR_SUCCESS ||
        Size != sizeof(*GdiRamp))
    {
        GammaIdentityRamp(GdiRamp);
    }
}

/* Sends the ramp to one monitor; the expected wire format has to match its mode, which is not known here */
static
BOOL
GammaSendToMonitor(
    _In_ HANDLE Monitor,
    _Inout_ COLORSPACE_TRANSFORM_SET_INPUT *Input)
{
    static const UINT32 BitsPerChannel[] = { 8, 10, 6, 12, 14, 16 };
    DWORD Returned;
    UINT Index;

    for (Index = 0; Index < ARRAYSIZE(BitsPerChannel); Index++)
    {
        Input->OutputWireFormatExpected.BitsPerPixel = BitsPerChannel[Index];
        if (DeviceIoControl(Monitor, IOCTL_COLORSPACE_TRANSFORM_SET, Input, sizeof(*Input), NULL, 0, &Returned, NULL))
            return TRUE;
    }
    return FALSE;
}

/**
 * @brief
 * Opens the color transform of every monitor the display adapters report, sending it the ramp if one is given.
 *
 * @return
 * The number of monitors opened, or with a ramp, the number that took it.
 */
static
UINT
GammaForEachMonitor(
    _Inout_opt_ COLORSPACE_TRANSFORM_SET_INPUT *Input)
{
    SP_DEVINFO_DATA Device = { sizeof(Device) };
    WCHAR Pdo[MAX_PATH];
    WCHAR Path[MAX_PATH + 32];
    HDEVINFO Devices;
    HANDLE Monitor;
    DWORD Index;
    UINT Count = 0;

    Devices = SetupDiGetClassDevsW(NULL, L"DISPLAY", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (Devices != INVALID_HANDLE_VALUE)
    {
        for (Index = 0; SetupDiEnumDeviceInfo(Devices, Index, &Device); Index++)
        {
            if (!SetupDiGetDeviceRegistryPropertyW(Devices, &Device, SPDRP_PHYSICAL_DEVICE_OBJECT_NAME, NULL,
                                                   (BYTE *)Pdo, sizeof(Pdo), NULL))
            {
                continue;
            }

            lstrcpyW(Path, L"\\\\?\\GLOBALROOT");
            lstrcatW(Path, Pdo);
            lstrcatW(Path, L"\\color");
            Monitor = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                  OPEN_EXISTING, 0, NULL);
            if (Monitor == INVALID_HANDLE_VALUE)
                continue;
            if (!Input || GammaSendToMonitor(Monitor, Input))
                Count++;
            CloseHandle(Monitor);
        }
        SetupDiDestroyDeviceInfoList(Devices);
    }
    return Count;
}

static
BOOL
GammaApply(
    _In_ DWORD Kelvin,
    _In_ const GAMMA_GDI_RAMP *GdiRamp)
{
    COLORSPACE_TRANSFORM_SET_INPUT *Input;
    GAMMA_RAMP_RGB256x3x16 *Ramp;
    double Gains[3];
    UINT Entry;
    UINT Applied;

    Input = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Input));
    if (!Input)
        return FALSE;

    GammaTemperatureGains(Kelvin, Gains);
    Input->OutputWireColorSpaceExpected = OUTPUT_WIRE_COLOR_SPACE_G22_P709;
    Input->OutputWireFormatExpected.ColorEncoding = OUTPUT_COLOR_ENCODING_RGB;
    Input->ColorSpaceTransform.Type = COLORSPACE_TRANSFORM_TYPE_RGB256x3x16;
    Ramp = &Input->ColorSpaceTransform.Data.Rgb256x3x16;
    for (Entry = 0; Entry < 256; Entry++)
    {
        Ramp->Red[Entry] = (USHORT)floor(GdiRamp->Channel[0][Entry] * Gains[0] + 0.5);
        Ramp->Green[Entry] = (USHORT)floor(GdiRamp->Channel[1][Entry] * Gains[1] + 0.5);
        Ramp->Blue[Entry] = (USHORT)floor(GdiRamp->Channel[2][Entry] * Gains[2] + 0.5);
    }

    Applied = GammaForEachMonitor(Input);
    HeapFree(GetProcessHeap(), 0, Input);
    return Applied != 0;
}

/**
 * @brief
 * Tints the displays to a light of the given color temperature, on top of the GDI gamma ramp.
 *
 * @param[in] hdc
 * A display DC.
 *
 * @param[in] Temperature
 * The color temperature in Kelvin; 6500 and above is neutral.
 *
 * @return
 * TRUE if a display took the tint.
 */
BOOL
WINAPI
InternalSetDeviceTemperature(
    _In_ HDC hdc,
    _In_ float Temperature,
    _In_ INT Reserved1,
    _In_ INT Reserved2)
{
    GAMMA_GDI_RAMP GdiRamp;
    DWORD Kelvin;
    HANDLE Lock;
    HKEY Key;
    BOOL Result;

    UNREFERENCED_PARAMETER(Reserved1);
    UNREFERENCED_PARAMETER(Reserved2);

    if (!hdc)
        return FALSE;

    Lock = GammaLock();
    Key = GammaOpenState();
    GammaReadState(Key, &Kelvin, &GdiRamp);
    Kelvin = (Temperature < GAMMA_LOWEST_KELVIN) ? GAMMA_LOWEST_KELVIN : (DWORD)(Temperature + 0.5f);
    if (Key)
    {
        RegSetValueExW(Key, GAMMA_TEMPERATURE_VALUE, 0, REG_DWORD, (const BYTE *)&Kelvin, sizeof(Kelvin));
        RegCloseKey(Key);
    }
    Result = GammaApply(Kelvin, &GdiRamp);
    GammaUnlock(Lock);
    return Result;
}

/**
 * @brief
 * Tells whether the displays take gamma ramps through their color transform.
 */
BOOL
WINAPI
InternalGetDeviceGammaCapability(
    _In_ HDC hdc)
{
    if (!hdc)
        return FALSE;
    return GammaForEachMonitor(NULL) != 0;
}

/**
 * @brief
 * Sets the GDI gamma ramp of the displays, under the night light tint.
 *
 * @param[in] hdc
 * A display DC.
 *
 * @param[in] lpRamp
 * The red, green and blue ramps, 256 words each.
 *
 * @return
 * TRUE if a display took the ramp.
 */
BOOL
WINAPI
InternalSetDeviceGDIGammaRamp(
    _In_ HDC hdc,
    _In_ LPVOID lpRamp)
{
    GAMMA_GDI_RAMP GdiRamp;
    DWORD Kelvin;
    HANDLE Lock;
    HKEY Key;
    BOOL Result;

    if (!hdc || !lpRamp)
        return FALSE;

    Lock = GammaLock();
    Key = GammaOpenState();
    GammaReadState(Key, &Kelvin, &GdiRamp);
    CopyMemory(&GdiRamp, lpRamp, sizeof(GdiRamp));
    if (Key)
    {
        RegSetValueExW(Key, GAMMA_GDI_RAMP_VALUE, 0, REG_BINARY, (const BYTE *)&GdiRamp, sizeof(GdiRamp));
        RegCloseKey(Key);
    }
    Result = GammaApply(Kelvin, &GdiRamp);
    GammaUnlock(Lock);
    return Result;
}

/**
 * @brief
 * Gets the GDI gamma ramp last set on the displays, without the night light tint.
 */
BOOL
WINAPI
InternalGetAppliedGDIGammaRamp(
    _In_ HDC hdc,
    _Out_ LPVOID lpRamp)
{
    GAMMA_GDI_RAMP GdiRamp;
    DWORD Kelvin;
    HANDLE Lock;
    HKEY Key;

    if (!hdc || !lpRamp)
        return FALSE;

    Lock = GammaLock();
    Key = GammaOpenState();
    GammaReadState(Key, &Kelvin, &GdiRamp);
    if (Key)
        RegCloseKey(Key);
    GammaUnlock(Lock);

    CopyMemory(lpRamp, &GdiRamp, sizeof(GdiRamp));
    return TRUE;
}
