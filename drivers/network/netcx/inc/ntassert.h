/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shim for the WDK <ntassert.h>
 *
 * The WDK splits the NT_ASSERT family out into its own header. The DDK here
 * declares all of them in xdk/rtlfuncs.h, which ntddk.h already pulls in, so
 * this only has to make sure ntddk.h has been seen.
 */

#pragma once

#include <ntddk.h>
