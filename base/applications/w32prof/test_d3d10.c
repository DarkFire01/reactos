/*
 * PROJECT:     ReactOS w32prof
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Direct3D10 availability / device-creation test
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 *
 * D3D10 is loaded dynamically rather than linked: ReactOS's PSDK ships d3d10.idl but does not
 * generate d3d10.h, and has no dxgi.idl at all, so there are no interface definitions to build
 * against. Everything used here is ABI-stable and documented - the two enum values, the SDK
 * version, and IUnknown::Release at vtable slot 2 - so nothing is guessed at.
 *
 * This reports where the D3D10 stack stops: whether the DLLs are present, whether the entry point
 * exists, and what device creation returns. Rendering a scene needs the full interface set and is
 * out of reach until the PSDK generates those headers.
 */

#include "profiler.h"
#include "fps.h"

#include <windows.h>
#include <tchar.h>

/* d3d10.h values (stable, documented). */
#define W32P_D3D10_DRIVER_TYPE_HARDWARE  0
#define W32P_D3D10_DRIVER_TYPE_REFERENCE 1
#define W32P_D3D10_DRIVER_TYPE_NULL      2
#define W32P_D3D10_DRIVER_TYPE_SOFTWARE  3
#define W32P_D3D10_DRIVER_TYPE_WARP      5
#define W32P_D3D10_SDK_VERSION           29

typedef HRESULT (WINAPI *PFN_D3D10CreateDevice)(void* pAdapter,
                                                UINT DriverType,
                                                HMODULE Software,
                                                UINT Flags,
                                                UINT SDKVersion,
                                                void** ppDevice);

typedef HRESULT (WINAPI *PFN_CreateDXGIFactory)(REFIID riid, void** ppFactory);

/* Release through the IUnknown slot every COM object shares. */
static void
ComRelease(void* pUnk)
{
    if (pUnk)
    {
        ULONG (WINAPI **vtbl)(void*) = *(ULONG (WINAPI ***)(void*))pUnk;
        vtbl[2](pUnk);
    }
}

static const TCHAR*
DriverTypeName(UINT type)
{
    switch (type)
    {
        case W32P_D3D10_DRIVER_TYPE_HARDWARE:  return TEXT("HARDWARE");
        case W32P_D3D10_DRIVER_TYPE_REFERENCE: return TEXT("REFERENCE");
        case W32P_D3D10_DRIVER_TYPE_NULL:      return TEXT("NULL");
        case W32P_D3D10_DRIVER_TYPE_SOFTWARE:  return TEXT("SOFTWARE");
        case W32P_D3D10_DRIVER_TYPE_WARP:      return TEXT("WARP");
        default:                               return TEXT("?");
    }
}

void
W32Prof_Test_D3D10Device(const ProfilerConfig* cfg)
{
    static const UINT DriverTypes[] =
    {
        W32P_D3D10_DRIVER_TYPE_HARDWARE,
        W32P_D3D10_DRIVER_TYPE_WARP,
        W32P_D3D10_DRIVER_TYPE_REFERENCE,
    };

    HMODULE hDxgi = NULL;
    HMODULE hD3D10 = NULL;
    PFN_D3D10CreateDevice pD3D10CreateDevice = NULL;
    PFN_CreateDXGIFactory pCreateDXGIFactory = NULL;
    UINT i;
    BOOL created = FALSE;

    UNREFERENCED_PARAMETER(cfg);

    /* dxgi first - d3d10.dll depends on it, and a missing dxgi shows up as a d3d10 load failure. */
    hDxgi = LoadLibrary(TEXT("dxgi.dll"));
    if (!hDxgi)
    {
        ResultsPrint(TEXT("D3D10: dxgi.dll failed to load (err %lu)"), GetLastError());
    }
    else
    {
        pCreateDXGIFactory = (PFN_CreateDXGIFactory)GetProcAddress(hDxgi, "CreateDXGIFactory");
        ResultsPrint(TEXT("D3D10: dxgi.dll loaded at %p, CreateDXGIFactory %s"),
                     hDxgi, pCreateDXGIFactory ? TEXT("present") : TEXT("MISSING"));
    }

    hD3D10 = LoadLibrary(TEXT("d3d10.dll"));
    if (!hD3D10)
    {
        ResultsPrint(TEXT("D3D10: d3d10.dll failed to load (err %lu)"), GetLastError());
        if (hDxgi)
            FreeLibrary(hDxgi);
        return;
    }
    ResultsPrint(TEXT("D3D10: d3d10.dll loaded at %p"), hD3D10);

    pD3D10CreateDevice = (PFN_D3D10CreateDevice)GetProcAddress(hD3D10, "D3D10CreateDevice");
    if (!pD3D10CreateDevice)
    {
        ResultsPrint(TEXT("D3D10: D3D10CreateDevice entry point MISSING (err %lu)"),
                     GetLastError());
        FreeLibrary(hD3D10);
        if (hDxgi)
            FreeLibrary(hDxgi);
        return;
    }

    /*
     * Try each driver type. HARDWARE is the one that exercises the WDDM UMD; the software types
     * tell us whether d3d10 itself works at all when the driver is taken out of the picture.
     */
    for (i = 0; i < ARRAYSIZE(DriverTypes); i++)
    {
        void* pDevice = NULL;
        HRESULT hr;
        LARGE_INTEGER t0, t1, freq;

        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        hr = pD3D10CreateDevice(NULL, DriverTypes[i], NULL, 0, W32P_D3D10_SDK_VERSION, &pDevice);
        QueryPerformanceCounter(&t1);

        if (SUCCEEDED(hr) && pDevice)
        {
            double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
            ResultsPrint(TEXT("D3D10: %s device created (%p) in %.3f ms"),
                         DriverTypeName(DriverTypes[i]), pDevice, ms);
            ComRelease(pDevice);
            created = TRUE;
        }
        else
        {
            ResultsPrint(TEXT("D3D10: %s device failed: 0x%08lx"),
                         DriverTypeName(DriverTypes[i]), (ULONG)hr);
        }
    }

    if (!created)
        ResultsPrint(TEXT("D3D10: no device could be created"));

    FreeLibrary(hD3D10);
    if (hDxgi)
        FreeLibrary(hDxgi);
}
