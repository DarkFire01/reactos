/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Process identity, close and _exit system calls
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

/**
 * @brief Fetches the identity bundle (pid, ppid, pgrp, uid, gid...) from the server.
 * This request cannot fail.
 */
static
VOID
PsxGetIds(
    _Out_ PSX_IDS_REPLY *Ids)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiGetIds, PSX_BODY_DATALEN(sizeof(*Ids)));
    NtRequestWaitReplyPort(PsxApiPort, &Message.Header, &Message.Header);
    *Ids = Message.Data.Ids;
}

int
__cdecl
getpid(void)
{
    PSX_IDS_REPLY Ids;

    PsxGetIds(&Ids);
    return (int)Ids.Pid;
}

int
__cdecl
getppid(void)
{
    PSX_IDS_REPLY Ids;

    PsxGetIds(&Ids);
    return (int)Ids.ParentPid;
}

int
__cdecl
getpgrp(void)
{
    PSX_IDS_REPLY Ids;

    PsxGetIds(&Ids);
    return (int)Ids.ProcessGroup;
}

int
__cdecl
getuid(void)
{
    PSX_IDS_REPLY Ids;

    PsxGetIds(&Ids);
    return (int)Ids.Uid;
}

int
__cdecl
geteuid(void)
{
    PSX_IDS_REPLY Ids;

    PsxGetIds(&Ids);
    return (int)Ids.EffectiveUid;
}

int
__cdecl
getgid(void)
{
    PSX_IDS_REPLY Ids;

    PsxGetIds(&Ids);
    return (int)Ids.Gid;
}

int
__cdecl
getegid(void)
{
    PSX_IDS_REPLY Ids;

    PsxGetIds(&Ids);
    return (int)Ids.EffectiveGid;
}

int
__cdecl
close(
    _In_ int FileDescriptor)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiClose, PSX_BODY_DATALEN(sizeof(ULONG)));
    Message.Data.ReadWrite.FileDescriptor = (ULONG)FileDescriptor;
    return (int)PsxCallServer(&Message);
}

/**
 * @brief Tells the server the process is exiting, then terminates it.
 */
VOID
__cdecl
_exit(
    _In_ int Status)
{
    PSX_API_MESSAGE Message;

    PsxInitMessage(&Message, PsxApiExit, PSX_BODY_DATALEN(sizeof(ULONG)));
    Message.Data.Raw[0] = (UCHAR)Status;
    NtRequestWaitReplyPort(PsxApiPort, &Message.Header, &Message.Header);

    NtTerminateProcess(NtCurrentProcess(), (NTSTATUS)Status);
    for (;;)
    {
        /* Not reached */
    }
}
