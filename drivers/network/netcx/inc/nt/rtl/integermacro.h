/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shim for the internal <nt/rtl/integermacro.h>
 *
 * The checked in arithmetic helpers come from ntintsafe.h, which defines
 * _NTINTSAFE_H_INCLUDED_ so intsafe.h emits the Rtl prefixed names.
 */

#pragma once

#include <ntintsafe.h>
