/*
 * PROJECT:     ReactOS Webcam Viewer
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Display video from webcams connected via USB Video (UVC)
 * COPYRIGHT:   Copyright 2026 ReactOS Contributors
 */

#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <wingdi.h>
#include <winioctl.h>
#include <winreg.h>
#include <mmreg.h>
#include <ndk/umtypes.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <setjmp.h>
#include <ks.h>
#include <ksmedia.h>
#include <commctrl.h>
#include <reactos/libs/libjpeg/jpeglib.h>
#pragma comment(lib, "ole32.lib")

/* IPicture GUID */
const IID IID_IPicture = {0x7BF80980, 0xBF32, 0x101A, {0x8B, 0xBB, 0x00, 0xAA, 0x00, 0x30, 0x0C, 0xAB}};
#include <debug.h>

#define WM_FRAME_UPDATE (WM_USER + 1)

/* Debug output macro */
#define DEBUG_PRINT(fmt, ...) \
    do { \
        char __buf[512]; \
        sprintf(__buf, fmt, ##__VA_ARGS__); \
        OutputDebugStringA(__buf); \
        OutputDebugStringA("\n"); \
    } while (0)
#define DEBUG_VERBOSE 0  /* Set to 1 for verbose logging */

/* Define video capture GUIDs */
const GUID KSCATEGORY_CAPTURE = {STATIC_KSCATEGORY_CAPTURE};
const GUID KSCATEGORY_VIDEO = {0x6994AD05, 0x93EF, 0x11D0, {0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};
const GUID KSPROPSETID_Pin = {STATIC_KSPROPSETID_Pin};
const GUID KSPROPSETID_Connection = {0x1D58C920L, 0xAC9B, 0x11CF, {0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00}};
const GUID KSINTERFACESETID_Standard = {0x1A8766A0L, 0x62CE, 0x11CF, {0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00}};
const GUID KSMEDIUMSETID_Standard = {0x4747B320L, 0x62CE, 0x11CF, {0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00}};
const GUID KSDATAFORMAT_TYPE_VIDEO = {STATIC_KSDATAFORMAT_TYPE_VIDEO};
const GUID KSDATAFORMAT_SPECIFIER_VIDEOINFO = {0x05589f80, 0xc356, 0x11ce, {0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a}};
const GUID KSDATAFORMAT_SPECIFIER_VIDEOINFO2 = {0xf72a76a0, 0xeb0a, 0x11d0, {0xac, 0xe4, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba}};
const GUID MEDIASUBTYPE_YUY2 = {0x32595559, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID MEDIASUBTYPE_MJPG = {0x47504a4d, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

typedef struct _KS_DATAFORMAT_VIDEOINFOHEADER2
{
    KSDATAFORMAT DataFormat;
    KS_VIDEOINFOHEADER2 VideoInfoHeader;
} KS_DATAFORMAT_VIDEOINFOHEADER2, *PKS_DATAFORMAT_VIDEOINFOHEADER2;

typedef struct {
    HWND hwndMain;
    HWND hwndStatus;
    HWND hwndList;
    HWND hwndPreview;
    HWND hwndVideoModes;     /* Video mode dropdown */
    HANDLE hFilter;
    HANDLE hPin;
    BOOL bIsStreaming;
    DWORD dwWidth;
    DWORD dwHeight;
    GUID dtSubtype;
    GUID dtSpecifier;
    UINT_PTR uTimerId;
    BYTE* pFrameBuf;
    DWORD dwFrameBufSize;
    BYTE* pDisplayBuf;
    DWORD dwDisplayBufSize;
    BITMAPINFO bmi;
    BOOL bFrameReady;
} APP_STATE;

APP_STATE g_AppState = { 0 };

/* Forward declarations */
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
BOOL OpenDevice(int iDeviceIndex);
VOID CloseDevice(void);
VOID UpdateStatus(const char* format, ...);

VOID UpdateStatus(const char* format, ...)
{
    char szBuffer[256];
    va_list args;
    
    va_start(args, format);
    vsprintf(szBuffer, format, args);
    va_end(args);
    
    if (g_AppState.hwndStatus)
        SetWindowTextA(g_AppState.hwndStatus, szBuffer);
    
    DEBUG_PRINT("[STATUS] %s", szBuffer);
}

static BOOL ks_prop_get(HANDLE h, const GUID* Set, ULONG Id, ULONG PinId, void* Out, DWORD OutCb)
{
    KSP_PIN prop;
    DWORD bytes = 0;
    
    prop.Property.Set = *Set;
    prop.Property.Id = Id;
    prop.Property.Flags = KSPROPERTY_TYPE_GET;
    prop.PinId = PinId;
    prop.Reserved = 0;
    
    return DeviceIoControl(h, IOCTL_KS_PROPERTY,
                          &prop, sizeof(prop),
                          Out, OutCb,
                          &bytes, NULL);
}

static BOOL set_pin_state(HANDLE hPin, KSSTATE State)
{
    DWORD bytes = 0;
    KSPROPERTY prop;
    KSSTATE outState = State;
    
    prop.Set = KSPROPSETID_Connection;
    prop.Id = KSPROPERTY_CONNECTION_STATE;
    prop.Flags = KSPROPERTY_TYPE_SET;
    
    return DeviceIoControl(hPin, IOCTL_KS_PROPERTY,
                          &prop, sizeof(prop),
                          &outState, sizeof(outState),
                          &bytes, NULL);
}

static BOOL get_pin_dataranges(HANDLE hFilter, ULONG pinId, BYTE** OutBuf, DWORD* OutCb)
{
    KSP_PIN prop;
    DWORD bytes = 0;
    DWORD cb = 256 * 1024;
    BYTE* buf;
    
    if (!OutBuf || !OutCb)
        return FALSE;
    
    *OutBuf = NULL;
    *OutCb = 0;
    
    buf = (BYTE*)malloc(cb);
    if (!buf)
        return FALSE;
    
    prop.Property.Set = KSPROPSETID_Pin;
    prop.Property.Id = KSPROPERTY_PIN_DATARANGES;
    prop.Property.Flags = KSPROPERTY_TYPE_GET;
    prop.PinId = pinId;
    prop.Reserved = 0;
    
    if (!DeviceIoControl(hFilter, IOCTL_KS_PROPERTY, &prop, sizeof(prop), buf, cb, &bytes, NULL))
    {
        free(buf);
        return FALSE;
    }
    
    *OutBuf = buf;
    *OutCb = bytes;
    return TRUE;
}

static BOOL get_pin_interfaces(HANDLE hFilter, ULONG pinId, BYTE** OutBuf, DWORD* OutCb)
{
    KSP_PIN prop;
    DWORD bytes = 0;
    DWORD cb = 64 * 1024;
    BYTE* buf;
    
    if (!OutBuf || !OutCb)
        return FALSE;
    *OutBuf = NULL;
    *OutCb = 0;
    
    buf = (BYTE*)malloc(cb);
    if (!buf)
        return FALSE;
    
    prop.Property.Set = KSPROPSETID_Pin;
    prop.Property.Id = KSPROPERTY_PIN_INTERFACES;
    prop.Property.Flags = KSPROPERTY_TYPE_GET;
    prop.PinId = pinId;
    prop.Reserved = 0;
    
    if (!DeviceIoControl(hFilter, IOCTL_KS_PROPERTY, &prop, sizeof(prop), buf, cb, &bytes, NULL))
    {
        free(buf);
        return FALSE;
    }
    
    *OutBuf = buf;
    *OutCb = bytes;
    return TRUE;
}

static BOOL get_pin_mediums(HANDLE hFilter, ULONG pinId, BYTE** OutBuf, DWORD* OutCb)
{
    KSP_PIN prop;
    DWORD bytes = 0;
    DWORD cb = 64 * 1024;
    BYTE* buf;
    
    if (!OutBuf || !OutCb)
        return FALSE;
    *OutBuf = NULL;
    *OutCb = 0;
    
    buf = (BYTE*)malloc(cb);
    if (!buf)
        return FALSE;
    
    prop.Property.Set = KSPROPSETID_Pin;
    prop.Property.Id = KSPROPERTY_PIN_MEDIUMS;
    prop.Property.Flags = KSPROPERTY_TYPE_GET;
    prop.PinId = pinId;
    prop.Reserved = 0;
    
    if (!DeviceIoControl(hFilter, IOCTL_KS_PROPERTY, &prop, sizeof(prop), buf, cb, &bytes, NULL))
    {
        free(buf);
        return FALSE;
    }
    
    *OutBuf = buf;
    *OutCb = bytes;
    return TRUE;
}

static BOOL guid_is_equal(const GUID* a, const GUID* b)
{
    return IsEqualGUID(a, b);
}

static BOOL pick_video_format_from_dataranges(const BYTE* Buf, DWORD Cb, GUID* OutSubtype, GUID* OutSpecifier)
{
    const KSMULTIPLE_ITEM* mi;
    const BYTE* p;
    ULONG i;
    
    if (!Buf || Cb < sizeof(KSMULTIPLE_ITEM) || !OutSubtype || !OutSpecifier)
        return FALSE;
    
    mi = (const KSMULTIPLE_ITEM*)Buf;
    if (mi->Size < sizeof(KSMULTIPLE_ITEM) || mi->Size > Cb)
        return FALSE;
    
    p = Buf + sizeof(KSMULTIPLE_ITEM);
    
    /* FIRST PASS: Prefer YUY2 specifically (best for raw pixel access) */
    for (i = 0; i < mi->Count; ++i)
    {
        const KSDATARANGE* dr = (const KSDATARANGE*)p;
        if ((DWORD)(p - Buf) + sizeof(KSDATARANGE) > Cb)
            break;
        if (dr->FormatSize < sizeof(KSDATARANGE) || (DWORD)(p - Buf) + dr->FormatSize > Cb)
            break;
        if (guid_is_equal(&dr->MajorFormat, &KSDATAFORMAT_TYPE_VIDEO))
        {
            if (guid_is_equal(&dr->SubFormat, &MEDIASUBTYPE_YUY2))
            {
                DEBUG_PRINT("[DEBUG] Found YUY2 format (preferred)");
                *OutSubtype = dr->SubFormat;
                *OutSpecifier = dr->Specifier;
                return TRUE;
            }
        }
        p += dr->FormatSize;
    }
    
    /* SECOND PASS: Fall back to MJPG if YUY2 not available */
    p = Buf + sizeof(KSMULTIPLE_ITEM);
    for (i = 0; i < mi->Count; ++i)
    {
        const KSDATARANGE* dr = (const KSDATARANGE*)p;
        if ((DWORD)(p - Buf) + sizeof(KSDATARANGE) > Cb)
            break;
        if (dr->FormatSize < sizeof(KSDATARANGE) || (DWORD)(p - Buf) + dr->FormatSize > Cb)
            break;
        if (guid_is_equal(&dr->MajorFormat, &KSDATAFORMAT_TYPE_VIDEO))
        {
            if (guid_is_equal(&dr->SubFormat, &MEDIASUBTYPE_MJPG))
            {
                DEBUG_PRINT("[DEBUG] Found MJPEG format (fallback)");
                *OutSubtype = dr->SubFormat;
                *OutSpecifier = dr->Specifier;
                return TRUE;
            }
        }
        p += dr->FormatSize;
    }
    
    /* THIRD PASS: Take any video format as last resort */
    p = Buf + sizeof(KSMULTIPLE_ITEM);
    for (i = 0; i < mi->Count; ++i)
    {
        const KSDATARANGE* dr = (const KSDATARANGE*)p;
        if ((DWORD)(p - Buf) + sizeof(KSDATARANGE) > Cb)
            break;
        if (dr->FormatSize < sizeof(KSDATARANGE) || (DWORD)(p - Buf) + dr->FormatSize > Cb)
            break;
        if (guid_is_equal(&dr->MajorFormat, &KSDATAFORMAT_TYPE_VIDEO))
        {
            *OutSubtype = dr->SubFormat;
            *OutSpecifier = dr->Specifier;
            return TRUE;
        }
        p += dr->FormatSize;
    }
    
    return FALSE;
}

static BOOL build_pin_format_from_dataranges(const BYTE* Buf, DWORD Cb, const GUID* ioSubtype, const GUID* outSpecifier,
                                              DWORD Width, DWORD Height, DWORD Fps,
                                              BYTE* outFormatBuf, DWORD outFormatCb, DWORD* outFormatUsed)
{
    const KSMULTIPLE_ITEM* mi;
    const BYTE* p;
    ULONG i;
    GUID spec = *outSpecifier;
    
    if (!Buf || Cb < sizeof(KSMULTIPLE_ITEM) || !outFormatBuf || !outFormatUsed)
        return FALSE;
    
    mi = (const KSMULTIPLE_ITEM*)Buf;
    if (mi->Size < sizeof(KSMULTIPLE_ITEM) || mi->Size > Cb)
        return FALSE;
    
    p = Buf + sizeof(KSMULTIPLE_ITEM);
    
    for (i = 0; i < mi->Count; ++i)
    {
        const KSDATARANGE* dr = (const KSDATARANGE*)p;
        if ((DWORD)(p - Buf) + sizeof(KSDATARANGE) > Cb)
            break;
        if (dr->FormatSize < sizeof(KSDATARANGE) || (DWORD)(p - Buf) + dr->FormatSize > Cb)
            break;
        if (!guid_is_equal(&dr->MajorFormat, &KSDATAFORMAT_TYPE_VIDEO))
        {
            p += dr->FormatSize;
            continue;
        }
        if (!guid_is_equal(&dr->SubFormat, ioSubtype))
        {
            p += dr->FormatSize;
            continue;
        }
        
        /* Found matching format - use device's template */
        if (guid_is_equal(&spec, &KSDATAFORMAT_SPECIFIER_VIDEOINFO2))
        {
            const KS_DATARANGE_VIDEO2* drv = (const KS_DATARANGE_VIDEO2*)dr;
            KS_DATAFORMAT_VIDEOINFOHEADER2* fmt = (KS_DATAFORMAT_VIDEOINFOHEADER2*)outFormatBuf;
            
            if (outFormatCb < sizeof(*fmt))
                return FALSE;
            
            ZeroMemory(fmt, sizeof(*fmt));
            
            /* Copy from device's template */
            fmt->DataFormat.FormatSize = sizeof(*fmt);
            fmt->DataFormat.Flags = 0;
            fmt->DataFormat.MajorFormat = drv->DataRange.MajorFormat;
            fmt->DataFormat.SubFormat = *ioSubtype;
            fmt->DataFormat.Specifier = spec;
            fmt->DataFormat.SampleSize = 0;
            
            /* Copy video info header template from device */
            fmt->VideoInfoHeader = drv->VideoInfoHeader;
            
            /* Override with requested parameters */
            fmt->VideoInfoHeader.bmiHeader.biWidth = Width;
            fmt->VideoInfoHeader.bmiHeader.biHeight = Height;
            fmt->VideoInfoHeader.bmiHeader.biPlanes = 1;
            fmt->VideoInfoHeader.rcSource.right = Width;
            fmt->VideoInfoHeader.rcSource.bottom = Height;
            fmt->VideoInfoHeader.rcTarget = fmt->VideoInfoHeader.rcSource;
            fmt->VideoInfoHeader.AvgTimePerFrame = 10000000 / (Fps > 0 ? Fps : 30);
            
            if (guid_is_equal(ioSubtype, &MEDIASUBTYPE_YUY2))
            {
                fmt->VideoInfoHeader.bmiHeader.biCompression = 0x32595559;
                fmt->VideoInfoHeader.bmiHeader.biBitCount = 16;
                fmt->VideoInfoHeader.bmiHeader.biSizeImage = Width * Height * 2;
                fmt->DataFormat.SampleSize = Width * Height * 2;
            }
            
            DEBUG_PRINT("[DEBUG] Using VIDEOINFO2 template: width=%lu, height=%lu", 
                       (unsigned long)fmt->VideoInfoHeader.bmiHeader.biWidth,
                       (unsigned long)fmt->VideoInfoHeader.bmiHeader.biHeight);
            
            *outFormatUsed = sizeof(*fmt);
            return TRUE;
        }
        else if (guid_is_equal(&spec, &KSDATAFORMAT_SPECIFIER_VIDEOINFO))
        {
            const KS_DATARANGE_VIDEO* drv = (const KS_DATARANGE_VIDEO*)dr;
            KS_DATAFORMAT_VIDEOINFOHEADER* fmt = (KS_DATAFORMAT_VIDEOINFOHEADER*)outFormatBuf;
            
            if (outFormatCb < sizeof(*fmt))
                return FALSE;
            
            ZeroMemory(fmt, sizeof(*fmt));
            
            /* Copy from device's template */
            fmt->DataFormat.FormatSize = sizeof(*fmt);
            fmt->DataFormat.Flags = 0;
            fmt->DataFormat.MajorFormat = drv->DataRange.MajorFormat;
            fmt->DataFormat.SubFormat = *ioSubtype;
            fmt->DataFormat.Specifier = spec;
            fmt->DataFormat.SampleSize = 0;
            
            /* Copy video info header template from device */
            fmt->VideoInfoHeader = drv->VideoInfoHeader;
            
            /* Override with requested parameters */
            fmt->VideoInfoHeader.bmiHeader.biWidth = Width;
            fmt->VideoInfoHeader.bmiHeader.biHeight = Height;
            fmt->VideoInfoHeader.bmiHeader.biPlanes = 1;
            fmt->VideoInfoHeader.rcSource.right = Width;
            fmt->VideoInfoHeader.rcSource.bottom = Height;
            fmt->VideoInfoHeader.rcTarget = fmt->VideoInfoHeader.rcSource;
            fmt->VideoInfoHeader.AvgTimePerFrame = 10000000 / (Fps > 0 ? Fps : 30);
            
            if (guid_is_equal(ioSubtype, &MEDIASUBTYPE_YUY2))
            {
                fmt->VideoInfoHeader.bmiHeader.biCompression = 0x32595559;
                fmt->VideoInfoHeader.bmiHeader.biBitCount = 16;
                fmt->VideoInfoHeader.bmiHeader.biSizeImage = Width * Height * 2;
                fmt->DataFormat.SampleSize = Width * Height * 2;
            }
            
            DEBUG_PRINT("[DEBUG] Using VIDEOINFO template: width=%lu, height=%lu", 
                       (unsigned long)fmt->VideoInfoHeader.bmiHeader.biWidth,
                       (unsigned long)fmt->VideoInfoHeader.bmiHeader.biHeight);
            
            *outFormatUsed = sizeof(*fmt);
            return TRUE;
        }
        
        p += dr->FormatSize;
    }
    
    DEBUG_PRINT("[DEBUG] No matching format found in dataranges");
    return FALSE;
}

static BOOL try_create_pin(HANDLE hFilter, ULONG pinId, const KSPIN_INTERFACE* iface,
                          const KSPIN_MEDIUM* medium, const void* format, DWORD formatCb, HANDLE* outPin)
{
    DWORD reqCb;
    BYTE* reqBuf;
    KSPIN_CONNECT* connect;
    DWORD st;
    
    if (!iface || !medium || !format || formatCb == 0 || !outPin)
        return FALSE;
    
    reqCb = (DWORD)(sizeof(KSPIN_CONNECT) + formatCb);
    reqBuf = (BYTE*)malloc(reqCb);
    if (!reqBuf)
        return FALSE;
    
    ZeroMemory(reqBuf, reqCb);
    connect = (KSPIN_CONNECT*)reqBuf;
    
    connect->Interface = *iface;
    connect->Medium = *medium;
    connect->PinId = pinId;
    connect->PinToHandle = NULL;
    connect->Priority.PriorityClass = KSPRIORITY_NORMAL;
    connect->Priority.PrioritySubClass = 1;
    
    memcpy(reqBuf + sizeof(KSPIN_CONNECT), format, formatCb);
    
    DEBUG_PRINT("[DEBUG] KsCreatePin: pinId=%lu, formatCb=%lu, totalCb=%lu", pinId, formatCb, reqCb);
    
    *outPin = NULL;
    st = (DWORD)KsCreatePin(hFilter, connect, GENERIC_READ | GENERIC_WRITE, outPin);
    
    DEBUG_PRINT("[DEBUG] KsCreatePin returned: 0x%lx, handle=%p", st, *outPin);
    
    free(reqBuf);
    
    return (st == 0 && *outPin && *outPin != INVALID_HANDLE_VALUE);
}

VOID QueryPinInfo(HANDLE hKsDevice)
{
    DEBUG_PRINT("[DEBUG] QueryPinInfo called");
}

VOID PaintPreview(HWND hwnd, HDC hdc)
{
    RECT rc;
    
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
}

BOOL EnumerateWebcams(HWND hwndList)
{
    HDEVINFO hDevInfo;
    SP_DEVICE_INTERFACE_DATA ifData;
    DWORD dwIndex = 0;
    DWORD dwRequiredSize;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W pDetail;
    SP_DEVINFO_DATA devInfoData;
    
    DEBUG_PRINT("[DEBUG] Starting webcam enumeration");
    
    SendMessageW(hwndList, LB_RESETCONTENT, 0, 0);
    
    /* Try KSCATEGORY_VIDEO first */
    hDevInfo = SetupDiGetClassDevsW(&KSCATEGORY_VIDEO, NULL, NULL,
                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE)
    {
        /* Fallback to KSCATEGORY_CAPTURE */
        hDevInfo = SetupDiGetClassDevsW(&KSCATEGORY_CAPTURE, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    }
    
    if (hDevInfo == INVALID_HANDLE_VALUE)
    {
        UpdateStatus("Failed to enumerate video devices");
        return FALSE;
    }
    
    ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    dwIndex = 0;
    
    while (SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &KSCATEGORY_VIDEO, dwIndex, &ifData) ||
           (dwIndex == 0 && SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &KSCATEGORY_CAPTURE, dwIndex, &ifData)))
    {
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, NULL, 0, &dwRequiredSize, NULL);
        
        pDetail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)LocalAlloc(LMEM_FIXED, dwRequiredSize);
        if (!pDetail)
        {
            dwIndex++;
            continue;
        }
        
        pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
        
        if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, pDetail, dwRequiredSize, &dwRequiredSize, &devInfoData))
        {
            HANDLE hTest = CreateFileW(pDetail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hTest != INVALID_HANDLE_VALUE)
            {
                ULONG ctypes = 0;
                if (ks_prop_get(hTest, &KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES, 0, &ctypes, sizeof(ctypes)))
                {
                    WCHAR nameBuf[256] = {0};
                    SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData, SPDRP_FRIENDLYNAME,
                                                      NULL, (PBYTE)nameBuf, sizeof(nameBuf), NULL);
                    if (!nameBuf[0])
                        SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData, SPDRP_DEVICEDESC,
                                                          NULL, (PBYTE)nameBuf, sizeof(nameBuf), NULL);
                    
                    SendMessageW(hwndList, LB_ADDSTRING, 0, (LPARAM)(nameBuf[0] ? nameBuf : pDetail->DevicePath));
                    DEBUG_PRINT("[DEBUG] Found device: %ls (%lu pins)", nameBuf[0] ? nameBuf : L"(unnamed)", ctypes);
                }
                CloseHandle(hTest);
            }
        }
        
        LocalFree(pDetail);
        dwIndex++;
    }
    
    SetupDiDestroyDeviceInfoList(hDevInfo);
    
    if (dwIndex == 0)
    {
        UpdateStatus("No video capture devices found");
        return FALSE;
    }
    
    SendMessageW(hwndList, LB_SETCURSEL, 0, 0);
    return TRUE;
}

BOOL OpenDevice(int iDeviceIndex)
{
    HDEVINFO hDevInfo;
    SP_DEVICE_INTERFACE_DATA ifData;
    DWORD dwIndex = 0;
    DWORD dwRequiredSize;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W pDetail;
    SP_DEVINFO_DATA devInfoData;
    HANDLE hFilter = INVALID_HANDLE_VALUE;
    ULONG ctypes = 0;
    ULONG pinId;
    
    DEBUG_PRINT("[DEBUG] Opening device %d", iDeviceIndex);
    
    CloseDevice();
    
    hDevInfo = SetupDiGetClassDevsW(&KSCATEGORY_VIDEO, NULL, NULL,
                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE)
    {
        hDevInfo = SetupDiGetClassDevsW(&KSCATEGORY_CAPTURE, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    }
    
    if (hDevInfo == INVALID_HANDLE_VALUE)
    {
        UpdateStatus("Failed to enumerate devices");
        return FALSE;
    }
    
    ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    dwIndex = 0;
    
    while (SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &KSCATEGORY_VIDEO, dwIndex, &ifData) ||
           (dwIndex == 0 && SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &KSCATEGORY_CAPTURE, dwIndex, &ifData)))
    {
        if (dwIndex == iDeviceIndex)
        {
            SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, NULL, 0, &dwRequiredSize, NULL);
            pDetail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)LocalAlloc(LMEM_FIXED, dwRequiredSize);
            if (!pDetail)
                break;
            
            pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
            
            if (!SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, pDetail, dwRequiredSize, &dwRequiredSize, &devInfoData))
            {
                LocalFree(pDetail);
                break;
            }
            
            hFilter = CreateFileW(pDetail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            LocalFree(pDetail);
            break;
        }
        dwIndex++;
    }
    
    SetupDiDestroyDeviceInfoList(hDevInfo);
    
    if (hFilter == INVALID_HANDLE_VALUE)
    {
        UpdateStatus("Failed to open device");
        return FALSE;
    }
    
    if (!ks_prop_get(hFilter, &KSPROPSETID_Pin, KSPROPERTY_PIN_CTYPES, 0, &ctypes, sizeof(ctypes)))
    {
        UpdateStatus("Device doesn't support KS property queries");
        CloseHandle(hFilter);
        return FALSE;
    }
    
    DEBUG_PRINT("[DEBUG] Device has %lu pins", ctypes);
    
    /* Find a video output pin */
    for (pinId = 0; pinId < ctypes; ++pinId)
    {
        KSPIN_DATAFLOW dataflow = 0;
        KSPIN_COMMUNICATION comm = 0;
        BYTE* drBuf = NULL;
        DWORD drCb = 0;
        BYTE* ifBuf = NULL;
        DWORD ifCb = 0;
        BYTE* medBuf = NULL;
        DWORD medCb = 0;
        GUID subtype = MEDIASUBTYPE_YUY2;
        GUID specifier = {0};
        
        DEBUG_PRINT("[DEBUG] Checking pin %lu", pinId);
        
        if (!ks_prop_get(hFilter, &KSPROPSETID_Pin, KSPROPERTY_PIN_DATAFLOW, pinId, &dataflow, sizeof(dataflow)))
            continue;
        if (!ks_prop_get(hFilter, &KSPROPSETID_Pin, KSPROPERTY_PIN_COMMUNICATION, pinId, &comm, sizeof(comm)))
            continue;
        
        if (dataflow != KSPIN_DATAFLOW_OUT)
            continue;
        if (comm != KSPIN_COMMUNICATION_SOURCE && comm != KSPIN_COMMUNICATION_BOTH)
            continue;
        
        if (!get_pin_dataranges(hFilter, pinId, &drBuf, &drCb))
            continue;
        
        if (!pick_video_format_from_dataranges(drBuf, drCb, &subtype, &specifier))
        {
            free(drBuf);
            continue;
        }
        
        if (!get_pin_interfaces(hFilter, pinId, &ifBuf, &ifCb))
        {
            free(drBuf);
            continue;
        }
        
        if (!get_pin_mediums(hFilter, pinId, &medBuf, &medCb))
        {
            free(drBuf);
            free(ifBuf);
            continue;
        }
        
        {
            const KSMULTIPLE_ITEM* ifMi = (const KSMULTIPLE_ITEM*)ifBuf;
            const KSMULTIPLE_ITEM* medMi = (const KSMULTIPLE_ITEM*)medBuf;
            const KSPIN_INTERFACE* ifList = (const KSPIN_INTERFACE*)(ifBuf + sizeof(KSMULTIPLE_ITEM));
            const KSPIN_MEDIUM* medList = (const KSPIN_MEDIUM*)(medBuf + sizeof(KSMULTIPLE_ITEM));
            ULONG ii, mi;
            BYTE formatBuf[sizeof(KS_DATAFORMAT_VIDEOINFOHEADER2)];
            DWORD formatUsed = 0;
            
            DEBUG_PRINT("[DEBUG] Pin %lu: trying %lu interfaces x %lu mediums", pinId, (unsigned long)ifMi->Count, (unsigned long)medMi->Count);
            
            for (ii = 0; ii < ifMi->Count; ++ii)
            {
                for (mi = 0; mi < medMi->Count; ++mi)
                {
                    DEBUG_PRINT("[DEBUG] Pin %lu: trying interface %lu, medium %lu", pinId, ii, mi);
                    
                    /* Try multiple format variants: no override, size override, size+fps override */
                    if ((build_pin_format_from_dataranges(drBuf, drCb, &subtype, &specifier,
                                                          640, 480, 30,
                                                          formatBuf, sizeof(formatBuf), &formatUsed) &&
                         try_create_pin(hFilter, pinId, &ifList[ii], &medList[mi], formatBuf, formatUsed, &g_AppState.hPin)) ||
                        (build_pin_format_from_dataranges(drBuf, drCb, &subtype, &specifier,
                                                          640, 480, 30,
                                                          formatBuf, sizeof(formatBuf), &formatUsed) &&
                         try_create_pin(hFilter, pinId, &ifList[ii], &medList[mi], formatBuf, formatUsed, &g_AppState.hPin)))
                    {
                        DEBUG_PRINT("[DEBUG] Pin %lu: KsCreatePin succeeded", pinId);
                        g_AppState.hFilter = hFilter;
                        g_AppState.dtSubtype = subtype;
                        g_AppState.dtSpecifier = specifier;
                        g_AppState.dwWidth = 640;
                        g_AppState.dwHeight = 480;
                        
                        /* Log negotiated format */
                        if (guid_is_equal(&subtype, &MEDIASUBTYPE_YUY2))
                        {
                            DEBUG_PRINT("[DEBUG] Format negotiated: YUY2 (0x32595559)");
                            UpdateStatus("Format: YUY2");
                        }
                        else if (guid_is_equal(&subtype, &MEDIASUBTYPE_MJPG))
                        {
                            DEBUG_PRINT("[DEBUG] Format negotiated: MJPEG (0x47504a4d)");
                            UpdateStatus("Format: MJPEG");
                        }
                        else
                        {
                            DEBUG_PRINT("[DEBUG] Format negotiated: Unknown (%08lx)", subtype.Data1);
                            UpdateStatus("Format: Unknown (0x%08lx)", subtype.Data1);
                        }
                        
                        /* Allocate frame buffer for streaming */
                        g_AppState.dwFrameBufSize = 640 * 480 * 2 + 1024;
                        g_AppState.pFrameBuf = (BYTE*)malloc(g_AppState.dwFrameBufSize);
                        if (!g_AppState.pFrameBuf)
                        {
                            UpdateStatus("Failed to allocate frame buffer");
                            CloseDevice();
                            return FALSE;
                        }
                        
                        /* Allocate display buffer (BGRA32) */
                        g_AppState.dwDisplayBufSize = 640 * 480 * 4;
                        g_AppState.pDisplayBuf = (BYTE*)malloc(g_AppState.dwDisplayBufSize);
                        if (!g_AppState.pDisplayBuf)
                        {
                            UpdateStatus("Failed to allocate display buffer");
                            CloseDevice();
                            return FALSE;
                        }
                        
                        /* Initialize BITMAPINFO for display */
                        ZeroMemory(&g_AppState.bmi, sizeof(g_AppState.bmi));
                        g_AppState.bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                        g_AppState.bmi.bmiHeader.biWidth = 640;
                        g_AppState.bmi.bmiHeader.biHeight = -480;  /* Negative for top-down */
                        g_AppState.bmi.bmiHeader.biPlanes = 1;
                        g_AppState.bmi.bmiHeader.biBitCount = 32;
                        g_AppState.bmi.bmiHeader.biCompression = BI_RGB;
                        
                        free(drBuf);
                        free(ifBuf);
                        free(medBuf);
                        
                        UpdateStatus("Device opened - starting streaming");
                        DEBUG_PRINT("[DEBUG] Pin created successfully");
                        return TRUE;
                    }
                    else
                    {
                        DEBUG_PRINT("[DEBUG] Pin %lu: KsCreatePin failed", pinId);
                    }
                }
            }
            
            free(drBuf);
            free(ifBuf);
            free(medBuf);
        }
    }
    
    UpdateStatus("Failed to create streaming pin");
    CloseHandle(hFilter);
    return FALSE;
}

VOID CloseDevice(void)
{
    if (g_AppState.hPin && g_AppState.hPin != INVALID_HANDLE_VALUE)
    {
        set_pin_state(g_AppState.hPin, KSSTATE_STOP);
        CloseHandle(g_AppState.hPin);
        g_AppState.hPin = NULL;
    }
    
    if (g_AppState.hFilter && g_AppState.hFilter != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_AppState.hFilter);
        g_AppState.hFilter = NULL;
    }
    
    if (g_AppState.pFrameBuf)
    {
        free(g_AppState.pFrameBuf);
        g_AppState.pFrameBuf = NULL;
        g_AppState.dwFrameBufSize = 0;
    }
    
    if (g_AppState.pDisplayBuf)
    {
        free(g_AppState.pDisplayBuf);
        g_AppState.pDisplayBuf = NULL;
        g_AppState.dwDisplayBufSize = 0;
    }
}

/* Simple YUY2 to BGR24 conversion */
static void yuy2_to_rgb32(const BYTE* yuy2, int w, int h, BYTE* rgb32)
{
    int i, j;
    const BYTE* src = yuy2;
    BYTE* dst = rgb32;
    
    for (i = 0; i < h; i++)
    {
        for (j = 0; j < w; j += 2)
        {
            int y1 = *src++;
            int u = *src++;
            int y2 = *src++;
            int v = *src++;
            
            /* YUV to RGB conversion */
            int c1 = y1 - 16;
            int c2 = y2 - 16;
            int d = u - 128;
            int e = v - 128;
            
            int r1 = (298 * c1 + 409 * e + 128) >> 8;
            int g1 = (298 * c1 - 100 * d - 208 * e + 128) >> 8;
            int b1 = (298 * c1 + 516 * d + 128) >> 8;
            
            int r2 = (298 * c2 + 409 * e + 128) >> 8;
            int g2 = (298 * c2 - 100 * d - 208 * e + 128) >> 8;
            int b2 = (298 * c2 + 516 * d + 128) >> 8;
            
            /* Clamp to 0-255 */
            r1 = (r1 < 0) ? 0 : (r1 > 255) ? 255 : r1;
            g1 = (g1 < 0) ? 0 : (g1 > 255) ? 255 : g1;
            b1 = (b1 < 0) ? 0 : (b1 > 255) ? 255 : b1;
            r2 = (r2 < 0) ? 0 : (r2 > 255) ? 255 : r2;
            g2 = (g2 < 0) ? 0 : (g2 > 255) ? 255 : g2;
            b2 = (b2 < 0) ? 0 : (b2 > 255) ? 255 : b2;
            
            /* Store as BGRA */
            *dst++ = b1;
            *dst++ = g1;
            *dst++ = r1;
            *dst++ = 0xFF;
            
            *dst++ = b2;
            *dst++ = g2;
            *dst++ = r2;
            *dst++ = 0xFF;
        }
    }
}

/* Decode JPEG to BGRA32 using IPicture interface */
/* JPEG error handler for libjpeg */
struct jpeg_error_handler
{
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void jpeg_error_exit(j_common_ptr cinfo)
{
    struct jpeg_error_handler *myerr = (struct jpeg_error_handler *)cinfo->err;
    longjmp(myerr->setjmp_buffer, 1);
}

/* Memory data source for libjpeg */
typedef struct
{
    struct jpeg_source_mgr pub;
    const BYTE* data;
    size_t size;
    size_t pos;
} mem_source_mgr;

static void init_source(j_decompress_ptr cinfo)
{
    mem_source_mgr* src = (mem_source_mgr*)cinfo->src;
    src->pos = 0;
}

static boolean fill_input_buffer(j_decompress_ptr cinfo)
{
    mem_source_mgr* src = (mem_source_mgr*)cinfo->src;
    if (src->pos >= src->size)
        return FALSE;
    src->pub.next_input_byte = src->data + src->pos;
    src->pub.bytes_in_buffer = src->size - src->pos;
    src->pos = src->size;
    return TRUE;
}

static void skip_input_data(j_decompress_ptr cinfo, long num_bytes)
{
    mem_source_mgr* src = (mem_source_mgr*)cinfo->src;
    if (num_bytes > 0)
    {
        src->pub.next_input_byte += num_bytes;
        src->pub.bytes_in_buffer -= num_bytes;
    }
}

static void term_source(j_decompress_ptr cinfo)
{
}

static BOOL jpeg_to_bgra32(const BYTE* jpegData, DWORD jpegSize, BYTE* bgra32, int w, int h)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_handler jerr;
    mem_source_mgr src;
    JSAMPARRAY buffer;
    int row_stride;
    int x, y;
    BYTE* dst;
    
    DEBUG_PRINT("[APP] Decoding JPEG: %u bytes, target %dx%d", jpegSize, w, h);
    
    /* Set up error handler */
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    
    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        DEBUG_PRINT("[APP] JPEG decode error");
        return FALSE;
    }
    
    /* Create decompressor */
    jpeg_create_decompress(&cinfo);
    
    /* Set up memory source */
    ZeroMemory(&src, sizeof(src));
    src.pub.init_source = init_source;
    src.pub.fill_input_buffer = fill_input_buffer;
    src.pub.skip_input_data = skip_input_data;
    src.pub.resync_to_restart = jpeg_resync_to_restart;
    src.pub.term_source = term_source;
    src.pub.next_input_byte = jpegData;
    src.pub.bytes_in_buffer = jpegSize;
    src.data = jpegData;
    src.size = jpegSize;
    src.pos = 0;
    
    cinfo.src = (struct jpeg_source_mgr *)&src;
    
    /* Read header */
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        DEBUG_PRINT("[APP] JPEG header read failed");
        return FALSE;
    }
    
    /* Set output format to RGB */
    cinfo.out_color_space = JCS_RGB;
    
    /* Start decompression */
    jpeg_start_decompress(&cinfo);
    
    DEBUG_PRINT("[APP] JPEG image: %u x %u, components: %u", 
               cinfo.output_width, cinfo.output_height, cinfo.output_components);
    
    /* Allocate scanline buffer */
    row_stride = cinfo.output_width * cinfo.output_components;
    buffer = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);
    
    dst = bgra32;
    
    /* Read and convert scanlines */
    while (cinfo.output_scanline < cinfo.output_height && cinfo.output_scanline < (UINT)h)
    {
        BYTE *src_row;
        int x;
        
        jpeg_read_scanlines(&cinfo, buffer, 1);
        src_row = buffer[0];
        
        /* Convert RGB to BGRA32 */
        for (x = 0; x < w && x < (int)cinfo.output_width; x++)
        {
            BYTE r = src_row[x * 3 + 0];
            BYTE g = src_row[x * 3 + 1];
            BYTE b = src_row[x * 3 + 2];
            
            *dst++ = b;  /* B */
            *dst++ = g;  /* G */
            *dst++ = r;  /* R */
            *dst++ = 0xFF;  /* A */
        }
        
        /* Pad remaining columns with black */
        for (; x < w; x++)
        {
            *dst++ = 0;
            *dst++ = 0;
            *dst++ = 0;
            *dst++ = 0xFF;
        }
    }
    
    /* Fill remaining rows with black */
    for (y = cinfo.output_scanline; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            *dst++ = 0;
            *dst++ = 0;
            *dst++ = 0;
            *dst++ = 0xFF;
        }
    }
    
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    
    DEBUG_PRINT("[APP] JPEG decoded successfully");
    return TRUE;
}

LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            
            RECT rc;
            GetClientRect(hwnd, &rc);
            
            /* Display the frame or show status */
            if (g_AppState.pDisplayBuf && g_AppState.bFrameReady && g_AppState.dwWidth > 0 && g_AppState.dwHeight > 0)
            {
                /* Display decoded video frame (both MJPEG and YUY2 are in BGRA32 format) */
                StretchDIBits(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                             0, 0, g_AppState.dwWidth, g_AppState.dwHeight,
                             g_AppState.pDisplayBuf, &g_AppState.bmi, DIB_RGB_COLORS, SRCCOPY);
            }
            else
            {
                /* Show placeholder */
                FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
                SetTextColor(hdc, RGB(128, 128, 128));
                DrawTextW(hdc, L"Waiting for video...", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            
            EndPaint(hwnd, &ps);
            break;
        }
        
        case WM_ERASEBKGND:
            return 1;  /* Don't erase background to avoid flicker */
        
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            HWND hwndButton;
            WNDCLASSW wcPreview;
            
            DEBUG_PRINT("[DEBUG] Creating main window");
            
            /* Register preview window class */
            ZeroMemory(&wcPreview, sizeof(wcPreview));
            wcPreview.lpfnWndProc = PreviewWndProc;
            wcPreview.hInstance = GetModuleHandle(NULL);
            wcPreview.lpszClassName = L"WebcamPreviewWindow";
            wcPreview.hCursor = LoadCursorW(NULL, IDC_ARROW);
            wcPreview.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
            RegisterClassW(&wcPreview);
            
            /* Create video preview area with custom class */
            g_AppState.hwndPreview = CreateWindowW(
                L"WebcamPreviewWindow",
                L"Video Preview",
                WS_CHILD | WS_VISIBLE,
                8, 8, 480, 360,
                hwnd, (HMENU)1001, GetModuleHandle(NULL), NULL);
            
            /* Create device list */
            g_AppState.hwndList = CreateWindowW(
                L"LISTBOX",
                NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER,
                495, 8, 295, 130,
                hwnd, (HMENU)2000, GetModuleHandle(NULL), NULL);
            
            /* Create Refresh button */
            hwndButton = CreateWindowW(
                L"BUTTON",
                L"Refresh Devices",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                495, 142, 140, 25,
                hwnd, (HMENU)101, GetModuleHandle(NULL), NULL);
            
            /* Create Open button */
            hwndButton = CreateWindowW(
                L"BUTTON",
                L"Open Device",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                650, 142, 140, 25,
                hwnd, (HMENU)102, GetModuleHandle(NULL), NULL);
            
            /* Create video mode label */
            hwndButton = CreateWindowW(
                L"STATIC",
                L"Video Mode:",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                495, 175, 290, 16,
                hwnd, (HMENU)3000, GetModuleHandle(NULL), NULL);
            
            /* Create video mode dropdown */
            g_AppState.hwndVideoModes = CreateWindowW(
                L"COMBOBOX",
                NULL,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                495, 195, 290, 20,
                hwnd, (HMENU)2001, GetModuleHandle(NULL), NULL);
            
            /* Add default video mode text */
            SendMessageW(g_AppState.hwndVideoModes, CB_ADDSTRING, 0, (LPARAM)L"640x480 (Default)");
            SendMessageW(g_AppState.hwndVideoModes, CB_SETCURSEL, 0, 0);
            
            /* Create Set Mode button */
            hwndButton = CreateWindowW(
                L"BUTTON",
                L"Set Video Mode",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                495, 220, 290, 25,
                hwnd, (HMENU)104, GetModuleHandle(NULL), NULL);
            
            /* Create status display */
            g_AppState.hwndStatus = CreateWindowW(
                L"STATIC",
                L"Ready",
                WS_CHILD | WS_VISIBLE | SS_LEFT | WS_BORDER,
                0, 375, 800, 20,
                hwnd, (HMENU)1000, GetModuleHandle(NULL), NULL);
            
            g_AppState.hwndMain = hwnd;
            DEBUG_PRINT("[DEBUG] Main window created successfully");
            EnumerateWebcams(g_AppState.hwndList);
            break;
        }
        
        case WM_COMMAND:
        {
            int wID = LOWORD(wParam);
            
            switch (wID)
            {
                case 101:  /* Refresh */
                    EnumerateWebcams(g_AppState.hwndList);
                    break;
                
                case 102:  /* Open Device */
                {
                    int iSel = SendMessageW(g_AppState.hwndList, LB_GETCURSEL, 0, 0);
                    if (iSel != LB_ERR)
                    {
                        if (OpenDevice(iSel))
                        {
                            UpdateStatus("Device opened - streaming ready");
                            /* Start a timer to capture frames */
                            g_AppState.uTimerId = SetTimer(hwnd, 1, 100, NULL);
                        }
                    }
                    break;
                }
                
                case 103:  /* Close */
                    if (g_AppState.uTimerId)
                    {
                        KillTimer(hwnd, (UINT_PTR)g_AppState.uTimerId);
                        g_AppState.uTimerId = 0;
                    }
                    CloseDevice();
                    UpdateStatus("Device closed");
                    break;
                
                case 104:  /* Set Video Mode */
                    if (g_AppState.hPin && g_AppState.hPin != INVALID_HANDLE_VALUE)
                    {
                        int iMode = SendMessageW(g_AppState.hwndVideoModes, CB_GETCURSEL, 0, 0);
                        if (iMode != CB_ERR)
                        {
                            UpdateStatus("Video mode change not yet implemented");
                        }
                    }
                    else
                    {
                        UpdateStatus("Please open a device first");
                    }
                    break;
            }
            break;
        }
        
        case WM_TIMER:
        {
            /* Timer for frame capture */
            if (g_AppState.hPin && g_AppState.hPin != INVALID_HANDLE_VALUE && g_AppState.pFrameBuf)
            {
                KSSTREAM_HEADER sh;
                DWORD got = 0;
                
                /* Set pin state to RUN if not already */
                static BOOL bStreaming = FALSE;
                if (!bStreaming)
                {
                    set_pin_state(g_AppState.hPin, KSSTATE_ACQUIRE);
                    set_pin_state(g_AppState.hPin, KSSTATE_PAUSE);
                    set_pin_state(g_AppState.hPin, KSSTATE_RUN);
                    bStreaming = TRUE;
                }
                
                ZeroMemory(&sh, sizeof(sh));
                sh.Size = sizeof(KSSTREAM_HEADER);
                sh.TypeSpecificFlags = 0;
                sh.FrameExtent = g_AppState.dwFrameBufSize;
                sh.DataUsed = 0;
                sh.Data = g_AppState.pFrameBuf;
                
                if (DeviceIoControl(g_AppState.hPin, IOCTL_KS_READ_STREAM,
                                   NULL, 0,
                                   &sh, sizeof(sh),
                                   &got, NULL))
                {
                    /* Check if frame was actually filled */
                    if (sh.DataUsed > 0)
                    {
                        /* Check if this is JPEG data (ff d8 ff = JPEG SOI) */
                        if (sh.DataUsed > 3 && g_AppState.pFrameBuf[0] == 0xFF && g_AppState.pFrameBuf[1] == 0xD8 && g_AppState.pFrameBuf[2] == 0xFF)
                        {
                            /* MJPEG frame - decode to BGRA32 */
                            if (g_AppState.pDisplayBuf)
                            {
                                if (jpeg_to_bgra32(g_AppState.pFrameBuf, sh.DataUsed, g_AppState.pDisplayBuf, 
                                                  g_AppState.dwWidth, g_AppState.dwHeight))
                                {
                                    g_AppState.bFrameReady = TRUE;
                                    UpdateStatus("MJPEG: %lu bytes", sh.DataUsed);
                                    InvalidateRect(g_AppState.hwndPreview, NULL, FALSE);
                                }
                                else
                                {
                                    UpdateStatus("MJPEG decode error");
                                }
                            }
                        }
                        else if (g_AppState.pDisplayBuf)
                        {
                            /* Convert YUY2 to BGRA32 */
                            yuy2_to_rgb32(g_AppState.pFrameBuf, g_AppState.dwWidth, g_AppState.dwHeight, g_AppState.pDisplayBuf);
                            g_AppState.bFrameReady = TRUE;
                            UpdateStatus("YUY2: %lu bytes", sh.DataUsed);
                            InvalidateRect(g_AppState.hwndPreview, NULL, FALSE);
                        }
                    }
                }
                else
                {
                    DWORD dwErr = GetLastError();
                    if (dwErr != ERROR_TIMEOUT && dwErr != ERROR_INVALID_HANDLE)
                    {
                        UpdateStatus("Read failed: 0x%lx", dwErr);
                    }
                }
            }
            break;
        }
        
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            UNREFERENCED_PARAMETER(hdc);
            EndPaint(hwnd, &ps);
            break;
        }
        
        case WM_SIZE:
        {
            int dwWidth = (int)(short)LOWORD(lParam);
            int dwHeight = (int)(short)HIWORD(lParam);
            
            /* Resize preview window */
            if (g_AppState.hwndPreview)
            {
                MoveWindow(g_AppState.hwndPreview, 8, 8, dwWidth - 313, dwHeight - 60, TRUE);
            }
            
            /* Resize device list */
            if (g_AppState.hwndList)
            {
                MoveWindow(g_AppState.hwndList, dwWidth - 305, 8, 295, 130, TRUE);
            }
            
            /* Resize status bar */
            if (g_AppState.hwndStatus)
            {
                MoveWindow(g_AppState.hwndStatus, 0, dwHeight - 20, dwWidth, 20, TRUE);
            }
            break;
        }
        
        case WM_DESTROY:
        {
            DEBUG_PRINT("[DEBUG] Destroying main window");
            if (g_AppState.uTimerId)
            {
                KillTimer(hwnd, (UINT_PTR)g_AppState.uTimerId);
                g_AppState.uTimerId = 0;
            }
            CloseDevice();
            PostQuitMessage(0);
            break;
        }
        
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
    WNDCLASSW wc = {0};
    HWND hwnd;
    MSG msg;
    
    DEBUG_PRINT("[DEBUG] === ReactOS Webcam Viewer Started ===");
    
    InitCommonControls();
    
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"WebcamViewerWindow";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    
    if (!RegisterClassW(&wc))
    {
        DEBUG_PRINT("[ERROR] RegisterClassW failed");
        MessageBoxW(NULL, L"Failed to register window class", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    DEBUG_PRINT("[DEBUG] Window class registered successfully");
    
    hwnd = CreateWindowW(
        L"WebcamViewerWindow",
        L"ReactOS Webcam Viewer",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        840, 420,
        NULL, NULL, hInstance, NULL);
    
    if (!hwnd)
    {
        DEBUG_PRINT("[ERROR] CreateWindowW failed");
        MessageBoxW(NULL, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    DEBUG_PRINT("[DEBUG] Main window created successfully");
    
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    
    DEBUG_PRINT("[DEBUG] === ReactOS Webcam Viewer Terminated ===");
    return 0;
}
