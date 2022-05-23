/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * FILE:        lib/drivers/hidparser/hidphelpers.h
 * PURPOSE:     HID Parser helper functions
 * COPYRIGHT:   Copyright (C) Roman Masanin (36927roma@gmail.com) 2022
 */

#include "preparsed.h"

NTSTATUS
HidParser_GetValuesForCap(
    IN PHIDPARSER_VALUE_CAPS ValueCap,
    IN PUCHAR Report,
    IN ULONG ReportLen,
    OUT PULONG Values,
    IN USHORT ValuesLen);

BOOLEAN
HidParser_CheckPreparsedMagic(IN PHIDPARSER_PREPARSED_DATA PreparsedData);

BOOLEAN
HidParser_FilterValueCap(
    IN PHIDPARSER_VALUE_CAPS ValueCap,
    IN USAGE UsagePage,
    IN USAGE Usage,
    IN USHORT LinkCollection);

LONG
HidParser_MapValue(LONG Value,
                   LONG LogicalMin,
                   LONG LogicalMax,
                   LONG PhysicalMin,
                   LONG PhysicalMax);
