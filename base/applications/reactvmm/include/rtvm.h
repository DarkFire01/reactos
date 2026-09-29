/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     What the manager and everything it loads agree on
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <windef.h>
#include <winbase.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Everything crossing the boundary between the manager and a hardware module
 * is called with this, so that a module built on its own still agrees with the
 * manager about how arguments are passed.
 */
#define RTVMAPI __stdcall

/*
 * How a call went. These are not NTSTATUS: a hardware module is a plain user
 * mode library and has no business carrying kernel status codes around, and a
 * short list that fits in a switch is easier to answer honestly than a long one.
 */
typedef enum _RTVM_STATUS
{
    RtvmOk = 0,
    RtvmFailed,
    /* The call was fine, the device simply does not answer for that address */
    RtvmNotClaimed,
    RtvmNoMemory,
    RtvmBadParameter,
    /* Asked for something the module or the manager does not implement */
    RtvmNotSupported,
    /* The range wanted is already answered for by something else */
    RtvmInUse,
    RtvmNotFound
} RTVM_STATUS;

#define RTVM_SUCCESS(s) ((s) == RtvmOk)

/* How wide a single access is, in bytes. Never anything but these four */
#define RTVM_WIDTH_BYTE     1
#define RTVM_WIDTH_WORD     2
#define RTVM_WIDTH_DWORD    4
#define RTVM_WIDTH_QWORD    8

/* What a line of log output is worth */
typedef enum _RTVM_LOG_LEVEL
{
    RtvmLogError = 0,
    RtvmLogWarning,
    RtvmLogInfo,
    RtvmLogTrace
} RTVM_LOG_LEVEL;

#ifdef __cplusplus
}
#endif
