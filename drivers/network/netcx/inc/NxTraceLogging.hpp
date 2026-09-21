/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     TraceLogging provider for the adapter half
 *
 * The TraceLogging surface is defined away in NxTrace.hpp. adapter/version.cpp
 * defines the handle declared here.
 */

#pragma once

#include <NxTrace.hpp>

/* The power policy sources expect the adapter types, allocator bases and WIL to arrive through here. */
#include <preview/netadaptercx.h>
#include <KNew.h>
#include <wil/resource.h>

TRACELOGGING_DECLARE_PROVIDER(g_hNetAdapterCxEtwProvider);
