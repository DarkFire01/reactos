/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     WPP software tracing stub
 *
 * WPP is a preprocessor pass that this tree does not run, and the recorder it
 * would set up needs a control block nothing here creates. The same approach
 * is already taken for pci.sys, see usb3-reactos/inc/pci/WppRecorder.h: define
 * the surface away so the sources stay untouched.
 */

#pragma once

#include <ntddk.h>

/* Control block setup and teardown, both no-ops without a WPP pass. */
#define WPP_INIT_TRACING(DriverObject, RegistryPath)    ((void)(DriverObject), (void)(RegistryPath))
#define WPP_CLEANUP(DriverObject)                       ((void)(DriverObject))

#define WPP_CONTROL_GUIDS
#define WPP_DEFINE_CONTROL_GUID(Name, Guid, ...)
#define WPP_DEFINE_BIT(Name)
#define WPP_LEVEL_ENABLED(Level)                        (FALSE)

/*
 * The recorder filters take a level and flags and decide whether a message is
 * emitted. With no recorder there is nothing to emit, so the filter is always
 * false and the argument list is empty.
 */
#define WPP_RECORDER_LEVEL_FLAGS_FILTER(Level, Flags)                   (FALSE)
#define WPP_RECORDER_LEVEL_FLAGS_ARGS(Level, Flags)
#define WPP_RECORDER_COMPNAME_LEVEL_NTEXPR_FILTER(Comp, Level, Expr)    (FALSE)
#define WPP_RECORDER_COMPNAME_LEVEL_NTEXPR_ARGS(Comp, Level, Expr)

/* TraceLogging is a separate ETW surface, equally absent. */
#define TraceLoggingWrite(...)
#define TraceLoggingKeyword(...)
#define TraceLoggingValue(...)
#define TraceLoggingString(...)
#define TraceLoggingUInt32(...)
#define TraceLoggingUInt64(...)
#define TraceLoggingHexUInt32(...)
#define TraceLoggingPointer(...)
#define TraceLoggingLevel(...)
#define TraceLoggingOpcode(...)
