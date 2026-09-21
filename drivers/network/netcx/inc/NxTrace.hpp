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

/*
 * Early return helpers from the WPP config block. The failure check is real;
 * the message only ever went to the recorder, so it is dropped.
 */
#define CX_RETURN_IF_NOT_NT_SUCCESS(Expression)                 \
    do                                                          \
    {                                                           \
        NTSTATUS const cxStatus__ = (Expression);               \
        if (!NT_SUCCESS(cxStatus__))                            \
            return cxStatus__;                                  \
    } while (0)

#define CX_RETURN_NTSTATUS_IF(Status, Condition)                \
    do                                                          \
    {                                                           \
        if (Condition)                                          \
            return (Status);                                    \
    } while (0)

#define CX_RETURN_IF_NOT_NT_SUCCESS_MSG(Expression, ...)            CX_RETURN_IF_NOT_NT_SUCCESS(Expression)
#define CX_RETURN_NTSTATUS_IF_MSG(Status, Condition, ...)           CX_RETURN_NTSTATUS_IF(Status, Condition)
#define CX_WARNING_RETURN_NTSTATUS_IF_MSG(Status, Condition, ...)   CX_RETURN_NTSTATUS_IF(Status, Condition)
#define CX_RETURN_STATUS_SUCCESS_MSG(...)                           return STATUS_SUCCESS
#define CX_RETURN_MSG(...)                                          return
#define CX_LOG_IF_NOT_NT_SUCCESS_MSG(Expression, ...)               ((void)(Expression))

/*
 * TraceLogging is a separate ETW surface, equally absent. A provider handle
 * still has to exist so the register and unregister calls have something to
 * name, but it never refers to a registration.
 */
typedef const void *TraceLoggingHProvider;

#define TRACELOGGING_DECLARE_PROVIDER(Handle)           extern TraceLoggingHProvider const Handle
#define TRACELOGGING_DEFINE_PROVIDER(Handle, Name, Guid, ...)     TraceLoggingHProvider const Handle = nullptr
#define TraceLoggingRegister(Handle)                    ((void)(Handle), STATUS_SUCCESS)
#define TraceLoggingUnregister(Handle)                  ((void)(Handle))

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
