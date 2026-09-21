/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Processor group limit
 *
 * Matches the bitmap size of KAFFINITY_EX on the Windows versions the class
 * extension targets: 32 groups on 64 bit, a single group on 32 bit.
 */

#pragma once

#ifdef _WIN64
#define MAXIMUM_GROUPS  32
#else
#define MAXIMUM_GROUPS  1
#endif
