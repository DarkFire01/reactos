
#ifndef _ACPI_PCH_
#define _ACPI_PCH_

#include <stdio.h>

#include <ntddk.h>
#include <uacpi/include/uacpi/acpi.h>
#include <uacpi/include/uacpi/uacpi.h>
#include <wdmguid.h>
#include <acpiioct.h>
#include <ntintsafe.h>

#pragma once

UINT32
ACPIInitUACPI();

#endif /* _ACPI_PCH_ */
