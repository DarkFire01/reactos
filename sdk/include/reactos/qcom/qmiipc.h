/*
 * PROJECT:     ReactOS Qualcomm SoC support
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     QMI transport binding over the IPC router
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <qcom/qmisvc.h>

/*
 * Opens one port on \Device\IPC and drives it as a QMI transport. The returned
 * pointer is handed to QmiServiceCreate or QmiClientConnect. A receive thread
 * delivers inbound messages to whichever of those claimed the transport.
 */
NTSTATUS
QmiIpcCreate(
    _Outptr_ PQMI_TRANSPORT *Transport);

VOID
QmiIpcDestroy(
    _In_ PQMI_TRANSPORT Transport);
