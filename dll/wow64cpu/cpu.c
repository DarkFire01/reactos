
#include "cpu.h"

NTSTATUS NTAPI BTCpuGetContext(HANDLE hThread, HANDLE hProcess, PVOID Unused, WOW64_CONTEXT* pCtx32)
{
    UNREFERENCED_PARAMETER(Unused);
    return NtQueryInformationThread(hThread, ThreadWow64Context, pCtx32, sizeof(*pCtx32), NULL);
}

NTSTATUS NTAPI BTCpuSetContext(HANDLE hThread, HANDLE hProcess, PVOID Unused, WOW64_CONTEXT* pCtx32)
{
    UNREFERENCED_PARAMETER(Unused);
    return NtSetInformationThread(hThread, ThreadWow64Context, pCtx32, sizeof(*pCtx32));
}

BOOL 
WINAPI 
DllMain(HINSTANCE hInstance, DWORD Reason, PVOID Unused)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(Reason);
    UNREFERENCED_PARAMETER(Unused);
    return TRUE;
}