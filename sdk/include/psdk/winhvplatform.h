/*
 * PROJECT:     ReactOS SDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Windows Hypervisor Platform, the documented way to ask for a partition
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * Covers what a client needs to make a partition, give it memory, run a
 * virtual processor and read why it stopped. The register, exit and capability
 * sets are larger than this, so a client that wants one not named here has to
 * add it, keeping the sizes the asserts pin down.
 */

#ifndef _WINHVPLATFORM_H_
#define _WINHVPLATFORM_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef VOID *WHV_PARTITION_HANDLE;
typedef UINT64 WHV_GUEST_PHYSICAL_ADDRESS;

/*
 * Aligned, and every register value with it, because the platform moves a
 * register value with an instruction that faults on anything less.
 */
typedef struct DECLSPEC_ALIGN(16) _WHV_UINT128
{
    UINT64 Low64;
    UINT64 High64;
} WHV_UINT128;

/* CAPABILITIES **************************************************************/

typedef enum _WHV_CAPABILITY_CODE
{
    WHvCapabilityCodeHypervisorPresent = 0x00000000,
    WHvCapabilityCodeFeatures = 0x00000001,
    WHvCapabilityCodeExtendedVmExits = 0x00000002,
    WHvCapabilityCodeExceptionExitBitmap = 0x00000003,
    WHvCapabilityCodeProcessorVendor = 0x00001000,
    WHvCapabilityCodeProcessorFeatures = 0x00001001,
    WHvCapabilityCodeProcessorClFlushSize = 0x00001002
} WHV_CAPABILITY_CODE;

typedef union _WHV_CAPABILITY
{
    BOOL HypervisorPresent;
    UINT64 Features;
    UINT64 ExtendedVmExits;
    UINT32 ProcessorVendor;
    UINT64 ProcessorFeatures;
    UINT8 ProcessorClFlushSize;
    UINT64 AsUINT64[3];
} WHV_CAPABILITY;

C_ASSERT(sizeof(WHV_CAPABILITY) == 24);

/* PARTITIONS ****************************************************************/

typedef enum _WHV_PARTITION_PROPERTY_CODE
{
    WHvPartitionPropertyCodeExtendedVmExits = 0x00000001,
    WHvPartitionPropertyCodeExceptionExitBitmap = 0x00000002,
    WHvPartitionPropertyCodeProcessorFeatures = 0x00001001,
    WHvPartitionPropertyCodeProcessorClFlushSize = 0x00001002,
    WHvPartitionPropertyCodeCpuidExitList = 0x00001003,
    WHvPartitionPropertyCodeProcessorCount = 0x00001FFF
} WHV_PARTITION_PROPERTY_CODE;

typedef union _WHV_EXTENDED_VM_EXITS
{
    struct
    {
        UINT64 X64CpuidExit:1;
        UINT64 X64MsrExit:1;
        UINT64 ExceptionExit:1;
        UINT64 X64RdtscExit:1;
        UINT64 X64ApicSmiExitTrap:1;
        UINT64 HypercallExit:1;
        UINT64 X64ApicInitSipiExitTrap:1;
        UINT64 Reserved:57;
    } DUMMYSTRUCTNAME;
    UINT64 AsUINT64;
} WHV_EXTENDED_VM_EXITS;

C_ASSERT(sizeof(WHV_EXTENDED_VM_EXITS) == 8);

typedef enum _WHV_MAP_GPA_RANGE_FLAGS
{
    WHvMapGpaRangeFlagNone = 0x00000000,
    WHvMapGpaRangeFlagRead = 0x00000001,
    WHvMapGpaRangeFlagWrite = 0x00000002,
    WHvMapGpaRangeFlagExecute = 0x00000004,
    WHvMapGpaRangeFlagTrackDirtyPages = 0x00000008
} WHV_MAP_GPA_RANGE_FLAGS;

/* REGISTERS *****************************************************************/

typedef enum _WHV_REGISTER_NAME
{
    WHvX64RegisterRax = 0x00000000,
    WHvX64RegisterRcx = 0x00000001,
    WHvX64RegisterRdx = 0x00000002,
    WHvX64RegisterRbx = 0x00000003,
    WHvX64RegisterRsp = 0x00000004,
    WHvX64RegisterRbp = 0x00000005,
    WHvX64RegisterRsi = 0x00000006,
    WHvX64RegisterRdi = 0x00000007,
    WHvX64RegisterR8 = 0x00000008,
    WHvX64RegisterR9 = 0x00000009,
    WHvX64RegisterR10 = 0x0000000A,
    WHvX64RegisterR11 = 0x0000000B,
    WHvX64RegisterR12 = 0x0000000C,
    WHvX64RegisterR13 = 0x0000000D,
    WHvX64RegisterR14 = 0x0000000E,
    WHvX64RegisterR15 = 0x0000000F,
    WHvX64RegisterRip = 0x00000010,
    WHvX64RegisterRflags = 0x00000011,
    WHvX64RegisterEs = 0x00000012,
    WHvX64RegisterCs = 0x00000013,
    WHvX64RegisterSs = 0x00000014,
    WHvX64RegisterDs = 0x00000015,
    WHvX64RegisterFs = 0x00000016,
    WHvX64RegisterGs = 0x00000017,
    WHvX64RegisterLdtr = 0x00000018,
    WHvX64RegisterTr = 0x00000019,
    WHvX64RegisterIdtr = 0x0000001A,
    WHvX64RegisterGdtr = 0x0000001B,
    /* What a client gives a processor, and what it asks to be told about */
    WHvRegisterPendingInterruption = 0x80000000,
    WHvRegisterInterruptState = 0x80000001,
    WHvRegisterPendingEvent = 0x80000002,
    WHvX64RegisterDeliverabilityNotifications = 0x80000004,
    WHvRegisterInternalActivityState = 0x80000005
} WHV_REGISTER_NAME;

typedef union _WHV_X64_PENDING_INTERRUPTION_REGISTER
{
    struct
    {
        UINT32 InterruptionPending:1;
        UINT32 InterruptionType:3;
        UINT32 DeliverErrorCode:1;
        UINT32 InstructionLength:4;
        UINT32 NestedEvent:1;
        UINT32 Reserved:6;
        UINT32 InterruptionVector:16;
        UINT32 ErrorCode;
    } DUMMYSTRUCTNAME;
    UINT64 AsUINT64;
} WHV_X64_PENDING_INTERRUPTION_REGISTER;

C_ASSERT(sizeof(WHV_X64_PENDING_INTERRUPTION_REGISTER) == 8);

typedef union _WHV_X64_INTERRUPT_STATE_REGISTER
{
    struct
    {
        UINT64 InterruptShadow:1;
        UINT64 NmiMasked:1;
        UINT64 Reserved:62;
    } DUMMYSTRUCTNAME;
    UINT64 AsUINT64;
} WHV_X64_INTERRUPT_STATE_REGISTER;

typedef union _WHV_X64_DELIVERABILITY_NOTIFICATIONS_REGISTER
{
    struct
    {
        UINT64 NmiNotification:1;
        UINT64 InterruptNotification:1;
        UINT64 InterruptPriority:4;
        UINT64 Reserved:58;
    } DUMMYSTRUCTNAME;
    UINT64 AsUINT64;
} WHV_X64_DELIVERABILITY_NOTIFICATIONS_REGISTER;

/* What kind of event a pending interruption is, numbered as the processor does */
typedef enum _WHV_X64_PENDING_INTERRUPTION_TYPE
{
    WHvX64PendingInterrupt = 0,
    WHvX64PendingNmi = 2,
    WHvX64PendingException = 3
} WHV_X64_PENDING_INTERRUPTION_TYPE;

typedef struct _WHV_X64_SEGMENT_REGISTER
{
    UINT64 Base;
    UINT32 Limit;
    UINT16 Selector;
    union
    {
        struct
        {
            UINT16 SegmentType:4;
            UINT16 NonSystemSegment:1;
            UINT16 DescriptorPrivilegeLevel:2;
            UINT16 Present:1;
            UINT16 Reserved:4;
            UINT16 Available:1;
            UINT16 Long:1;
            UINT16 Default:1;
            UINT16 Granularity:1;
        } DUMMYSTRUCTNAME;
        UINT16 Attributes;
    } DUMMYUNIONNAME;
} WHV_X64_SEGMENT_REGISTER;

C_ASSERT(sizeof(WHV_X64_SEGMENT_REGISTER) == 16);

typedef struct _WHV_X64_TABLE_REGISTER
{
    UINT16 Pad[3];
    UINT16 Limit;
    UINT64 Base;
} WHV_X64_TABLE_REGISTER;

typedef union _WHV_REGISTER_VALUE
{
    WHV_UINT128 Reg128;
    UINT64 Reg64;
    UINT32 Reg32;
    UINT16 Reg16;
    UINT8 Reg8;
    WHV_X64_SEGMENT_REGISTER Segment;
    WHV_X64_TABLE_REGISTER Table;
} WHV_REGISTER_VALUE;

C_ASSERT(sizeof(WHV_REGISTER_VALUE) == 16);
C_ASSERT(TYPE_ALIGNMENT(WHV_REGISTER_VALUE) == 16);

/* EXITS *********************************************************************/

typedef enum _WHV_RUN_VP_EXIT_REASON
{
    WHvRunVpExitReasonNone = 0x00000000,
    WHvRunVpExitReasonMemoryAccess = 0x00000001,
    WHvRunVpExitReasonX64IoPortAccess = 0x00000002,
    WHvRunVpExitReasonUnrecoverableException = 0x00000004,
    WHvRunVpExitReasonInvalidVpRegisterValue = 0x00000005,
    WHvRunVpExitReasonUnsupportedFeature = 0x00000006,
    WHvRunVpExitReasonX64InterruptWindow = 0x00000007,
    WHvRunVpExitReasonX64Halt = 0x00000008,
    WHvRunVpExitReasonX64ApicEoi = 0x00000009,
    WHvRunVpExitReasonX64MsrAccess = 0x00001000,
    WHvRunVpExitReasonX64Cpuid = 0x00001001,
    WHvRunVpExitReasonException = 0x00001002,
    WHvRunVpExitReasonCanceled = 0x00002001
} WHV_RUN_VP_EXIT_REASON;

typedef union _WHV_X64_VP_EXECUTION_STATE
{
    struct
    {
        UINT16 Cpl:2;
        UINT16 Cr0Pe:1;
        UINT16 Cr0Am:1;
        UINT16 EferLma:1;
        UINT16 DebugActive:1;
        UINT16 InterruptionPending:1;
        UINT16 Reserved0:5;
        UINT16 InterruptShadow:1;
        UINT16 Reserved1:3;
    } DUMMYSTRUCTNAME;
    UINT16 AsUINT16;
} WHV_X64_VP_EXECUTION_STATE;

C_ASSERT(sizeof(WHV_X64_VP_EXECUTION_STATE) == 2);

typedef struct _WHV_X64_VP_EXIT_CONTEXT
{
    WHV_X64_VP_EXECUTION_STATE ExecutionState;
    UINT8 InstructionLength:4;
    UINT8 Cr8:4;
    UINT8 Reserved;
    UINT32 Reserved2;
    WHV_X64_SEGMENT_REGISTER Cs;
    UINT64 Rip;
    UINT64 Rflags;
} WHV_X64_VP_EXIT_CONTEXT, WHV_VP_EXIT_CONTEXT;

C_ASSERT(sizeof(WHV_X64_VP_EXIT_CONTEXT) == 40);

typedef union _WHV_X64_IO_PORT_ACCESS_INFO
{
    struct
    {
        UINT32 IsWrite:1;
        UINT32 AccessSize:3;
        UINT32 StringOp:1;
        UINT32 RepPrefix:1;
        UINT32 Reserved:26;
    } DUMMYSTRUCTNAME;
    UINT32 AsUINT32;
} WHV_X64_IO_PORT_ACCESS_INFO;

typedef struct _WHV_X64_IO_PORT_ACCESS_CONTEXT
{
    UINT8 InstructionByteCount;
    UINT8 Reserved[3];
    UINT8 InstructionBytes[16];
    WHV_X64_IO_PORT_ACCESS_INFO AccessInfo;
    UINT16 PortNumber;
    UINT16 Reserved2[3];
    UINT64 Rax;
    UINT64 Rcx;
    UINT64 Rsi;
    UINT64 Rdi;
    WHV_X64_SEGMENT_REGISTER Ds;
    WHV_X64_SEGMENT_REGISTER Es;
} WHV_X64_IO_PORT_ACCESS_CONTEXT;

C_ASSERT(sizeof(WHV_X64_IO_PORT_ACCESS_CONTEXT) == 96);

/*
 * What a processor was asked, and what the hypervisor would have answered on
 * its own. A client with nothing of its own to say puts the default back.
 */
typedef struct _WHV_X64_CPUID_ACCESS_CONTEXT
{
    UINT64 Rax;
    UINT64 Rcx;
    UINT64 Rdx;
    UINT64 Rbx;
    UINT64 DefaultResultRax;
    UINT64 DefaultResultRcx;
    UINT64 DefaultResultRdx;
    UINT64 DefaultResultRbx;
} WHV_X64_CPUID_ACCESS_CONTEXT;

C_ASSERT(sizeof(WHV_X64_CPUID_ACCESS_CONTEXT) == 64);

typedef union _WHV_X64_MSR_ACCESS_INFO
{
    struct
    {
        UINT32 IsWrite:1;
        UINT32 Reserved:31;
    } DUMMYSTRUCTNAME;
    UINT32 AsUINT32;
} WHV_X64_MSR_ACCESS_INFO;

typedef struct _WHV_X64_MSR_ACCESS_CONTEXT
{
    WHV_X64_MSR_ACCESS_INFO AccessInfo;
    UINT32 MsrNumber;
    UINT64 Rax;
    UINT64 Rdx;
} WHV_X64_MSR_ACCESS_CONTEXT;

C_ASSERT(sizeof(WHV_X64_MSR_ACCESS_CONTEXT) == 24);

/* What the processor could now be given, which is what the client asked about */
typedef struct _WHV_X64_INTERRUPTION_DELIVERABLE_CONTEXT
{
    WHV_X64_PENDING_INTERRUPTION_TYPE DeliverableType;
} WHV_X64_INTERRUPTION_DELIVERABLE_CONTEXT;

typedef struct _WHV_RUN_VP_EXIT_CONTEXT
{
    WHV_RUN_VP_EXIT_REASON ExitReason;
    UINT32 Reserved;
    WHV_VP_EXIT_CONTEXT VpContext;
    union
    {
        WHV_X64_IO_PORT_ACCESS_CONTEXT IoPortAccess;
        WHV_X64_CPUID_ACCESS_CONTEXT CpuidAccess;
        WHV_X64_MSR_ACCESS_CONTEXT MsrAccess;
        WHV_X64_INTERRUPTION_DELIVERABLE_CONTEXT InterruptWindow;
        UINT64 AsUINT64[22];
    } DUMMYUNIONNAME;
} WHV_RUN_VP_EXIT_CONTEXT;

C_ASSERT(sizeof(WHV_RUN_VP_EXIT_CONTEXT) == 224);
C_ASSERT(FIELD_OFFSET(WHV_RUN_VP_EXIT_CONTEXT, VpContext) == 8);

/* FUNCTIONS *****************************************************************/

HRESULT
WINAPI
WHvGetCapability(
    _In_ WHV_CAPABILITY_CODE CapabilityCode,
    _Out_writes_bytes_to_(CapabilityBufferSizeInBytes, *WrittenSizeInBytes) VOID *CapabilityBuffer,
    _In_ UINT32 CapabilityBufferSizeInBytes,
    _Out_opt_ UINT32 *WrittenSizeInBytes);

HRESULT
WINAPI
WHvCreatePartition(
    _Out_ WHV_PARTITION_HANDLE *Partition);

HRESULT
WINAPI
WHvSetupPartition(
    _In_ WHV_PARTITION_HANDLE Partition);

HRESULT
WINAPI
WHvDeletePartition(
    _In_ WHV_PARTITION_HANDLE Partition);

HRESULT
WINAPI
WHvSetPartitionProperty(
    _In_ WHV_PARTITION_HANDLE Partition,
    _In_ WHV_PARTITION_PROPERTY_CODE PropertyCode,
    _In_reads_bytes_(PropertyBufferSizeInBytes) const VOID *PropertyBuffer,
    _In_ UINT32 PropertyBufferSizeInBytes);

HRESULT
WINAPI
WHvMapGpaRange(
    _In_ WHV_PARTITION_HANDLE Partition,
    _In_ VOID *SourceAddress,
    _In_ WHV_GUEST_PHYSICAL_ADDRESS GuestAddress,
    _In_ UINT64 SizeInBytes,
    _In_ WHV_MAP_GPA_RANGE_FLAGS Flags);

HRESULT
WINAPI
WHvUnmapGpaRange(
    _In_ WHV_PARTITION_HANDLE Partition,
    _In_ WHV_GUEST_PHYSICAL_ADDRESS GuestAddress,
    _In_ UINT64 SizeInBytes);

HRESULT
WINAPI
WHvCreateVirtualProcessor(
    _In_ WHV_PARTITION_HANDLE Partition,
    _In_ UINT32 VpIndex,
    _In_ UINT32 Flags);

HRESULT
WINAPI
WHvDeleteVirtualProcessor(
    _In_ WHV_PARTITION_HANDLE Partition,
    _In_ UINT32 VpIndex);

HRESULT
WINAPI
WHvRunVirtualProcessor(
    _In_ WHV_PARTITION_HANDLE Partition,
    _In_ UINT32 VpIndex,
    _Out_writes_bytes_(ExitContextSizeInBytes) VOID *ExitContext,
    _In_ UINT32 ExitContextSizeInBytes);

HRESULT
WINAPI
WHvSetVirtualProcessorRegisters(
    _In_ WHV_PARTITION_HANDLE Partition,
    _In_ UINT32 VpIndex,
    _In_reads_(RegisterCount) const WHV_REGISTER_NAME *RegisterNames,
    _In_ UINT32 RegisterCount,
    _In_reads_(RegisterCount) const WHV_REGISTER_VALUE *RegisterValues);

HRESULT
WINAPI
WHvGetVirtualProcessorRegisters(
    _In_ WHV_PARTITION_HANDLE Partition,
    _In_ UINT32 VpIndex,
    _In_reads_(RegisterCount) const WHV_REGISTER_NAME *RegisterNames,
    _In_ UINT32 RegisterCount,
    _Out_writes_(RegisterCount) WHV_REGISTER_VALUE *RegisterValues);

#ifdef __cplusplus
}
#endif

#endif /* _WINHVPLATFORM_H_ */
