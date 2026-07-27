/*
 * PROJECT:         ReactOS Win32 Base API
 * LICENSE:         GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:         System Information Functions
 * COPYRIGHT:       Emanuele Aliberti
 *                  Christoph von Wittich
 *                  Thomas Weidenmueller
 *                  Gunnar Andre Dalsnes
 *                  Stanislav Motylkov (x86corez@gmail.com)
 *                  Mark Jansen (mark.jansen@reactos.org)
 *                  Copyright 2023 Ratin Gao <ratin@knsoft.org>
 */

/* INCLUDES *******************************************************************/

#include <k32.h>

#define NDEBUG
#include <debug.h>

#define PV_NT351 0x00030033

/* PROCESSOR_RELATIONSHIP.Flags value for SMT cores; not in the NT 5.2 PSDK headers */
#ifndef LTP_PC_SMT
#define LTP_PC_SMT 0x1
#endif

/* PRIVATE FUNCTIONS **********************************************************/

VOID
WINAPI
GetSystemInfoInternal(IN PSYSTEM_BASIC_INFORMATION BasicInfo,
                      IN PSYSTEM_PROCESSOR_INFORMATION ProcInfo,
                      OUT LPSYSTEM_INFO SystemInfo)
{
    RtlZeroMemory(SystemInfo, sizeof (SYSTEM_INFO));
    SystemInfo->wProcessorArchitecture = ProcInfo->ProcessorArchitecture;
    SystemInfo->wReserved = 0;
    SystemInfo->dwPageSize = BasicInfo->PageSize;
    SystemInfo->lpMinimumApplicationAddress = (PVOID)BasicInfo->MinimumUserModeAddress;
    SystemInfo->lpMaximumApplicationAddress = (PVOID)BasicInfo->MaximumUserModeAddress;
    SystemInfo->dwActiveProcessorMask = BasicInfo->ActiveProcessorsAffinityMask;
    SystemInfo->dwNumberOfProcessors = BasicInfo->NumberOfProcessors;
    SystemInfo->wProcessorLevel = ProcInfo->ProcessorLevel;
    SystemInfo->wProcessorRevision = ProcInfo->ProcessorRevision;
    SystemInfo->dwAllocationGranularity = BasicInfo->AllocationGranularity;

    switch (ProcInfo->ProcessorArchitecture)
    {
        case PROCESSOR_ARCHITECTURE_INTEL:
            switch (ProcInfo->ProcessorLevel)
            {
                case 3:
                    SystemInfo->dwProcessorType = PROCESSOR_INTEL_386;
                    break;
                case 4:
                    SystemInfo->dwProcessorType = PROCESSOR_INTEL_486;
                    break;
                default:
                    SystemInfo->dwProcessorType = PROCESSOR_INTEL_PENTIUM;
            }
            break;

        case PROCESSOR_ARCHITECTURE_AMD64:
            SystemInfo->dwProcessorType = PROCESSOR_AMD_X8664;
            break;

        case PROCESSOR_ARCHITECTURE_IA64:
            SystemInfo->dwProcessorType = PROCESSOR_INTEL_IA64;
            break;

        default:
            SystemInfo->dwProcessorType = 0;
            break;
    }

    if (PV_NT351 > GetProcessVersion(0))
    {
        SystemInfo->wProcessorLevel = 0;
        SystemInfo->wProcessorRevision = 0;
    }
}

static
UINT
BaseQuerySystemFirmware(
    _In_ DWORD FirmwareTableProviderSignature,
    _In_ DWORD FirmwareTableID,
    _Out_writes_bytes_to_opt_(BufferSize, return) PVOID pFirmwareTableBuffer,
    _In_ DWORD BufferSize,
    _In_ SYSTEM_FIRMWARE_TABLE_ACTION Action)
{
    SYSTEM_FIRMWARE_TABLE_INFORMATION* SysFirmwareInfo;
    ULONG Result = 0, ReturnedSize;
    ULONG TotalSize = BufferSize + sizeof(SYSTEM_FIRMWARE_TABLE_INFORMATION);
    NTSTATUS Status;

    SysFirmwareInfo = RtlAllocateHeap(RtlGetProcessHeap(), HEAP_ZERO_MEMORY, TotalSize);
    if (!SysFirmwareInfo)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    _SEH2_TRY
    {
        SysFirmwareInfo->ProviderSignature = FirmwareTableProviderSignature;
        SysFirmwareInfo->TableID = FirmwareTableID;
        SysFirmwareInfo->Action = Action;
        SysFirmwareInfo->TableBufferLength = BufferSize;

        Status = NtQuerySystemInformation(SystemFirmwareTableInformation, SysFirmwareInfo, TotalSize, &ReturnedSize);

        if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_TOO_SMALL)
            Result = SysFirmwareInfo->TableBufferLength;

        if (NT_SUCCESS(Status) && pFirmwareTableBuffer)
        {
            RtlCopyMemory(pFirmwareTableBuffer, SysFirmwareInfo->TableBuffer, SysFirmwareInfo->TableBufferLength);
        }
    }
    _SEH2_FINALLY
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, SysFirmwareInfo);
    }
    _SEH2_END;

    BaseSetLastNTError(Status);
    return Result;
}

/* PUBLIC FUNCTIONS ***********************************************************/

/*
 * @implemented
 */
SIZE_T
WINAPI
GetLargePageMinimum(VOID)
{
    return SharedUserData->LargePageMinimum;
}

/*
 * @implemented
 */
VOID
WINAPI
GetSystemInfo(IN LPSYSTEM_INFO lpSystemInfo)
{
    SYSTEM_BASIC_INFORMATION BasicInfo;
    SYSTEM_PROCESSOR_INFORMATION ProcInfo;
    NTSTATUS Status;

    Status = NtQuerySystemInformation(SystemBasicInformation,
                                      &BasicInfo,
                                      sizeof(BasicInfo),
                                      0);
    if (!NT_SUCCESS(Status)) return;

    Status = NtQuerySystemInformation(SystemProcessorInformation,
                                      &ProcInfo,
                                      sizeof(ProcInfo),
                                      0);
    if (!NT_SUCCESS(Status)) return;

    GetSystemInfoInternal(&BasicInfo, &ProcInfo, lpSystemInfo);
}

/*
 * @implemented
 */
BOOL
WINAPI
IsProcessorFeaturePresent(IN DWORD ProcessorFeature)
{
    if (ProcessorFeature >= PROCESSOR_FEATURE_MAX) return FALSE;
    return ((BOOL)SharedUserData->ProcessorFeatures[ProcessorFeature]);
}

/*
 * @implemented
 */
BOOL
WINAPI
GetSystemRegistryQuota(OUT PDWORD pdwQuotaAllowed,
                       OUT PDWORD pdwQuotaUsed)
{
    SYSTEM_REGISTRY_QUOTA_INFORMATION QuotaInfo;
    ULONG BytesWritten;
    NTSTATUS Status;

    Status = NtQuerySystemInformation(SystemRegistryQuotaInformation,
                                      &QuotaInfo,
                                      sizeof(QuotaInfo),
                                      &BytesWritten);
    if (NT_SUCCESS(Status))
    {
      if (pdwQuotaAllowed) *pdwQuotaAllowed = QuotaInfo.RegistryQuotaAllowed;
      if (pdwQuotaUsed) *pdwQuotaUsed = QuotaInfo.RegistryQuotaUsed;
      return TRUE;
    }

    BaseSetLastNTError(Status);
    return FALSE;
}

/*
 * @implemented
 */
VOID
WINAPI
GetNativeSystemInfo(IN LPSYSTEM_INFO lpSystemInfo)
{
    SYSTEM_BASIC_INFORMATION BasicInfo;
    SYSTEM_PROCESSOR_INFORMATION ProcInfo;
    NTSTATUS Status;

    Status = RtlGetNativeSystemInformation(SystemBasicInformation,
                                           &BasicInfo,
                                           sizeof(BasicInfo),
                                           0);
    if (!NT_SUCCESS(Status)) return;

    Status = RtlGetNativeSystemInformation(SystemProcessorInformation,
                                           &ProcInfo,
                                           sizeof(ProcInfo),
                                           0);
    if (!NT_SUCCESS(Status)) return;

    GetSystemInfoInternal(&BasicInfo, &ProcInfo, lpSystemInfo);
}

/*
 * @implemented
 */
BOOL
WINAPI
GetLogicalProcessorInformation(OUT PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Buffer,
                               IN OUT PDWORD ReturnLength)
{
    NTSTATUS Status;

    if (!ReturnLength)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Status = NtQuerySystemInformation(SystemLogicalProcessorInformation,
                                      Buffer,
                                      *ReturnLength,
                                      ReturnLength);

    /* Normalize the error to what Win32 expects */
    if (Status == STATUS_INFO_LENGTH_MISMATCH) Status = STATUS_BUFFER_TOO_SMALL;
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    return TRUE;
}

/*
 * @implemented
 *
 * The kernel does not support SystemLogicalProcessorInformationEx, so the
 * extended records are synthesized from the legacy information. ReactOS is
 * a single-group system: every record gets group number 0, and the package
 * and group records (which the legacy interface cannot describe) are
 * generated to span all active processors.
 */
BOOL
WINAPI
GetLogicalProcessorInformationEx(IN LOGICAL_PROCESSOR_RELATIONSHIP RelationshipType,
                                 OUT PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Buffer OPTIONAL,
                                 IN OUT PDWORD ReturnedLength)
{
    /* Sizes of the variable-length records we emit, one group entry each */
    const DWORD HeaderSize = FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor);
    const DWORD ProcessorSize = HeaderSize + FIELD_OFFSET(PROCESSOR_RELATIONSHIP, GroupMask) + sizeof(GROUP_AFFINITY);
    const DWORD NumaSize = HeaderSize + sizeof(NUMA_NODE_RELATIONSHIP);
    const DWORD CacheSize = HeaderSize + sizeof(CACHE_RELATIONSHIP);
    const DWORD GroupSize = HeaderSize + FIELD_OFFSET(GROUP_RELATIONSHIP, GroupInfo) + sizeof(PROCESSOR_GROUP_INFO);

    NTSTATUS Status;
    SYSTEM_BASIC_INFORMATION BasicInfo;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION LegacyInfo = NULL;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Entry;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX ExEntry;
    ULONG LegacyLength = 0;
    ULONG Count, i;
    DWORD RequiredLength;
    KAFFINITY ActiveMask;
    UCHAR ActiveCount;
    BOOLEAN SeenPackage = FALSE;
    PUCHAR Output;

    if (!ReturnedLength ||
        ((RelationshipType != RelationProcessorCore) &&
         (RelationshipType != RelationNumaNode) &&
         (RelationshipType != RelationCache) &&
         (RelationshipType != RelationProcessorPackage) &&
         (RelationshipType != RelationGroup) &&
         (RelationshipType != RelationAll)))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Needed for the synthesized package and group records */
    Status = NtQuerySystemInformation(SystemBasicInformation,
                                      &BasicInfo,
                                      sizeof(BasicInfo),
                                      NULL);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Grab the legacy processor topology, resizing if it grows under us */
    for (;;)
    {
        Status = NtQuerySystemInformation(SystemLogicalProcessorInformation,
                                          LegacyInfo,
                                          LegacyLength,
                                          &LegacyLength);
        if (Status != STATUS_INFO_LENGTH_MISMATCH) break;

        if (LegacyInfo) RtlFreeHeap(RtlGetProcessHeap(), 0, LegacyInfo);
        LegacyInfo = RtlAllocateHeap(RtlGetProcessHeap(), 0, LegacyLength);
        if (!LegacyInfo)
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
    }

    if (!NT_SUCCESS(Status))
    {
        if (LegacyInfo) RtlFreeHeap(RtlGetProcessHeap(), 0, LegacyInfo);
        BaseSetLastNTError(Status);
        return FALSE;
    }

    Count = LegacyLength / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);

    /* First pass: compute the size of what we will return */
    RequiredLength = 0;
    for (i = 0; i < Count; i++)
    {
        Entry = &LegacyInfo[i];
        switch (Entry->Relationship)
        {
            case RelationProcessorCore:
                if ((RelationshipType == RelationProcessorCore) ||
                    (RelationshipType == RelationAll))
                {
                    RequiredLength += ProcessorSize;
                }
                break;

            case RelationProcessorPackage:
                SeenPackage = TRUE;
                if ((RelationshipType == RelationProcessorPackage) ||
                    (RelationshipType == RelationAll))
                {
                    RequiredLength += ProcessorSize;
                }
                break;

            case RelationNumaNode:
                if ((RelationshipType == RelationNumaNode) ||
                    (RelationshipType == RelationAll))
                {
                    RequiredLength += NumaSize;
                }
                break;

            case RelationCache:
                if ((RelationshipType == RelationCache) ||
                    (RelationshipType == RelationAll))
                {
                    RequiredLength += CacheSize;
                }
                break;

            default:
                break;
        }
    }

    if (!SeenPackage &&
        ((RelationshipType == RelationProcessorPackage) ||
         (RelationshipType == RelationAll)))
    {
        RequiredLength += ProcessorSize;
    }

    if ((RelationshipType == RelationGroup) ||
        (RelationshipType == RelationAll))
    {
        RequiredLength += GroupSize;
    }

    if (!Buffer || (*ReturnedLength < RequiredLength))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, LegacyInfo);
        *ReturnedLength = RequiredLength;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    ActiveMask = (KAFFINITY)BasicInfo.ActiveProcessorsAffinityMask;

    /* Second pass: convert each matching record */
    Output = (PUCHAR)Buffer;
    for (i = 0; i < Count; i++)
    {
        Entry = &LegacyInfo[i];
        ExEntry = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Output;

        switch (Entry->Relationship)
        {
            case RelationProcessorCore:
            case RelationProcessorPackage:
                if ((RelationshipType != Entry->Relationship) &&
                    (RelationshipType != RelationAll))
                {
                    break;
                }

                RtlZeroMemory(ExEntry, ProcessorSize);
                ExEntry->Relationship = Entry->Relationship;
                ExEntry->Size = ProcessorSize;
                if (Entry->Relationship == RelationProcessorCore)
                {
                    ExEntry->Processor.Flags = Entry->ProcessorCore.Flags ? LTP_PC_SMT : 0;
                }
                ExEntry->Processor.GroupCount = 1;
                ExEntry->Processor.GroupMask[0].Mask = (KAFFINITY)Entry->ProcessorMask;
                Output += ProcessorSize;
                break;

            case RelationNumaNode:
                if ((RelationshipType != RelationNumaNode) &&
                    (RelationshipType != RelationAll))
                {
                    break;
                }

                RtlZeroMemory(ExEntry, NumaSize);
                ExEntry->Relationship = RelationNumaNode;
                ExEntry->Size = NumaSize;
                ExEntry->NumaNode.NodeNumber = Entry->NumaNode.NodeNumber;
                ExEntry->NumaNode.GroupMask.Mask = (KAFFINITY)Entry->ProcessorMask;
                Output += NumaSize;
                break;

            case RelationCache:
                if ((RelationshipType != RelationCache) &&
                    (RelationshipType != RelationAll))
                {
                    break;
                }

                RtlZeroMemory(ExEntry, CacheSize);
                ExEntry->Relationship = RelationCache;
                ExEntry->Size = CacheSize;
                ExEntry->Cache.Level = Entry->Cache.Level;
                ExEntry->Cache.Associativity = Entry->Cache.Associativity;
                ExEntry->Cache.LineSize = Entry->Cache.LineSize;
                ExEntry->Cache.CacheSize = Entry->Cache.Size;
                ExEntry->Cache.Type = Entry->Cache.Type;
                ExEntry->Cache.GroupMask.Mask = (KAFFINITY)Entry->ProcessorMask;
                Output += CacheSize;
                break;

            default:
                break;
        }
    }

    if (!SeenPackage &&
        ((RelationshipType == RelationProcessorPackage) ||
         (RelationshipType == RelationAll)))
    {
        ExEntry = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Output;
        RtlZeroMemory(ExEntry, ProcessorSize);
        ExEntry->Relationship = RelationProcessorPackage;
        ExEntry->Size = ProcessorSize;
        ExEntry->Processor.GroupCount = 1;
        ExEntry->Processor.GroupMask[0].Mask = ActiveMask;
        Output += ProcessorSize;
    }

    if ((RelationshipType == RelationGroup) ||
        (RelationshipType == RelationAll))
    {
        KAFFINITY Mask;

        ActiveCount = 0;
        for (Mask = ActiveMask; Mask != 0; Mask >>= 1)
        {
            if (Mask & 1) ActiveCount++;
        }

        ExEntry = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Output;
        RtlZeroMemory(ExEntry, GroupSize);
        ExEntry->Relationship = RelationGroup;
        ExEntry->Size = GroupSize;
        ExEntry->Group.MaximumGroupCount = 1;
        ExEntry->Group.ActiveGroupCount = 1;
        ExEntry->Group.GroupInfo[0].MaximumProcessorCount = BasicInfo.NumberOfProcessors;
        ExEntry->Group.GroupInfo[0].ActiveProcessorCount = ActiveCount;
        ExEntry->Group.GroupInfo[0].ActiveProcessorMask = ActiveMask;
        Output += GroupSize;
    }

    RtlFreeHeap(RtlGetProcessHeap(), 0, LegacyInfo);
    *ReturnedLength = RequiredLength;
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetNumaHighestNodeNumber(OUT PULONG HighestNodeNumber)
{
    NTSTATUS Status;
    ULONG Length;
    ULONG PartialInfo[2]; // First two members of SYSTEM_NUMA_INFORMATION

    /* Query partial NUMA info */
    Status = NtQuerySystemInformation(SystemNumaProcessorMap,
                                      PartialInfo,
                                      sizeof(PartialInfo),
                                      &Length);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    if (Length < sizeof(ULONG))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* First member of the struct is the highest node number */
    *HighestNodeNumber = PartialInfo[0];
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetNumaNodeProcessorMask(IN UCHAR Node,
                         OUT PULONGLONG ProcessorMask)
{
    NTSTATUS Status;
    SYSTEM_NUMA_INFORMATION NumaInformation;
    ULONG Length;

    /* Query NUMA information */
    Status = NtQuerySystemInformation(SystemNumaProcessorMap,
                                      &NumaInformation,
                                      sizeof(NumaInformation),
                                      &Length);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Validate input node number */
    if (Node > NumaInformation.HighestNodeNumber)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Return mask for that node */
    *ProcessorMask = NumaInformation.ActiveProcessorsAffinityMask[Node];
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetNumaProcessorNode(IN UCHAR Processor,
                     OUT PUCHAR NodeNumber)
{
    NTSTATUS Status;
    SYSTEM_NUMA_INFORMATION NumaInformation;
    ULONG Length;
    ULONG Node;
    ULONGLONG Proc;

    /* Can't handle processor number >= 32 */
    if (Processor >= MAXIMUM_PROCESSORS)
    {
        *NodeNumber = -1;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Query NUMA information */
    Status = NtQuerySystemInformation(SystemNumaProcessorMap,
                                      &NumaInformation,
                                      sizeof(NumaInformation),
                                      &Length);
    if (!NT_SUCCESS(Status))
    {
        *NodeNumber = -1;
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Find ourselves */
    Node = 0;
    Proc = 1ULL << Processor;
    while ((Proc & NumaInformation.ActiveProcessorsAffinityMask[Node]) == 0ULL)
    {
        ++Node;
        /* Out of options */
        if (Node > NumaInformation.HighestNodeNumber)
        {
            *NodeNumber = -1;
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }

    /* Return found node */
    *NodeNumber = Node;
    return TRUE;
}

/*
 * @implemented
 *
 * ReactOS only has processor group 0.
 */
BOOL
WINAPI
GetNumaProcessorNodeEx(IN PPROCESSOR_NUMBER Processor,
                       OUT PUSHORT NodeNumber)
{
    NTSTATUS Status;
    SYSTEM_NUMA_INFORMATION NumaInformation;
    ULONG Length;
    ULONG Node;
    ULONGLONG Proc;

    /* Only group 0 exists, and the processor must fit in the affinity mask */
    if ((Processor->Group != 0) ||
        (Processor->Number >= MAXIMUM_PROCESSORS))
    {
        *NodeNumber = (USHORT)-1;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Query NUMA information */
    Status = NtQuerySystemInformation(SystemNumaProcessorMap,
                                      &NumaInformation,
                                      sizeof(NumaInformation),
                                      &Length);
    if (!NT_SUCCESS(Status))
    {
        *NodeNumber = (USHORT)-1;
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Find the node owning the processor */
    Node = 0;
    Proc = 1ULL << Processor->Number;
    while ((Proc & NumaInformation.ActiveProcessorsAffinityMask[Node]) == 0ULL)
    {
        ++Node;
        /* Out of options */
        if (Node > NumaInformation.HighestNodeNumber)
        {
            *NodeNumber = (USHORT)-1;
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }

    /* Return found node */
    *NodeNumber = (USHORT)Node;
    return TRUE;
}

/*
 * @implemented
 */
BOOL
WINAPI
GetNumaAvailableMemoryNode(IN UCHAR Node,
                           OUT PULONGLONG AvailableBytes)
{
    NTSTATUS Status;
    SYSTEM_NUMA_INFORMATION NumaInformation;
    ULONG Length;

    /* Query NUMA information */
    Status = NtQuerySystemInformation(SystemNumaAvailableMemory,
                                      &NumaInformation,
                                      sizeof(NumaInformation),
                                      &Length);
    if (!NT_SUCCESS(Status))
    {
        BaseSetLastNTError(Status);
        return FALSE;
    }

    /* Validate input node number */
    if (Node > NumaInformation.HighestNodeNumber)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Return available memory for that node */
    *AvailableBytes = NumaInformation.AvailableMemory[Node];
    return TRUE;
}

_Success_(return > 0)
DWORD
WINAPI
GetFirmwareEnvironmentVariableExW(
    _In_ LPCWSTR lpName,
    _In_ LPCWSTR lpGuid,
    _Out_writes_bytes_to_opt_(nSize, return) PVOID pBuffer,
    _In_ DWORD nSize,
    _Out_opt_ PDWORD pdwAttribubutes);

_Success_(return > 0)
DWORD
WINAPI
GetFirmwareEnvironmentVariableExA(
    _In_ LPCSTR lpName,
    _In_ LPCSTR lpGuid,
    _Out_writes_bytes_to_opt_(nSize, return) PVOID pBuffer,
    _In_ DWORD nSize,
    _Out_opt_ PDWORD pdwAttribubutes);

BOOL
WINAPI
SetFirmwareEnvironmentVariableExW(
    _In_ LPCWSTR lpName,
    _In_ LPCWSTR lpGuid,
    _In_reads_bytes_opt_(nSize) PVOID pValue,
    _In_ DWORD nSize,
    _In_ DWORD dwAttributes);

BOOL
WINAPI
SetFirmwareEnvironmentVariableExA(
    _In_ LPCSTR lpName,
    _In_ LPCSTR lpGuid,
    _In_reads_bytes_opt_(nSize) PVOID pValue,
    _In_ DWORD nSize,
    _In_ DWORD dwAttributes);

_Success_(return > 0)
DWORD
WINAPI
GetFirmwareEnvironmentVariableW(
    _In_ LPCWSTR lpName,
    _In_ LPCWSTR lpGuid,
    _Out_writes_bytes_to_opt_(nSize, return) PVOID pBuffer,
    _In_ DWORD nSize)
{
    return GetFirmwareEnvironmentVariableExW(lpName, lpGuid, pBuffer, nSize, NULL);
}

BOOL
WINAPI
SetFirmwareEnvironmentVariableW(
    _In_ LPCWSTR lpName,
    _In_ LPCWSTR lpGuid,
    _In_reads_bytes_opt_(nSize) PVOID pValue,
    _In_ DWORD nSize)
{
    return SetFirmwareEnvironmentVariableExW(lpName,
                                             lpGuid,
                                             pValue,
                                             nSize,
                                             VARIABLE_ATTRIBUTE_NON_VOLATILE);
}

/**
 * @name EnumSystemFirmwareTables
 * @implemented
 *
 * Obtains firmware table identifiers.
 * https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-enumsystemfirmwaretables
 *
 * @param FirmwareTableProviderSignature
 * Can be either ACPI, FIRM, or RSMB.
 *
 * @param pFirmwareTableBuffer
 * Pointer to the output buffer, can be NULL.
 *
 * @param BufferSize
 * Size of the output buffer.
 *
 * @return
 * Actual size of the data in case of success, 0 otherwise.
 *
 * @remarks
 * Data would be written to buffer only if the specified size is
 * larger or equal to the actual size, in the other case Last Error
 * value would be set to ERROR_INSUFFICIENT_BUFFER.
 * In case of incorrect provider signature, Last Error value would be
 * set to ERROR_INVALID_FUNCTION.
 *
 */
UINT
WINAPI
EnumSystemFirmwareTables(
    _In_ DWORD FirmwareTableProviderSignature,
    _Out_writes_bytes_to_opt_(BufferSize, return) PVOID pFirmwareTableEnumBuffer,
    _In_ DWORD BufferSize)
{
    return BaseQuerySystemFirmware(FirmwareTableProviderSignature,
                                   0,
                                   pFirmwareTableEnumBuffer,
                                   BufferSize,
                                   SystemFirmwareTable_Enumerate);
}

/**
 * @name GetSystemFirmwareTable
 * @implemented
 *
 * Obtains the firmware table data.
 * https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getsystemfirmwaretable
 *
 * @param FirmwareTableProviderSignature
 * Can be either ACPI, FIRM, or RSMB.
 *
 * @param FirmwareTableID
 * Correct table identifier.
 *
 * @param pFirmwareTableBuffer
 * Pointer to the output buffer, can be NULL.
 *
 * @param BufferSize
 * Size of the output buffer.
 *
 * @return
 * Actual size of the data in case of success, 0 otherwise.
 *
 * @remarks
 * Data would be written to buffer only if the specified size is
 * larger or equal to the actual size, in the other case Last Error
 * value would be set to ERROR_INSUFFICIENT_BUFFER.
 * In case of incorrect provider signature, Last Error value would be
 * set to ERROR_INVALID_FUNCTION.
 * Also Last Error value becomes ERROR_NOT_FOUND if incorrect
 * table identifier was specified along with ACPI provider, and
 * ERROR_INVALID_PARAMETER along with FIRM provider. The RSMB provider
 * accepts any table identifier.
 *
 */
UINT
WINAPI
GetSystemFirmwareTable(
    _In_ DWORD FirmwareTableProviderSignature,
    _In_ DWORD FirmwareTableID,
    _Out_writes_bytes_to_opt_(BufferSize, return) PVOID pFirmwareTableBuffer,
    _In_ DWORD BufferSize)
{
    return BaseQuerySystemFirmware(FirmwareTableProviderSignature,
                                   FirmwareTableID,
                                   pFirmwareTableBuffer,
                                   BufferSize,
                                   SystemFirmwareTable_Get);
}

/*
 * @unimplemented
 */
BOOL
WINAPI
GetSystemFileCacheSize(OUT PSIZE_T lpMinimumFileCacheSize,
                       OUT PSIZE_T lpMaximumFileCacheSize,
                       OUT PDWORD lpFlags)
{
    STUB;
    return FALSE;
}

/*
 * @unimplemented
 */
BOOL
WINAPI
SetSystemFileCacheSize(IN SIZE_T MinimumFileCacheSize,
                       IN SIZE_T MaximumFileCacheSize,
                       IN DWORD Flags)
{
    STUB;
    return FALSE;
}

/*
 * @unimplemented
 */
LONG
WINAPI
GetCurrentPackageId(UINT32 *BufferLength,
                    BYTE *Buffer)
{
    STUB;
    return APPMODEL_ERROR_NO_PACKAGE;
}
