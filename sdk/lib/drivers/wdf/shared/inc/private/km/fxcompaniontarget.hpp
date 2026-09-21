/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Target a kernel driver sends tasks to its device companion through
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * A device companion is a UMDF driver the reflector starts beside the kernel
 * driver. With no reflector, loading one fails and the device carries on
 * without it, the same as when the companion service fails to start.
 */

#pragma once

class FxCompanionTarget : public FxNonPagedObject
{
public:
    FxCompanionTarget(
        _In_ PFX_DRIVER_GLOBALS FxDriverGlobals,
        _In_ USHORT ObjectSize);

    _Must_inspect_result_
    NTSTATUS
    Init(
        _In_ FxDevice *Device);

    VOID
    QueryPnPDeviceStateNotification(
        VOID);

    _Must_inspect_result_
    NTSTATUS
    HandleQueryInterfaceForSecureDriver(
        _In_ FxIrp *Irp,
        _Out_ PBOOLEAN CompleteRequest);
};
