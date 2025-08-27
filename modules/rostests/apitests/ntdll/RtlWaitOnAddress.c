/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test for WaitOnAddress / WakeByAddress APIs
 * COPYRIGHT:   Copyright 2025 <justin.miller@reactos.org>
 */

#include "precomp.h"

#define WAKE_ALL_THREAD_COUNT 4

typedef
NTSTATUS
(NTAPI *FN_RtlWaitOnAddress)(
    _In_ const volatile VOID *Address,
    _In_ PVOID CompareAddress,
    _In_ SIZE_T AddressSize,
    _In_opt_ PLARGE_INTEGER Timeout);

typedef
VOID
(NTAPI *FN_RtlWakeAddress)(
    _In_ const volatile VOID *Address);

static FN_RtlWaitOnAddress pfnRtlWaitOnAddress;
static FN_RtlWakeAddress pfnRtlWakeAddressSingle;
static FN_RtlWakeAddress pfnRtlWakeAddressAll;

static volatile LONG TestValue;
static volatile LONG WokenCount;
static volatile NTSTATUS WorkerStatus;

static
DWORD
WINAPI
SingleWakeThread(
    _In_ PVOID Parameter)
{
    LONG CompareValue = 0;

    UNREFERENCED_PARAMETER(Parameter);

    WorkerStatus = pfnRtlWaitOnAddress(&TestValue, &CompareValue, sizeof(TestValue), NULL);
    if (TestValue == 1)
        InterlockedIncrement(&WokenCount);

    return 0;
}

static
DWORD
WINAPI
TimeoutThread(
    _In_ PVOID Parameter)
{
    LONG CompareValue = 0;
    LARGE_INTEGER Timeout;

    UNREFERENCED_PARAMETER(Parameter);

    /* 100 ms */
    Timeout.QuadPart = -100 * 10000;
    WorkerStatus = pfnRtlWaitOnAddress(&TestValue, &CompareValue, sizeof(TestValue), &Timeout);

    return 0;
}

static
DWORD
WINAPI
WakeAllThread(
    _In_ PVOID Parameter)
{
    LONG CompareValue = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Parameter);

    Status = pfnRtlWaitOnAddress(&TestValue, &CompareValue, sizeof(TestValue), NULL);
    if (NT_SUCCESS(Status) && TestValue == 1)
        InterlockedIncrement(&WokenCount);

    return 0;
}

START_TEST(RtlWaitOnAddress)
{
    HMODULE hNtDll;
    HANDLE hThread;
    HANDLE hThreads[WAKE_ALL_THREAD_COUNT];
    LONG CompareValue;
    NTSTATUS Status;
    ULONG i;

    hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtDll)
    {
        skip("GetModuleHandleW for ntdll failed with %lu\n", GetLastError());
        return;
    }

    pfnRtlWaitOnAddress = (FN_RtlWaitOnAddress)GetProcAddress(hNtDll, "RtlWaitOnAddress");
    pfnRtlWakeAddressSingle = (FN_RtlWakeAddress)GetProcAddress(hNtDll, "RtlWakeAddressSingle");
    pfnRtlWakeAddressAll = (FN_RtlWakeAddress)GetProcAddress(hNtDll, "RtlWakeAddressAll");
    if (!pfnRtlWaitOnAddress || !pfnRtlWakeAddressSingle || !pfnRtlWakeAddressAll)
    {
        skip("RtlWaitOnAddress functions not found in ntdll.dll\n");
        return;
    }

    /* Bad sizes are rejected */
    InterlockedExchange(&TestValue, 0);
    CompareValue = 0;
    Status = pfnRtlWaitOnAddress(&TestValue, &CompareValue, 0, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = pfnRtlWaitOnAddress(&TestValue, &CompareValue, 3, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
    Status = pfnRtlWaitOnAddress(&TestValue, &CompareValue, 16, NULL);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    /* Values that already differ return without waiting */
    CompareValue = 1;
    Status = pfnRtlWaitOnAddress(&TestValue, &CompareValue, sizeof(TestValue), NULL);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Single wake */
    InterlockedExchange(&TestValue, 0);
    InterlockedExchange(&WokenCount, 0);
    WorkerStatus = STATUS_PENDING;
    hThread = CreateThread(NULL, 0, SingleWakeThread, NULL, 0, NULL);
    ok(hThread != NULL, "CreateThread failed with %lu\n", GetLastError());
    Sleep(100);

    InterlockedExchange(&TestValue, 1);
    pfnRtlWakeAddressSingle(&TestValue);
    WaitForSingleObject(hThread, INFINITE);
    ok_eq_long(WokenCount, 1L);
    ok_eq_hex(WorkerStatus, STATUS_SUCCESS);
    CloseHandle(hThread);

    /* Timeout */
    InterlockedExchange(&TestValue, 0);
    WorkerStatus = STATUS_PENDING;
    hThread = CreateThread(NULL, 0, TimeoutThread, NULL, 0, NULL);
    ok(hThread != NULL, "CreateThread failed with %lu\n", GetLastError());
    WaitForSingleObject(hThread, INFINITE);
    ok_eq_hex(WorkerStatus, STATUS_TIMEOUT);
    CloseHandle(hThread);

    /* Wake all */
    InterlockedExchange(&TestValue, 0);
    InterlockedExchange(&WokenCount, 0);
    for (i = 0; i < WAKE_ALL_THREAD_COUNT; i++)
    {
        hThreads[i] = CreateThread(NULL, 0, WakeAllThread, NULL, 0, NULL);
        ok(hThreads[i] != NULL, "CreateThread %lu failed with %lu\n", i, GetLastError());
    }
    Sleep(200);

    InterlockedExchange(&TestValue, 1);
    pfnRtlWakeAddressAll(&TestValue);
    WaitForMultipleObjects(WAKE_ALL_THREAD_COUNT, hThreads, TRUE, INFINITE);
    ok_eq_long(WokenCount, (LONG)WAKE_ALL_THREAD_COUNT);

    for (i = 0; i < WAKE_ALL_THREAD_COUNT; i++)
        CloseHandle(hThreads[i]);
}
