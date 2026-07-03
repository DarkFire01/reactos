/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Anonymous pipes -- pipe() (ApiNumber 0x27) plus the pipe
 *              file-object read/write ops. A pipe is an in-server ring buffer
 *              with a read end (O_RDONLY file object) and a write end (O_WRONLY);
 *              bytes move to/from the client via NtRead/WriteVirtualMemory, the
 *              same as disk I/O. Faithful in spirit to the NT 4.0 pipe handler
 *              (sub_1F48840) + the pipe vtables.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"
#include <ndk/exfuncs.h>    // NtCreateEvent / NtSetEvent / NtClearEvent / NtWaitForSingleObject

#define PSX_ENOMEM   12
#define PSX_EMFILE   24
#define PSX_EPIPE    32
#define PSX_EINVAL   22

#define PSX_PIPE_SIZE   4096

typedef struct _PSX_PIPE
{
    RTL_CRITICAL_SECTION Lock;
    LONG   ReadRefs;            // open read ends
    LONG   WriteRefs;           // open write ends
    ULONG  Count;              // bytes currently buffered
    ULONG  ReadPos;
    ULONG  WritePos;
    HANDLE DataEvent;          // signalled when data is available / write end closed
    HANDLE SpaceEvent;         // signalled when space is available / read end closed
    UCHAR  Buffer[PSX_PIPE_SIZE];
} PSX_PIPE, *PPSX_PIPE;

static HANDLE
PsxCreateGate(VOID)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    HANDLE Handle = NULL;

    InitializeObjectAttributes(&ObjectAttributes, NULL, 0, NULL, NULL);
    NtCreateEvent(&Handle, EVENT_ALL_ACCESS, &ObjectAttributes, NotificationEvent, FALSE);
    return Handle;
}

//
// pipe() -- ApiNumber 0x27. Returns the read fd at body +0x3C and the write fd
// at +0x40 (Arg[3] / Arg[4]).
//
VOID
PsxSrvPipe(IN PPSX_PROCESS Process, IN OUT PPSX_API_MESSAGE Message)
{
    PULONG Args = (PULONG)Message->Data.Raw;
    PPSX_PIPE Pipe;
    PPSX_FILE_OBJECT ReadEnd = NULL;
    PPSX_FILE_OBJECT WriteEnd = NULL;
    INT ReadFd;
    INT WriteFd;

    Pipe = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PSX_PIPE));
    ReadEnd = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PSX_FILE_OBJECT));
    WriteEnd = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PSX_FILE_OBJECT));
    if ((Pipe == NULL) || (ReadEnd == NULL) || (WriteEnd == NULL))
        goto NoMemory;

    Pipe->ReadRefs = 1;
    Pipe->WriteRefs = 1;
    RtlInitializeCriticalSection(&Pipe->Lock);
    Pipe->DataEvent = PsxCreateGate();
    Pipe->SpaceEvent = PsxCreateGate();
    if ((Pipe->DataEvent == NULL) || (Pipe->SpaceEvent == NULL))
        goto NoMemory;
    NtSetEvent(Pipe->SpaceEvent, NULL);     // starts empty -> writable

    ReadEnd->RefCount = 1;
    ReadEnd->FileType = PSX_FILE_PIPE;
    ReadEnd->OpenFlags = 0;                  // O_RDONLY -> read end
    ReadEnd->Pipe = Pipe;

    WriteEnd->RefCount = 1;
    WriteEnd->FileType = PSX_FILE_PIPE;
    WriteEnd->OpenFlags = 1;                 // O_WRONLY -> write end
    WriteEnd->Pipe = Pipe;

    ReadFd = PsxAllocateFd(Process, ReadEnd);
    WriteFd = PsxAllocateFd(Process, WriteEnd);
    if ((ReadFd < 0) || (WriteFd < 0))
    {
        // An installed end is released via the fd table; an end that didn't fit
        // is torn down directly (drop its pipe ref, then free the file object).
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

    Args[3] = (ULONG)ReadFd;        // body +0x3C
    Args[4] = (ULONG)WriteFd;       // body +0x40
    Message->Errno = 0;
    Message->ReturnValue = 0;
    return;

NoMemory:
    if (ReadEnd != NULL) RtlFreeHeap(RtlGetProcessHeap(), 0, ReadEnd);
    if (WriteEnd != NULL) RtlFreeHeap(RtlGetProcessHeap(), 0, WriteEnd);
    if (Pipe != NULL)
    {
        if (Pipe->DataEvent != NULL) NtClose(Pipe->DataEvent);
        if (Pipe->SpaceEvent != NULL) NtClose(Pipe->SpaceEvent);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Pipe);
    }
    Message->Errno = PSX_ENOMEM;
    Message->ReturnValue = -1;
}

//
// Read from a pipe: block until data arrives or all write ends close (EOF).
//
VOID
PsxPipeRead(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File, IN OUT PPSX_API_MESSAGE Message)
{
    PPSX_PIPE Pipe = (PPSX_PIPE)File->Pipe;
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PUCHAR Bounce;          // heap, not stack -- keeps the frame under a page
    ULONG Got = 0;

    if (Count == 0)
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }
    if (Count > PSX_PIPE_SIZE)
        Count = PSX_PIPE_SIZE;

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
            ULONG Take = (Count < Pipe->Count) ? Count : Pipe->Count;
            ULONG Index;
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
        if (Pipe->WriteRefs == 0)       // no writers left -> EOF
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

//
// Write to a pipe: block until space is available; EPIPE if all read ends close.
//
VOID
PsxPipeWrite(IN PPSX_PROCESS Process, IN PPSX_FILE_OBJECT File, IN OUT PPSX_API_MESSAGE Message)
{
    PPSX_PIPE Pipe = (PPSX_PIPE)File->Pipe;
    ULONG_PTR ClientBuffer = Message->Data.ReadWrite.Buffer;
    ULONG Count = Message->Data.ReadWrite.Count;
    PUCHAR Bounce;          // heap, not stack -- keeps the frame under a page
    ULONG Total;
    ULONG Done = 0;

    if (Count == 0)
    {
        Message->Errno = 0;
        Message->ReturnValue = 0;
        return;
    }

    Total = (Count < PSX_PIPE_SIZE) ? Count : PSX_PIPE_SIZE;
    Bounce = RtlAllocateHeap(RtlGetProcessHeap(), 0, Total);
    if (Bounce == NULL)
    {
        Message->Errno = PSX_ENOMEM;
        Message->ReturnValue = -1;
        return;
    }

    if (!NT_SUCCESS(NtReadVirtualMemory(Process->ProcessHandle, (PVOID)ClientBuffer,
                                        Bounce, Total, NULL)))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Bounce);
        Message->Errno = PSX_EINVAL;
        Message->ReturnValue = -1;
        return;
    }

    while (Done < Total)
    {
        RtlEnterCriticalSection(&Pipe->Lock);
        if (Pipe->ReadRefs == 0)        // no readers -> EPIPE (TODO: also SIGPIPE)
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

//
// Readability for poll()/select(): a read end is "ready" if bytes are buffered or all
// write ends have closed (a read would return EOF without blocking).
//
BOOLEAN
PsxPipeReady(IN PPSX_FILE_OBJECT File)
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

//
// One end of a pipe is closing (its file object hit refcount 0). Drop the end's
// reference, wake the opposite end, and free the pipe once both ends are gone.
//
VOID
PsxPipeCloseEnd(IN PPSX_FILE_OBJECT File)
{
    PPSX_PIPE Pipe = (PPSX_PIPE)File->Pipe;
    BOOLEAN FreePipe;

    if (Pipe == NULL)
        return;

    RtlEnterCriticalSection(&Pipe->Lock);
    if ((File->OpenFlags & 7) == 0)         // read end
    {
        Pipe->ReadRefs--;
        if (Pipe->ReadRefs == 0)
            NtSetEvent(Pipe->SpaceEvent, NULL);     // wake blocked writers -> EPIPE
    }
    else                                    // write end
    {
        Pipe->WriteRefs--;
        if (Pipe->WriteRefs == 0)
            NtSetEvent(Pipe->DataEvent, NULL);      // wake blocked readers -> EOF
    }
    FreePipe = (Pipe->ReadRefs <= 0) && (Pipe->WriteRefs <= 0);
    RtlLeaveCriticalSection(&Pipe->Lock);

    if (FreePipe)
    {
        if (Pipe->DataEvent != NULL) NtClose(Pipe->DataEvent);
        if (Pipe->SpaceEvent != NULL) NtClose(Pipe->SpaceEvent);
        RtlDeleteCriticalSection(&Pipe->Lock);
        RtlFreeHeap(RtlGetProcessHeap(), 0, Pipe);
    }
}
