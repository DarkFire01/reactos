/*
 * PROJECT:     ReactOS Display Control Panel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Switches the WDDM basic display drivers for the installed display driver model
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "desk.h"
#include <winsvc.h>
#include <shlwapi.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/* The driver model of a display miniport, told by the port driver it links against */
typedef enum _DISPLAY_DRIVER_MODEL
{
    DisplayModelUnknown,
    DisplayModelXddm,
    DisplayModelWddm
} DISPLAY_DRIVER_MODEL;

/* The WDDM fallback drivers, both report their own root device */
static const PCWSTR BasicDriverServices[] = { L"BasicDisplay", L"BasicRender" };

/* FUNCTIONS ******************************************************************/

static
PUCHAR
ImageRvaToFileData(
    _In_ PIMAGE_NT_HEADERS NtHeaders,
    _In_reads_bytes_(ImageSize) PUCHAR Image,
    _In_ SIZE_T ImageSize,
    _In_ ULONG Rva,
    _In_ ULONG Length)
{
    PIMAGE_SECTION_HEADER Section = IMAGE_FIRST_SECTION(NtHeaders);
    ULONG_PTR Offset;
    USHORT i;

    for (i = 0; i < NtHeaders->FileHeader.NumberOfSections; i++, Section++)
    {
        if ((Rva < Section->VirtualAddress) ||
            ((Rva - Section->VirtualAddress) >= Section->SizeOfRawData))
        {
            continue;
        }

        Offset = (ULONG_PTR)Section->PointerToRawData + (Rva - Section->VirtualAddress);
        if ((Offset >= ImageSize) || (Length > ImageSize - Offset))
            return NULL;

        return Image + Offset;
    }

    return NULL;
}

static
BOOL
ImageImportIs(
    _In_ PIMAGE_NT_HEADERS NtHeaders,
    _In_reads_bytes_(ImageSize) PUCHAR Image,
    _In_ SIZE_T ImageSize,
    _In_ ULONG NameRva,
    _In_ PCSTR ModuleName)
{
    SIZE_T Length = strlen(ModuleName);
    PCSTR Name;

    Name = (PCSTR)ImageRvaToFileData(NtHeaders, Image, ImageSize, NameRva, (ULONG)Length + 1);
    if (!Name)
        return FALSE;

    return (_strnicmp(Name, ModuleName, Length) == 0) && (Name[Length] == ANSI_NULL);
}

static
DISPLAY_DRIVER_MODEL
GetImageDisplayModel(
    _In_reads_bytes_(ImageSize) PUCHAR Image,
    _In_ SIZE_T ImageSize)
{
    PIMAGE_DOS_HEADER DosHeader = (PIMAGE_DOS_HEADER)Image;
    PIMAGE_NT_HEADERS NtHeaders;
    PIMAGE_DATA_DIRECTORY ImportDirectory;
    PIMAGE_IMPORT_DESCRIPTOR Descriptor;
    DISPLAY_DRIVER_MODEL Model = DisplayModelUnknown;
    SIZE_T HeadersEnd;
    ULONG Rva;

    if ((ImageSize < sizeof(*DosHeader)) ||
        (DosHeader->e_magic != IMAGE_DOS_SIGNATURE) ||
        (DosHeader->e_lfanew < 0) ||
        ((SIZE_T)DosHeader->e_lfanew > ImageSize - FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader)))
    {
        return DisplayModelUnknown;
    }

    NtHeaders = (PIMAGE_NT_HEADERS)(Image + DosHeader->e_lfanew);
    if (NtHeaders->Signature != IMAGE_NT_SIGNATURE)
        return DisplayModelUnknown;

    /* The optional header and the section table have to be in the file */
    HeadersEnd = (SIZE_T)DosHeader->e_lfanew + FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) +
                 NtHeaders->FileHeader.SizeOfOptionalHeader +
                 (SIZE_T)NtHeaders->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (HeadersEnd > ImageSize)
        return DisplayModelUnknown;

    if ((NtHeaders->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) &&
        (NtHeaders->FileHeader.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER64)))
    {
        ImportDirectory = &((PIMAGE_NT_HEADERS64)NtHeaders)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    }
    else if ((NtHeaders->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) &&
             (NtHeaders->FileHeader.SizeOfOptionalHeader >= sizeof(IMAGE_OPTIONAL_HEADER32)))
    {
        ImportDirectory = &((PIMAGE_NT_HEADERS32)NtHeaders)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    }
    else
    {
        return DisplayModelUnknown;
    }

    for (Rva = ImportDirectory->VirtualAddress; Rva != 0; Rva += sizeof(*Descriptor))
    {
        Descriptor = (PIMAGE_IMPORT_DESCRIPTOR)ImageRvaToFileData(NtHeaders,
                                                                 Image,
                                                                 ImageSize,
                                                                 Rva,
                                                                 sizeof(*Descriptor));
        if (!Descriptor || (Descriptor->Name == 0))
            break;

        /* A vendor wrapper can link both, dxgkrnl is what makes it WDDM */
        if (ImageImportIs(NtHeaders, Image, ImageSize, Descriptor->Name, "dxgkrnl.sys"))
            return DisplayModelWddm;

        if (ImageImportIs(NtHeaders, Image, ImageSize, Descriptor->Name, "videoprt.sys"))
            Model = DisplayModelXddm;
    }

    return Model;
}

static
BOOL
GetServiceImagePath(
    _In_ PCWSTR ServiceName,
    _Out_writes_(cchPath) PWSTR Path,
    _In_ DWORD cchPath)
{
    WCHAR KeyName[MAX_PATH];
    WCHAR RawPath[MAX_PATH];
    WCHAR Expanded[MAX_PATH];
    WCHAR WindowsDir[MAX_PATH];
    PCWSTR Relative;
    DWORD cbData = sizeof(RawPath) - sizeof(WCHAR);
    DWORD Type;
    HKEY hKey;
    LONG rc;

    if (FAILED(StringCchPrintfW(KeyName, ARRAYSIZE(KeyName),
                                L"SYSTEM\\CurrentControlSet\\Services\\%s", ServiceName)))
    {
        return FALSE;
    }

    rc = RegOpenKeyExW(HKEY_LOCAL_MACHINE, KeyName, 0, KEY_QUERY_VALUE, &hKey);
    if (rc != ERROR_SUCCESS)
        return FALSE;

    rc = RegQueryValueExW(hKey, L"ImagePath", NULL, &Type, (PBYTE)RawPath, &cbData);
    RegCloseKey(hKey);

    if ((rc == ERROR_SUCCESS) && ((Type == REG_SZ) || (Type == REG_EXPAND_SZ)))
    {
        RawPath[cbData / sizeof(WCHAR)] = UNICODE_NULL;
    }
    else if (FAILED(StringCchPrintfW(RawPath, ARRAYSIZE(RawPath),
                                     L"System32\\drivers\\%s.sys", ServiceName)))
    {
        return FALSE;
    }

    if (!ExpandEnvironmentStringsW(RawPath, Expanded, ARRAYSIZE(Expanded)) ||
        !GetWindowsDirectoryW(WindowsDir, ARRAYSIZE(WindowsDir)))
    {
        return FALSE;
    }

    /* The kernel takes NT paths and paths relative to the system root */
    if (_wcsnicmp(Expanded, L"\\SystemRoot\\", 12) == 0)
        Relative = Expanded + 12;
    else if (_wcsnicmp(Expanded, L"\\??\\", 4) == 0)
        return SUCCEEDED(StringCchCopyW(Path, cchPath, Expanded + 4));
    else if ((Expanded[0] != UNICODE_NULL) && (Expanded[1] == L':'))
        return SUCCEEDED(StringCchCopyW(Path, cchPath, Expanded));
    else
        Relative = Expanded;

    return SUCCEEDED(StringCchPrintfW(Path, cchPath, L"%s\\%s", WindowsDir, Relative));
}

static
DISPLAY_DRIVER_MODEL
GetServiceDisplayModel(
    _In_ PCWSTR ServiceName)
{
    DISPLAY_DRIVER_MODEL Model;
    WCHAR ImagePath[MAX_PATH];
    LARGE_INTEGER FileSize;
    HANDLE hFile, hMapping;
    PUCHAR Image;

    if (!GetServiceImagePath(ServiceName, ImagePath, ARRAYSIZE(ImagePath)))
        return DisplayModelUnknown;

    hFile = CreateFileW(ImagePath,
                        GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_DELETE,
                        NULL,
                        OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL,
                        NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        DPRINT1("Cannot open %S for %S, error %lu\n", ImagePath, ServiceName, GetLastError());
        return DisplayModelUnknown;
    }

    if (!GetFileSizeEx(hFile, &FileSize) ||
        (FileSize.QuadPart == 0) ||
        ((ULONGLONG)FileSize.QuadPart > (SIZE_T)-1))
    {
        CloseHandle(hFile);
        return DisplayModelUnknown;
    }

    hMapping = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(hFile);
    if (!hMapping)
        return DisplayModelUnknown;

    Image = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hMapping);
    if (!Image)
        return DisplayModelUnknown;

    Model = GetImageDisplayModel(Image, (SIZE_T)FileSize.QuadPart);
    UnmapViewOfFile(Image);

    return Model;
}

/**
 * @brief Turns the WDDM basic drivers on for a WDDM display driver and off for an XDDM one,
 *        so the desktop stays on the XDDM driver. Takes effect on the next boot.
 */
VOID
UpdateBasicDisplayDrivers(
    _In_ PCWSTR ServiceName)
{
    DISPLAY_DRIVER_MODEL Model;
    SC_HANDLE hManager, hService;
    WCHAR EnumKey[MAX_PATH];
    DWORD StartType;
    ULONG i;

    Model = GetServiceDisplayModel(ServiceName);
    if (Model == DisplayModelUnknown)
        return;

    StartType = (Model == DisplayModelWddm) ? SERVICE_SYSTEM_START : SERVICE_DISABLED;
    DPRINT1("%S is an %s display driver, basic drivers get start type %lu\n",
            ServiceName, (Model == DisplayModelWddm) ? "WDDM" : "XDDM", StartType);

    hManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hManager)
    {
        DPRINT1("OpenSCManagerW() failed with error %lu\n", GetLastError());
        return;
    }

    for (i = 0; i < ARRAYSIZE(BasicDriverServices); i++)
    {
        hService = OpenServiceW(hManager, BasicDriverServices[i], SERVICE_CHANGE_CONFIG);
        if (!hService)
            continue;

        if (!ChangeServiceConfigW(hService,
                                  SERVICE_NO_CHANGE,
                                  StartType,
                                  SERVICE_NO_CHANGE,
                                  NULL,
                                  NULL,
                                  NULL,
                                  NULL,
                                  NULL,
                                  NULL,
                                  NULL))
        {
            DPRINT1("ChangeServiceConfigW(%S) failed with error %lu\n",
                    BasicDriverServices[i], GetLastError());
        }
        CloseServiceHandle(hService);

        /* Its driver reports the root device again once the service is back on */
        if ((Model == DisplayModelXddm) &&
            SUCCEEDED(StringCchPrintfW(EnumKey, ARRAYSIZE(EnumKey),
                                       L"SYSTEM\\CurrentControlSet\\Enum\\Root\\%s",
                                       BasicDriverServices[i])))
        {
            SHDeleteKeyW(HKEY_LOCAL_MACHINE, EnumKey);
        }
    }

    CloseServiceHandle(hManager);
}
