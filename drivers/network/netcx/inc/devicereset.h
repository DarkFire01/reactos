/*
 * PROJECT:     ReactOS NetAdapterCx
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     The device reset interface below its wdm.h version gate
 *
 * wdm.h only declares this from Windows 10 on, and KMDF drivers here build
 * for Windows 8.1. The layout is the one bus drivers answer the interface
 * query with, so it has to match field for field.
 */

#pragma once

#if (NTDDI_VERSION < NTDDI_WINTHRESHOLD)

#define DEVICE_RESET_INTERFACE_VERSION  1

typedef enum _DEVICE_RESET_TYPE
{
    FunctionLevelDeviceReset,
    PlatformLevelDeviceReset
} DEVICE_RESET_TYPE;

/* A function level reset completes asynchronously through this. */
typedef
VOID
(DEVICE_RESET_COMPLETION)(
    _In_ NTSTATUS Status,
    _Inout_opt_ PVOID Context);

typedef DEVICE_RESET_COMPLETION *PDEVICE_RESET_COMPLETION;

typedef struct _FUNCTION_LEVEL_DEVICE_RESET_PARAMETERS
{
    ULONG Size;
    PDEVICE_RESET_COMPLETION DeviceResetCompletion;
    PVOID CompletionContext;
} FUNCTION_LEVEL_DEVICE_RESET_PARAMETERS, *PFUNCTION_LEVEL_DEVICE_RESET_PARAMETERS;

typedef
NTSTATUS
(*PDEVICE_RESET_HANDLER)(
    _In_ PVOID InterfaceContext,
    _In_ DEVICE_RESET_TYPE ResetType,
    _In_ ULONG Flags,
    _In_opt_ PVOID ResetParameters);

typedef struct _DEVICE_RESET_INTERFACE_STANDARD
{
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PINTERFACE_REFERENCE InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;
    PDEVICE_RESET_HANDLER DeviceReset;
    ULONG SupportedResetTypes;
    PVOID Reserved;
} DEVICE_RESET_INTERFACE_STANDARD, *PDEVICE_RESET_INTERFACE_STANDARD;

DEFINE_GUID(GUID_DEVICE_RESET_INTERFACE_STANDARD,
            0x649fdf26, 0x3bc0, 0x4813, 0xad, 0x24, 0x7e, 0x0c, 0x1e, 0xda, 0x3f, 0xa3);

#endif
