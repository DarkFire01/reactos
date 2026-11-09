/*
 * PROJECT:     ReactOS Qualcomm remote file system server
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     QMI request dispatch
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "remotefs.h"

#define NDEBUG
#include <debug.h>

/*
 * Request dispatch for service 0x1C. The transport and service registration
 * are real, so the modem's requests arrive and are answered. The per message
 * payloads (iovec transfer through the shared regions, path based file access)
 * are the remaining work and currently answer with a plain success result.
 */
NTSTATUS
RfsRequest(
    _In_ PQMI_SERVICE Service,
    _In_ const QMI_ADDR *Client,
    _In_ USHORT MessageId,
    _In_reads_bytes_(RequestLength) const VOID *Request,
    _In_ ULONG RequestLength,
    _Inout_ PQMI_ENCODER Response,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Service);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Context);

    DPRINT1("[rfs] request 0x%04x from %lu:%lu, %lu bytes\n",
            MessageId, Client->Node, Client->Port, RequestLength);

    switch (MessageId)
    {
        case RFS_MSG_IOVEC:
        case RFS_MSG_ALLOC_BUFFER:
        case RFS_MSG_GET_DEV_ERROR:
        case RFS_MSG_OPEN:
        case RFS_MSG_CLOSE:
        case RFS_MSG_WRITE:
        case RFS_MSG_READ:
            DPRINT1("[rfs]   0x%04x not yet served, answering failure\n", MessageId);
            QmiEncodeResult(Response, QMI_RESULT_FAILURE, 0);
            return STATUS_SUCCESS;

        default:
            DPRINT1("[rfs]   unknown request 0x%04x\n", MessageId);
            QmiEncodeResult(Response, QMI_RESULT_FAILURE, 0);
            return STATUS_SUCCESS;
    }
}
