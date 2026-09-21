/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Shim for the WDK <wdfcxbase.h>
 *
 * The class extension surface the drop wants, WDFCXDEVICE_INIT,
 * WDF_CLASS_BIND_INFO and the WdfCxDeviceInit routines, is already declared
 * by the KMDF in this tree.
 */

#pragma once

#include <wdfcx.h>
