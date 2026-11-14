/*
 * PROJECT:     ReactOS ACPI Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device DMA cache coherency, from the _CCA object
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#pragma once

#define ACPI_DMA_COHERENCY_INTERFACE_VERSION 1

/* Returned for GUID_DMA_CACHE_COHERENCY_INTERFACE when the device has a _CCA object */
typedef struct _ACPI_DMA_COHERENCY_INTERFACE
{
    INTERFACE Header;
    BOOLEAN CacheCoherent;
} ACPI_DMA_COHERENCY_INTERFACE, *PACPI_DMA_COHERENCY_INTERFACE;

#ifdef _WIN64
C_ASSERT(sizeof(ACPI_DMA_COHERENCY_INTERFACE) == 0x28);
#endif
