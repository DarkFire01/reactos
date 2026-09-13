/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Driver entry points into the uACPI host layer
 */
#ifndef _UACPI_HOST_H_
#define _UACPI_HOST_H_

#include <ntddk.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Translated SCI interrupt resource from START_DEVICE.
typedef struct _ACPI_HOST_SCI_RESOURCE
{
    PDEVICE_OBJECT  DeviceObject;   ///< PDO beneath the ACPI FDO
    ULONG           Vector;         ///< translated system interrupt vector
    KIRQL           Irql;           ///< translated DIRQL
    KAFFINITY       Affinity;       ///< target affinity
    KINTERRUPT_MODE Mode;           ///< LevelSensitive for the SCI
    BOOLEAN         ShareVector;    ///< SCI is shareable
    ULONG           Gsi;            ///< FADT SCI_INT
} UACPI_HOST_SCI_RESOURCE, *PUACPI_HOST_SCI_RESOURCE;

// Copies the struct. Call before uacpi_namespace_load.
VOID UacpiHostSetSciResource(PUACPI_HOST_SCI_RESOURCE Resource);

// Call after UacpiIrqLibInitialize and before GPEs are enabled.
NTSTATUS UacpiHostConnectSci(VOID);

// RSDP to use instead of the loader and BIOS scan. 0 clears it.
VOID UacpiHostRsdpOverride(PHYSICAL_ADDRESS RsdpPhysical);

// Nonzero logs every uACPI line at DPFLTR_ERROR_LEVEL.
extern int UacpiHostVerbose;

#ifdef __cplusplus
}
#endif

#endif // _UACPI_HOST_H_
