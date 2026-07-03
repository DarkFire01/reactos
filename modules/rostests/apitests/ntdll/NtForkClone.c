/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test for NtCreateProcess address space cloning
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "precomp.h"

#define TEST_HEAP_VALUE  0xCA11AB1EUL
#define TEST_STACK_VALUE 0xDEADBEEFUL

START_TEST(NtForkClone)
{
    NTSTATUS Status;
    HANDLE ChildHandle = NULL;
    PVOID Buffer = NULL;
    SIZE_T RegionSize = 0x1000;
    volatile ULONG StackValue = TEST_STACK_VALUE;
    ULONG ReadValue;
    SIZE_T BytesRead;
    PROCESS_BASIC_INFORMATION BasicInfo;
    PEB ChildPeb;

    /* Write a known value into a committed private allocation */
    Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                     &Buffer,
                                     0,
                                     &RegionSize,
                                     MEM_RESERVE | MEM_COMMIT,
                                     PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;
    *(volatile ULONG *)Buffer = TEST_HEAP_VALUE;

    /* Clone the current process: no section, self as parent, inherit handles */
    Status = NtCreateProcess(&ChildHandle,
                             PROCESS_ALL_ACCESS,
                             NULL,
                             NtCurrentProcess(),
                             TRUE,
                             NULL,
                             NULL,
                             NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        goto Cleanup;
    ok(ChildHandle != NULL, "Child handle is NULL\n");

    /* The heap value must be present at the same address in the child */
    ReadValue = 0;
    Status = NtReadVirtualMemory(ChildHandle, Buffer, &ReadValue, sizeof(ReadValue), &BytesRead);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_hex(ReadValue, TEST_HEAP_VALUE);

    /* The stack value must be present as well */
    ReadValue = 0;
    Status = NtReadVirtualMemory(ChildHandle, (PVOID)&StackValue, &ReadValue, sizeof(ReadValue), &BytesRead);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_hex(ReadValue, TEST_STACK_VALUE);

    /* The child PEB must be a copy flagged as an inherited address space */
    RtlZeroMemory(&BasicInfo, sizeof(BasicInfo));
    Status = NtQueryInformationProcess(ChildHandle,
                                       ProcessBasicInformation,
                                       &BasicInfo,
                                       sizeof(BasicInfo),
                                       NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok(BasicInfo.PebBaseAddress != NULL, "Child PEB base is NULL\n");
    if (BasicInfo.PebBaseAddress != NULL)
    {
        RtlZeroMemory(&ChildPeb, sizeof(ChildPeb));
        Status = NtReadVirtualMemory(ChildHandle,
                                     BasicInfo.PebBaseAddress,
                                     &ChildPeb,
                                     sizeof(ChildPeb),
                                     &BytesRead);
        ok_ntstatus(Status, STATUS_SUCCESS);
        ok(ChildPeb.InheritedAddressSpace == TRUE,
           "InheritedAddressSpace = %u, expected 1\n", ChildPeb.InheritedAddressSpace);
        ok(ChildPeb.ImageBaseAddress == NtCurrentPeb()->ImageBaseAddress,
           "Child ImageBaseAddress %p != parent %p\n",
           ChildPeb.ImageBaseAddress, NtCurrentPeb()->ImageBaseAddress);
    }

Cleanup:
    if (ChildHandle != NULL)
    {
        NtTerminateProcess(ChildHandle, 0);
        NtClose(ChildHandle);
    }
    if (Buffer != NULL)
    {
        RegionSize = 0;
        NtFreeVirtualMemory(NtCurrentProcess(), &Buffer, &RegionSize, MEM_RELEASE);
    }
}
