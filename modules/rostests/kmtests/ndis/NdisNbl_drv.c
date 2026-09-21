/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite for ndis.sys, driver entry
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kmt_test.h>
#include "ndisnbl.h"

VOID NTAPI TestNbl(VOID);
VOID NTAPI TestXlate(VOID);
VOID NTAPI TestMiniport6(PDRIVER_OBJECT, PUNICODE_STRING);

static PDRIVER_OBJECT TestDriverObject;
static UNICODE_STRING TestRegistryPath;

static KMT_MESSAGE_HANDLER RunNblTest;
static KMT_MESSAGE_HANDLER RunXlateTest;
static KMT_MESSAGE_HANDLER RunMiniport6Test;

static
NTSTATUS
RunNblTest(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ControlCode,
    _In_opt_ PVOID Buffer,
    _In_ SIZE_T InLength,
    _Inout_ PSIZE_T OutLength)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(ControlCode);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);

    *OutLength = 0;
    TestNbl();

    return STATUS_SUCCESS;
}

static
NTSTATUS
RunXlateTest(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ControlCode,
    _In_opt_ PVOID Buffer,
    _In_ SIZE_T InLength,
    _Inout_ PSIZE_T OutLength)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(ControlCode);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);

    *OutLength = 0;
    TestXlate();

    return STATUS_SUCCESS;
}

static
NTSTATUS
RunMiniport6Test(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ControlCode,
    _In_opt_ PVOID Buffer,
    _In_ SIZE_T InLength,
    _Inout_ PSIZE_T OutLength)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(ControlCode);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);

    *OutLength = 0;
    TestMiniport6(TestDriverObject, &TestRegistryPath);

    return STATUS_SUCCESS;
}

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Flags);

    /* NdisMRegisterMiniportDriver needs both, so keep them for the test. */
    TestDriverObject = DriverObject;
    TestRegistryPath = *(PUNICODE_STRING)RegistryPath;

    *DeviceName = L"NdisNbl";

    KmtRegisterMessageHandler(IOCTL_TEST_NBL, NULL, RunNblTest);
    KmtRegisterMessageHandler(IOCTL_TEST_XLATE, NULL, RunXlateTest);
    KmtRegisterMessageHandler(IOCTL_TEST_MINIPORT6, NULL, RunMiniport6Test);

    return STATUS_SUCCESS;
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
}
