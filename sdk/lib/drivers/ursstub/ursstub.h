/*
 * PROJECT:     ReactOS USB Role Switch Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     URS client binding for drivers built in tree
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Binds the calling driver to urscx01000. Call from DriverEntry after WdfDriverCreate. */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
NTAPI
UrsStubBindClass(VOID);

/** Drops the binding taken by UrsStubBindClass. Safe to call when not bound. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
UrsStubUnbindClass(VOID);

#ifdef __cplusplus
}
#endif
