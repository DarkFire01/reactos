/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     TraceLogging provider for the translator half
 *
 * The TraceLogging surface is defined away in NxTrace.hpp.
 * translator/nxtranslationapp.cpp defines the handle declared here.
 */

#pragma once

#include <NxTrace.hpp>

TRACELOGGING_DECLARE_PROVIDER(g_hNetAdapterCxXlatProvider);
