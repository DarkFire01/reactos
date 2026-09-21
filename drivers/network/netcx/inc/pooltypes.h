/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shim for the internal <pooltypes.h>
 *
 * rtl/inc/knew.h wants POOL_TYPE and the pool constants it templates on. The
 * DDK declares all of them in ntddk.h.
 */

#pragma once

#include <ntddk.h>
