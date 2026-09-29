/*
 * PROJECT:     ReacTVmm
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The tables a machine describes itself with
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include "rtvmm.h"

namespace rtvm
{

/*
 * Where the tables are put. Anything looking for them reads the two ranges it
 * has always read, and this is in the second of them, below where the firmware
 * itself sits.
 */
#define ACPI_TABLE_BASE     0x000E0000
#define ACPI_TABLE_SIZE     0x00001000

/* The ports the part that handles power answers on */
#define ACPI_EVENT_PORT     0x0400
#define ACPI_EVENT_LENGTH   4
#define ACPI_CONTROL_PORT   0x0404
#define ACPI_CONTROL_LENGTH 2
#define ACPI_TIMER_PORT     0x0408
#define ACPI_TIMER_LENGTH   4
#define ACPI_GENERAL_PORT   0x0420
#define ACPI_GENERAL_LENGTH 4

/* Which line it puts its own interrupt on, of the ones nothing else wanted */
#define ACPI_EVENT_LINE     9

/* Writes the whole set into the machine's memory. False means it would not fit */
bool DescribeWithTables(Memory &Ram, ULONG ProcessorCount);

} /* namespace rtvm */
