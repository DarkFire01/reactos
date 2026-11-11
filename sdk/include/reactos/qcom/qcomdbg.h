/*
 * PROJECT:     ReactOS Qualcomm platform drivers
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Shared bring-up log and fatal freeze (qcomdbg.sys)
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/*
 * Every driver of a bring-up stack logs through qcomdbg so one ring holds the
 * whole story. QcDbgFatal takes the display back, prints that ring and stops
 * every processor, so a photo of the screen is the debug log.
 */

#define QCDBG_LINE_LENGTH   78

VOID
__cdecl
QcDbgLog(
    _In_z_ PCSTR Component,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...);

DECLSPEC_NORETURN
VOID
__cdecl
QcDbgFatal(
    _In_z_ PCSTR Component,
    _In_ NTSTATUS Status,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...);

/* Each driver defines QCDBG_COMPONENT before including this header */
#ifdef QCDBG_COMPONENT
#define QC_LOG(...)             QcDbgLog(QCDBG_COMPONENT, __VA_ARGS__)
#define QC_FATAL(Status, ...)   QcDbgFatal(QCDBG_COMPONENT, (Status), __VA_ARGS__)
#endif
