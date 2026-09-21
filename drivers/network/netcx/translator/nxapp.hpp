/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     What the adapter half knows about a translator
 *
 * The adapter half keeps each translator only as an INxApp so it can destroy
 * it, and creates them through INxAppFactory. The 26100 pdb adds Wi-Fi peer
 * demux virtuals here that this revision of the drop does not implement.
 */

#pragma once

#include <KNew.h>
#include <NetClientApi.h>

class INxApp :
    public KALLOCATOR_NONPAGED<'pAxN'>
{
public:

    virtual
    ~INxApp(
        void
    ) = default;
};

class INxAppFactory
{
public:

    virtual
    ~INxAppFactory(
        void
    ) = default;

    _IRQL_requires_(PASSIVE_LEVEL)
    virtual
    NTSTATUS
    CreateApp(
        _In_ NET_CLIENT_DISPATCH const * Dispatch,
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NET_CLIENT_ADAPTER_DISPATCH const * AdapterDispatch,
        _Outptr_ void ** ClientContext,
        _Outptr_ NET_CLIENT_CONTROL_DISPATCH const ** ClientDispatch
    ) = 0;
};
