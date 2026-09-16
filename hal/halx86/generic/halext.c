/*
 * PROJECT:     ReactOS Hardware Abstraction Layer
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Processor registration, WHEA bugcheck, counter and firmware variable routines
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/* INCLUDES ******************************************************************/

#include <hal.h>
#include <smp.h>

/* WHEA DEFINITIONS **********************************************************/

typedef enum _WHEA_ERROR_SOURCE_TYPE
{
    WheaErrSrcTypeMCE = 0,
    WheaErrSrcTypeCMC = 1,
    WheaErrSrcTypeCPE = 2,
    WheaErrSrcTypeNMI = 3,
    WheaErrSrcTypePCIe = 4,
    WheaErrSrcTypeGeneric = 5,
    WheaErrSrcTypeINIT = 6,
    WheaErrSrcTypeBOOT = 7,
    WheaErrSrcTypeSCIGeneric = 8,
    WheaErrSrcTypeIPFMCA = 9,
    WheaErrSrcTypeIPFCMC = 10,
    WheaErrSrcTypeIPFCPE = 11,
    WheaErrSrcTypeMax = 12
} WHEA_ERROR_SOURCE_TYPE, *PWHEA_ERROR_SOURCE_TYPE;

#define WHEA_ERROR_RECORD_SIGNATURE 'REPC'

#include <pshpack1.h>

typedef struct _WHEA_ERROR_RECORD_HEADER
{
    ULONG Signature;
    WHEA_REVISION Revision;
    ULONG SignatureEnd;
    USHORT SectionCount;
    WHEA_ERROR_SEVERITY Severity;
    ULONG ValidBits;
    ULONG Length;
    LARGE_INTEGER Timestamp;
    GUID PlatformId;
    GUID PartitionId;
    GUID CreatorId;
    GUID NotifyType;
    ULONGLONG RecordId;
    ULONG Flags;
    ULONGLONG PersistenceInfo;
    UCHAR Reserved[12];
} WHEA_ERROR_RECORD_HEADER, *PWHEA_ERROR_RECORD_HEADER;

typedef union _WHEA_NMI_ERROR_SECTION_FLAGS
{
    struct
    {
        ULONG HypervisorError:1;
        ULONG Reserved:31;
    } DUMMYSTRUCTNAME;
    ULONG AsULONG;
} WHEA_NMI_ERROR_SECTION_FLAGS, *PWHEA_NMI_ERROR_SECTION_FLAGS;

typedef struct _WHEA_NMI_ERROR_SECTION
{
    UCHAR Data[8];
    WHEA_NMI_ERROR_SECTION_FLAGS Flags;
} WHEA_NMI_ERROR_SECTION, *PWHEA_NMI_ERROR_SECTION;

typedef struct _WHEA_XPF_MCA_SECTION
{
    ULONG VersionNumber;
    ULONG CpuVendor;
    LARGE_INTEGER Timestamp;
    ULONG ProcessorNumber;
    ULONGLONG GlobalStatus;
    ULONGLONG InstructionPointer;
    ULONG BankNumber;
    MCI_STATS Status;
    ULONGLONG Address;
    ULONGLONG Misc;
    ULONG ExtendedRegisterCount;
    ULONG Reserved2;
    ULONGLONG ExtendedRegisters[24];
} WHEA_XPF_MCA_SECTION, *PWHEA_XPF_MCA_SECTION;

/* Leading fields of WHEA_ERROR_SOURCE_DESCRIPTOR */
typedef struct _HALP_WHEA_SOURCE_HEADER
{
    ULONG Length;
    ULONG Version;
    WHEA_ERROR_SOURCE_TYPE Type;
} HALP_WHEA_SOURCE_HEADER, *PHALP_WHEA_SOURCE_HEADER;

/*
 * WHEA_ERROR_PACKET_V1. The error section union is kept opaque at the size
 * of its largest member, the PCI Express section.
 */
typedef struct _HALP_WHEA_PACKET
{
    ULONG Signature;
    ULONG Flags;
    ULONG Size;
    ULONG RawDataLength;
    ULONGLONG Reserved1;
    ULONGLONG Context;
    ULONG ErrorType;
    WHEA_ERROR_SEVERITY ErrorSeverity;
    ULONG ErrorSourceId;
    WHEA_ERROR_SOURCE_TYPE ErrorSourceType;
    ULONG Reserved2;
    ULONG Version;
    ULONGLONG Cpu;
    UCHAR ErrorSection[208];
    ULONG RawDataFormat;
    ULONG RawDataOffset;
    UCHAR RawData[ANYSIZE_ARRAY];
} HALP_WHEA_PACKET, *PHALP_WHEA_PACKET;

#include <poppack.h>

typedef struct _WHEA_ERROR_RECORD
{
    WHEA_ERROR_RECORD_HEADER Header;
    WHEA_ERROR_RECORD_SECTION_DESCRIPTOR SectionDescriptor[ANYSIZE_ARRAY];
} WHEA_ERROR_RECORD;

C_ASSERT(sizeof(WHEA_ERROR_RECORD_HEADER) == 128);
C_ASSERT(FIELD_OFFSET(WHEA_XPF_MCA_SECTION, Status) == 40);

#define HalpGetSectionData(Record, Section) \
    ((PVOID)((PUCHAR)(Record) + (Section)->SectionOffset))

/* NMI_HARDWARE_FAILURE parameter 1 for a fatal WHEA NMI section */
#define HALP_WHEA_NMI_BUGCHECK_ID 0x4F4454

#if (NTDDI_VERSION >= NTDDI_WIN7)
static const GUID HalpXpfMcaSectionGuid =
    {0x8A1E1D01, 0x42F9, 0x4557, {0x9C, 0x33, 0x56, 0x5E, 0x5C, 0xC3, 0xF7, 0xE8}};
#else
static const GUID HalpProcessorGenericSectionGuid =
    {0x9876CCAD, 0x47B4, 0x4BDB, {0xB6, 0x5E, 0x16, 0xF1, 0x93, 0xC4, 0xF3, 0xDB}};
static const GUID HalpPciExpressSectionGuid =
    {0xD995E954, 0xBBC1, 0x430F, {0xAD, 0x91, 0xB4, 0x4D, 0xCB, 0x3C, 0x6F, 0x35}};
static const GUID HalpErrorPacketSectionGuid =
    {0xE71254E9, 0xC1B9, 0x4940, {0xAB, 0x76, 0x90, 0x97, 0x03, 0xA4, 0x32, 0x0F}};
#endif
static const GUID HalpNmiSectionGuid =
    {0xE71254E7, 0xC1B9, 0x4940, {0xAB, 0x76, 0x90, 0x97, 0x03, 0xA4, 0x32, 0x0F}};

/* GLOBALS *******************************************************************/

extern HALP_APIC_INFO_TABLE HalpApicInfoTable;

/* UEFI variable attributes */
#define EFI_VARIABLE_NON_VOLATILE   0x00000001
#define EFI_VARIABLE_APPEND_WRITE   0x00000040

/* HalEnumerateEnvironmentVariablesEx information classes */
#define HALP_VARIABLE_NAMES             1
#define HALP_VARIABLE_NAMES_AND_VALUES  2

/* Counter set handles count up from here and are never zero */
#define HALP_COUNTER_SET_HANDLE_BASE 0x80000000

static LONG HalpCounterSetAllocated;
static ULONG_PTR HalpCounterSetHandle = HALP_COUNTER_SET_HANDLE_BASE;

/* FUNCTIONS *****************************************************************/

/**
 * @brief
 * Returns the number of processors the platform can have.
 */
ULONG
NTAPI
HalQueryMaximumProcessorCount(VOID)
{
    ULONG Count;

    /* A uniprocessor HAL only runs the boot processor */
    if (HalpBuildType & PRCB_BUILD_UNIPROCESSOR)
    {
        return 1;
    }

    /* The MADT count stays zero when no MADT was parsed */
    Count = HalpApicInfoTable.ProcessorCount;
    if (Count < (ULONG)KeNumberProcessors)
    {
        Count = KeNumberProcessors;
    }

    return Count;
}

/**
 * @brief
 * Checks whether a new processor can be brought online.
 *
 * @param[in] ProcessorNumber
 * Number of processors the kernel has registered so far.
 *
 * @param[in] ProcessorId
 * The local APIC ID of the new processor.
 *
 * @return
 * STATUS_CONFLICTING_ADDRESSES when flat logical destinations are already
 * full, STATUS_SUCCESS otherwise.
 */
NTSTATUS
NTAPI
HalRegisterDynamicProcessor(
    _In_ ULONG ProcessorNumber,
    _In_ ULONG ProcessorId)
{
    HAL_INTERRUPT_TARGET_DESCRIPTOR TargetInfo;

    UNREFERENCED_PARAMETER(ProcessorNumber);
    UNREFERENCED_PARAMETER(ProcessorId);

    /* Flat logical destinations have one bit per processor, and all 8 are taken */
    if (NT_SUCCESS(HalGetInterruptTargetInformation(TargetGlobal, 0, &TargetInfo)) &&
        (TargetInfo.ApicRouting.DestinationFormat == ApicDestinationModeLogicalFlat) &&
        (HalQueryMaximumProcessorCount() >= 8))
    {
        return STATUS_CONFLICTING_ADDRESSES;
    }

    return STATUS_SUCCESS;
}

/**
 * @brief
 * Registers the HAL errata callbacks with the kernel.
 */
NTSTATUS
NTAPI
HalRegisterErrataCallbacks(VOID)
{
    /* The kernel has no errata manager */
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Bugchecks the system for a fatal WHEA error record.
 */
#if (NTDDI_VERSION >= NTDDI_WIN7)
DECLSPEC_NORETURN
VOID
NTAPI
HalBugCheckSystem(
    _In_ PWHEA_ERROR_SOURCE_DESCRIPTOR ErrorSource,
    _In_ PWHEA_ERROR_RECORD ErrorRecord)
{
    PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR Section;
    PWHEA_NMI_ERROR_SECTION NmiSection;
    PWHEA_XPF_MCA_SECTION McaSection;
    ULONGLONG McaStatus = 0;
    ULONG Index;

    for (Index = 0; Index < ErrorRecord->Header.SectionCount; Index++)
    {
        Section = &ErrorRecord->SectionDescriptor[Index];

        if (IsEqualGUID(&Section->SectionType, &HalpNmiSectionGuid))
        {
            NmiSection = HalpGetSectionData(ErrorRecord, Section);
            if (!NmiSection->Flags.HypervisorError)
            {
                KeBugCheckEx(NMI_HARDWARE_FAILURE, HALP_WHEA_NMI_BUGCHECK_ID, 0, 0, 0);
            }
        }
        else if (IsEqualGUID(&Section->SectionType, &HalpXpfMcaSectionGuid))
        {
            McaSection = HalpGetSectionData(ErrorRecord, Section);
            McaStatus = McaSection->Status.QuadPart;
            break;
        }
    }

    KeBugCheckEx(WHEA_UNCORRECTABLE_ERROR,
                 ((PHALP_WHEA_SOURCE_HEADER)ErrorSource)->Type,
                 (ULONG_PTR)ErrorRecord,
                 (ULONG)(McaStatus >> 32),
                 (ULONG)McaStatus);
}
#else
static
PHALP_WHEA_PACKET
NTAPI
HalpFindErrorPacket(
    _In_ PWHEA_ERROR_RECORD ErrorRecord)
{
    PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR Section;
    ULONG Index, Count, Length;

    Count = ErrorRecord->Header.SectionCount;
    Length = ErrorRecord->Header.Length;

    if ((ErrorRecord->Header.Signature != WHEA_ERROR_RECORD_SIGNATURE) ||
        (Length < sizeof(WHEA_ERROR_RECORD_HEADER) +
                  Count * sizeof(WHEA_ERROR_RECORD_SECTION_DESCRIPTOR)))
    {
        return NULL;
    }

    for (Index = 0; Index < Count; Index++)
    {
        Section = &ErrorRecord->SectionDescriptor[Index];
        if (!IsEqualGUID(&Section->SectionType, &HalpErrorPacketSectionGuid))
        {
            continue;
        }

        /* The packet must be inside the record */
        if ((Section->SectionOffset > Length) ||
            (Section->SectionLength > Length - Section->SectionOffset))
        {
            return NULL;
        }

        return HalpGetSectionData(ErrorRecord, Section);
    }

    return NULL;
}

DECLSPEC_NORETURN
VOID
NTAPI
HalBugCheckSystem(
    _In_ PWHEA_ERROR_RECORD ErrorRecord)
{
    PWHEA_ERROR_RECORD_SECTION_DESCRIPTOR Section;
    PWHEA_NMI_ERROR_SECTION NmiSection;
    PHALP_WHEA_PACKET Packet;
    ULONGLONG McaStatus;
    ULONG Index, Count;

    Count = ErrorRecord->Header.SectionCount;
    if (Count != 0)
    {
        /* Use the primary section, or the first one if none is marked */
        Section = &ErrorRecord->SectionDescriptor[0];
        for (Index = 0; Index < Count; Index++)
        {
            if (ErrorRecord->SectionDescriptor[Index].Flags.Primary)
            {
                Section = &ErrorRecord->SectionDescriptor[Index];
                break;
            }
        }

        if (IsEqualGUID(&Section->SectionType, &HalpProcessorGenericSectionGuid))
        {
            Packet = HalpFindErrorPacket(ErrorRecord);
            if (Packet)
            {
                McaStatus = ((PMCA_EXCEPTION)Packet->RawData)->u.Mca.Status.QuadPart;
                KeBugCheckEx(WHEA_UNCORRECTABLE_ERROR,
                             Packet->ErrorSourceType,
                             (ULONG_PTR)ErrorRecord,
                             (ULONG)(McaStatus >> 32),
                             (ULONG)McaStatus);
            }
        }
        else if (IsEqualGUID(&Section->SectionType, &HalpNmiSectionGuid))
        {
            NmiSection = HalpGetSectionData(ErrorRecord, Section);
            if (!NmiSection->Flags.HypervisorError)
            {
                /* Report it as a hardware NMI, which halts the system */
                HalHandleNMI(NULL);
            }
        }
        else if (IsEqualGUID(&Section->SectionType, &HalpPciExpressSectionGuid))
        {
            KeBugCheckEx(WHEA_UNCORRECTABLE_ERROR,
                         WheaErrSrcTypePCIe,
                         (ULONG_PTR)ErrorRecord,
                         0,
                         0);
        }
    }

    KeBugCheckEx(WHEA_UNCORRECTABLE_ERROR,
                 WheaErrSrcTypeMCE,
                 (ULONG_PTR)ErrorRecord,
                 0,
                 0);
}
#endif // (NTDDI_VERSION >= NTDDI_WIN7)

/**
 * @brief
 * Reserves the performance monitoring unit for one caller.
 */
NTSTATUS
NTAPI
HalAllocateHardwareCounters(
    _In_reads_(GroupCount) PGROUP_AFFINITY GroupAffinty,
    _In_ ULONG GroupCount,
    _In_ PPHYSICAL_COUNTER_RESOURCE_LIST ResourceList,
    _Out_ PHANDLE CounterSetHandle)
{
    UNREFERENCED_PARAMETER(GroupAffinty);
    UNREFERENCED_PARAMETER(GroupCount);
    UNREFERENCED_PARAMETER(ResourceList);

    if (!CounterSetHandle)
    {
        return STATUS_INVALID_PARAMETER;
    }

    *CounterSetHandle = NULL;

    if (InterlockedCompareExchange(&HalpCounterSetAllocated, 1, 0) != 0)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    HalpCounterSetHandle++;
    if (HalpCounterSetHandle == 0)
    {
        HalpCounterSetHandle = HALP_COUNTER_SET_HANDLE_BASE;
    }

    *CounterSetHandle = (HANDLE)HalpCounterSetHandle;
    return STATUS_SUCCESS;
}

/**
 * @brief
 * Releases the counter set returned by HalAllocateHardwareCounters.
 */
NTSTATUS
NTAPI
HalFreeHardwareCounters(
    _In_ HANDLE CounterSetHandle)
{
    if (CounterSetHandle != (HANDLE)HalpCounterSetHandle)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (InterlockedCompareExchange(&HalpCounterSetAllocated, 0, 1) != 1)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

/*
 * The firmware variable routines need EFI runtime services, which the HAL
 * does not have. Requests are validated when booted through EFI and then
 * fail with STATUS_NOT_IMPLEMENTED.
 */

/**
 * @brief
 * Reads a firmware variable.
 */
NTSTATUS
NTAPI
HalGetEnvironmentVariableEx(
    _In_ PCWSTR VariableName,
    _In_ LPGUID VendorGuid,
    _Out_writes_bytes_opt_(*ValueLength) PVOID Value,
    _Inout_ PULONG ValueLength,
    _Out_opt_ PULONG Attributes)
{
    UNREFERENCED_PARAMETER(VariableName);
    UNREFERENCED_PARAMETER(VendorGuid);
    UNREFERENCED_PARAMETER(Value);
    UNREFERENCED_PARAMETER(ValueLength);
    UNREFERENCED_PARAMETER(Attributes);

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Writes or deletes a firmware variable.
 */
NTSTATUS
NTAPI
HalSetEnvironmentVariableEx(
    _In_ PCWSTR VariableName,
    _In_ LPGUID VendorGuid,
    _In_reads_bytes_opt_(ValueLength) PVOID Value,
    _In_ ULONG ValueLength,
    _In_ ULONG Attributes)
{
    UNREFERENCED_PARAMETER(VariableName);
    UNREFERENCED_PARAMETER(VendorGuid);
    UNREFERENCED_PARAMETER(Value);

    if (!HalBootViaEfi)
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!(Attributes & EFI_VARIABLE_NON_VOLATILE))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Appending is not offered */
    if (Attributes & EFI_VARIABLE_APPEND_WRITE)
        return STATUS_INVALID_PARAMETER;

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Lists the firmware variables.
 */
NTSTATUS
NTAPI
HalEnumerateEnvironmentVariablesEx(
    _In_ ULONG InformationClass,
    _Out_writes_bytes_opt_(*BufferLength) PVOID Buffer,
    _Inout_ PULONG BufferLength)
{
    UNREFERENCED_PARAMETER(BufferLength);

    if (!HalBootViaEfi)
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    if ((InformationClass != HALP_VARIABLE_NAMES) &&
        (InformationClass != HALP_VARIABLE_NAMES_AND_VALUES))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if ((ULONG_PTR)Buffer & (sizeof(ULONG) - 1))
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_NOT_IMPLEMENTED;
}

/**
 * @brief
 * Reports the size of the firmware variable store.
 */
NTSTATUS
NTAPI
HalQueryEnvironmentVariableInfoEx(
    _In_ ULONG Attributes,
    _Out_ PULONGLONG MaximumVariableStorageSize,
    _Out_ PULONGLONG RemainingVariableStorageSize,
    _Out_ PULONGLONG MaximumVariableSize)
{
    UNREFERENCED_PARAMETER(MaximumVariableStorageSize);
    UNREFERENCED_PARAMETER(RemainingVariableStorageSize);
    UNREFERENCED_PARAMETER(MaximumVariableSize);

    if (!HalBootViaEfi)
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    if (!(Attributes & EFI_VARIABLE_NON_VOLATILE))
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_NOT_IMPLEMENTED;
}

/* EOF */
