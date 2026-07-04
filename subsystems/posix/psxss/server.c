/*
 * PROJECT:     ReactOS POSIX+ Environment Subsystem
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     PSXSS LPC server core -- the worker-thread dispatch loop.
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "psxss.h"

#define PSX_ENOSYS  40

//
// The textbook NT LPC server: block in NtReplyWaitReceivePort, classify the
// PORT_MESSAGE by type, and for an ordinary request read the opcode and call
// g_OpHandlers[opcode]. The handler fills the reply in-place; the reply is then
// fused into the next receive.
//
VOID NTAPI
PsxApiServerLoop(PVOID Parameter)
{
    PSX_API_MESSAGE Request;
    PPSX_API_MESSAGE Reply = NULL;
    PVOID PortContext = NULL;
    NTSTATUS Status;
    ULONG ApiNumber;

    UNREFERENCED_PARAMETER(Parameter);

    PSXTRACE("ApiServerLoop: worker started, waiting on \\PSXSS\\ApiPort\n");

    for (;;)
    {
        Status = NtReplyWaitReceivePort(g_ApiPort,
                                        &PortContext,
                                        (Reply != NULL) ? &Reply->Header : NULL,
                                        &Request.Header);
        if (!NT_SUCCESS(Status))
        {
            PSXTRACE("ApiServerLoop: NtReplyWaitReceivePort -> 0x%08lx\n", Status);
            Reply = NULL;
            continue;
        }

        switch (Request.Header.u2.s2.Type & 0x000000FF)
        {
            case LPC_CONNECTION_REQUEST:
                PSXTRACE("ApiServerLoop: connection request from cid %p.%p\n",
                         Request.Header.ClientId.UniqueProcess,
                         Request.Header.ClientId.UniqueThread);
                PsxAcceptConnection(&Request.Header);
                Reply = NULL;
                continue;

            case LPC_REQUEST:
                ApiNumber = Request.ApiNumber;
                // Skip tracing the high-frequency I/O path (read/write/poll): an
                // animating X client fires thousands per second and floods the kd log.
                if (ApiNumber != PsxApiRead && ApiNumber != PsxApiWrite &&
                    ApiNumber != PSX_API_POLL)
                    PSXTRACE("ApiServerLoop: request api 0x%lx from cid %p\n",
                             ApiNumber, Request.Header.ClientId.UniqueProcess);
                if (ApiNumber == PSX_API_POLL)
                {
                    // Extension opcode (>= PsxApiMaxApiNumber, not in the NT4 table):
                    // handle ahead of the fixed dispatch table.
                    PsxSrvPoll((PPSX_PROCESS)PortContext, &Request);
                }
                else if (ApiNumber == PSX_API_IOCTL)
                {
                    PsxSrvIoctl((PPSX_PROCESS)PortContext, &Request);
                }
                else if (ApiNumber == PSX_API_SELECT)
                {
                    PsxSrvSelect((PPSX_PROCESS)PortContext, &Request);
                }
                else if ((ApiNumber < PsxApiMaxApiNumber) && (g_OpHandlers[ApiNumber] != NULL))
                {
                    g_OpHandlers[ApiNumber]((PPSX_PROCESS)PortContext, &Request);
                }
                else
                {
                    // Unimplemented opcode -> ENOSYS.
                    PSXTRACE("ApiServerLoop: api 0x%lx not implemented -> ENOSYS\n", ApiNumber);
                    Request.Errno = PSX_ENOSYS;
                    Request.ReturnValue = -1;
                }
                Reply = &Request;
                break;

            case LPC_CLIENT_DIED:
            case LPC_ERROR_EVENT:
                PSXTRACE("ApiServerLoop: client died/error (type %lu), reaping %p\n",
                         Request.Header.u2.s2.Type & 0xFF, PortContext);
                PsxReapProcess((PPSX_PROCESS)PortContext);
                Reply = NULL;
                continue;

            case LPC_PORT_CLOSED:
            default:
                PSXTRACE("ApiServerLoop: type %lu (ignored)\n",
                         Request.Header.u2.s2.Type & 0xFF);
                Reply = NULL;
                continue;
        }
    }
}
