/*
 * PROJECT:     ReactOS HID Parser Library
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * FILE:        drivers/hid/hidparse/hidparse.h
 * PURPOSE:     HID Parser kernel mode
 * COPYRIGHT:   Copyright (C) Michael Martin (michael.martin@reactos.org)
 *              Copyright (C) Johannes Anderwald (johannes.anderwald@reactos.org)
 */

#pragma once

#define _HIDPI_
#define _HIDPI_NO_FUNCTION_MACROS_
#include <ntddk.h>
#include <hidpddi.h>
#include <hidpi.h>
#include <stdio.h>

#define HIDPARSE_TAG 'PdiH'
