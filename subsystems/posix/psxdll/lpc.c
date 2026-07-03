/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXDLL request thunk: every control syscall funnels through here.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

//
// errno values used on the wire (1:1 with <errno.h> / the server's Errno field).
//
#define PSX_EINTR   4
#define PSX_EIO     5

//
// Send Message to \PSXSS\ApiPort and wait for the reply. On EINTR + retry tag
// the request is resent (the canonical NT 4.0 pattern). Returns the POSIX
// return value, or -1 with errno set on failure.
//
LONG
PsxCallServer(IN OUT PPSX_API_MESSAGE Message)
{
    NTSTATUS Status;

    do
    {
        Status = NtRequestWaitReplyPort(PsxApiPort,
                                        &Message->Header,
                                        &Message->Header);
        if (!NT_SUCCESS(Status))
        {
            PsxSetErrno(PSX_EIO);
            return -1;
        }
    }
    while (Message->Errno == PSX_EINTR && Message->RetryTag == PSX_RETRY_TAG);

    if (Message->Errno != 0)
    {
        PsxSetErrno(Message->Errno);
        return -1;
    }

    return Message->ReturnValue;
}
