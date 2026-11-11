/*
 * PROJECT:     ReactOS USB Role Switch Class Extension
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     URS class function table layout
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

/* Slot order is ABI */
typedef enum _URSFUNCENUM
{
    UrsDeviceInitializeTableIndex = 0,
    UrsDeviceInitInitializeTableIndex = 1,
    UrsIoResourceListAppendDescriptorTableIndex = 2,
    UrsReportHardwareEventTableIndex = 3,
    UrsSetPoHandleTableIndex = 4,
    UrsSetHardwareEventSupportTableIndex = 5,
    UrsFunctionTableNumEntries = 6
} URSFUNCENUM;

#define URS_BOUND_FUNCTION(PfnType, Slot) ((PfnType)UrsFunctions[(Slot)])
