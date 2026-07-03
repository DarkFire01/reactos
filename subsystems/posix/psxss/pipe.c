/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Anonymous pipes backed by an in-server ring buffer
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <ndk/exfuncs.h>

#define PSX_ENOMEM   12
#define PSX_EINVAL   22
#define PSX_EMFILE   24
#define PSX_EPIPE    32

#define PSX_PIPE_SIZE   4096

typedef struct _PSX_PIPE
{
    RTL_CRITICAL_SECTION Lock;
    LONG ReadRefs;
    LONG WriteRefs;
    ULONG Count;
    ULONG ReadPos;
    ULONG WritePos;
    HANDLE DataEvent;   // Set when data is buffered or all write ends closed
    HANDLE SpaceEvent;  // Set when space is free or all read ends closed
    UCHAR Buffer[PSX_PIPE_SIZE];
} PSX_PIPE, *PPSX_PIPE;

static
HANDLE
PsxCreatePipeEvent(VOID)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle = NULL;

    InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);
    NtCreateEvent(&Handle, EVENT_ALL_ACCESS, &ObjectAttributes, NotificationEvent, FALSE);
    return Handle;
}

/**
 * @brief pipe(), API 0x27.
 *
 * The read descriptor is returned at body offset 0x3C and the write descriptor at 0x40.
 */
VOID
PsxSrvPipe(
    _Inout_ PPSX_PROCESS Process,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    PPSX_PIPE Pipe;
    PPSX_FILE_OBJECT ReadEnd = NULL;
    PPSX_FILE_OBJECT WriteEnd = NULL;
    INT ReadFd;
    INT WriteFd;

    Pipe = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Pipe));
    ReadEnd = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*ReadEnd));
    WriteEnd = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*WriteEnd));
    if ((Pipe == NULL) || (ReadEnd == NULL) || (WriteEnd == NULL))
        goto NoMemory;

    Pipe->ReadRefs = 1;
    Pipe->WriteRefs = 1;
    RtlInitializeCriticalSection(&Pipe->Lock);
    Pipe->DataEvent = PsxCreatePipeEvent();
    Pipe->SpaceEvent = PsxCreatePipeEvent();
    if ((Pipe->DataEvent == NULL) || (Pipe->SpaceEvent == NULL))
        goto NoMemory;

    /* An empty pipe is writable */
    NtSetEvent(Pipe->SpaceEvent, NULL);

    /* The read end is O_RDONLY and the write end O_WRONLY */
    ReadEnd->RefCount = 1;
    ReadEnd->FileType = PSX_FILE_PIPE;
    ReadEnd->OpenFlags = 0;
    ReadEnd->Pipe = Pipe;

    WriteEnd->RefCount = 1;
    WriteEnd->FileType = PSX_FILE_PIPE;
    WriteEnd->OpenFlags = 1;
    WriteEnd->Pipe = Pipe;

    ReadFd = PsxAllocateFd(Process, ReadEnd);
    WriteFd = PsxAllocateFd(Process, WriteEnd);
    if ((ReadFd < 0) || (WriteFd < 0))
    {
        /* Installed ends are released through the fd table, the others directly */
        if (ReadFd >= 0)
        {
            PsxCloseFd(Process, ReadFd);
        }
        else
        {
            PsxPipeCloseEnd(ReadEnd);
            RtlFreeHeap(RtlGetProcessHeap(), 0, ReadEnd);
        }

        if (WriteFd >= 0)
        {
            PsxCloseFd(Process, WriteFd);
        }
        else
        {
            PsxPipeCloseEnd(WriteEnd);
            RtlFreeHeap(RtlGetProcessHeap(), 0, WriteEnd);
        }

        Message->Errno = PSX_EMFILE;
        Message->ReturnValue = -1;
        return;
    }

    Args[3] = (ULONG)ReadFd;
    Args[4] = (ULONG)WriteFd;
    Message->Errno = 0;
    Message->ReturnValue = 0;
    return;

NoMemory:
    if (ReadEnd != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, ReadEnd);

    if (WriteEnd != NULL)
        RtlFreeHeap(RtlGetProcessHeap(), 0, WriteEnd);

    if (Pipe != NULL)
    {
        if (Pipe->DataEvent != NULL)
            NtClose(Pipe->DataEvent);

        if (Pipe->SpaceEvent != NULL)
            NtClose(Pipe->SpaceEvent);

        RtlFreeHeap(RtlGetProcessHeap(), 0, Pipe);
    }

    Message->Errno = PSX_ENOMEM;
    Message->ReturnValue = -1;
}

/**
 * @brief Read from a pipe, blocking until data arrives or all write ends close.
 */
VOID
PsxPipeRead(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PPSX_PIPE Pipe = (PPSX_PIPE)File->Pipe;
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PUCHAR Bounce;
    ULONG Got = 0;
    ULONG Take;
    ULONG Index;

    if (Count == 0)
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    if (Count > PSX_PIPE_SIZE)
        Count = PSX_PIPE_SIZE;

    /* Heap allocated to keep the stack frame small */
    Bounce = RtlAllocateHeap(RtlGetProcessHeap(), 0, Count);
    if (Bounce == NULL)
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    for (;;)
    {
        RtlEnterCriticalSection(&Pipe->Lock);
        if (Pipe->Count > 0)
        {
            Take = (Count < Pipe->Count) ? Count : Pipe->Count;
            for (Index = 0; Index < Take; Index++)
            {
                Bounce[Index] = Pipe->Buffer[Pipe->ReadPos];
                Pipe->ReadPos = (Pipe->ReadPos + 1) % PSX_PIPE_SIZE;
            }

            Pipe->Count -= Take;
            Got = Take;
            NtSetEvent(Pipe->SpaceEvent, NULL);
            if (Pipe->Count == 0)
                NtClearEvent(Pipe->DataEvent);

            RtlLeaveCriticalSection(&Pipe->Lock);
            break;
        }

        /* No writers left, report end of file */
        if (Pipe->WriteRefs == 0)
        {
            RtlLeaveCriticalSection(&Pipe->Lock);
            break;
        }

        RtlLeaveCriticalSection(&Pipe->Lock);
        NtWaitForSingleObject(Pipe->DataEvent, FALSE, NULL);
    }

    if (Got > 0)
        NtWriteVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer, Bounce, Got, NULL);

    RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
    Message->Errno = 0;
    Message->ReturnValue = (LONG)Got;
}

/**
 * @brief Write to a pipe, blocking until space is available.
 *
 * Fails with EPIPE if all read ends are closed before anything is written.
 */
VOID
PsxPipeWrite(
    _In_ PPSX_PROCESS Process,
    _In_ PPSX_FILE_OBJECT File,
    _Inout_ PPSX_API_MESSAGE Message)
{
    PPSX_PIPE Pipe = (PPSX_PIPE)File->Pipe;
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PUCHAR Bounce;
    ULONG Total;
    ULONG Done = 0;
    NTSTATUS Status;

    if (Count == 0)
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    Total = (Count < PSX_PIPE_SIZE) ? Count : PSX_PIPE_SIZE;

    /* Heap allocated to keep the stack frame small */
    Bounce = RtlAllocateHeap(RtlGetProcessHeap(), 0, Total);
    if (Bounce == NULL)
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    Status = NtReadVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer, Bounce, Total, NULL);
    if (!NT_SUCCESS(Status))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    while (Done < Total)
    {
        RtlEnterCriticalSection(&Pipe->Lock);

        /* TODO: raise SIGPIPE as well */
        if (Pipe->ReadRefs == 0)
        {
            RtlLeaveCriticalSection(&Pipe->Lock);
            if (Done == 0)
            {
                RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
                Message->Errno = PSX_EPIPE;
                Message->ReturnValue = -1;
                return;
            }
            break;
        }

        if (Pipe->Count < PSX_PIPE_SIZE)
        {
            while ((Done < Total) && (Pipe->Count < PSX_PIPE_SIZE))
            {
                Pipe->Buffer[Pipe->WritePos] = Bounce[Done++];
                Pipe->WritePos = (Pipe->WritePos + 1) % PSX_PIPE_SIZE;
                Pipe->Count++;
            }

            NtSetEvent(Pipe->DataEvent, NULL);
            if (Pipe->Count == PSX_PIPE_SIZE)
                NtClearEvent(Pipe->SpaceEvent);

            RtlLeaveCriticalSection(&Pipe->Lock);
            continue;
        }

        RtlLeaveCriticalSection(&Pipe->Lock);
        NtWaitForSingleObject(Pipe->SpaceEvent, FALSE, NULL);
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
    Message->Errno = 0;
    Message->ReturnValue = (LONG)Done;
}

/**
 * @brief Check whether a read on the pipe would complete without blocking.
 */
BOOLEAN
PsxPipeReady(
    _In_ PPSX_FILE_OBJECT File)
{
    PPSX_PIPE Pipe = (PPSX_PIPE)File->Pipe;
    BOOLEAN Ready;

    if (Pipe == NULL)
        return TRUE;

    RtlEnterCriticalSection(&Pipe->Lock);
    Ready = (Pipe->Count > 0) || (Pipe->WriteRefs == 0);
    RtlLeaveCriticalSection(&Pipe->Lock);
    return Ready;
}

/**
 * @brief Release one end of a pipe, waking the other end and freeing the pipe once both are closed.
 */
VOID
PsxPipeCloseEnd(
    _In_ PPSX_FILE_OBJECT File)
{
    PPSX_PIPE Pipe = (PPSX_PIPE)File->Pipe;
    BOOLEAN FreePipe;

    if (Pipe == NULL)
        return;

    RtlEnterCriticalSection(&Pipe->Lock);
    if ((File->OpenFlags & 7) == 0)
    {
        /* Read end: wake blocked writers so they fail with EPIPE */
        Pipe->ReadRefs--;
        if (Pipe->ReadRefs == 0)
            NtSetEvent(Pipe->SpaceEvent, NULL);
    }
    else
    {
        /* Write end: wake blocked readers so they see end of file */
        Pipe->WriteRefs--;
        if (Pipe->WriteRefs == 0)
            NtSetEvent(Pipe->DataEvent, NULL);
    }

    FreePipe = (Pipe->ReadRefs <= 0) && (Pipe->WriteRefs <= 0);
    RtlLeaveCriticalSection(&Pipe->Lock);

    if (FreePipe)
    {
        if (Pipe->DataEvent != NULL)
            NtClose(Pipe->DataEvent);

        if (Pipe->SpaceEvent != NULL)
            NtClose(Pipe->SpaceEvent);

        RtlDeleteCriticalSection(&Pipe->Lock);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Pipe);
    }
}
