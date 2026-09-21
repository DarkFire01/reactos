/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Missing headers (WdfVersionLog.h), the library's event log messages
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 *
 * Message identifiers as the Windows 11 Wdf01000.sys message table has them.
 */

#pragma once

#define WDFVER_MINOR_VERSION_NOT_SUPPORTED      0x80070001
#define WDFVER_CLIENT_INVALID_DDI_COUNT         0x80070002
#define WDFVER_DRIVER_COMPANION_FAIL_TO_LOAD    0x80070003
