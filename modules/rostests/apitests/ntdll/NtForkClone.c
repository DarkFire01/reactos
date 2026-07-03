/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     See COPYING in the top level directory
 * PURPOSE:     Test for NtCreateProcess address-space cloning (fork support)
 * PROGRAMMER:  POSIX-on-NT project
 *
 * Exercises the kernel fork/clone path: NtCreateProcess() with a NULL section
 * and the current process as parent must produce a child whose private address
 * space and PEB are copies of the parent's. This used to hit
 * ASSERTMSG("No support for cloning yet") in PspCreateProcess. The test runs no
 * child thread; it verifies the clone from the parent via NtReadVirtualMemory.
 */

#include "precomp.h"

#define SENTINEL_HEAP  0xCA11AB1EUL
#define SENTINEL_STACK 0xDEADBEEFUL

START_TEST(NtForkClone)
{
    NTSTATUS Status;
    HANDLE ChildHandle = NULL;
    PVOID Buffer = NULL;
    SIZE_T RegionSize = 0x1000;
    volatile ULONG StackSentinel = SENTINEL_STACK;
    ULONG ReadValue;
    SIZE_T BytesRead;
    PROCESS_BASIC_INFORMATION BasicInfo;
    PEB ChildPeb;

    /* Stamp a sentinel into a committed, allocation-granularity private VAD */
    Status = NtAllocateVirtualMemory(NtCurrentProcess(),
                                     &Buffer,
                                     0,
                                     &RegionSize,
                                     MEM_RESERVE | MEM_COMMIT,
                                     PAGE_READWRITE);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) return;
    *(volatile ULONG *)Buffer = SENTINEL_HEAP;

    /* Clone this process: NULL section + self as parent + inherit handles.
     * This is exactly how the POSIX subsystem requests a fork. */
    Status = NtCreateProcess(&ChildHandle,
                             PROCESS_ALL_ACCESS,
                             NULL,
                             NtCurrentProcess(),
                             TRUE,
                             NULL,
                             NULL,
                             NULL);
    ok_ntstatus(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status)) goto Cleanup;
    ok(ChildHandle != NULL, "Child handle is NULL\n");

    /* The committed heap sentinel must have been cloned at the same VA */
    ReadValue = 0;
    Status = NtReadVirtualMemory(ChildHandle, Buffer, &ReadValue, sizeof(ReadValue), &BytesRead);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_hex(ReadValue, SENTINEL_HEAP);

    /* The stack sentinel (in the cloned stack VAD) must also be present */
    ReadValue = 0;
    Status = NtReadVirtualMemory(ChildHandle, (PVOID)&StackSentinel, &ReadValue, sizeof(ReadValue), &BytesRead);
    ok_ntstatus(Status, STATUS_SUCCESS);
    ok_hex(ReadValue, SENTINEL_STACK);

    /* The child PEB must have been cloned and flagged as an inherited space */
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
