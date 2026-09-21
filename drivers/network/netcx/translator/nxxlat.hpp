/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The translator as the adapter half sees it
 *
 * The driver context owns one factory, which sets up the translator's global
 * state and hands out one NxTranslationApp per adapter.
 */

#pragma once

#include "NxApp.hpp"

class NxTranslationAppFactory :
    public INxAppFactory
{
public:

    ~NxTranslationAppFactory(
        void
    ) override;

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    Initialize(
        void
    );

    _IRQL_requires_(PASSIVE_LEVEL)
    NTSTATUS
    CreateApp(
        _In_ NET_CLIENT_DISPATCH const * Dispatch,
        _In_ NET_CLIENT_ADAPTER Adapter,
        _In_ NET_CLIENT_ADAPTER_DISPATCH const * AdapterDispatch,
        _Outptr_ void ** ClientContext,
        _Outptr_ NET_CLIENT_CONTROL_DISPATCH const ** ClientDispatch
    ) override;
};
