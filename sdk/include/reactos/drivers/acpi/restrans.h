/*
 * PROJECT:     ReactOS ACPI
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     The resource-translation interface ACPI and the Resource Hub
 *              exchange, so that connection resources can be translated
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

/*
 * ACPI does not translate ACPI 5.0 Connection() descriptors itself. It hands the
 * job to whoever owns \Device\RESOURCE_HUB by sending this one IOCTL, whose
 * buffer carries callbacks in *both* directions:
 *
 *   ACPI -> hub   the imports: how to resolve a BIOS name, allocate a GSIV for a
 *                 secondary (GPIO-backed) interrupt, and update interrupt
 *                 properties. The hub needs ACPI for all three.
 *   hub  -> ACPI  the exports: how to translate a firmware descriptor into NT
 *                 resources, associate a BIOS name with a device object, and
 *                 look a translated descriptor up by GSIV.
 *
 * ACPI stores the result and calls into it from
 * AcpiExternalTranslateBiosToNtResources / AddBiosNameDeviceAssociation /
 * QueryTranslatedDescriptorForGsiv, under a shared lock, dropping it again if
 * the hub device disappears.
 *
 * Recovered from acpi.sys (QueryExternalTranslatorInterface at
 * Reference_Win10/acpi.sys.c:101989, and the three call sites at :107152-:107227
 * which is where the member names and signatures come from) together with the
 * hub's side, RhpProcessTranslationInterfaceQueryIoctl at
 * HidRef/acpiex.sys.c:16536.
 *
 * PEER COUPLING, intentional and documented: the reference reaches the import
 * block through an ImportOffset field and its exports through hardcoded x64
 * offsets, none of which survive a rebuild for i386. Both peers here are ours
 * (acpi_new and acpiex), so this declares one plainly laid-out structure and
 * lets the compiler place it. Function signatures, argument order, the IOCTL
 * code and the retry contract below are the reference's exactly -- only the byte
 * offsets differ, and they could not have been preserved anyway.
 */

#pragma once

/*
 * CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0xA, METHOD_BUFFERED,
 *          FILE_READ_ACCESS | FILE_WRITE_ACCESS) == 0x2AC028, the literal
 * acpi.sys passes to IoBuildDeviceIoControlRequest. FILE_DEVICE_RESOURCE_HUB is
 * the same device type, so this belongs with the RH IOCTLs in <reshub.h>; it
 * lives here rather than there because it is private to these two drivers.
 */
#define IOCTL_RH_QUERY_TRANSLATION_INTERFACE \
    CTL_CODE(FILE_DEVICE_BUS_EXTENDER,       \
             0xA,                            \
             METHOD_BUFFERED,                \
             FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define RESOURCE_TRANSLATION_INTERFACE_VERSION 1

/*
 * What TranslateBiosToNtResources fills in. The caller sizes its first attempt
 * at sizeof(RESOURCE_TRANSLATION_RESULT) and, on STATUS_BUFFER_TOO_SMALL with a
 * non-zero SizeInOut, retries exactly once at the size it was told -- acpi.sys
 * caps the loop at two passes, so the callee must not ask twice.
 */
typedef struct _RESOURCE_TRANSLATION_RESULT
{
    ULONG Count;
    ULONG Reserved;
    /*
     * A *requirements* descriptor, not a CM one: ACPI is mid-way through
     * PnpBiosResourcesToNtResources when it calls, and the PnP manager turns the
     * requirements list into CM resources afterwards. The reference writes
     * Option and Type as one word (0x8400 -- Option 0, Type 0x84), which only
     * lands correctly on IO_RESOURCE_DESCRIPTOR's leading byte pair.
     */
    IO_RESOURCE_DESCRIPTOR Descriptor;
    /* Offset is from the start of this structure, and is always 48 */
    ULONG BiosNameOffset;
    ULONG BiosNameLength;
    CHAR BiosName[ANYSIZE_ARRAY];
} RESOURCE_TRANSLATION_RESULT, *PRESOURCE_TRANSLATION_RESULT;

/* The reference floors its size request here even for an empty name */
#define RESOURCE_TRANSLATION_RESULT_MIN_SIZE 56

/* ACPI -> hub */

typedef VOID
(NTAPI *PACPI_UNLOAD_TRANSLATION_INTERFACE)(
    _In_ PVOID Context);

typedef NTSTATUS
(NTAPI *PACPI_GET_FULLY_QUALIFIED_BIOS_NAME)(
    _In_ PDEVICE_OBJECT BiosDeviceObject,
    _In_ PSTRING BiosName,
    _Out_ PUNICODE_STRING FullyQualifiedBiosName,
    _Out_ PULONG StringLength);

typedef NTSTATUS
(NTAPI *PACPI_ALLOCATE_GSIV_FOR_SECONDARY_INTERRUPT)(
    _In_ PCHAR DescriptorName,
    _In_ ULONG DescriptorNameLength,
    _Out_ PULONG Gsiv);

typedef NTSTATUS
(NTAPI *PACPI_UPDATE_INTERRUPT_PROPERTIES)(
    _In_ ULONG Gsiv,
    _In_ KINTERRUPT_MODE Mode,
    _In_ KINTERRUPT_POLARITY Polarity);

/* hub -> ACPI */

typedef NTSTATUS
(NTAPI *PRH_TRANSLATE_BIOS_TO_NT_RESOURCES)(
    _In_ PVOID Context,
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_reads_bytes_(DescriptorLength) PVOID Descriptor,
    _In_ ULONG DescriptorLength,
    _In_ ULONG Flags,
    _Out_ PRESOURCE_TRANSLATION_RESULT Result,
    _Inout_ PULONG SizeInOut);

typedef NTSTATUS
(NTAPI *PRH_BIOS_NAME_TO_DEVICE)(
    _In_ PVOID Context,
    _In_ PCUNICODE_STRING BiosName,
    _In_ PDEVICE_OBJECT DeviceObject);

/*
 * Takes no Context -- the reference passes (Gsiv, 0, IoDescriptor), so the
 * second argument is a flags word ACPI always leaves clear.
 */
typedef NTSTATUS
(NTAPI *PRH_QUERY_TRANSLATED_DESCRIPTOR_FOR_GSIV)(
    _In_ ULONG Gsiv,
    _In_ ULONG Flags,
    _Out_ PIO_RESOURCE_DESCRIPTOR IoDescriptor);

typedef struct _RESOURCE_TRANSLATION_INTERFACE_STANDARD
{
    INTERFACE Interface;

    /* Filled in by ACPI before the IOCTL */
    PVOID AcpiContext;
    PACPI_UNLOAD_TRANSLATION_INTERFACE UnloadTranslationInterface;
    PACPI_GET_FULLY_QUALIFIED_BIOS_NAME GetFullyQualifiedBiosName;
    PACPI_ALLOCATE_GSIV_FOR_SECONDARY_INTERRUPT AllocateGsivForSecondaryInterrupt;
    PACPI_UPDATE_INTERRUPT_PROPERTIES UpdateInterruptProperties;

    /* Filled in by the hub before the IOCTL completes */
    PVOID Context;
    PRH_TRANSLATE_BIOS_TO_NT_RESOURCES TranslateBiosToNtResources;
    PRH_BIOS_NAME_TO_DEVICE BiosNameToDeviceCallback;
    PRH_QUERY_TRANSLATED_DESCRIPTOR_FOR_GSIV QueryTranslatedDescriptorForGsiv;
} RESOURCE_TRANSLATION_INTERFACE_STANDARD, *PRESOURCE_TRANSLATION_INTERFACE_STANDARD;
