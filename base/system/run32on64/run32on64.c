/*
 * run32on64
 * Copyright (C) 2003-2025 Marcin Jabłoński
 *
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         run32on64.exe
 * FILE:            base/system/run32on64/run32on64.c
 * PURPOSE:         Test utility for WOW64
 * PROGRAMMER:      Marcin Jabłoński
 */

#ifdef UNICODE
#ifndef _UNICODE
#define _UNICODE
#endif
#else
#ifdef _UNICODE
#define UNICODE
#endif
#endif

unsigned long __readfsdword(unsigned long);

#define WIN32_NO_STATUS
#include <Windows.h>
#include <stdio.h>

#include <ntndk.h>

void PrintError(HRESULT hResult)
{
    LPWSTR errorText = NULL;
    
    FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,  
        NULL,
        hResult,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR) &errorText,
        0,
        NULL);
       
    if (errorText != NULL)
    {
        wprintf(L"Last error: '%ls'\n", errorText);
        LocalFree(errorText);
    }
    else
    {
        wprintf(L"Format message failed\n");
    }
}

int wmain(int argc, WCHAR* argv[])
{
    NTSTATUS status;
    
    HMODULE ntdll32 = LoadLibraryW(L"x:\\reactos\\syswow64\\ntdll.dll");
    PrintError(GetLastError());
    wprintf(L"LoadLibraryW result: %p\n", ntdll32);
    
    HMODULE ntdll64 = LoadLibraryW(L"ntdll.dll");
    PrintError(GetLastError());
    wprintf(L"Result: %p ?= %p\n", ntdll32, ntdll64);
    
    if (ntdll32 == ntdll64)
    {
        wprintf(L"Remapping due to conflict.\n");
        ntdll32 = LoadLibraryW(L"x:\\reactos\\syswow64\\ntdll32.dll");
        PrintError(GetLastError());
    }
    
    PTEB currentTeb = NtCurrentTeb();
    wprintf(L"Current TEB %p\n", currentTeb);
    
    PPEB32 wowPeb;
    PTEB32 wowTeb;
    SIZE_T size;
    
    wowTeb = NULL;
    wowPeb = NULL;
    
    /* These allocations are undone on process exit (not explicitly, though). */
    size = sizeof(TEB32);
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &wowTeb, 32, &size, MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(status))
    {
        wprintf(L"TEB32 Allocation failed: %lx\n", status);
        return -1;
    }
    
    size = sizeof(PEB32);
    status = NtAllocateVirtualMemory(NtCurrentProcess(), &wowPeb, 32, &size, MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(status))
    {
        wprintf(L"PEB32 Allocation failed: %lx\n", status);
        return -1;
    }
    
    /* Set the 64 bit Teb's TlsSlots[1] to the TEB32 before setting process information. */
    NtCurrentTeb()->TlsSlots[1] = wowTeb;
    
    wprintf(L"Initializing PEB32 and TEB32\n");
    wowTeb->NtTib.Self = (ULONG)(ULONG_PTR)wowTeb;
    
    /* TODO: Check types - _WOW64_PROCESS has only one field, is this supposed to be the PEB?
       According to https://stackoverflow.com/a/69171561 - yes, it is */
    status = NtSetInformationProcess(NtCurrentProcess(), ProcessWow64Information, &wowPeb, sizeof(wowPeb));
    if (!NT_SUCCESS(status))
    {
        wprintf(L"Setting info failed: %lx\n", status);
        return -1;
    }
    
    status = NtQueryInformationProcess(NtCurrentProcess(), ProcessWow64Information, &wowPeb, sizeof(wowPeb), NULL);
    if (!NT_SUCCESS(status))
    {
        wprintf(L"Getting info failed: %lx\n", status);
        return -1;
    }
    
    wprintf(L"Got PEB32 address: %p, TEB32 %p\n", wowPeb, NtCurrentTeb()->TlsSlots[1]);
    
    void SetupFs(ULONG_PTR segSelector);
    SetupFs(0x0053);
    
    FARPROC proc = GetProcAddress(ntdll32, "LdrInitializeThunk");
    wprintf(L"Getting init function ptr %p\n", proc);
    
    void Enter32(FARPROC where);
    
    wprintf(L"Entering\n");
    __debugbreak();
    Enter32(proc);
    wprintf(L"Exiting\n");
    return 0;
}