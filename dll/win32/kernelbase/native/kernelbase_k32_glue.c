/*
 * Glue for linking kernel32_shared into kernelbase.dll (MSVC).
 * Supplies globals and startup entrypoints that normally live in kernel32.dll.
 */

#include <k32.h>

#include <reactos/subsys/win/basemsg.h>

#define NDEBUG
#include <debug.h>

UNICODE_STRING BasePathVariableName = RTL_CONSTANT_STRING(L"PATH");

PBASE_STATIC_SERVER_DATA BaseStaticServerData;
HANDLE BaseNamedObjectDirectory;
PLDR_DATA_TABLE_ENTRY BasepExeLdrEntry;
BOOLEAN BaseRunningInServerProcess;
WaitForInputIdleType UserWaitForInputIdleRoutine;

RTL_CRITICAL_SECTION BaseDllDirectoryLock;

WCHAR BaseDefaultPathBuffer[6140];

static BOOL s_k32_glue_initialized;

VOID
WINAPI
KernelBase_InitK32SharedFromPeb(VOID)
{
    NTSTATUS Status;
    PPEB peb = NtCurrentPeb();

    if (s_k32_glue_initialized)
        return;
    s_k32_glue_initialized = TRUE;

    if (!peb || !peb->ReadOnlyStaticServerData)
        return;

    BaseStaticServerData = (PBASE_STATIC_SERVER_DATA)
        peb->ReadOnlyStaticServerData[BASESRV_SERVERDLL_INDEX];

    if (!BaseStaticServerData)
        return;

    BaseWindowsDirectory = BaseStaticServerData->WindowsDirectory;
    BaseWindowsSystemDirectory = BaseStaticServerData->WindowsSystemDirectory;

    Status = RtlStringCbPrintfW(BaseDefaultPathBuffer,
                                sizeof(BaseDefaultPathBuffer),
                                L"%wZ;%wZ\\system;%wZ;",
                                &BaseWindowsSystemDirectory,
                                &BaseWindowsDirectory,
                                &BaseWindowsDirectory);
    if (!NT_SUCCESS(Status))
        return;

    RtlInitUnicodeString(&BaseDefaultPath, NULL);
    BaseDefaultPath.Buffer = BaseDefaultPathBuffer;
    BaseDefaultPath.Length = (USHORT)(wcslen(BaseDefaultPathBuffer) * sizeof(WCHAR));
    BaseDefaultPath.MaximumLength = sizeof(BaseDefaultPathBuffer);

    BaseDefaultPathAppend.Buffer = (PWSTR)((ULONG_PTR)BaseDefaultPathBuffer + BaseDefaultPath.Length);
    BaseDefaultPathAppend.Length = 0;
    BaseDefaultPathAppend.MaximumLength = BaseDefaultPath.MaximumLength - BaseDefaultPath.Length;

    RtlInitializeCriticalSection(&BaseDllDirectoryLock);
}

DECLSPEC_NORETURN
VOID
WINAPI
BaseThreadStartup(
    _In_ LPTHREAD_START_ROUTINE lpStartAddress,
    _In_ LPVOID lpParameter)
{
    RtlExitUserThread(lpStartAddress(lpParameter));
}

DECLSPEC_NORETURN
VOID
WINAPI
BaseProcessStartup(
    _In_ PPROCESS_START_ROUTINE lpStartAddress)
{
    RtlExitUserThread(lpStartAddress());
}
