/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Class extension tunables read from the service key
 *
 * The values are loaded once at driver entry and are read only afterwards,
 * which is why the queries are safe at dispatch level.
 */

#pragma once

#include <NetClientDriverConfigurationConstants.h>

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
DriverConfigurationInitialize(
    void);

_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG
NetClientQueryDriverConfigurationUlong(
    _In_ DRIVER_CONFIG_ENUM ConfigurationEnum);

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
NetClientQueryDriverConfigurationBoolean(
    _In_ DRIVER_CONFIG_ENUM ConfigurationEnum);
