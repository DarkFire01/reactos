/*
 * PROJECT:     ReactOS Kernel - Vista+ APIs
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private declarations of the Vista+ compatibility library
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ntdef.h>
#include <ntifs.h>

/*
 * The routines below are exported by ntoskrnl.exe but have no declaration in
 * the XDK yet. They live here so the compatibility sources build without
 * touching the shared header templates.
 */

/* EXECUTIVE ******************************************************************/

typedef LONG EX_SPIN_LOCK, *PEX_SPIN_LOCK;

/* Bit 31 of an EX_SPIN_LOCK is the writer, the low bits count the readers */
#define EX_SPIN_LOCK_WRITER ((LONG)0x80000000)

_IRQL_raises_(DISPATCH_LEVEL)
_IRQL_saves_
NTKRNLVISTAAPI
KIRQL
FASTCALL
ExAcquireSpinLockExclusive(
    _Inout_ PEX_SPIN_LOCK SpinLock);

_IRQL_raises_(DISPATCH_LEVEL)
_IRQL_saves_
NTKRNLVISTAAPI
KIRQL
FASTCALL
ExAcquireSpinLockShared(
    _Inout_ PEX_SPIN_LOCK SpinLock);

NTKRNLVISTAAPI
VOID
FASTCALL
ExReleaseSpinLockExclusive(
    _Inout_ PEX_SPIN_LOCK SpinLock,
    _In_ _IRQL_restores_ KIRQL OldIrql);

NTKRNLVISTAAPI
VOID
FASTCALL
ExReleaseSpinLockShared(
    _Inout_ PEX_SPIN_LOCK SpinLock,
    _In_ _IRQL_restores_ KIRQL OldIrql);

NTKRNLVISTAAPI
BOOLEAN
NTAPI
ExTryQueueWorkItem(
    _Inout_ PWORK_QUEUE_ITEM WorkItem,
    _In_ WORK_QUEUE_TYPE QueueType);

/* HYPERVISOR LIBRARY *********************************************************/

NTKRNLVISTAAPI
BOOLEAN
NTAPI
HvlIsAnyHypervisorPresent(VOID);

NTKRNLVISTAAPI
ULONG
NTAPI
HvlQueryActiveHypervisorProcessorCount(VOID);

/* ERRATA MANAGER *************************************************************/

NTKRNLVISTAAPI
NTSTATUS
NTAPI
EmProviderRegister(
    _In_ PVOID Registration,
    _In_opt_ PVOID Context,
    _Out_ PVOID *ProviderHandle);

NTKRNLVISTAAPI
NTSTATUS
NTAPI
EmProviderDeregister(
    _In_ PVOID ProviderHandle);

NTKRNLVISTAAPI
NTSTATUS
NTAPI
EmClientRuleEvaluate(
    _In_ PVOID ClientHandle,
    _In_ PVOID RuleId,
    _Out_ PVOID Result);

NTKRNLVISTAAPI
NTSTATUS
NTAPI
EmClientQueryRuleState(
    _In_ PVOID ClientHandle,
    _In_ PVOID RuleId,
    _Out_ PVOID State);

/* I/O MANAGER ****************************************************************/

NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoSynchronousCallDriver(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoReportInterruptActive(
    _In_ PVOID Parameters);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoReportInterruptInactive(
    _In_ PVOID Parameters);

_IRQL_requires_max_(PASSIVE_LEVEL)
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoRequestDeviceRemovalForReset(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject,
    _In_ ULONG Flags);

/*
 * Reset recovery dependency tracking. The dependency object is private to the
 * I/O manager, so the exported surface passes it around as an opaque pointer.
 */
NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoReserveDependency(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Out_ PVOID *Dependency);

NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoSetDependency(
    _In_ PVOID Dependency,
    _In_ PDEVICE_OBJECT DependentDeviceObject);

NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoResolveDependency(
    _In_ PVOID Dependency);

NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoTestDependency(
    _In_ PVOID Dependency);

NTKRNLVISTAAPI
NTSTATUS
NTAPI
IoDuplicateDependency(
    _In_ PVOID Dependency,
    _Out_ PVOID *DuplicateDependency);

/* KERNEL *********************************************************************/

NTKRNLVISTAAPI
KAFFINITY
NTAPI
KeProcessorGroupAffinity(
    _In_ USHORT GroupNumber);

NTKRNLVISTAAPI
ULONGLONG
NTAPI
KeQueryInterruptTimePrecise(
    _Out_ PULONGLONG PerfCounter);

/* The KTIMER2 family maps onto the classic dispatcher timer on ReactOS */
NTKRNLVISTAAPI
VOID
NTAPI
KeInitializeTimer2(
    _Out_ PKTIMER Timer);

NTKRNLVISTAAPI
BOOLEAN
NTAPI
KeSetTimer2(
    _Inout_ PKTIMER Timer,
    _In_ LARGE_INTEGER DueTime,
    _In_ LONGLONG Period,
    _In_opt_ PKDPC Dpc);

NTKRNLVISTAAPI
BOOLEAN
NTAPI
KeCancelTimer2(
    _Inout_ PKTIMER Timer);

NTKRNLVISTAAPI
NTSTATUS
NTAPI
KeStartDynamicProcessor(
    _In_ PVOID ProcessorState);

/* SYSTEM INFORMATION *********************************************************/

NTKRNLVISTAAPI
NTSTATUS
NTAPI
NtQuerySystemInformationEx(
    _In_ ULONG SystemInformationClass,
    _In_reads_bytes_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength);

NTKRNLVISTAAPI
NTSTATUS
NTAPI
ZwQuerySystemInformationEx(
    _In_ ULONG SystemInformationClass,
    _In_reads_bytes_(InputBufferLength) PVOID InputBuffer,
    _In_ ULONG InputBufferLength,
    _Out_writes_bytes_opt_(SystemInformationLength) PVOID SystemInformation,
    _In_ ULONG SystemInformationLength,
    _Out_opt_ PULONG ReturnLength);

/* RUNTIME LIBRARY ************************************************************/

/*
 * rtlfuncs.h redirects the name to the WDM library helper, which resolves the
 * kernel export at run time. Undo it so the export itself can be declared.
 */
#undef RtlIsNtDdiVersionAvailable

NTKRNLVISTAAPI
BOOLEAN
NTAPI
RtlIsNtDdiVersionAvailable(
    _In_ ULONG Version);

NTKRNLVISTAAPI
NTSTATUS
NTAPI
RtlQueryRegistryValuesEx(
    _In_ ULONG RelativeTo,
    _In_ PCWSTR Path,
    _Inout_ PRTL_QUERY_REGISTRY_TABLE QueryTable,
    _In_opt_ PVOID Context,
    _In_opt_ PVOID Environment);

/* HARDWARE ERROR ARCHITECTURE ************************************************/

NTKRNLVISTAAPI
VOID
NTAPI
WheaInitializeRecordHeader(
    _Out_ PVOID Header);

NTKRNLVISTAAPI
NTSTATUS
NTAPI
WheaReportHwError(
    _In_ PVOID ErrorPacket);

NTKRNLVISTAAPI
NTSTATUS
NTAPI
WheaAddErrorSource(
    _In_ PVOID ErrorSource,
    _In_opt_ PVOID Context);

NTKRNLVISTAAPI
NTSTATUS
NTAPI
WheaConfigureErrorSource(
    _In_ ULONG SourceType,
    _In_ PVOID Configuration);

NTKRNLVISTAAPI
NTSTATUS
NTAPI
WheaGetErrorSource(
    _In_ ULONG ErrorSourceId,
    _Out_ PVOID *ErrorSource);

/* C RUNTIME ******************************************************************/

#ifndef _ERRNO_T_DEFINED
#define _ERRNO_T_DEFINED
typedef int errno_t;
#endif

#ifndef _RSIZE_T_DEFINED
#define _RSIZE_T_DEFINED
typedef size_t rsize_t;
#endif

unsigned __int64
__cdecl
_strtoui64(
    const char *String,
    char **EndPointer,
    int Base);

errno_t
__cdecl
memcpy_s(
    void *Destination,
    rsize_t DestinationSize,
    const void *Source,
    rsize_t Count);

errno_t
__cdecl
strncpy_s(
    char *Destination,
    rsize_t DestinationSize,
    const char *Source,
    rsize_t Count);

char *
__cdecl
strtok_s(
    char *String,
    const char *Delimiters,
    char **Context);
