/*
 * PROJECT:     ReactOS OpenCL ICD Loader
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ICD and layer discovery
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "precomp.h"
#include <initguid.h>
#include <devguid.h>

/* GLOBALS ********************************************************************/

/* GUID_DEVCLASS_SOFTWARECOMPONENT */
DEFINE_GUID(OclpSoftwareComponentClass, 0x5c4c3332, 0x344d, 0x483c, 0x87, 0x39, 0x25, 0x9e, 0x93, 0x4c, 0x9c, 0xc8);

#define OCL_GUID_STRING_LENGTH 39

typedef struct _OCL_LAYER_ENTRY
{
    PSTR LibraryName;
    DWORD Priority;
} OCL_LAYER_ENTRY, *POCL_LAYER_ENTRY;

typedef LONG
(WINAPI *PFN_GET_PACKAGES_BY_FAMILY)(
    _In_ PCWSTR packageFamilyName,
    _Inout_ UINT32 *count,
    _Out_writes_opt_(*count) PWSTR *packageFullNames,
    _Inout_ UINT32 *bufferLength,
    _Out_writes_opt_(*bufferLength) WCHAR *buffer);

typedef LONG
(WINAPI *PFN_GET_PACKAGE_PATH)(
    _In_ PCWSTR packageFullName,
    _Inout_ UINT32 *pathLength,
    _Out_writes_opt_(*pathLength) PWSTR path);

typedef HRESULT
(WINAPI *PFN_CREATE_DXGI_FACTORY)(
    _In_ REFIID riid,
    _Out_ void **ppFactory);

/* Libraries collected by the discovery passes, loaded once all of them ran */
static POCL_ADAPTER OclpAdapters;
static ULONG OclpAdapterCount;
static ULONG OclpAdapterCapacity;

/* Registered layers, sorted by priority before they are installed */
static POCL_LAYER_ENTRY OclpLayerEntries;
static ULONG OclpLayerEntryCount;
static ULONG OclpLayerEntryCapacity;

static const LUID OclpNullLuid = { 0, 0 };

/* FUNCTIONS ******************************************************************/

static
PSTR
OclpDuplicateString(
    _In_z_ PCSTR String)
{
    SIZE_T Size = strlen(String) + 1;
    PSTR Copy;

    Copy = OclpAllocate(Size);
    if (Copy)
        memcpy(Copy, String, Size);

    return Copy;
}

VOID
OclpTraceWrite(
    _In_z_ PCSTR File,
    _In_ INT Line,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...)
{
    va_list Args;

    fprintf(stderr, "KHR ICD trace at %s:%d: ", File, Line);
    va_start(Args, Format);
    vfprintf(stderr, Format, Args);
    va_end(Args);
}

VOID
OclpTraceWriteW(
    _In_z_ PCSTR File,
    _In_ INT Line,
    _In_z_ _Printf_format_string_ PCWSTR Format,
    ...)
{
    va_list Args;

    fwprintf(stderr, L"KHR ICD trace at %hs:%d: ", File, Line);
    va_start(Args, Format);
    vfwprintf(stderr, Format, Args);
    va_end(Args);
}

HMODULE
OclpLoadLibrary(
    _In_z_ PCSTR LibraryName)
{
    HMODULE Library;

    Library = LoadLibraryExA(LibraryName, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!Library && GetLastError() == ERROR_INVALID_PARAMETER)
        Library = LoadLibraryExA(LibraryName, NULL, 0);

    if (!Library)
        OclpTrace("Failed to load driver. Windows error code is %lu.\n", GetLastError());

    return Library;
}

/**
 * @brief Reads an environment variable into a heap buffer.
 *
 * @return The value, or NULL when the variable is not set.
 */
PSTR
OclpGetEnvironment(
    _In_z_ PCSTR Name)
{
    DWORD Size;
    PSTR Value;

    Size = GetEnvironmentVariableA(Name, NULL, 0);
    if (!Size)
        return NULL;

    Value = OclpAllocate(Size);
    if (Value)
        GetEnvironmentVariableA(Name, Value, Size);

    return Value;
}

static
BOOLEAN
OclpIsHighIntegrity(VOID)
{
    BYTE Buffer[SECURITY_MAX_SID_SIZE + sizeof(DWORD)] = { 0 };
    PTOKEN_MANDATORY_LABEL Label = (PTOKEN_MANDATORY_LABEL)Buffer;
    BOOLEAN HighIntegrity = FALSE;
    HANDLE Token;
    DWORD Length;
    DWORD Level;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_QUERY_SOURCE, &Token))
        return FALSE;

    if (GetTokenInformation(Token, TokenIntegrityLevel, Buffer, sizeof(Buffer), &Length))
    {
        Level = *GetSidSubAuthority(Label->Label.Sid, *GetSidSubAuthorityCount(Label->Label.Sid) - 1);
        HighIntegrity = (Level > SECURITY_MANDATORY_MEDIUM_RID);
    }

    CloseHandle(Token);
    return HighIntegrity;
}

/**
 * @brief Like OclpGetEnvironment, but ignores the environment in elevated processes.
 */
PSTR
OclpGetSecureEnvironment(
    _In_z_ PCSTR Name)
{
    if (OclpIsHighIntegrity())
    {
        OclpTrace("Running at a high integrity level, so secure_getenv is returning NULL\n");
        return NULL;
    }

    return OclpGetEnvironment(Name);
}

static
VOID
OclpInitializeTrace(VOID)
{
    PSTR Value;

    Value = OclpGetEnvironment("OCL_ICD_ENABLE_TRACE");
    if (!Value)
        return;

    if (!strcmp(Value, "True") || !strcmp(Value, "true") || !strcmp(Value, "T") || !strcmp(Value, "1"))
        OclpTraceEnabled = TRUE;

    OclpFree(Value);
}

static
BOOL
OclpQueueAdapter(
    _In_z_ PCSTR LibraryName,
    _In_ LUID Luid)
{
    POCL_ADAPTER NewArray;
    ULONG NewCapacity;
    PSTR Name;

    if (OclpAdapterCount == OclpAdapterCapacity)
    {
        NewCapacity = OclpAdapterCapacity ? OclpAdapterCapacity * 2 : 1;
        NewArray = OclpAllocate(NewCapacity * sizeof(*NewArray));
        if (!NewArray)
            return FALSE;

        if (OclpAdapters)
        {
            memcpy(NewArray, OclpAdapters, OclpAdapterCount * sizeof(*NewArray));
            OclpFree(OclpAdapters);
        }

        OclpAdapters = NewArray;
        OclpAdapterCapacity = NewCapacity;
    }

    Name = OclpDuplicateString(LibraryName);
    if (!Name)
        return FALSE;

    OclpAdapters[OclpAdapterCount].LibraryName = Name;
    OclpAdapters[OclpAdapterCount].Luid = Luid;
    OclpAdapterCount++;
    return TRUE;
}

/**
 * @brief Value name holding the ICD path in a display driver key.
 *
 * 32-bit processes on a 64-bit system read the Wow variant.
 */
static
PCSTR
OclpGetDriverValueName(VOID)
{
#ifndef _WIN64
    BOOL IsWow64;

    if (IsWow64Process(GetCurrentProcess(), &IsWow64) && IsWow64)
        return "OpenCLDriverNameWow";
#endif

    return "OpenCLDriverName";
}

/**
 * @brief Asks the graphics kernel for the ICD path of every WDDM adapter.
 *
 * Only used where gdi32 exposes the WDDM 2.4 registry query.
 */
static
BOOL
OclpEnumerateDxgkAdapters(VOID)
{
    PFND3DKMT_QUERYADAPTERINFO QueryAdapterInfo;
    PFND3DKMT_ENUMADAPTERS2 EnumAdapters2;
    D3DDDI_QUERYREGISTRY_INFO *Result;
    D3DDDI_QUERYREGISTRY_INFO *Large;
    D3DDDI_QUERYREGISTRY_INFO Query;
    D3DKMT_QUERYADAPTERINFO Info;
    D3DKMT_ENUMADAPTERS2 Enum;
    CHAR LibraryName[MAX_PATH];
    PCSTR ValueName;
    BOOL Found = FALSE;
    NTSTATUS Status;
    HMODULE Gdi32;
    ULONG LargeSize;
    ULONG i;

    Enum.NumAdapters = 0;
    Enum.pAdapters = NULL;

    Gdi32 = LoadLibraryA("gdi32.dll");
    if (!Gdi32)
        return FALSE;

    /* Present on WDDM 2.4 and newer, which is also when the registry query appeared */
    if (!GetProcAddress(Gdi32, "D3DKMTSubmitPresentBltToHwQueue"))
        goto Exit;

    EnumAdapters2 = (PFND3DKMT_ENUMADAPTERS2)GetProcAddress(Gdi32, "D3DKMTEnumAdapters2");
    if (!EnumAdapters2)
    {
        OclpTrace("GetProcAddress failed for D3DKMTEnumAdapters2\n");
        goto Exit;
    }

    QueryAdapterInfo = (PFND3DKMT_QUERYADAPTERINFO)GetProcAddress(Gdi32, "D3DKMTQueryAdapterInfo");
    if (!QueryAdapterInfo)
    {
        OclpTrace("GetProcAddress failed for D3DKMTQueryAdapterInfo\n");
        goto Exit;
    }

    /* Count first, retrying while adapters keep arriving */
    do
    {
        Enum.NumAdapters = 0;
        Enum.pAdapters = NULL;
        Status = EnumAdapters2(&Enum);
    } while (Status == STATUS_BUFFER_TOO_SMALL);

    if (!NT_SUCCESS(Status))
    {
        OclpTrace("D3DKMTEnumAdapters2 status != SUCCESS\n");
        goto Exit;
    }

    Enum.pAdapters = OclpAllocate(Enum.NumAdapters * sizeof(*Enum.pAdapters));
    if (!Enum.pAdapters)
    {
        OclpTrace("Allocation failure for adapters buffer\n");
        goto Exit;
    }

    Status = EnumAdapters2(&Enum);
    if (!NT_SUCCESS(Status))
    {
        OclpTrace("D3DKMTEnumAdapters2 status != SUCCESS\n");
        goto Exit;
    }

    ValueName = OclpGetDriverValueName();

    for (i = 0; i < Enum.NumAdapters; i++)
    {
        RtlZeroMemory(&Query, sizeof(Query));
        Query.QueryType = D3DDDI_QUERYREGISTRY_ADAPTERKEY;
        Query.QueryFlags.TranslatePath = TRUE;
        Query.ValueType = REG_SZ;

        if (!MultiByteToWideChar(CP_UTF8,
                                 0,
                                 ValueName,
                                 (int)strlen(ValueName) + 1,
                                 Query.ValueName,
                                 ARRAYSIZE(Query.ValueName)))
        {
            OclpTrace("MultiByteToWideChar status != SUCCESS\n");
            continue;
        }

        RtlZeroMemory(&Info, sizeof(Info));
        Info.hAdapter = Enum.pAdapters[i].hAdapter;
        Info.Type = KMTQAITYPE_QUERYREGISTRY;
        Info.pPrivateDriverData = &Query;
        Info.PrivateDriverDataSize = sizeof(Query);

        Status = QueryAdapterInfo(&Info);
        if (!NT_SUCCESS(Status))
        {
            /* Some drivers store the path as a multi string */
            Query.ValueType = REG_MULTI_SZ;
            Status = QueryAdapterInfo(&Info);
            if (!NT_SUCCESS(Status))
                continue;

            OclpTrace("Accepting multi-string registry key type\n");
        }

        Result = &Query;
        Large = NULL;

        if (Query.Status == D3DDDI_QUERYREGISTRY_STATUS_BUFFER_OVERFLOW)
        {
            LargeSize = sizeof(Query) + Query.OutputValueSize;
            Large = OclpAllocate(LargeSize);
            if (!Large)
                continue;

            memcpy(Large, &Query, sizeof(Query));
            Info.pPrivateDriverData = Large;
            Info.PrivateDriverDataSize = LargeSize;
            Status = QueryAdapterInfo(&Info);
            Result = Large;
        }

        if (NT_SUCCESS(Status) && Result->Status == D3DDDI_QUERYREGISTRY_STATUS_SUCCESS)
        {
            if (WideCharToMultiByte(CP_UTF8,
                                    0,
                                    Result->OutputString,
                                    -1,
                                    LibraryName,
                                    sizeof(LibraryName),
                                    NULL,
                                    NULL))
                Found |= OclpQueueAdapter(LibraryName, Enum.pAdapters[i].AdapterLuid);
            else
                OclpTrace("WideCharToMultiByte status != SUCCESS\n");
        }
        else if (Status == STATUS_INVALID_PARAMETER)
        {
            /* The kernel does not know this query, no point asking the others */
            OclpFree(Large);
            break;
        }

        OclpFree(Large);
    }

Exit:
    OclpFree(Enum.pAdapters);
    FreeLibrary(Gdi32);
    return Found;
}

/**
 * @brief Queues the ICD named in the software key of a device node, if any.
 */
static
BOOL
OclpReadDeviceKey(
    _In_ DEVINST DevInst)
{
    PSTR LibraryName = NULL;
    BOOL Found = FALSE;
    HKEY Key = NULL;
    CONFIGRET Cr;
    DWORD Type = 0;
    DWORD Size = 0;
    LONG Error;

    Cr = CM_Open_DevNode_Key(DevInst,
                             KEY_QUERY_VALUE,
                             0,
                             RegDisposition_OpenExisting,
                             &Key,
                             CM_REGISTRY_SOFTWARE);
    if (Cr != CR_SUCCESS)
    {
        OclpTrace("Failed with ret 0x%lx\n", Cr);
        goto Exit;
    }

    Error = RegQueryValueExA(Key, OclpGetDriverValueName(), NULL, &Type, NULL, &Size);
    if (Error != ERROR_SUCCESS)
    {
        OclpTrace("Failed to open sub key 0x%lx\n", Error);
        goto Exit;
    }

    LibraryName = OclpAllocate(Size);
    if (!LibraryName)
    {
        OclpTrace("Failed to allocate %lu bytes for registry value\n", Size);
        goto Exit;
    }

    Error = RegQueryValueExA(Key, OclpGetDriverValueName(), NULL, &Type, (PBYTE)LibraryName, &Size);
    if (Error != ERROR_SUCCESS)
    {
        OclpTrace("Failed to open sub key 0x%lx\n", Error);
        goto Exit;
    }

    if (Type == REG_MULTI_SZ)
        OclpTrace("Accepting multi-string registry key type\n");
    else if (Type != REG_SZ)
    {
        OclpTrace("Unexpected registry entry 0x%lx! continuing\n", Type);
        goto Exit;
    }

    OclpTrace("    Path: %s\n", LibraryName);
    Found = OclpQueueAdapter(LibraryName, OclpNullLuid);

Exit:
    OclpFree(LibraryName);

    if (Key)
    {
        Error = RegCloseKey(Key);
        if (Error != ERROR_SUCCESS)
            OclpTrace("WARNING: failed to close hkey 0x%lx\n", Error);
    }

    return Found;
}

/**
 * @brief Checks that a device node is not waiting for a reboot.
 */
static
BOOLEAN
OclpIsDeviceUsable(
    _In_ DEVINST DevInst)
{
    ULONG Problem;
    ULONG Status;
    CONFIGRET Cr;

    Cr = CM_Get_DevNode_Status(&Status, &Problem, DevInst, 0);
    if (Cr != CR_SUCCESS)
    {
        OclpTrace("    WARNING: failed to probe the status of the device 0x%lx\n", Cr);
        return FALSE;
    }

    /* Covers a started device stuck on its old driver and a stopped one that cannot restart */
    if ((Status & DN_NEED_RESTART) || ((Status & DN_HAS_PROBLEM) && Problem == CM_PROB_NEED_RESTART))
    {
        OclpTrace("    WARNING: device is pending reboot (0x%lx), skipping...\n", Status);
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief Walks the software component children of a display adapter.
 */
static
BOOL
OclpReadChildKeys(
    _In_ DEVINST Parent)
{
    WCHAR InstanceId[MAX_DEVICE_ID_LEN];
    DEVINST Child;
    CONFIGRET Cr;
    ULONG Size;
    GUID Class;

    Cr = CM_Get_Child(&Child, Parent, 0);
    if (Cr != CR_SUCCESS)
    {
        OclpTrace("    CM_Get_Child returned 0x%lx, skipping children...\n", Cr);
        return FALSE;
    }

    do
    {
        OclpTrace("    devchild: %lu\n", Child);

        RtlZeroMemory(InstanceId, sizeof(InstanceId));
        Cr = CM_Get_Device_IDW(Child, InstanceId, sizeof(InstanceId), 0);
        if (Cr != CR_SUCCESS)
        {
            OclpTrace("    CM_Get_Device_ID returned 0x%lx, skipping device...\n", Cr);
            continue;
        }

        OclpTraceW(L"    deviceInstanceID: %ls\n", InstanceId);

        Size = sizeof(Class);
        Cr = CM_Get_DevNode_Registry_PropertyW(Child, CM_DRP_CLASSGUID, NULL, &Class, &Size, 0);
        if (Cr != CR_SUCCESS)
        {
            OclpTrace("    CM_Get_DevNode_Registry_PropertyW returned 0x%lx, skipping device...\n", Cr);
            continue;
        }

        if (!IsEqualGUID(&Class, &OclpSoftwareComponentClass))
        {
            OclpTrace("    GUID does not match, skipping device...\n");
            continue;
        }

        if (!OclpIsDeviceUsable(Child))
            continue;

        if (OclpReadDeviceKey(Child))
            return TRUE;
    } while (CM_Get_Sibling(&Child, Child, 0) == CR_SUCCESS);

    return FALSE;
}

/**
 * @brief Looks up the ICD of every present display adapter through the PnP manager.
 *
 * The adapter's own driver key is tried first, then its software component children.
 */
static
BOOL
OclpEnumerateDisplayDevices(VOID)
{
    WCHAR ClassString[OCL_GUID_STRING_LENGTH];
    PWSTR DeviceList = NULL;
    BOOL Found = FALSE;
    ULONG Length = 0;
    DEVINST DevInst;
    PWSTR DeviceId;
    CONFIGRET Cr;
    int Chars;

    Chars = StringFromGUID2(&GUID_DEVCLASS_DISPLAY, ClassString, ARRAYSIZE(ClassString));
    if (Chars != ARRAYSIZE(ClassString))
    {
        OclpTrace("StringFromGUID2 failed with %d\n", Chars);
        return FALSE;
    }

    /* The list can grow between the size query and the fetch */
    do
    {
        Cr = CM_Get_Device_ID_List_SizeW(&Length,
                                         ClassString,
                                         CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT);
        if (Cr != CR_SUCCESS)
        {
            OclpTrace("CM_Get_Device_ID_List_size failed with 0x%lx\n", Cr);
            break;
        }

        DeviceList = OclpAllocate(Length * sizeof(WCHAR));
        if (!DeviceList)
        {
            OclpTrace("Failed to allocate %lu bytes for device ID strings\n", Length);
            break;
        }

        Cr = CM_Get_Device_ID_ListW(ClassString,
                                    DeviceList,
                                    Length,
                                    CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT);
        if (Cr != CR_SUCCESS)
        {
            OclpTrace("CM_Get_Device_ID_List failed with 0x%lx\n", Cr);
            OclpFree(DeviceList);
            DeviceList = NULL;
        }
    } while (Cr == CR_BUFFER_SMALL);

    if (!DeviceList)
        return FALSE;

    for (DeviceId = DeviceList; *DeviceId; DeviceId += wcslen(DeviceId) + 1)
    {
        OclpTraceW(L"Device ID: %ls\n", DeviceId);

        Cr = CM_Locate_DevNodeW(&DevInst, DeviceId, 0);
        if (Cr != CR_SUCCESS)
        {
            OclpTrace("CM_Locate_DevNode failed with 0x%lx\n", Cr);
            continue;
        }

        OclpTrace("    devinst: %lu\n", DevInst);

        if (!OclpIsDeviceUsable(DevInst))
            continue;

        OclpTrace("    Trying to look for the key in the display adapter HKR...\n");
        if (OclpReadDeviceKey(DevInst))
        {
            Found = TRUE;
            continue;
        }

        OclpTrace("    Could not find the key, proceeding to children software components...\n");
        if (OclpReadChildKeys(DevInst))
            Found = TRUE;
    }

    OclpFree(DeviceList);
    return Found;
}

/**
 * @brief Picks up OpenCLOn12 from the D3D mapping layers package when it is installed.
 */
static
BOOL
OclpEnumerateMappingLayers(VOID)
{
    static const WCHAR FamilyName[] = L"Microsoft.D3DMappingLayers_8wekyb3d8bbwe";
    PFN_GET_PACKAGES_BY_FAMILY GetPackagesByFamily;
    PFN_GET_PACKAGE_PATH GetPackagePath;
    CHAR LibraryName[MAX_PATH];
    WCHAR DllPath[MAX_PATH];
    WCHAR Path[MAX_PATH];
    UINT32 PackageCount = 0;
    UINT32 BufferLength = 0;
    UINT32 PathLength = 0;
    PWSTR *Packages = NULL;
    PWSTR Buffer = NULL;
    BOOL Found = FALSE;
    HMODULE Kernel32;

    Kernel32 = LoadLibraryA("kernel32.dll");
    if (!Kernel32)
        return FALSE;

    GetPackagesByFamily = (PFN_GET_PACKAGES_BY_FAMILY)GetProcAddress(Kernel32, "GetPackagesByPackageFamily");
    if (!GetPackagesByFamily)
    {
        OclpTrace("GetProcAddress failed for GetPackagesByPackageFamily\n");
        goto Exit;
    }

    GetPackagePath = (PFN_GET_PACKAGE_PATH)GetProcAddress(Kernel32, "GetPackagePathByFullName");
    if (!GetPackagePath)
    {
        OclpTrace("GetProcAddress failed for GetPackagePathByFullName\n");
        goto Exit;
    }

    if (GetPackagesByFamily(FamilyName, &PackageCount, NULL, &BufferLength, NULL) != ERROR_INSUFFICIENT_BUFFER ||
        !PackageCount ||
        !BufferLength)
    {
        OclpTrace("Failed to find mapping layers packages by family name\n");
        goto Exit;
    }

    Buffer = OclpAllocate(BufferLength * sizeof(WCHAR));
    Packages = OclpAllocate(PackageCount * sizeof(PWSTR));
    if (!Buffer || !Packages)
    {
        OclpTrace("Failed to allocate memory for package names\n");
        goto Exit;
    }

    if (GetPackagesByFamily(FamilyName, &PackageCount, Packages, &BufferLength, Buffer) != ERROR_SUCCESS)
    {
        OclpTrace("Failed to get mapping layers package full names\n");
        goto Exit;
    }

    if (GetPackagePath(Packages[0], &PathLength, NULL) != ERROR_INSUFFICIENT_BUFFER ||
        PathLength > ARRAYSIZE(Path) ||
        GetPackagePath(Packages[0], &PathLength, Path) != ERROR_SUCCESS)
    {
        OclpTrace("Failed to get mapping layers package path length\n");
        goto Exit;
    }

#if defined(_M_AMD64)
    _snwprintf(DllPath, ARRAYSIZE(DllPath), L"%ls\\x64\\OpenCLOn12.dll", Path);
#elif defined(_M_ARM64)
    _snwprintf(DllPath, ARRAYSIZE(DllPath), L"%ls\\arm64\\OpenCLOn12.dll", Path);
#elif defined(_M_ARM)
    _snwprintf(DllPath, ARRAYSIZE(DllPath), L"%ls\\arm\\OpenCLOn12.dll", Path);
#else
    _snwprintf(DllPath, ARRAYSIZE(DllPath), L"%ls\\x86\\OpenCLOn12.dll", Path);
#endif
    DllPath[ARRAYSIZE(DllPath) - 1] = UNICODE_NULL;

    WideCharToMultiByte(CP_UTF8, 0, DllPath, -1, LibraryName, sizeof(LibraryName), NULL, NULL);
    Found = OclpQueueAdapter(LibraryName, OclpNullLuid);

Exit:
    FreeLibrary(Kernel32);
    OclpFree(Buffer);
    OclpFree(Packages);
    return Found;
}

/**
 * @brief Calls Callback for every REG_DWORD value of HKLM\KeyName.
 *
 * Enumeration stops at the first value that cannot be read, including values
 * whose data does not fit a DWORD.
 */
static
BOOL
OclpWalkRegistryList(
    _In_z_ PCSTR KeyName,
    _In_ BOOL (*Callback)(_In_z_ PCSTR LibraryName, _In_ DWORD Value))
{
    CHAR LibraryName[1024];
    DWORD NameLength;
    DWORD DataSize;
    BOOL Found = FALSE;
    DWORD Index;
    DWORD Value;
    DWORD Type;
    HKEY Key;
    LONG Error;

    OclpTrace("Opening key HKLM\\%s...\n", KeyName);

    Error = RegOpenKeyExA(HKEY_LOCAL_MACHINE, KeyName, 0, KEY_READ, &Key);
    if (Error != ERROR_SUCCESS)
    {
        OclpTrace("Failed to open key %s, continuing\n", KeyName);
        return FALSE;
    }

    for (Index = 0;; Index++)
    {
        LibraryName[0] = ANSI_NULL;
        NameLength = sizeof(LibraryName);
        DataSize = sizeof(Value);
        Value = 0;
        Type = 0;

        OclpTrace("Reading value %lu...\n", Index);
        Error = RegEnumValueA(Key, Index, LibraryName, &NameLength, NULL, &Type, (PBYTE)&Value, &DataSize);
        if (Error != ERROR_SUCCESS)
        {
            OclpTrace("Failed to read value %lu, done reading key.\n", Index);
            break;
        }

        OclpTrace("Value %s found...\n", LibraryName);

        if (Type != REG_DWORD)
        {
            OclpTrace("Value not a DWORD, skipping\n");
            continue;
        }

        Found |= Callback(LibraryName, Value);
    }

    RegCloseKey(Key);
    return Found;
}

static
BOOL
OclpQueueRegisteredVendor(
    _In_z_ PCSTR LibraryName,
    _In_ DWORD Value)
{
    /* A nonzero value disables the entry */
    if (Value)
    {
        OclpTrace("Value not zero, skipping\n");
        return FALSE;
    }

    return OclpQueueAdapter(LibraryName, OclpNullLuid);
}

static
BOOL
OclpQueueRegisteredLayer(
    _In_z_ PCSTR LibraryName,
    _In_ DWORD Value)
{
    POCL_LAYER_ENTRY NewArray;
    ULONG NewCapacity;
    PSTR Name;

    if (OclpLayerEntryCount == OclpLayerEntryCapacity)
    {
        NewCapacity = OclpLayerEntryCapacity ? OclpLayerEntryCapacity * 2 : 1;
        NewArray = OclpAllocate(NewCapacity * sizeof(*NewArray));
        if (!NewArray)
        {
            OclpTrace("Failed allocate space for Layers array\n");
            return FALSE;
        }

        if (OclpLayerEntries)
        {
            memcpy(NewArray, OclpLayerEntries, OclpLayerEntryCount * sizeof(*NewArray));
            OclpFree(OclpLayerEntries);
        }

        OclpLayerEntries = NewArray;
        OclpLayerEntryCapacity = NewCapacity;
    }

    Name = OclpDuplicateString(LibraryName);
    if (!Name)
    {
        OclpTrace("Failed allocate space for Layer file path\n");
        return FALSE;
    }

    OclpLayerEntries[OclpLayerEntryCount].LibraryName = Name;
    OclpLayerEntries[OclpLayerEntryCount].Priority = Value;
    OclpLayerEntryCount++;
    return TRUE;
}

/**
 * @brief Installs the layers registered under HKLM, lowest value first.
 */
static
BOOL
OclpEnumerateRegistryLayers(VOID)
{
    OCL_LAYER_ENTRY Entry;
    BOOL Found;
    ULONG i, j;

    Found = OclpWalkRegistryList("SOFTWARE\\Khronos\\OpenCL\\Layers", OclpQueueRegisteredLayer);

    for (i = 1; i < OclpLayerEntryCount; i++)
    {
        Entry = OclpLayerEntries[i];
        for (j = i; j > 0 && OclpLayerEntries[j - 1].Priority > Entry.Priority; j--)
            OclpLayerEntries[j] = OclpLayerEntries[j - 1];
        OclpLayerEntries[j] = Entry;
    }

    for (i = 0; i < OclpLayerEntryCount; i++)
    {
        OclpAddLayer(OclpLayerEntries[i].LibraryName);
        OclpFree(OclpLayerEntries[i].LibraryName);
    }

    OclpFree(OclpLayerEntries);
    OclpLayerEntries = NULL;
    OclpLayerEntryCount = 0;
    OclpLayerEntryCapacity = 0;
    return Found;
}

/**
 * @brief Loads the queued ICDs, adapters in DXGI order first so the default GPU comes first.
 */
static
VOID
OclpLoadQueuedAdapters(VOID)
{
    PFN_CREATE_DXGI_FACTORY CreateFactory;
    IDXGIFactory *Factory;
    IDXGIAdapter *Adapter;
    DXGI_ADAPTER_DESC Desc;
    HMODULE Dxgi;
    UINT Index;
    ULONG i;

    Dxgi = LoadLibraryA("dxgi.dll");
    if (Dxgi)
    {
        CreateFactory = (PFN_CREATE_DXGI_FACTORY)GetProcAddress(Dxgi, "CreateDXGIFactory");
        if (CreateFactory && SUCCEEDED(CreateFactory(&IID_IDXGIFactory, (void **)&Factory)))
        {
            for (Index = 0; SUCCEEDED(IDXGIFactory_EnumAdapters(Factory, Index, &Adapter)); Index++)
            {
                if (SUCCEEDED(IDXGIAdapter_GetDesc(Adapter, &Desc)))
                {
                    for (i = 0; i < OclpAdapterCount; i++)
                    {
                        if (OclpAdapters[i].Luid.LowPart == Desc.AdapterLuid.LowPart &&
                            OclpAdapters[i].Luid.HighPart == Desc.AdapterLuid.HighPart)
                        {
                            OclpAddVendor(OclpAdapters[i].LibraryName);
                            break;
                        }
                    }
                }

                IDXGIAdapter_Release(Adapter);
            }

            IDXGIFactory_Release(Factory);
        }

        FreeLibrary(Dxgi);
    }

    /* Whatever DXGI did not claim goes last, duplicates are dropped by OclpAddVendor */
    for (i = 0; i < OclpAdapterCount; i++)
    {
        OclpAddVendor(OclpAdapters[i].LibraryName);
        OclpFree(OclpAdapters[i].LibraryName);
    }

    OclpFree(OclpAdapters);
    OclpAdapters = NULL;
    OclpAdapterCount = 0;
    OclpAdapterCapacity = 0;
}

/**
 * @brief One time discovery of every ICD and layer, run through InitOnceExecuteOnce.
 *
 * @return TRUE when anything was found. FALSE leaves the init once open so the next
 *         API call searches again.
 */
BOOL
CALLBACK
OclpEnumerateIcds(
    _Inout_ PINIT_ONCE InitOnce,
    _Inout_opt_ PVOID Parameter,
    _Outptr_opt_result_maybenull_ PVOID *Context)
{
    BOOL Found = FALSE;

    UNREFERENCED_PARAMETER(InitOnce);
    UNREFERENCED_PARAMETER(Parameter);
    UNREFERENCED_PARAMETER(Context);

    OclpInitializeTrace();

    /* Libraries named in the environment are added immediately and come first */
    OclpEnumerateEnvironment();

    if (OclpEnumerateDxgkAdapters())
        Found = TRUE;
    else
        OclpTrace("Failed to load via DXGK interface on RS4, continuing\n");

    if (OclpEnumerateDisplayDevices())
        Found = TRUE;
    else
        OclpTrace("Failed to enumerate HKR entries, continuing\n");

    if (OclpEnumerateMappingLayers())
        Found = TRUE;
    else
        OclpTrace("Failed to enumerate App package entry, continuing\n");

    if (OclpWalkRegistryList("SOFTWARE\\Khronos\\OpenCL\\Vendors", OclpQueueRegisteredVendor))
        Found = TRUE;

    OclpLoadQueuedAdapters();

    if (OclpEnumerateRegistryLayers())
        Found = TRUE;

    OclpEnumerateEnvironmentLayers();

    return Found;
}
