/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Link to the user mode driver host for device companions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Companions run under the UMDF reflector, which ReactOS does not have, so the
 * library never finds one to start for a device.
 */

#pragma once

class FxDevice;

class FxCompanionLibrary
{
public:
    static
    NTSTATUS
    _CreateAndInitialize(
        _Out_ FxCompanionLibrary **CompanionLibrary);

    BOOLEAN
    IsCompanionRequiredForDevice(
        _In_ FxDevice *Device,
        _Out_ PCWSTR *CompanionName);

    PVOID
    operator new(
        _In_ size_t Size);

    VOID
    operator delete(
        _In_ PVOID Pointer);
};
