/*
 * PROJECT:     ReactOS Processor Power Management Library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Driver load, kernel interface, registry and FADT data
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES *******************************************************************/

#include "ppmlib.h"
#include <drivers/acpi/acpi.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

PPMLIB_DATA PpmLib;

#define PPMLIB_FIRMWARE_TABLE_CLASS     76
#define PPMLIB_FIRMWARE_PROVIDER_ACPI   'ACPI'
#define PPMLIB_FIRMWARE_TABLE_FADT      'PCAF'

static const WCHAR PpmLibProcessorKey[] =
    L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Processor";
static const WCHAR PpmLibThrottleKey[] =
    L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Session Manager\\Throttle";
static const WCHAR PpmLibCpuKey[] =
    L"\\Registry\\Machine\\Hardware\\Description\\System\\CentralProcessor\\0";

NTSYSAPI
NTSTATUS
NTAPI
ZwQuerySystemInformation(
    _In_ ULONG SystemInformationClass,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength);

/* PRIVATE FUNCTIONS **********************************************************/

/**
 * @brief Reads a registry value of an exact type and size.
 */
static
NTSTATUS
NTAPI
PpmLibQueryValue(
    _In_z_ PCWSTR KeyPath,
    _In_z_ PCWSTR ValueName,
    _In_ ULONG Type,
    _Out_writes_bytes_(Size) PVOID Data,
    _In_ ULONG Size)
{
    UCHAR Buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + sizeof(ULONG64)];
    PKEY_VALUE_PARTIAL_INFORMATION Info = (PKEY_VALUE_PARTIAL_INFORMATION)Buffer;
    OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING String;
    HANDLE Key;
    ULONG Length;
    NTSTATUS Status;

    PAGED_CODE();

    if (Size > sizeof(ULONG64))
        return STATUS_INVALID_PARAMETER;

    RtlInitUnicodeString(&String, KeyPath);
    InitializeObjectAttributes(&Attributes, &String, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwOpenKey(&Key, KEY_QUERY_VALUE, &Attributes);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(&String, ValueName);
    Status = ZwQueryValueKey(Key, &String, KeyValuePartialInformation, Info, sizeof(Buffer), &Length);
    ZwClose(Key);
    if (!NT_SUCCESS(Status))
        return Status;

    if (Info->Type != Type || Info->DataLength != Size)
        return STATUS_OBJECT_TYPE_MISMATCH;

    RtlCopyMemory(Data, Info->Data, Size);
    return STATUS_SUCCESS;
}

/* A missing or mistyped value reads as 0 */
static
ULONG
NTAPI
PpmLibQueryDword(
    _In_z_ PCWSTR KeyPath,
    _In_z_ PCWSTR ValueName)
{
    ULONG Value;

    if (!NT_SUCCESS(PpmLibQueryValue(KeyPath, ValueName, REG_DWORD, &Value, sizeof(Value))))
        return 0;

    return Value;
}

/* Capabilities may be stored as either width */
static
ULONG64
NTAPI
PpmLibQueryDisableMask(VOID)
{
    ULONG64 Wide;
    ULONG Narrow;

    if (NT_SUCCESS(PpmLibQueryValue(PpmLibProcessorKey, L"Capabilities", REG_QWORD, &Wide, sizeof(Wide))))
        return Wide;

    if (NT_SUCCESS(PpmLibQueryValue(PpmLibProcessorKey, L"Capabilities", REG_DWORD, &Narrow, sizeof(Narrow))))
        return Narrow;

    return 0;
}

static
VOID
NTAPI
PpmLibSetPortRegister(
    _Out_ PPPMLIB_GAS Register,
    _In_ ULONG Port,
    _In_ UCHAR Bits)
{
    if (Port == 0)
        return;

    Register->SpaceId = PPMLIB_SPACE_IO;
    Register->BitWidth = Bits;
    Register->BitOffset = 0;
    Register->AccessBits = Bits;
    Register->Address = Port;
}

/**
 * @brief Pulls the PM blocks and the SMI command values out of the FADT.
 */
static
NTSTATUS
NTAPI
PpmLibReadFadt(VOID)
{
    PSYSTEM_FIRMWARE_TABLE_INFORMATION Request;
    SYSTEM_FIRMWARE_TABLE_INFORMATION Probe;
    PPPMLIB_FADT_INFO Info = &PpmLib.Fadt;
    ULONG Size;
    ULONG Copy;
    FADT Fadt;
    NTSTATUS Status;

    PAGED_CODE();

    RtlZeroMemory(&Probe, sizeof(Probe));
    Probe.ProviderSignature = PPMLIB_FIRMWARE_PROVIDER_ACPI;
    Probe.Action = SystemFirmwareTable_Get;
    Probe.TableID = PPMLIB_FIRMWARE_TABLE_FADT;
    Status = ZwQuerySystemInformation(PPMLIB_FIRMWARE_TABLE_CLASS,
                                      &Probe,
                                      FIELD_OFFSET(SYSTEM_FIRMWARE_TABLE_INFORMATION, TableBuffer),
                                      NULL);
    if (Status != STATUS_BUFFER_TOO_SMALL)
        return NT_SUCCESS(Status) ? STATUS_UNSUCCESSFUL : Status;

    if (Probe.TableBufferLength == 0)
        return STATUS_UNSUCCESSFUL;

    Size = FIELD_OFFSET(SYSTEM_FIRMWARE_TABLE_INFORMATION, TableBuffer) + Probe.TableBufferLength;
    Request = ExAllocatePoolWithTag(PagedPool, Size, PPMLIB_TAG);
    if (Request == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(Request, Size);
    Request->ProviderSignature = PPMLIB_FIRMWARE_PROVIDER_ACPI;
    Request->Action = SystemFirmwareTable_Get;
    Request->TableID = PPMLIB_FIRMWARE_TABLE_FADT;
    Request->TableBufferLength = Probe.TableBufferLength;
    Status = ZwQuerySystemInformation(PPMLIB_FIRMWARE_TABLE_CLASS, Request, Size, NULL);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Request, PPMLIB_TAG);
        return Status;
    }

    /* Short revision 1 tables leave the later fields zero */
    RtlZeroMemory(&Fadt, sizeof(Fadt));
    Copy = min(Request->TableBufferLength, (ULONG)sizeof(Fadt));
    RtlCopyMemory(&Fadt, Request->TableBuffer, Copy);
    ExFreePoolWithTag(Request, PPMLIB_TAG);

    PpmLibSetPortRegister(&Info->Pm1aEvent, Fadt.pm1a_evt_blk_io_port, 16);
    PpmLibSetPortRegister(&Info->Pm1bEvent, Fadt.pm1b_evt_blk_io_port, 16);
    PpmLibSetPortRegister(&Info->Pm1aControl, Fadt.pm1a_ctrl_blk_io_port, 16);
    PpmLibSetPortRegister(&Info->Pm1bControl, Fadt.pm1b_ctrl_blk_io_port, 16);
    PpmLibSetPortRegister(&Info->Pm2Control, Fadt.pm2_ctrl_blk_io_port, 8);

    Info->SmiCommand = Fadt.smi_cmd_io_port;
    Info->DutyWidth = Fadt.duty_width;
    Info->DutyOffset = Fadt.duty_offset;
    Info->C2Latency = Fadt.lvl2_latency;
    Info->C3Latency = Fadt.lvl3_latency;
    Info->PStateControl = Fadt.pstate_control;
    Info->CStateControl = Fadt.cstate_control;
    return STATUS_SUCCESS;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/**
 * @brief Nominal frequency in MHz from the CPU description, 0 when unknown.
 */
ULONG
NTAPI
PpmLibQueryNominalFrequency(VOID)
{
    return PpmLibQueryDword(PpmLibCpuKey, L"~MHz");
}

/**
 * @brief Common DriverEntry of the processor drivers.
 *
 * @param[in] VendorInit
 * Fills the vendor hooks and returns the capabilities the vendor adds.
 */
NTSTATUS
NTAPI
PpmLibDriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath,
    _In_ PPPMLIB_VENDOR_INIT VendorInit)
{
    POWER_STATE_NOTIFY_HANDLER SleepNotify;
    ULONG64 VendorCaps = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlZeroMemory(&PpmLib, sizeof(PpmLib));
    PpmLib.DriverObject = DriverObject;
    PpmLib.ProcessorCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);

    Status = ZwPowerInformation(ProcessorStateHandler, NULL, 0, &PpmLib.Kernel, sizeof(PpmLib.Kernel));
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PPM: no processor driver interface, 0x%08lx\n", Status);
        return Status;
    }

    if (PpmLib.Kernel.InterfaceVersion != PPM_DRIVER_INTERFACE_VERSION)
    {
        DPRINT1("PPM: kernel interface version %lu\n", PpmLib.Kernel.InterfaceVersion);
        return STATUS_REVISION_MISMATCH;
    }

    KeInitializeMutex(&PpmLib.Lock, 0);
    InitializeListHead(&PpmLib.DeviceList);
    InitializeListHead(&PpmLib.PerfDomainList);
    InitializeListHead(&PpmLib.PackageList);
    InitializeListHead(&PpmLib.IdleDomainList);

    PpmLib.AllowPepPerf = (PpmLibQueryDword(PpmLibProcessorKey, L"AllowPepPerfStates") != 0);
    PpmLib.Overrides = PpmLibQueryDword(PpmLibProcessorKey, L"Overrides");
    PpmLib.DisableMask = PpmLibQueryDisableMask();
    PpmLib.SyntheticPsdAllowed = (PpmLibQueryDword(PpmLibThrottleKey, L"PerfEnablePackageIdle") == 0);

    /* Only the S4 resume handoff depends on it, so a kernel without it is tolerated */
    SleepNotify.Handler = PpmLibSleepNotify;
    SleepNotify.Context = NULL;
    Status = ZwPowerInformation(SystemPowerStateNotifyHandler, &SleepNotify, sizeof(SleepNotify), NULL, 0);
    if (!NT_SUCCESS(Status))
        DPRINT1("PPM: no sleep state notification, 0x%08lx\n", Status);

    Status = PpmLibReadFadt();
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("PPM: FADT not readable, 0x%08lx\n", Status);
        return Status;
    }

    PpmLib.Capabilities = PPMLIB_DEFAULT_CAPABILITIES;
    if (PpmLib.AllowPepPerf)
        PpmLib.Capabilities |= PPMLIB_PEP_PERF;

    Status = VendorInit(&PpmLib.Vendor, &VendorCaps);
    if (!NT_SUCCESS(Status))
    {
        RtlZeroMemory(&PpmLib.Vendor, sizeof(PpmLib.Vendor));
        return Status;
    }

    PpmLib.Capabilities |= VendorCaps;
    PpmLib.Capabilities &= ~PpmLib.DisableMask;

    /* An override of any P-state kind drops the vendor _PSS check */
    if (PpmLib.Overrides & (ULONG)PPMLIB_PSTATE_MASK)
        PpmLib.Vendor.ValidatePStates = NULL;

    DriverObject->MajorFunction[IRP_MJ_CREATE] = PpmLibDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = PpmLibDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = PpmLibDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_PNP] = PpmLibDispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = PpmLibDispatchPower;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = PpmLibDispatchForward;
    DriverObject->DriverExtension->AddDevice = PpmLibAddDevice;

    DPRINT("PPM: capabilities 0x%I64x\n", PpmLib.Capabilities);
    return STATUS_SUCCESS;
}

/* EOF */
