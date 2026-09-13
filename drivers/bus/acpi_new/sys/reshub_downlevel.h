/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Resource Hub constants for targets without <reshub.h>
 */

#ifndef _UACPI_NT_RESHUB_DOWNLEVEL_H
#define _UACPI_NT_RESHUB_DOWNLEVEL_H

/* Opening the hub by this name fails when no hub is present */
#ifndef RESOURCE_HUB_DEVICE_NAME
#define RESOURCE_HUB_DEVICE_NAME    L"\\Device\\RESOURCE_HUB"
#endif

/* ACPI large resource descriptors that only the hub translates */
#ifndef FUNCTION_CONFIG_DESCRIPTOR
#define FUNCTION_CONFIG_DESCRIPTOR      0x8d
#endif

#ifndef GPIO_INTERRUPT_IO_DESCRIPTOR
#define GPIO_INTERRUPT_IO_DESCRIPTOR    0x8c
#endif

#ifndef SERIAL_BUS_DESCRIPTOR
#define SERIAL_BUS_DESCRIPTOR           0x8e
#endif

#endif /* _UACPI_NT_RESHUB_DOWNLEVEL_H */
