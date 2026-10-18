/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test for NtAlertThreadByThreadId / NtWaitForAlertByThreadId
 * COPYRIGHT:   Copyright 2025 <justin.miller@reactos.org>
 */

#include "precomp.h"

typedef
NTSTATUS
(NTAPI *FN_NtAlertThreadByThreadId)(
    _In_ HANDLE ThreadId);

typedef
NTSTATUS
(NTAPI *FN_NtWaitForAlertByThreadId)(
    _In_opt_ PVOID Address,
    _In_opt_ PLARGE_INTEGER Timeout);

static FN_NtAlertThreadByThreadId pfnNtAlertThreadByThreadId;
static FN_NtWaitForAlertByThreadId pfnNtWaitForAlertByThreadId;

static volatile NTSTATUS WorkerStatus;

static
HANDLE
ThreadIdToHandle(
    _In_ DWORD ThreadId)
{
    return (HANDLE)(ULONG_PTR)ThreadId;
}

static
DWORD
WINAPI
BlockedWaiterThread(
    _In_ PVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);

    WorkerStatus = pfnNtWaitForAlertByThreadId(NULL, NULL);
    return 0;
}

static
DWORD
WINAPI
AlertOwnerThread(
    _In_ PVOID Parameter)
{
    /* Give the owner time to block before it gets alerted */
    Sleep(100);
    WorkerStatus = pfnNtAlertThreadByThreadId(Parameter);
    return 0;
}

static
VOID
TestInvalidTargets(VOID)
{
    WCHAR ImagePath[MAX_PATH];
    STARTUPINFOW StartupInfo;
    PROCESS_INFORMATION ProcessInfo;
    NTSTATUS Status;

    Status = pfnNtAlertThreadByThreadId(NULL);
    ok_eq_hex(Status, STATUS_INVALID_CID);

    Status = pfnNtAlertThreadByThreadId((HANDLE)(ULONG_PTR)0xFFFFFFF0);
    ok_eq_hex(Status, STATUS_INVALID_CID);

    /* Threads of other processes are refused */
    if (!GetModuleFileNameW(NULL, ImagePath, _countof(ImagePath)))
    {
        skip("GetModuleFileNameW failed with %lu\n", GetLastError());
        return;
    }

    ZeroMemory(&StartupInfo, sizeof(StartupInfo));
    StartupInfo.cb = sizeof(StartupInfo);
    if (!CreateProcessW(ImagePath, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED,
                        NULL, NULL, &StartupInfo, &ProcessInfo))
    {
        skip("CreateProcessW failed with %lu\n", GetLastError());
        return;
    }

    Status = pfnNtAlertThreadByThreadId(ThreadIdToHandle(ProcessInfo.dwThreadId));
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);

    TerminateProcess(ProcessInfo.hProcess, 0);
    WaitForSingleObject(ProcessInfo.hProcess, INFINITE);
    CloseHandle(ProcessInfo.hThread);
    CloseHandle(ProcessInfo.hProcess);
}

static
VOID
TestTimeouts(VOID)
{
    LARGE_INTEGER Timeout;
    NTSTATUS Status;
    DWORD Start;
    DWORD Elapsed;

    Timeout.QuadPart = 0;
    Status = pfnNtWaitForAlertByThreadId(NULL, &Timeout);
    ok_eq_hex(Status, STATUS_TIMEOUT);

    /* Absolute time already in the past */
    Timeout.QuadPart = 1;
    Status = pfnNtWaitForAlertByThreadId(NULL, &Timeout);
    ok_eq_hex(Status, STATUS_TIMEOUT);

    /* 50 ms relative */
    Timeout.QuadPart = -50 * 10000;
    Start = GetTickCount();
    Status = pfnNtWaitForAlertByThreadId(&Timeout, &Timeout);
    Elapsed = GetTickCount() - Start;
    ok_eq_hex(Status, STATUS_TIMEOUT);
    ok(Elapsed >= 30, "Wait returned after %lu ms\n", Elapsed);
}

static
VOID
TestPendingAlert(VOID)
{
    HANDLE Self;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    Self = ThreadIdToHandle(GetCurrentThreadId());
    Timeout.QuadPart = 0;

    /* An alert sent before the wait is kept until the next wait */
    Status = pfnNtAlertThreadByThreadId(Self);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = pfnNtWaitForAlertByThreadId(NULL, &Timeout);
    ok_eq_hex(Status, STATUS_ALERTED);
    Status = pfnNtWaitForAlertByThreadId(NULL, &Timeout);
    ok_eq_hex(Status, STATUS_TIMEOUT);

    /* Repeated alerts collapse into one */
    pfnNtAlertThreadByThreadId(Self);
    pfnNtAlertThreadByThreadId(Self);
    pfnNtAlertThreadByThreadId(Self);
    Status = pfnNtWaitForAlertByThreadId(NULL, &Timeout);
    ok_eq_hex(Status, STATUS_ALERTED);
    Status = pfnNtWaitForAlertByThreadId(NULL, &Timeout);
    ok_eq_hex(Status, STATUS_TIMEOUT);

    /* The hint does not have to match anything */
    pfnNtAlertThreadByThreadId(Self);
    Status = pfnNtWaitForAlertByThreadId((PVOID)&Timeout, NULL);
    ok_eq_hex(Status, STATUS_ALERTED);
}

static
VOID
TestCrossThread(VOID)
{
    HANDLE hThread;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;
    DWORD ThreadId;
    DWORD WaitResult;

    /* Wake a thread that is blocked with no timeout */
    WorkerStatus = STATUS_PENDING;
    hThread = CreateThread(NULL, 0, BlockedWaiterThread, NULL, 0, &ThreadId);
    ok(hThread != NULL, "CreateThread failed with %lu\n", GetLastError());
    if (!hThread)
        return;

    WaitResult = WaitForSingleObject(hThread, 200);
    ok_eq_ulong(WaitResult, (DWORD)WAIT_TIMEOUT);
    ok_eq_hex(WorkerStatus, STATUS_PENDING);

    Status = pfnNtAlertThreadByThreadId(ThreadIdToHandle(ThreadId));
    ok_eq_hex(Status, STATUS_SUCCESS);

    WaitResult = WaitForSingleObject(hThread, 5000);
    ok_eq_ulong(WaitResult, (DWORD)WAIT_OBJECT_0);
    ok_eq_hex(WorkerStatus, STATUS_ALERTED);
    CloseHandle(hThread);

    /* Get woken by another thread before a long timeout expires */
    WorkerStatus = STATUS_PENDING;
    hThread = CreateThread(NULL, 0, AlertOwnerThread,
                           ThreadIdToHandle(GetCurrentThreadId()), 0, NULL);
    ok(hThread != NULL, "CreateThread failed with %lu\n", GetLastError());
    if (!hThread)
        return;

    Timeout.QuadPart = -5000LL * 10000;
    Status = pfnNtWaitForAlertByThreadId(NULL, &Timeout);
    ok_eq_hex(Status, STATUS_ALERTED);

    WaitForSingleObject(hThread, INFINITE);
    ok_eq_hex(WorkerStatus, STATUS_SUCCESS);
    CloseHandle(hThread);
}

START_TEST(NtAlertThreadByThreadId)
{
    HMODULE hNtDll;

    hNtDll = GetModuleHandleW(L"ntdll.dll");
    pfnNtAlertThreadByThreadId = (FN_NtAlertThreadByThreadId)GetProcAddress(hNtDll, "NtAlertThreadByThreadId");
    pfnNtWaitForAlertByThreadId = (FN_NtWaitForAlertByThreadId)GetProcAddress(hNtDll, "NtWaitForAlertByThreadId");
    if (!pfnNtAlertThreadByThreadId || !pfnNtWaitForAlertByThreadId)
    {
        skip("Thread ID alert functions not found in ntdll.dll\n");
        return;
    }

    TestInvalidTargets();
    TestTimeouts();
    TestPendingAlert();
    TestCrossThread();
}
