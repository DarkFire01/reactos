/*
 * PROJECT:     ReactOS Setup Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Device utility functions
 * COPYRIGHT:   Copyright 2024 Hermès Bélusca-Maïto <hermes.belusca-maito@reactos.org>
 */

#include "precomp.h"
#include "devutils.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS *****************************************************************/

/**
 * @brief
 * Open an existing device given by its NT-style path, which is assumed to be
 * for a disk device or a partition. The open is for synchronous I/O access.
 *
 * @param[in]   DevicePath
 * Supplies the NT-style path to the device to open.
 *
 * @param[out]  DeviceHandle
 * If successful, receives the NT handle of the opened device.
 * Once the handle is no longer in use, call NtClose() to close it.
 *
 * @param[in]   DesiredAccess
 * An ACCESS_MASK value combination that determines the requested access
 * to the device. Because the open is for synchronous access, SYNCHRONIZE
 * is automatically added to the access mask.
 *
 * @param[in]   ShareAccess
 * Specifies the type of share access for the device.
 *
 * @return  An NTSTATUS code indicating success or failure.
 *
 * @see pOpenDeviceEx()
 **/
NTSTATUS
pOpenDeviceEx_UStr(
    _In_ PCUNICODE_STRING DevicePath,
    _Out_ PHANDLE DeviceHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ ULONG ShareAccess)
{
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;

    InitializeObjectAttributes(&ObjectAttributes,
                               (PUNICODE_STRING)DevicePath,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);
    return NtOpenFile(DeviceHandle,
                      DesiredAccess | SYNCHRONIZE,
                      &ObjectAttributes,
                      &IoStatusBlock,
                      ShareAccess,
                      /* FILE_NON_DIRECTORY_FILE | */
                      FILE_SYNCHRONOUS_IO_NONALERT);
}

/**
 * @brief
 * Open an existing device given by its NT-style path, which is assumed to be
 * for a disk device or a partition. The open is share read/write/delete, for
 * synchronous I/O and read access.
 *
 * @param[in]   DevicePath
 * @param[out]  DeviceHandle
 * See the DevicePath and DeviceHandle parameters of pOpenDeviceEx_UStr().
 *
 * @return  An NTSTATUS code indicating success or failure.
 *
 * @see pOpenDevice(), pOpenDeviceEx(), pOpenDeviceEx_UStr()
 **/
NTSTATUS
pOpenDevice_UStr(
    _In_ PCUNICODE_STRING DevicePath,
    _Out_ PHANDLE DeviceHandle)
{
    return pOpenDeviceEx_UStr(DevicePath,
                              DeviceHandle,
                              FILE_READ_DATA | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_ALL);
}

/**
 * @brief
 * Open an existing device given by its NT-style path, which is assumed to be
 * for a disk device or a partition. The open is for synchronous I/O access.
 *
 * @param[in]   DevicePath
 * @param[out]  DeviceHandle
 * @param[in]   DesiredAccess
 * @param[in]   ShareAccess
 * See pOpenDeviceEx_UStr() parameters.
 *
 * @return  An NTSTATUS code indicating success or failure.
 *
 * @see pOpenDeviceEx_UStr()
 **/
NTSTATUS
pOpenDeviceEx(
    _In_ PCWSTR DevicePath,
    _Out_ PHANDLE DeviceHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_ ULONG ShareAccess)
{
    UNICODE_STRING Name;
    RtlInitUnicodeString(&Name, DevicePath);
    return pOpenDeviceEx_UStr(&Name, DeviceHandle, DesiredAccess, ShareAccess);
}

/**
 * @brief
 * Open an existing device given by its NT-style path, which is assumed to be
 * for a disk device or a partition. The open is share read/write/delete, for
 * synchronous I/O and read access.
 *
 * @param[in]   DevicePath
 * @param[out]  DeviceHandle
 * See the DevicePath and DeviceHandle parameters of pOpenDeviceEx_UStr().
 *
 * @return  An NTSTATUS code indicating success or failure.
 *
 * @see pOpenDeviceEx(), pOpenDevice_UStr(), pOpenDeviceEx_UStr()
 **/
NTSTATUS
pOpenDevice(
    _In_ PCWSTR DevicePath,
    _Out_ PHANDLE DeviceHandle)
{
    UNICODE_STRING Name;
    RtlInitUnicodeString(&Name, DevicePath);
    return pOpenDevice_UStr(&Name, DeviceHandle);
}

/**
 * @brief   Tells whether a UEFI firmware started this system.
 *
 * What starts the installer is what will start the installation, so this is
 * also what decides how the installed system is made bootable.
 **/
BOOLEAN
IsUefiBoot(VOID)
{
    SYSTEM_BOOT_ENVIRONMENT_INFORMATION BootInfo;
    NTSTATUS Status;

    Status = NtQuerySystemInformation(SystemBootEnvironmentInformation,
                                      &BootInfo,
                                      sizeof(BootInfo),
                                      NULL);
    if (!NT_SUCCESS(Status))
    {
        /* A kernel that cannot say was started by a BIOS, as all of them were */
        DPRINT1("Could not retrieve the firmware type (Status 0x%08lx)\n", Status);
        return FALSE;
    }

    DPRINT1("Firmware type: %lu\n", BootInfo.FirmwareType);
    return (BootInfo.FirmwareType == FirmwareTypeUefi);
}

/* EOF */
