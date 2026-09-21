/*
 * PROJECT:     ReactOS WDI upper edge
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private declarations
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#include <ndis.h>
#include <netioddk.h>
#include <dot11wdi.h>
#include <drivers/ndis/ndishook.h>

#define WDI_TAG                             'iWdW'

/* Every command gets at least this much room for its response */
#define WDI_MIN_RESPONSE_LENGTH             2046

/* A port slot per port the IHV can create */
#define WDI_MAX_PORTS                       5

/* The version this upper edge speaks, and the oldest it tells an IHV about */
#define WDI_UPPER_EDGE_VERSION              WDI_VERSION_1_1_13
#define WDI_UPPER_EDGE_FLOOR_VERSION        WDI_VERSION_1_1_12

/* WDI_ADAPTER::Progress, what bring-up got through and halt undoes */
#define WDI_PROGRESS_CONTROL_PATH           0x00000001
#define WDI_PROGRESS_ALLOCATED              0x00000002
#define WDI_PROGRESS_OPENED                 0x00000004
#define WDI_PROGRESS_DATAPATH_READY         0x00000008
#define WDI_PROGRESS_DATAPATH_STARTED       0x00000010
#define WDI_PROGRESS_OPERATING              0x00000020

/* Where bring-up stopped, for the error log */
typedef enum _WDI_INIT_STEP
{
    WdiInitNotStarted = 0,
    WdiInitAllocate,
    WdiInitOpen,
    WdiInitCapabilities,
    WdiInitConfiguration,
    WdiInitDataPath,
    WdiInitCreatePort,
    WdiInitRadioState,
    WdiInitAttributes,
    WdiInitStartOperation
} WDI_INIT_STEP;

typedef struct _WDI_GLOBALS
{
    PDRIVER_OBJECT DriverObject;
    HANDLE NmrProvider;

    /* NDIS, once it attaches as the hook client */
    KSPIN_LOCK BindingLock;
    HANDLE NmrBinding;
    PVOID ClientBindingContext;
    NDIS_HOOK_CLIENT_DISPATCH Ndis;

    KSPIN_LOCK ListLock;
    LIST_ENTRY Miniports;
    LIST_ENTRY Adapters;
} WDI_GLOBALS, *PWDI_GLOBALS;

extern WDI_GLOBALS WdiGlobals;

/* One per WLAN miniport driver */
typedef struct _WDI_MINIPORT
{
    LIST_ENTRY Link;
    PDRIVER_OBJECT DriverObject;

    /* What NDIS hands the wrappers, the IHV's or this block when it gave none */
    NDIS_HANDLE DriverContext;
    BOOLEAN OwnDriverContext;
    NDIS_HANDLE NdisDriverHandle;

    /* The IHV's handlers, OID handlers cleared since those go through NDIS */
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS Ndis;
    NDIS_MINIPORT_DRIVER_WDI_CHARACTERISTICS Wdi;
    NDIS_WDI_INIT_PARAMETERS InitParameters;
} WDI_MINIPORT, *PWDI_MINIPORT;

/* The parts of WDI_GET_ADAPTER_CAPABILITIES bring-up needs */
typedef struct _WDI_CAPABILITIES
{
    UINT32 MtuSize;
    UINT32 MaxMulticastListSize;
    UINT16 BackFillSize;
    WDI_MAC_ADDRESS PermanentAddress;
    UINT32 MaxTxRate;
    UINT32 MaxRxRate;
    BOOLEAN HardwareRadioOn;
    BOOLEAN SoftwareRadioOn;
    BOOLEAN ActionFramesSupported;
    BOOLEAN NonWdiOidsSupported;

    BOOLEAN HasDataPath;
    UINT32 InterconnectType;
    UINT8 MaxNumPeers;
    UINT8 TxTargetPriorityQueueing;
    UINT16 TxMaxScatterGatherElements;
    UINT8 TxExplicitSendComplete;
    UINT16 TxMinEffectiveFrameSize;
    UINT16 TxFrameSizeGranularity;
    UINT8 RxTxForwarding;
    UINT32 RxMaxThroughput;
} WDI_CAPABILITIES, *PWDI_CAPABILITIES;

typedef struct _WDI_PORT
{
    BOOLEAN InUse;
    WDI_PORT_ID PortId;
    NDIS_PORT_NUMBER NdisPortNumber;
    UINT16 OpModeMask;
    WDI_MAC_ADDRESS Address;
} WDI_PORT, *PWDI_PORT;

/* A WDI message sent as a method OID. The request outlives a waiter that gave up on it */
typedef struct _WDI_REQUEST
{
    NDIS_OID_REQUEST Oid;
    struct _WDI_ADAPTER *Adapter;
    KEVENT Done;
    LONG State;
    NDIS_STATUS Status;
    ULONG BufferLength;
    UCHAR Buffer[ANYSIZE_ARRAY];
} WDI_REQUEST, *PWDI_REQUEST;

#define WDI_REQUEST_PENDING                 0
#define WDI_REQUEST_COMPLETED               1
#define WDI_REQUEST_ABANDONED               2

/* A received message, WDI header first */
typedef struct _WDI_MESSAGE
{
    UINT16 MessageId;
    ULONG Length;
    PUCHAR Buffer;
} WDI_MESSAGE, *PWDI_MESSAGE;

typedef struct _WDI_ADAPTER
{
    LIST_ENTRY Link;
    PWDI_MINIPORT Miniport;
    NDIS_HANDLE MiniportAdapterHandle;
    NDIS_HANDLE MiniportAdapterContext;
    ULONG PeerVersion;
    ULONG Progress;
    LONG ShutDown;
    BOOLEAN SurpriseRemoved;

    /* Registry knobs */
    ULONG TaskTimeout;
    ULONG CommandTimeout;
    ULONG UnreachableThreshold;
    BOOLEAN SoftwareRadioOff;

    /* OpenAdapter and CloseAdapter finish through the init parameters */
    KEVENT OpenCloseDone;
    NDIS_STATUS OpenCloseStatus;

    /* One command at a time; a task also waits for the indication ending it */
    KEVENT CommandLock;
    LONG NextTransactionId;
    KSPIN_LOCK TaskLock;
    BOOLEAN TaskArmed;
    UINT32 TaskTransactionId;
    WDI_PORT_ID TaskPortId;
    KEVENT TaskDone;
    WDI_MESSAGE TaskResult;

    /* Data path */
    NDIS_WDI_DATA_API DataApi;
    NDIS_MINIPORT_WDI_DATA_HANDLERS DataHandlers;
    TAL_TXRX_HANDLE TalTxRx;
    UINT32 FrameExtraSpace;
    USHORT FrameSize;
    BOOLEAN FrameLookasideReady;
    NPAGED_LOOKASIDE_LIST FrameLookaside;
    UINT16 MaxOutstandingTransfers;

    WDI_CAPABILITIES Caps;
    WDI_PORT Ports[WDI_MAX_PORTS];

    /* Pause and restart finish on a work item */
    NDIS_HANDLE StateWorkItem;
    BOOLEAN Pausing;
    PNDIS_MINIPORT_PAUSE_PARAMETERS PauseParameters;
    PNDIS_MINIPORT_RESTART_PARAMETERS RestartParameters;
} WDI_ADAPTER, *PWDI_ADAPTER;

/* Frames the IHV allocates metadata for, the metadata sits after this header */
typedef struct _WDI_FRAME
{
    ULONG Size;
    ULONG Reserved[3];
    WDI_FRAME_METADATA Metadata;
} WDI_FRAME, *PWDI_FRAME;

/* driver.c */

PWDI_ADAPTER
NTAPI
WdiFindAdapterByHandle(
    _In_ NDIS_HANDLE MiniportAdapterHandle);

PWDI_ADAPTER
NTAPI
WdiFindAdapterByContext(
    _In_ NDIS_HANDLE MiniportAdapterContext);

/* miniport.c */

NDIS_HOOK_REGISTER_WDI_DRIVER WdiRegisterDriver;
NDIS_HOOK_DEREGISTER_WDI_DRIVER WdiDeregisterDriver;

/* adapter.c */

NDIS_STATUS
NTAPI
WdiInitializeAdapter(
    _In_ PWDI_ADAPTER Adapter,
    _In_ PNDIS_MINIPORT_INIT_PARAMETERS InitParameters);

VOID
NTAPI
WdiHaltAdapter(
    _In_ PWDI_ADAPTER Adapter);

/* command.c */

VOID
NTAPI
WdiInitializeCommands(
    _In_ PWDI_ADAPTER Adapter);

NDIS_STATUS
NTAPI
WdiSendCommand(
    _In_ PWDI_ADAPTER Adapter,
    _In_ UINT16 MessageId,
    _In_ WDI_PORT_ID PortId,
    _In_reads_bytes_(TlvLength) const UCHAR *Tlvs,
    _In_ ULONG TlvLength,
    _In_ BOOLEAN Task,
    _Out_opt_ PWDI_MESSAGE Result);

VOID
NTAPI
WdiFreeMessage(
    _Inout_ PWDI_MESSAGE Message);

VOID
NTAPI
WdiOidRequestComplete(
    _In_ PWDI_ADAPTER Adapter,
    _In_ PNDIS_OID_REQUEST OidRequest,
    _In_ NDIS_STATUS Status);

VOID
NTAPI
WdiIndication(
    _In_ PWDI_ADAPTER Adapter,
    _In_ PNDIS_STATUS_INDICATION StatusIndication);

/* tlv.c */

ULONG
NTAPI
WdiTlvPut(
    _Out_writes_bytes_opt_(4 + Length) PUCHAR Buffer,
    _In_ UINT16 Type,
    _In_reads_bytes_opt_(Length) const VOID *Value,
    _In_ UINT16 Length);

BOOLEAN
NTAPI
WdiTlvFind(
    _In_reads_bytes_(Length) const UCHAR *Tlvs,
    _In_ ULONG Length,
    _In_ UINT16 Type,
    _Outptr_result_bytebuffer_(*ValueLength) const UCHAR **Value,
    _Out_ PUSHORT ValueLength);

NDIS_STATUS
NTAPI
WdiParseCapabilities(
    _In_ ULONG PeerVersion,
    _In_reads_bytes_(Length) const UCHAR *Tlvs,
    _In_ ULONG Length,
    _Out_ PWDI_CAPABILITIES Caps);

ULONG
NTAPI
WdiBuildAdapterConfiguration(
    _In_ PWDI_ADAPTER Adapter,
    _In_opt_ PCWDI_MAC_ADDRESS ConfiguredAddress,
    _Out_writes_bytes_opt_(return) PUCHAR Buffer);

ULONG
NTAPI
WdiBuildCreatePort(
    _In_ UINT16 OpModeMask,
    _In_ NDIS_PORT_NUMBER NdisPortNumber,
    _Out_writes_bytes_opt_(return) PUCHAR Buffer);

NDIS_STATUS
NTAPI
WdiParsePortAttributes(
    _In_reads_bytes_(Length) const UCHAR *Tlvs,
    _In_ ULONG Length,
    _Out_ PWDI_MAC_ADDRESS Address,
    _Out_ WDI_PORT_ID *PortId);

/* datapath.c */

VOID
NTAPI
WdiSetDataApi(
    _Out_ PNDIS_WDI_DATA_API DataApi);

NDIS_STATUS
NTAPI
WdiCreateFrameLookaside(
    _In_ PWDI_ADAPTER Adapter);

VOID
NTAPI
WdiDeleteFrameLookaside(
    _In_ PWDI_ADAPTER Adapter);
