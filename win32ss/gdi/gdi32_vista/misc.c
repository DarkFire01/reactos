#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H
#define NTOS_MODE_USER
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <windef.h>
#include <winbase.h>
#include <winreg.h>
#include <ndk/rtlfuncs.h>
#include <wingdi.h>
#include <winddi.h>
#include <prntfont.h>
#include <ntgdityp.h>
#include <ntgdi.h>
#include <devguid.h>
#include <winuser.h>
#include <setupapi.h>

#include <debug.h>

NTSTATUS WINAPI D3DKMTOpenAdapterFromGdiDisplayName( D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME *desc )
{
    __debugbreak();
#if 0
    WCHAR *end, key_nameW[MAX_PATH], bufferW[MAX_PATH];
    HDEVINFO devinfo = INVALID_HANDLE_VALUE;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    D3DKMT_OPENADAPTERFROMLUID luid_desc;
    SP_DEVINFO_DATA device_data;
    DWORD size, state_flags;
    DEVPROPTYPE type;
    int index;

   // TRACE("(%p)\n", desc);

    if (!desc)
        return STATUS_UNSUCCESSFUL;

   // TRACE("DeviceName: %s\n", wine_dbgstr_w( desc->DeviceName ));
    if (wcsnicmp( desc->DeviceName, L"\\\\.\\DISPLAY", lstrlenW(L"\\\\.\\DISPLAY") ))
        return STATUS_UNSUCCESSFUL;

    index = wcstol( desc->DeviceName + lstrlenW(L"\\\\.\\DISPLAY"), &end, 10 ) - 1;
    if (*end)
        return STATUS_UNSUCCESSFUL;

    size = sizeof( bufferW );
    swprintf( key_nameW,  L"\\Device\\Video%d", index );
    if (RegGetValueW( HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\VIDEO", key_nameW,
                      RRF_RT_REG_SZ, NULL, bufferW, &size ))
        goto done;

    /* Strip \Registry\Machine\ prefix and retrieve Wine specific data set by the display driver */
    lstrcpyW( key_nameW, bufferW + 18 );
    size = sizeof( state_flags );
    if (RegGetValueW( HKEY_CURRENT_CONFIG, key_nameW, L"StateFlags", RRF_RT_REG_DWORD, NULL,
                      &state_flags, &size ))
        goto done;

    if (!(state_flags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
        goto done;

    size = sizeof( bufferW );
    if (RegGetValueW( HKEY_CURRENT_CONFIG, key_nameW, L"GPUID", RRF_RT_REG_SZ, NULL, bufferW, &size ))
        goto done;

    devinfo = SetupDiCreateDeviceInfoList( &GUID_DEVCLASS_DISPLAY, NULL );
    device_data.cbSize = sizeof( device_data );
    SetupDiOpenDeviceInfoW( devinfo, bufferW, NULL, 0, &device_data );
    if (!SetupDiGetDevicePropertyW( devinfo, &device_data, &DEVPROPKEY_GPU_LUID, &type,
                                    (BYTE *)&luid_desc.AdapterLuid, sizeof( luid_desc.AdapterLuid ),
                                    NULL, 0))
        goto done;
    if ((status = NtGdiDdDDIOpenAdapterFromLuid( &luid_desc ))) goto done;

    desc->hAdapter = luid_desc.hAdapter;
    desc->AdapterLuid = luid_desc.AdapterLuid;
    desc->VidPnSourceId = index;

done:
    SetupDiDestroyDeviceInfoList( devinfo );
    return status;
#endif
    return 0;
}


/* Direct calls into win32k */
NTSTATUS WINAPI D3DKMTCheckVidPnExclusiveOwnership(const D3DKMT_CHECKVIDPNEXCLUSIVEOWNERSHIP *desc)
{
    return NtGdiDdDDICheckVidPnExclusiveOwnership(desc);
}

NTSTATUS WINAPI D3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER *desc)
{
    return NtGdiDdDDICloseAdapter(desc);
}
NTSTATUS WINAPI D3DKMTCreateDevice(D3DKMT_CREATEDEVICE *desc)
{
    return NtGdiDdDDICreateDevice(desc);
}
NTSTATUS WINAPI D3DKMTDestroyDevice(const D3DKMT_DESTROYDEVICE *desc)
{
    return NtGdiDdDDIDestroyDevice(desc);
}
NTSTATUS WINAPI D3DKMTEscape( const D3DKMT_ESCAPE *desc )
{
    return NtGdiDdDDIEscape(desc);
}
NTSTATUS WINAPI D3DKMTOpenAdapterFromHdc( D3DKMT_OPENADAPTERFROMHDC *desc )
{
    return NtGdiDdDDIOpenAdapterFromHdc(desc);
}
NTSTATUS WINAPI D3DKMTOpenAdapterFromLuid( D3DKMT_OPENADAPTERFROMLUID * desc )
{
    return NtGdiDdDDIOpenAdapterFromLuid(desc);
}
NTSTATUS WINAPI D3DKMTQueryStatistics(D3DKMT_QUERYSTATISTICS *stats)
{
    return NtGdiDdDDIQueryStatistics(stats);
}
NTSTATUS WINAPI D3DKMTQueryVideoMemoryInfo(D3DKMT_QUERYVIDEOMEMORYINFO *desc)
{
    return NtGdiDdDDIQueryVideoMemoryInfo(desc);
}
NTSTATUS WINAPI D3DKMTSetQueuedLimit(D3DKMT_SETQUEUEDLIMIT *desc)
{
    return NtGdiDdDDISetQueuedLimit(desc);
}
NTSTATUS WINAPI D3DKMTSetVidPnSourceOwner(const D3DKMT_SETVIDPNSOURCEOWNER *desc)
{
    return NtGdiDdDDISetVidPnSourceOwner(desc);
}