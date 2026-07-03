/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Representative PSXDLL syscalls (the skeleton's worked examples).
 *              The remaining ~110 POSIX entries follow the same patterns.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

//
// Identity bundle -- ApiNumber 0x0A. One server call returns pid/ppid/uid/...;
// each wrapper reads a different reply slot. These calls cannot fail.
//
static VOID
PsxGetIds(OUT PSX_IDS_REPLY *Ids)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiGetIds, PSX_BODY_DATALEN(sizeof(PSX_IDS_REPLY)));
    NtRequestWaitReplyPort(PsxApiPort, &Message.Header, &Message.Header);
    *Ids = Message.Data.Ids;
}

int __cdecl getpid(void)   { PSX_IDS_REPLY i; PsxGetIds(&i); return (int)i.Pid; }
int __cdecl getppid(void)  { PSX_IDS_REPLY i; PsxGetIds(&i); return (int)i.ParentPid; }
int __cdecl getpgrp(void)  { PSX_IDS_REPLY i; PsxGetIds(&i); return (int)i.ProcessGroup; }
int __cdecl getuid(void)   { PSX_IDS_REPLY i; PsxGetIds(&i); return (int)i.Uid; }
int __cdecl geteuid(void)  { PSX_IDS_REPLY i; PsxGetIds(&i); return (int)i.EffectiveUid; }
int __cdecl getgid(void)   { PSX_IDS_REPLY i; PsxGetIds(&i); return (int)i.Gid; }
int __cdecl getegid(void)  { PSX_IDS_REPLY i; PsxGetIds(&i); return (int)i.EffectiveGid; }

//
// close(fd) -- ApiNumber 0x2A.
//
int __cdecl
close(int FileDescriptor)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiClose, PSX_BODY_DATALEN(sizeof(ULONG)));
    Message.Data.ReadWrite.FileDescriptor = (ULONG)FileDescriptor;
    return (int)PsxCallServer(&Message);
}

//
// _exit(status) -- ApiNumber 0x03. Tells the server to tear down, then makes
// sure the process really dies.
//
VOID __cdecl
_exit(int Status)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiExit, PSX_BODY_DATALEN(sizeof(ULONG)));
    Message.Data.Raw[0] = (UCHAR)Status;
    NtRequestWaitReplyPort(PsxApiPort, &Message.Header, &Message.Header);

    NtTerminateProcess(NtCurrentProcess(), (NTSTATUS)Status);
    for (;;) { /* unreachable */ }
}
