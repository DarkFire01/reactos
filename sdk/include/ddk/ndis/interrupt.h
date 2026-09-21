/*
 * PROJECT:     ReactOS NDIS 6 support
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     NDIS 6.x miniport interrupt registration
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define NDIS_OBJECT_TYPE_MINIPORT_INTERRUPT                          0x84

#define NDIS_MINIPORT_INTERRUPT_REVISION_1                           1

typedef BOOLEAN (NTAPI MINIPORT_ISR)(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _Out_ PBOOLEAN QueueDefaultInterruptDpc,
    _Out_ PULONG TargetProcessors);
typedef MINIPORT_ISR *MINIPORT_ISR_HANDLER;

typedef VOID (NTAPI MINIPORT_INTERRUPT_DPC)(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _In_ PVOID MiniportDpcContext,
    _In_ PVOID ReceiveThrottleParameters,
    _In_ PVOID NdisReserved2);
typedef MINIPORT_INTERRUPT_DPC *MINIPORT_INTERRUPT_DPC_HANDLER;

#if NDIS_SUPPORT_NDIS620
typedef struct _NDIS_RECEIVE_THROTTLE_PARAMETERS
{
    _In_ ULONG MaxNblsToIndicate;
    _Out_ ULONG MoreNblsPending:1;
} NDIS_RECEIVE_THROTTLE_PARAMETERS, *PNDIS_RECEIVE_THROTTLE_PARAMETERS;

#define NDIS_INDICATE_ALL_NBLS                                       (~0ul)
#endif

typedef VOID (NTAPI MINIPORT_DISABLE_INTERRUPT)(
    _In_ NDIS_HANDLE MiniportInterruptContext);
typedef MINIPORT_DISABLE_INTERRUPT *MINIPORT_DISABLE_INTERRUPT_HANDLER;

typedef VOID (NTAPI MINIPORT_ENABLE_INTERRUPT)(
    _In_ NDIS_HANDLE MiniportInterruptContext);
typedef MINIPORT_ENABLE_INTERRUPT *MINIPORT_ENABLE_INTERRUPT_HANDLER;

typedef BOOLEAN (NTAPI MINIPORT_MESSAGE_INTERRUPT)(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _In_ ULONG MessageId,
    _Out_ PBOOLEAN QueueDefaultInterruptDpc,
    _Out_ PULONG TargetProcessors);
typedef MINIPORT_MESSAGE_INTERRUPT *MINIPORT_MSI_ISR_HANDLER;

typedef VOID (NTAPI MINIPORT_MESSAGE_INTERRUPT_DPC)(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _In_ ULONG MessageId,
    _In_ PVOID MiniportDpcContext,
#if NDIS_SUPPORT_NDIS620
    _In_ PVOID ReceiveThrottleParameters,
    _In_ PVOID NdisReserved2
#else
    _In_ PULONG NdisReserved1,
    _In_ PULONG NdisReserved2
#endif
    );
typedef MINIPORT_MESSAGE_INTERRUPT_DPC *MINIPORT_MSI_INTERRUPT_DPC_HANDLER;

typedef VOID (NTAPI MINIPORT_DISABLE_MESSAGE_INTERRUPT)(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _In_ ULONG MessageId);
typedef MINIPORT_DISABLE_MESSAGE_INTERRUPT *MINIPORT_DISABLE_MSI_INTERRUPT_HANDLER;

typedef VOID (NTAPI MINIPORT_ENABLE_MESSAGE_INTERRUPT)(
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _In_ ULONG MessageId);
typedef MINIPORT_ENABLE_MESSAGE_INTERRUPT *MINIPORT_ENABLE_MSI_INTERRUPT_HANDLER;

typedef BOOLEAN (NTAPI MINIPORT_SYNCHRONIZE_INTERRUPT)(
    _In_ NDIS_HANDLE SynchronizeContext);
typedef MINIPORT_SYNCHRONIZE_INTERRUPT *MINIPORT_SYNCHRONIZE_INTERRUPT_HANDLER;
typedef MINIPORT_SYNCHRONIZE_INTERRUPT MINIPORT_SYNCHRONIZE_MESSAGE_INTERRUPT;
typedef MINIPORT_SYNCHRONIZE_MESSAGE_INTERRUPT *MINIPORT_SYNCHRONIZE_MSI_INTERRUPT_HANDLER;

typedef enum _NDIS_INTERRUPT_TYPE
{
    NDIS_CONNECT_LINE_BASED = 1,
    NDIS_CONNECT_MESSAGE_BASED
} NDIS_INTERRUPT_TYPE, *PNDIS_INTERRUPT_TYPE;

typedef struct _NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS
{
    _In_ NDIS_OBJECT_HEADER Header;
    _In_ MINIPORT_ISR_HANDLER InterruptHandler;
    _In_ MINIPORT_INTERRUPT_DPC_HANDLER InterruptDpcHandler;
    _In_ MINIPORT_DISABLE_INTERRUPT_HANDLER DisableInterruptHandler;
    _In_ MINIPORT_ENABLE_INTERRUPT_HANDLER EnableInterruptHandler;
    _In_ BOOLEAN MsiSupported;
    _In_ BOOLEAN MsiSyncWithAllMessages;
    _In_ MINIPORT_MSI_ISR_HANDLER MessageInterruptHandler;
    _In_ MINIPORT_MSI_INTERRUPT_DPC_HANDLER MessageInterruptDpcHandler;
    _In_ MINIPORT_DISABLE_MSI_INTERRUPT_HANDLER DisableMessageInterruptHandler;
    _In_ MINIPORT_ENABLE_MSI_INTERRUPT_HANDLER EnableMessageInterruptHandler;
    _Out_ NDIS_INTERRUPT_TYPE InterruptType;
    _Out_ PIO_INTERRUPT_MESSAGE_INFO MessageInfoTable;
} NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS, *PNDIS_MINIPORT_INTERRUPT_CHARACTERISTICS;

#define NDIS_SIZEOF_MINIPORT_INTERRUPT_CHARACTERISTICS_REVISION_1 \
    RTL_SIZEOF_THROUGH_FIELD(NDIS_MINIPORT_INTERRUPT_CHARACTERISTICS, MessageInfoTable)

_Must_inspect_result_
_IRQL_requires_(PASSIVE_LEVEL)
NDIS_STATUS
NTAPI
NdisMRegisterInterruptEx(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_HANDLE MiniportInterruptContext,
    _In_ PNDIS_MINIPORT_INTERRUPT_CHARACTERISTICS MiniportInterruptCharacteristics,
    _Out_ PNDIS_HANDLE NdisInterruptHandle);

_IRQL_requires_(PASSIVE_LEVEL)
VOID
NTAPI
NdisMDeregisterInterruptEx(
    _In_ NDIS_HANDLE NdisInterruptHandle);

BOOLEAN
NTAPI
NdisMSynchronizeWithInterruptEx(
    _In_ NDIS_HANDLE NdisInterruptHandle,
    _In_ ULONG MessageId,
#if NDIS_SUPPORT_NDIS620
    _In_ MINIPORT_SYNCHRONIZE_INTERRUPT_HANDLER SynchronizeFunction,
#else
    _In_ PVOID SynchronizeFunction,
#endif
    _In_ PVOID SynchronizeContext);

_IRQL_requires_max_(DISPATCH_LEVEL)
ULONG
NTAPI
NdisMQueueDpc(
    _In_ NDIS_HANDLE NdisInterruptHandle,
    _In_ ULONG MessageId,
    _In_ ULONG TargetProcessors,
    _In_opt_ PVOID MiniportDpcContext);

#if NDIS_SUPPORT_NDIS620
_IRQL_requires_max_(HIGH_LEVEL)
KAFFINITY
NTAPI
NdisMQueueDpcEx(
    _In_ NDIS_HANDLE NdisInterruptHandle,
    _In_ ULONG MessageId,
    _In_ PGROUP_AFFINITY TargetProcessors,
    _In_opt_ PVOID MiniportDpcContext);
#endif

#ifdef __cplusplus
}
#endif
