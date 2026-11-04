/*
 * PROJECT:     ReactOS ARM64 HAL
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Firmware, clock, display and bus services
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* PSCI 1.0 function IDs */
#define PSCI_SYSTEM_OFF     0x84000008
#define PSCI_SYSTEM_RESET   0x84000009

ULONG64
NTAPI
HalpCallFirmware(
    _In_ ULONG64 Function,
    _In_ ULONG64 Argument1,
    _In_ ULONG64 Argument2,
    _In_ ULONG64 Argument3,
    _In_ BOOLEAN UseHvc);

VOID
NTAPI
HalpWaitForInterrupt(VOID);

/* GLOBALS *******************************************************************/

/* Without CMOS the last known good flag only lives as long as the boot */
static BOOLEAN HalpLastKnownGood = TRUE;

/* EFI_TIME from the UEFI specification */
typedef struct _HALP_EFI_TIME
{
    USHORT Year;
    UCHAR Month;
    UCHAR Day;
    UCHAR Hour;
    UCHAR Minute;
    UCHAR Second;
    UCHAR Pad1;
    ULONG Nanosecond;
    SHORT TimeZone;
    UCHAR Daylight;
    UCHAR Pad2;
} HALP_EFI_TIME, *PHALP_EFI_TIME;

/* The runtime services are not reentrant */
static KSPIN_LOCK HalpEfiLock;
static PVOID HalpEfiGetTime;
static PVOID HalpEfiSetTime;

/* FUNCTIONS *****************************************************************/

VOID
NTAPI
HalReturnToFirmware(
    _In_ FIRMWARE_REENTRY Action)
{
    BOOLEAN UseHvc;
    ULONG64 Function;

    UseHvc = !!(HalpPlatform.ArmBootFlags & FADT_ARM_PSCI_USE_HVC);
    Function = ((Action == HalHaltRoutine) || (Action == HalPowerDownRoutine)) ?
               PSCI_SYSTEM_OFF : PSCI_SYSTEM_RESET;

    _disable();
    HalpCallFirmware(Function, 0, 0, 0, UseHvc);

    /* PSCI is missing or refused, park the processor */
    for (;;)
        HalpWaitForInterrupt();
}

/**
 * @brief Takes the clock services the loader moved to their virtual addresses.
 */
CODE_SEG("INIT")
VOID
HalpInitializeEfiRuntime(
    _In_ PLOADER_PARAMETER_BLOCK LoaderBlock)
{
    PEFI_FIRMWARE_INFORMATION EfiInformation = &LoaderBlock->FirmwareInformation.u.EfiInformation;
    PVIRTUAL_EFI_RUNTIME_SERVICES Services;

    KeInitializeSpinLock(&HalpEfiLock);

    if (!LoaderBlock->FirmwareInformation.FirmwareTypeEfi ||
        (EfiInformation->SetVirtualAddressMapStatus != 0) ||
        !EfiInformation->VirtualEfiRuntimeServices)
    {
        DPRINT1("No UEFI runtime services, the clock starts from a fixed date\n");
        return;
    }

    /* The loader block goes away, the entry points are kept */
    Services = EfiInformation->VirtualEfiRuntimeServices;
    HalpEfiGetTime = (PVOID)Services->GetTime;
    HalpEfiSetTime = (PVOID)Services->SetTime;
}

/* A firmware call masks every interrupt, the firmware runs without the kernel's state */
static
ULONG64
HalpCallEfiClock(
    _In_ PVOID Function,
    _Inout_ PHALP_EFI_TIME EfiTime)
{
    ULONG64 Status;
    KIRQL OldIrql;

    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    KeAcquireSpinLockAtDpcLevel(&HalpEfiLock);
    Status = HalpCallEfiFirmware(Function, (ULONG64)EfiTime, 0);
    KeReleaseSpinLockFromDpcLevel(&HalpEfiLock);
    KeLowerIrql(OldIrql);

    return Status;
}

/**
 * @brief Reports the time of day from the firmware clock.
 *
 * Without UEFI runtime services there is no RTC to read, the clock then
 * starts from a fixed date.
 */
BOOLEAN
NTAPI
HalQueryRealTimeClock(
    _Out_ PTIME_FIELDS Time)
{
    HALP_EFI_TIME EfiTime;
    LARGE_INTEGER SystemTime;

    RtlZeroMemory(Time, sizeof(*Time));

    if (HalpEfiGetTime && (HalpCallEfiClock(HalpEfiGetTime, &EfiTime) == 0))
    {
        Time->Year = EfiTime.Year;
        Time->Month = EfiTime.Month;
        Time->Day = EfiTime.Day;
        Time->Hour = EfiTime.Hour;
        Time->Minute = EfiTime.Minute;
        Time->Second = EfiTime.Second;
        Time->Milliseconds = (CSHORT)(EfiTime.Nanosecond / 1000000);

        /* The firmware has no weekday, a round trip through the system time fills it in */
        if (RtlTimeFieldsToTime(Time, &SystemTime))
        {
            RtlTimeToTimeFields(&SystemTime, Time);
            return TRUE;
        }
    }

    RtlZeroMemory(Time, sizeof(*Time));
    Time->Year = 2026;
    Time->Month = 1;
    Time->Day = 1;
    Time->Weekday = 4;
    return TRUE;
}

BOOLEAN
NTAPI
HalSetRealTimeClock(
    _In_ PTIME_FIELDS Time)
{
    HALP_EFI_TIME EfiTime;

    if (!HalpEfiGetTime || !HalpEfiSetTime)
        return FALSE;

    /* Keep the time zone and daylight settings the firmware holds */
    if (HalpCallEfiClock(HalpEfiGetTime, &EfiTime) != 0)
        return FALSE;

    EfiTime.Year = Time->Year;
    EfiTime.Month = (UCHAR)Time->Month;
    EfiTime.Day = (UCHAR)Time->Day;
    EfiTime.Hour = (UCHAR)Time->Hour;
    EfiTime.Minute = (UCHAR)Time->Minute;
    EfiTime.Second = (UCHAR)Time->Second;
    EfiTime.Nanosecond = Time->Milliseconds * 1000000;

    return (HalpCallEfiClock(HalpEfiSetTime, &EfiTime) == 0);
}

ARC_STATUS
NTAPI
HalGetEnvironmentVariable(
    _In_ PCH Name,
    _In_ USHORT ValueLength,
    _Out_writes_z_(ValueLength) PCH Value)
{
    if (_stricmp(Name, "LastKnownGood"))
        return ENOENT;

    strncpy(Value, HalpLastKnownGood ? "TRUE" : "FALSE", ValueLength);
    return ESUCCESS;
}

ARC_STATUS
NTAPI
HalSetEnvironmentVariable(
    _In_ PCH Name,
    _In_ PCH Value)
{
    if (_stricmp(Name, "LastKnownGood"))
        return ENOMEM;

    if (!_stricmp(Value, "TRUE"))
        HalpLastKnownGood = TRUE;
    else if (!_stricmp(Value, "FALSE"))
        HalpLastKnownGood = FALSE;
    else
        return ENOMEM;

    return ESUCCESS;
}

BOOLEAN
NTAPI
HalMakeBeep(
    _In_ ULONG Frequency)
{
    UNREFERENCED_PARAMETER(Frequency);
    return FALSE;
}

VOID
NTAPI
HalAcquireDisplayOwnership(
    _In_ PHAL_RESET_DISPLAY_PARAMETERS ResetDisplayParameters)
{
    UNREFERENCED_PARAMETER(ResetDisplayParameters);
}

VOID
NTAPI
HalDisplayString(
    _In_ PCSTR String)
{
    InbvDisplayString(String);
}

VOID
NTAPI
HalQueryDisplayParameters(
    _Out_ PULONG DispSizeX,
    _Out_ PULONG DispSizeY,
    _Out_ PULONG CursorPosX,
    _Out_ PULONG CursorPosY)
{
    UNREFERENCED_PARAMETER(DispSizeX);
    UNREFERENCED_PARAMETER(DispSizeY);
    UNREFERENCED_PARAMETER(CursorPosX);
    UNREFERENCED_PARAMETER(CursorPosY);
}

VOID
NTAPI
HalSetDisplayParameters(
    _In_ ULONG CursorPosX,
    _In_ ULONG CursorPosY)
{
    UNREFERENCED_PARAMETER(CursorPosX);
    UNREFERENCED_PARAMETER(CursorPosY);
}

/* Devices sit in the physical address space as is */
BOOLEAN
NTAPI
HalTranslateBusAddress(
    _In_ INTERFACE_TYPE InterfaceType,
    _In_ ULONG BusNumber,
    _In_ PHYSICAL_ADDRESS BusAddress,
    _Inout_ PULONG AddressSpace,
    _Out_ PPHYSICAL_ADDRESS TranslatedAddress)
{
    UNREFERENCED_PARAMETER(InterfaceType);
    UNREFERENCED_PARAMETER(BusNumber);

    *AddressSpace = 0;
    *TranslatedAddress = BusAddress;
    return TRUE;
}

NTSTATUS
NTAPI
HalAssignSlotResources(
    _In_ PUNICODE_STRING RegistryPath,
    _In_opt_ PUNICODE_STRING DriverClassName,
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ INTERFACE_TYPE BusType,
    _In_ ULONG BusNumber,
    _In_ ULONG SlotNumber,
    _Inout_ PCM_RESOURCE_LIST *AllocatedResources)
{
    UNREFERENCED_PARAMETER(RegistryPath);
    UNREFERENCED_PARAMETER(DriverClassName);
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(BusType);
    UNREFERENCED_PARAMETER(BusNumber);
    UNREFERENCED_PARAMETER(SlotNumber);
    UNREFERENCED_PARAMETER(AllocatedResources);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
NTAPI
HalAdjustResourceList(
    _Inout_ PIO_RESOURCE_REQUIREMENTS_LIST *ResourceList)
{
    UNREFERENCED_PARAMETER(ResourceList);
    return STATUS_SUCCESS;
}
