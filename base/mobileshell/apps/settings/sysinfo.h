/*
 * PROJECT:     ReactOS Mobile Shell
 * LICENSE:     MIT
 * PURPOSE:     System information from the documented sources: OS identity, processor, memory, volumes
 */

#pragma once

#include <windows.h>
#include <string>
#include <vector>

#ifdef __REACTOS__
/* Exported by ReactOS kernel32 but not declared in its headers */
EXTERN_C
BOOL
WINAPI
GetLogicalProcessorInformation(
    _Out_writes_bytes_to_opt_(*ReturnedLength, *ReturnedLength) PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Buffer,
    _Inout_ PDWORD ReturnedLength);

EXTERN_C
INT
WINAPI
GetNumberFormatEx(
    _In_opt_ LPCWSTR LocaleName,
    _In_ DWORD Flags,
    _In_ LPCWSTR Value,
    _In_opt_ const NUMBERFMTW *Format,
    _Out_writes_opt_(Count) LPWSTR Number,
    _In_ INT Count);

EXTERN_C
INT
WINAPI
GetCurrencyFormatEx(
    _In_opt_ LPCWSTR LocaleName,
    _In_ DWORD Flags,
    _In_ LPCWSTR Value,
    _In_opt_ const CURRENCYFMTW *Format,
    _Out_writes_opt_(Count) LPWSTR Currency,
    _In_ INT Count);
#endif

typedef struct _SYS_OS
{
    std::wstring Product;
    std::wstring Edition;
    std::wstring Release;
    std::wstring Build;
    std::wstring Kernel;
    std::wstring ServicePack;
    BOOL IsReactOS;
} SYS_OS, *PSYS_OS;

typedef struct _SYS_CPU
{
    std::wstring Name;
    std::wstring Architecture;
    ULONG Packages;
    ULONG Cores;
    ULONG Logical;
    ULONG MaxMhz;
} SYS_CPU, *PSYS_CPU;

typedef struct _SYS_MEMORY
{
    ULONGLONG Installed;
    ULONGLONG Usable;
    ULONGLONG Available;
    ULONGLONG CommitUsed;
    ULONGLONG CommitLimit;
    ULONG Load;
} SYS_MEMORY, *PSYS_MEMORY;

typedef struct _SYS_VOLUME
{
    std::wstring Root;
    std::wstring Label;
    std::wstring FileSystem;
    UINT Type;
    ULONGLONG Total;
    ULONGLONG Free;
} SYS_VOLUME, *PSYS_VOLUME;

#define SYS_CURRENT_VERSION L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"

inline
std::wstring
SysRegString(
    _In_ HKEY Root,
    _In_z_ PCWSTR Key,
    _In_z_ PCWSTR Value)
{
    WCHAR Text[256];
    DWORD Size = sizeof(Text);

    if (RegGetValueW(Root, Key, Value, RRF_RT_REG_SZ, NULL, Text, &Size) != ERROR_SUCCESS)
        return std::wstring();
    return Text;
}

inline
BOOL
SysRegDword(
    _In_ HKEY Root,
    _In_z_ PCWSTR Key,
    _In_z_ PCWSTR Value,
    _Out_ DWORD *Data)
{
    DWORD Size = sizeof(*Data);

    return RegGetValueW(Root, Key, Value, RRF_RT_REG_DWORD, NULL, Data, &Size) == ERROR_SUCCESS;
}

/* ProductVersion of a file's version resource, read through version.dll at run time */
inline
std::wstring
SysFileProductVersion(
    _In_z_ PCWSTR Path)
{
    typedef DWORD (WINAPI *PGET_SIZE)(LPCWSTR, LPDWORD);
    typedef BOOL (WINAPI *PGET_INFO)(LPCWSTR, DWORD, DWORD, LPVOID);
    typedef BOOL (WINAPI *PQUERY)(LPCVOID, LPCWSTR, LPVOID *, PUINT);
    struct { WORD Language; WORD CodePage; } *Translation;
    HMODULE Module = LoadLibraryW(L"version.dll");
    std::vector<BYTE> Block;
    std::wstring Result;
    WCHAR Key[64];
    PWSTR Value;
    UINT Length;
    DWORD Size;

    if (!Module)
        return Result;

    PGET_SIZE GetSize = reinterpret_cast<PGET_SIZE>(GetProcAddress(Module, "GetFileVersionInfoSizeW"));
    PGET_INFO GetInfo = reinterpret_cast<PGET_INFO>(GetProcAddress(Module, "GetFileVersionInfoW"));
    PQUERY Query = reinterpret_cast<PQUERY>(GetProcAddress(Module, "VerQueryValueW"));

    if (GetSize && GetInfo && Query && (Size = GetSize(Path, NULL)) != 0)
    {
        Block.resize(Size);
        if (GetInfo(Path, 0, Size, Block.data()) &&
            Query(Block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID *>(&Translation), &Length) &&
            Length >= sizeof(*Translation))
        {
            swprintf_s(Key, ARRAYSIZE(Key), L"\\StringFileInfo\\%04x%04x\\ProductVersion",
                       Translation->Language, Translation->CodePage);
            if (Query(Block.data(), Key, reinterpret_cast<LPVOID *>(&Value), &Length) && Length)
                Result = Value;
        }
    }
    FreeLibrary(Module);
    return Result;
}

inline
VOID
SysQueryOs(
    _Out_ PSYS_OS Os)
{
    typedef LONG (WINAPI *PRTL_GET_VERSION)(PRTL_OSVERSIONINFOEXW);
    RTL_OSVERSIONINFOEXW Version = { sizeof(Version) };
    PRTL_GET_VERSION GetVersionRoutine;
    WCHAR Kernel[MAX_PATH];
    DWORD Revision;

    *Os = SYS_OS();
    Os->Product = SysRegString(HKEY_LOCAL_MACHINE, SYS_CURRENT_VERSION, L"ProductName");
    Os->Edition = SysRegString(HKEY_LOCAL_MACHINE, SYS_CURRENT_VERSION, L"EditionID");
    Os->ServicePack = SysRegString(HKEY_LOCAL_MACHINE, SYS_CURRENT_VERSION, L"CSDVersion");
    Os->IsReactOS = (Os->Product.find(L"ReactOS") != std::wstring::npos);

    /* RtlGetVersion is the version the kernel reports, unaffected by compatibility shims */
    GetVersionRoutine = reinterpret_cast<PRTL_GET_VERSION>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (GetVersionRoutine && GetVersionRoutine(&Version) == 0)
    {
        WCHAR Text[64];

        swprintf_s(Text, ARRAYSIZE(Text), L"NT %lu.%lu.%lu", Version.dwMajorVersion, Version.dwMinorVersion, Version.dwBuildNumber);
        Os->Kernel = Text;
    }

    Os->Build = SysRegString(HKEY_LOCAL_MACHINE, SYS_CURRENT_VERSION, L"CurrentBuild");
    if (Os->Build.empty())
        Os->Build = SysRegString(HKEY_LOCAL_MACHINE, SYS_CURRENT_VERSION, L"CurrentBuildNumber");
    if (SysRegDword(HKEY_LOCAL_MACHINE, SYS_CURRENT_VERSION, L"UBR", &Revision))
        Os->Build += L"." + std::to_wstring(Revision);

    if (Os->IsReactOS)
    {
        /* ReactOS keeps NT compatible numbers in the registry and its own release in the kernel's resource */
        if (GetSystemDirectoryW(Kernel, ARRAYSIZE(Kernel)))
            Os->Release = SysFileProductVersion((std::wstring(Kernel) + L"\\ntoskrnl.exe").c_str());
        Os->Build = SysRegString(HKEY_LOCAL_MACHINE, SYS_CURRENT_VERSION, L"BuildLab");
        return;
    }

    Os->Release = SysRegString(HKEY_LOCAL_MACHINE, SYS_CURRENT_VERSION, L"DisplayVersion");
    if (Os->Release.empty())
        Os->Release = SysRegString(HKEY_LOCAL_MACHINE, SYS_CURRENT_VERSION, L"ReleaseId");

    /* Windows 11 still writes "Windows 10" into ProductName; the build number tells them apart */
    if (Version.dwMajorVersion == 10 && Version.dwBuildNumber >= 22000 && Os->Product.compare(0, 10, L"Windows 10") == 0)
        Os->Product.replace(8, 2, L"11");
}

inline
VOID
SysQueryCpu(
    _Out_ PSYS_CPU Cpu)
{
    typedef LONG (WINAPI *PCALL_NT_POWER_INFORMATION)(POWER_INFORMATION_LEVEL, PVOID, ULONG, PVOID, ULONG);
    typedef struct _CPU_POWER
    {
        ULONG Number;
        ULONG MaxMhz;
        ULONG CurrentMhz;
        ULONG MhzLimit;
        ULONG MaxIdleState;
        ULONG CurrentIdleState;
    } CPU_POWER;
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> Relations;
    std::vector<CPU_POWER> Power;
    PCALL_NT_POWER_INFORMATION CallPower;
    SYSTEM_INFO System;
    HMODULE PowerModule;
    DWORD Size = 0;

    *Cpu = SYS_CPU();

    /* The marketing name is only published in the processor's registry key */
    Cpu->Name = SysRegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    if (Cpu->Name.empty())
        Cpu->Name = SysRegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"Identifier");

    typedef BOOL (WINAPI *PIS_WOW64_PROCESS2)(HANDLE, PUSHORT, PUSHORT);
    PIS_WOW64_PROCESS2 IsWow64Process2Routine;
    USHORT ProcessMachine;
    USHORT NativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;

    /*
     * An emulated process sees the emulated architecture in GetNativeSystemInfo, so an x64
     * build on ARM64 would claim x64. IsWow64Process2 names the real machine where it exists.
     */
    GetNativeSystemInfo(&System);
    IsWow64Process2Routine = reinterpret_cast<PIS_WOW64_PROCESS2>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (!IsWow64Process2Routine || !IsWow64Process2Routine(GetCurrentProcess(), &ProcessMachine, &NativeMachine))
        NativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;

    switch (NativeMachine)
    {
        case IMAGE_FILE_MACHINE_ARM64:
            Cpu->Architecture = L"ARM64";
            break;
        case IMAGE_FILE_MACHINE_AMD64:
            Cpu->Architecture = L"x64";
            break;
        case IMAGE_FILE_MACHINE_ARMNT:
            Cpu->Architecture = L"ARM";
            break;
        case IMAGE_FILE_MACHINE_I386:
            Cpu->Architecture = L"x86";
            break;
        default:
            switch (System.wProcessorArchitecture)
            {
                case PROCESSOR_ARCHITECTURE_AMD64:
                    Cpu->Architecture = L"x64";
                    break;
                case PROCESSOR_ARCHITECTURE_ARM64:
                    Cpu->Architecture = L"ARM64";
                    break;
                case PROCESSOR_ARCHITECTURE_ARM:
                    Cpu->Architecture = L"ARM";
                    break;
                case PROCESSOR_ARCHITECTURE_INTEL:
                    Cpu->Architecture = L"x86";
                    break;
                default:
                    Cpu->Architecture = L"Unknown";
                    break;
            }
            break;
    }
    Cpu->Logical = System.dwNumberOfProcessors;

    /* Physical cores and packages from the processor relationships */
    GetLogicalProcessorInformation(NULL, &Size);
    if (Size)
    {
        Relations.resize(Size / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (GetLogicalProcessorInformation(Relations.data(), &Size))
        {
            for (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION &Relation : Relations)
            {
                if (Relation.Relationship == RelationProcessorCore)
                    Cpu->Cores++;
                else if (Relation.Relationship == RelationProcessorPackage)
                    Cpu->Packages++;
            }
        }
    }

    /* Rated clock from the power manager, loaded at run time so a missing powrprof is harmless */
    PowerModule = LoadLibraryW(L"powrprof.dll");
    if (PowerModule)
    {
        CallPower = reinterpret_cast<PCALL_NT_POWER_INFORMATION>(GetProcAddress(PowerModule, "CallNtPowerInformation"));
        Power.resize(max(1UL, Cpu->Logical));
        if (CallPower && CallPower(ProcessorInformation, NULL, 0, Power.data(),
                                   static_cast<ULONG>(Power.size() * sizeof(CPU_POWER))) == 0)
        {
            for (const CPU_POWER &Entry : Power)
                Cpu->MaxMhz = max(Cpu->MaxMhz, Entry.MaxMhz);
        }
        FreeLibrary(PowerModule);
    }
}

inline
VOID
SysQueryMemory(
    _Out_ PSYS_MEMORY Memory)
{
    typedef BOOL (WINAPI *PGET_INSTALLED)(PULONGLONG);
    MEMORYSTATUSEX Status = { sizeof(Status) };
    PGET_INSTALLED GetInstalled;
    ULONGLONG Kilobytes;

    *Memory = SYS_MEMORY();
    if (GlobalMemoryStatusEx(&Status))
    {
        Memory->Usable = Status.ullTotalPhys;
        Memory->Available = Status.ullAvailPhys;
        Memory->CommitLimit = Status.ullTotalPageFile;
        Memory->CommitUsed = Status.ullTotalPageFile - Status.ullAvailPageFile;
        Memory->Load = Status.dwMemoryLoad;
    }

    /* Installed RAM includes what firmware and devices reserve; older systems lack the call */
    GetInstalled = reinterpret_cast<PGET_INSTALLED>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetPhysicallyInstalledSystemMemory"));
    if (GetInstalled && GetInstalled(&Kilobytes))
        Memory->Installed = Kilobytes * 1024;
    else
        Memory->Installed = Memory->Usable;
}

inline
VOID
SysQueryVolumes(
    _Out_ std::vector<SYS_VOLUME> &Volumes)
{
    WCHAR Drives[512];
    WCHAR Label[MAX_PATH];
    WCHAR FileSystem[MAX_PATH];
    ULARGE_INTEGER Free;
    ULARGE_INTEGER Total;
    UINT OldMode;

    Volumes.clear();
    if (!GetLogicalDriveStringsW(ARRAYSIZE(Drives), Drives))
        return;

    /* Empty card readers must not raise a dialog */
    OldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    for (PCWSTR Root = Drives; *Root; Root += wcslen(Root) + 1)
    {
        SYS_VOLUME Volume;

        Volume.Type = GetDriveTypeW(Root);
        if (Volume.Type != DRIVE_FIXED && Volume.Type != DRIVE_REMOVABLE)
            continue;
        if (!GetDiskFreeSpaceExW(Root, &Free, &Total, NULL))
            continue;

        Volume.Root = Root;
        Volume.Total = Total.QuadPart;
        Volume.Free = Free.QuadPart;
        if (GetVolumeInformationW(Root, Label, ARRAYSIZE(Label), NULL, NULL, NULL, FileSystem, ARRAYSIZE(FileSystem)))
        {
            Volume.Label = Label;
            Volume.FileSystem = FileSystem;
        }
        Volumes.push_back(Volume);
    }
    SetErrorMode(OldMode);
}
