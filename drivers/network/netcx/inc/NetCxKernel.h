/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel surface the class extension is built against
 *
 * The drop expects an internal header that covers everything ntifs.h does. The
 * stub it finds under that name only covers ntddk.h, so this one is included
 * ahead of every source by the CMakeLists to supply push locks and the 64 bit
 * indexed bitmap.
 */

#pragma once

#include <ntifs.h>
