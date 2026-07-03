/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Request path to the POSIX server API port
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxdllp.h"

#define PSX_EINTR   4
#define PSX_EIO     5

/**
 * @brief Sends a request to the server and waits for the reply, resending it
 * while the server asks for a retry after EINTR.
 *
 * @return The POSIX return value, or -1 with errno set.
 */
LONG
PsxCallServer(
    _Inout_ PPSX_API_MESSAGE Message)
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
