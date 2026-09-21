/*
 * COPYRIGHT:   See COPYING in the top level directory
 * PROJECT:     ReactOS NDIS library
 * FILE:        ndis/miniport.h
 * PURPOSE:     Definitions for routines used by NDIS miniport drivers
 */

#pragma once

struct _ADAPTER_BINDING;

typedef struct _HARDWARE_ADDRESS {
    union {
        UCHAR Medium802_3[ETH_LENGTH_OF_ADDRESS];
    } Type;
} HARDWARE_ADDRESS, *PHARDWARE_ADDRESS;

/* Information about a miniport */
typedef struct _NDIS_M_DRIVER_BLOCK {
    LIST_ENTRY                      ListEntry;                /* Entry on global list */
    KSPIN_LOCK                      Lock;                     /* Protecting spin lock */
    NDIS_MINIPORT_CHARACTERISTICS   MiniportCharacteristics;  /* Miniport characteristics */
    WORK_QUEUE_ITEM                 WorkItem;                 /* Work item */
    PDRIVER_OBJECT                  DriverObject;             /* Driver object of miniport */
    LIST_ENTRY                      DeviceList;               /* Adapters created by miniport */
    PUNICODE_STRING                 RegistryPath;             /* SCM Registry key */
    /* NDIS 6.x registration. Characteristics6 is only meaningful when
     * Ndis6Driver is set, and the two registration paths are mutually
     * exclusive: a driver is 5.x or 6.x, never both. */
    NDIS_MINIPORT_DRIVER_CHARACTERISTICS Characteristics6;
    NDIS_HANDLE                     MiniportDriverContext;
    BOOLEAN                         Ndis6Driver;
    /* From NdisSetOptionalHandlers */
    NDIS_MINIPORT_PNP_CHARACTERISTICS PnpCharacteristics;
    NDIS_MINIPORT_SS_CHARACTERISTICS SsCharacteristics;
    /* The class extension that registered the driver, from NdisWdfRegisterMiniportDriver */
    struct _CORE_WDF_CX_DRIVER      *CxDriver;
    /* A 6.x driver's own copy of its service key path, which RegistryPath points to */
    UNICODE_STRING                  ServiceKeyPath;
#if !defined(_MSC_VER) && defined(_NDIS_)
} NDIS_M_DRIVER_BLOCK_COMPATIBILITY_HACK_DONT_USE;
#else
} NDIS_M_DRIVER_BLOCK, *PNDIS_M_DRIVER_BLOCK;
#endif

/* There must be some defined struct to do this... */
typedef struct _NDIS_M_DEVICE_BLOCK {
    PDEVICE_OBJECT DeviceObject;
    PNDIS_STRING   SymbolicName;
    PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION+1];
    /* NdisRegisterDeviceEx keeps its own copy of the link name, and the caller's extension */
    UNICODE_STRING SymbolicLink;
    PVOID          ReservedExtension;
} NDIS_M_DEVICE_BLOCK, *PNDIS_M_DEVICE_BLOCK;

/* resources allocated on behalf on the miniport */
#define MINIPORT_RESOURCE_TYPE_REGISTRY_DATA 0
#define MINIPORT_RESOURCE_TYPE_MEMORY        1
typedef struct _MINIPORT_RESOURCE {
    LIST_ENTRY     ListEntry;
    ULONG          ResourceType;
    PVOID          Resource;
} MINIPORT_RESOURCE, *PMINIPORT_RESOURCE;

/* Configuration context */
typedef struct _MINIPORT_CONFIGURATION_CONTEXT {
    NDIS_HANDLE    Handle;
    LIST_ENTRY     ResourceListHead;
    KSPIN_LOCK     ResourceLock;
} MINIPORT_CONFIGURATION_CONTEXT, *PMINIPORT_CONFIGURATION_CONTEXT;

/* Bugcheck callback context */
typedef struct _MINIPORT_BUGCHECK_CONTEXT {
    PVOID                       DriverContext;
    ADAPTER_SHUTDOWN_HANDLER    ShutdownHandler;
    PKBUGCHECK_CALLBACK_RECORD  CallbackRecord;
} MINIPORT_BUGCHECK_CONTEXT, *PMINIPORT_BUGCHECK_CONTEXT;

/* a miniport's shared memory */
typedef struct _MINIPORT_SHARED_MEMORY {
    PDMA_ADAPTER          AdapterObject;
    ULONG                 Length;
    PHYSICAL_ADDRESS      PhysicalAddress;
    PVOID                 VirtualAddress;
    BOOLEAN               Cached;
    PNDIS_MINIPORT_BLOCK  Adapter;
    PVOID                 Context;
    PIO_WORKITEM          WorkItem;
} MINIPORT_SHARED_MEMORY, *PMINIPORT_SHARED_MEMORY;

/* A structure of WrapperConfigurationContext (not compatible with the
   Windows one). */
typedef struct _NDIS_WRAPPER_CONTEXT {
    HANDLE            RegistryHandle;
    PDEVICE_OBJECT    DeviceObject;
    ULONG             BusNumber;
    ULONG             SlotNumber;
} NDIS_WRAPPER_CONTEXT, *PNDIS_WRAPPER_CONTEXT;

#define GET_MINIPORT_DRIVER(Handle)((PNDIS_M_DRIVER_BLOCK)Handle)

/*
 * Where an adapter is in the pause and restart life cycle. Every adapter has
 * one: NDIS 5 miniports go through the same states by way of the shim.
 */
typedef enum _CORE_MINIPORT_STATE
{
    CoreMiniportHalted,
    CoreMiniportInitializing,
    CoreMiniportPaused,
    CoreMiniportRestarting,
    CoreMiniportRunning,
    CoreMiniportPausing
} CORE_MINIPORT_STATE;

struct _CORE_INTERRUPT;
struct _CORE_SG_DMA;
struct _CORE_OID_REQUEST;

/* Why a data path is held paused. Restart waits until none is left. */
#define CORE_PAUSE_WDF              0x00000001
#define CORE_PAUSE_LOW_POWER        0x00000002

/*
 * The NDIS 6 view of an adapter. The core drives a miniport only through
 * Dispatch, the 6.x driver's own characteristics or the NDIS 5 shim.
 */
typedef struct _MINIPORT_CORE
{
    PNDIS_MINIPORT_DRIVER_CHARACTERISTICS Dispatch;

    /* Only while the miniport initializes, for the NDIS 5 shim */
    PNDIS_WRAPPER_CONTEXT WrapperContext;

    /* From MiniportAddDevice, for the miniport's PnP handlers and MiniportInitializeEx */
    NDIS_HANDLE AddDeviceContext;

    /* Guards State, OutstandingSends and the OID queue. Kept apart from the
       miniport block lock, which the NDIS 5 paths hold while calling out. */
    KSPIN_LOCK Lock;
    CORE_MINIPORT_STATE State;
    BOOLEAN GeneralAttributesSet;

    /* A pause or restart waiting on its completion */
    PKEVENT OperationEvent;
    NDIS_STATUS OperationStatus;

    /* CORE_PAUSE_* */
    ULONG PauseReasons;

    /* NET_BUFFER_LISTs the miniport owns and has yet to complete */
    LONG OutstandingSends;
    KEVENT SendsDrained;

    /* OID requests reach the miniport one at a time */
    struct _CORE_OID_REQUEST *ActiveOidRequest;
    LIST_ENTRY OidQueue;

    /* The general attributes, set by a 6.x miniport or built by the shim */
    NDIS_MEDIUM MediaType;
    NDIS_PHYSICAL_MEDIUM PhysicalMediumType;
    ULONG MtuSize;
    ULONG64 MaxXmitLinkSpeed;
    ULONG64 MaxRcvLinkSpeed;
    ULONG LookaheadSize;
    ULONG CurrentLookahead;
    ULONG MacOptions;
    ULONG SupportedPacketFilters;
    ULONG CurrentPacketFilter;
    ULONG MaxMulticastListSize;
    USHORT MacAddressLength;
    UCHAR PermanentMacAddress[NDIS_MAX_PHYS_ADDRESS_LENGTH];
    UCHAR CurrentMacAddress[NDIS_MAX_PHYS_ADDRESS_LENGTH];
    ULONG SupportedStatistics;
    ULONG DataBackFillSize;
    ULONG ContextBackFillSize;
    PNDIS_OID SupportedOidList;
    ULONG SupportedOidListLength;
    NET_IF_ACCESS_TYPE AccessType;
    NET_IF_CONNECTION_TYPE ConnectionType;
    NDIS_RECEIVE_SCALE_CAPABILITIES RecvScaleCapabilities;

    /* Kept current by NDIS_STATUS_LINK_STATE */
    NDIS_LINK_STATE LinkState;

    /* NDIS 5 protocol sends become NET_BUFFER_LISTs from here */
    NDIS_HANDLE SendNblPool;
    /* NDIS 5 miniport indications become NET_BUFFER_LISTs from here */
    NDIS_HANDLE ReceiveNblPool;
    /* NET_BUFFER_LISTs become packets for NDIS 5 protocols from here */
    NDIS_HANDLE ReceivePacketPool;
    /* NET_BUFFER_LISTs become packets for NDIS 5 miniports from here */
    NDIS_HANDLE SendPacketPool;

    struct _CORE_INTERRUPT *Interrupt;
    struct _CORE_SG_DMA *SgDma;

    /* Ports from NdisMAllocatePort, under Lock. Bit n of PortIndices is port n. */
    LIST_ENTRY PortList;
    PUCHAR PortIndices;
    ULONG PortIndicesLength;
    ULONG PortCount;
} MINIPORT_CORE, *PMINIPORT_CORE;

/*
 * A packet laid over a NET_BUFFER's own MDLs, trimmed in place and restored
 * before the NET_BUFFER_LIST moves on. It sits past the protocol reserved area.
 */
typedef struct _CORE_PACKET_STATE
{
    PNET_BUFFER_LIST NetBufferList;
    PNET_BUFFER NetBuffer;
    PMDL LastMdl;
    PMDL LastMdlNext;
    ULONG LastMdlByteCount;
    ULONG FirstMdlOffset;
} CORE_PACKET_STATE, *PCORE_PACKET_STATE;

#define CORE_PACKET_STATE_SIZE sizeof(CORE_PACKET_STATE)
#define CORE_PACKET_STATE(_Packet) \
    ((PCORE_PACKET_STATE)&(_Packet)->ProtocolReserved[PROTOCOL_RESERVED_SIZE_IN_PACKET])

/*
 * A NET_BUFFER_LIST built around an NDIS_PACKET remembers the packet, so the
 * other edge can hand the same packet on instead of translating twice.
 */
#define CORE_NBL_FROM_PACKET(_Nbl)          ((PNDIS_PACKET)(_Nbl)->NdisReserved[0])
#define CORE_NBL_PENDING_COUNT(_Nbl)        (*(PLONG)&(_Nbl)->NdisReserved[1])

/* A miniport's packet indicated up carries the NET_BUFFER_LIST built around it */
#define CORE_PACKET_OWNER_NBL(_Packet)      (*(PNET_BUFFER_LIST *)&(_Packet)->WrapperReservedEx[0])

/* The network interface an adapter is, see mpif.c */
typedef struct _CORE_INTERFACE
{
    LIST_ENTRY ListEntry;
    BOOLEAN Registered;
    NET_LUID NetLuid;
    NET_IFINDEX IfIndex;
    GUID InterfaceGuid;
} CORE_INTERFACE, *PCORE_INTERFACE;

/* The execution context tuning NetAdapterCx reads from NDIS_WDF_COMPLETE_ADD_PARAMS */
typedef struct _CORE_WDF_EC_KNOBS
{
    ULONG Size;
    ULONG Flags;
    ULONG MaxTimeAtDispatch;
    ULONG DispatchTimeWarning;
    ULONG DispatchTimeWarningInterval;
    ULONG DpcWatchdogTimerThreshold;
    ULONG WorkerThreadPriority;

    /* Each pair is at passive, then at dispatch */
    ULONG MaxPacketsSend[2];
    ULONG MaxPacketsSendComplete[2];
    ULONG MaxPacketsReceive[2];
    ULONG MaxPacketsReceiveComplete[2];
} CORE_WDF_EC_KNOBS, *PCORE_WDF_EC_KNOBS;

/* An adapter whose device objects belong to a WDF class extension, see mpwdf.c */
typedef struct _CORE_WDF_ADAPTER
{
    struct _CORE_WDF_CX_DRIVER *CxDriver;

    /* The class extension's own context for the adapter, for its callbacks */
    NDIS_HANDLE CxAdapter;

    /* NdisWdfMiniportTryReference, run down by removal */
    EX_RUNDOWN_REF Rundown;

    /* Serializes start, stop, removal and the data path */
    KEVENT StateLock;

    /* Handles from NdisWdfCreateIrpHandler, under CoreWdfOpenLock */
    LIST_ENTRY OpenList;
    BOOLEAN OpensAllowed;

    /* NdisWdfMiniportStarted came, and protocols were offered the adapter */
    BOOLEAN Started;
    BOOLEAN Bound;
    BOOLEAN Removed;

    /* The class extension wants the data path running */
    LONG DataPathRunning;

    /* A queued CoreWdfApplyState */
    LONG ApplyQueued;

    UNICODE_STRING BaseName;
    UNICODE_STRING InstanceName;
    UNICODE_STRING DriverImageName;
    UNICODE_STRING InterfaceLink;
    CORE_WDF_EC_KNOBS Knobs;
} CORE_WDF_ADAPTER, *PCORE_WDF_ADAPTER;

/* Information about a logical adapter */
typedef struct _LOGICAL_ADAPTER
{
    NDIS_MINIPORT_BLOCK         NdisMiniportBlock;      /* NDIS defined fields */
    PNDIS_MINIPORT_WORK_ITEM    WorkQueueHead;          /* Head of work queue */
    PNDIS_MINIPORT_WORK_ITEM    WorkQueueTail;          /* Tail of work queue */
    LIST_ENTRY                  ListEntry;              /* Entry on global list */
    LIST_ENTRY                  MiniportListEntry;      /* Entry on miniport driver list */
    LIST_ENTRY                  ProtocolListHead;       /* List of bound protocols */
    ULONG                       MediumHeaderSize;       /* Size of medium header */
    HARDWARE_ADDRESS            Address;                /* Hardware address of adapter */
    ULONG                       AddressLength;          /* Length of hardware address */
    PMINIPORT_BUGCHECK_CONTEXT  BugcheckContext;        /* Adapter's shutdown handler */
    BUS_INTERFACE_STANDARD     BusInterface;
    BOOLEAN                    BusInterfaceQueried;
    MINIPORT_CORE              Core;
    CORE_INTERFACE             Interface;
    CORE_WDF_ADAPTER           Wdf;
} LOGICAL_ADAPTER, *PLOGICAL_ADAPTER;

#define MINIPORT_IS_NDIS6(Adapter) ((Adapter)->NdisMiniportBlock.DriverHandle->Ndis6Driver)

/* The device objects belong to a class extension, not to NDIS */
#define MINIPORT_IS_WDF(Adapter) ((Adapter)->Wdf.CxDriver != NULL)

#define GET_LOGICAL_ADAPTER(Handle)((PLOGICAL_ADAPTER)Handle)

extern LIST_ENTRY MiniportListHead;
extern KSPIN_LOCK MiniportListLock;
extern LIST_ENTRY AdapterListHead;
extern KSPIN_LOCK AdapterListLock;


#if DBG
VOID
MiniDisplayPacket(
    PNDIS_PACKET Packet,
    PCSTR Reason);
#endif /* DBG */

BOOLEAN
MiniAdapterHasAddress(
    PLOGICAL_ADAPTER Adapter,
    PNDIS_PACKET Packet);

PLOGICAL_ADAPTER
MiniLocateDevice(
    PNDIS_STRING AdapterName);

VOID
FASTCALL
MiniQueueWorkItem(
    PLOGICAL_ADAPTER    Adapter,
    NDIS_WORK_ITEM_TYPE WorkItemType,
    PVOID               WorkItemContext,
    BOOLEAN             Top);

NDIS_STATUS
FASTCALL
MiniDequeueWorkItem(
    PLOGICAL_ADAPTER    Adapter,
    NDIS_WORK_ITEM_TYPE *WorkItemType,
    PVOID               *WorkItemContext);

BOOLEAN
NdisFindDevice(
    UINT   VendorID,
    UINT   DeviceID,
    PUINT  BusNumber,
    PUINT  SlotNumber);

VOID
NdisStartDevices(VOID);

VOID
NTAPI
MiniportWorker(
    IN PDEVICE_OBJECT DeviceObject,
    IN PVOID WorkItem);

BOOLEAN
MiniIsBusy(
    PLOGICAL_ADAPTER Adapter,
    NDIS_WORK_ITEM_TYPE Type);

NDIS_STATUS
MiniReset(
    PLOGICAL_ADAPTER Adapter);

VOID
NTAPI
MiniDoAddressingReset(
    _In_ PLOGICAL_ADAPTER Adapter);

VOID
MiniWorkItemComplete(
    PLOGICAL_ADAPTER     Adapter,
    NDIS_WORK_ITEM_TYPE  WorkItemType);

VOID NTAPI
MiniResetComplete(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ NDIS_STATUS Status,
    _In_ BOOLEAN AddressingReset);

/* The context the core hands to Dispatch: the 6.x driver's own, or the adapter for the shim */
#define CORE_DISPATCH_CONTEXT(_Adapter) \
    ((_Adapter)->NdisMiniportBlock.DriverHandle->Ndis6Driver ? \
     (_Adapter)->NdisMiniportBlock.MiniportAdapterContext : (NDIS_HANDLE)(_Adapter))

/* mpcore.c */

VOID
NTAPI
CoreInitializeAdapterBlock(
    _In_ PLOGICAL_ADAPTER Adapter);

NDIS_STATUS
NTAPI
CoreSetGeneralAttributes(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_MINIPORT_ADAPTER_GENERAL_ATTRIBUTES General);

NDIS_STATUS
NTAPI
CoreInitializeAdapter(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_WRAPPER_CONTEXT WrapperContext);

VOID
NTAPI
CoreHaltAdapter(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_HALT_ACTION HaltAction);

VOID
NTAPI
CoreShutdownAdapter(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_SHUTDOWN_ACTION ShutdownAction);

VOID
NTAPI
CoreIndicateStatus(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_STATUS_INDICATION StatusIndication);

VOID
NTAPI
CoreIndicateStatusCode(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_STATUS StatusCode);

VOID
NTAPI
CoreHoldPaused(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ ULONG Reason);

NDIS_STATUS
NTAPI
CoreReleasePaused(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ ULONG Reason);

NDIS_STATUS
NTAPI
MiniCallResetHandler(
    _In_ PLOGICAL_ADAPTER Adapter,
    _Out_ PBOOLEAN AddressingReset);

BOOLEAN
NTAPI
MiniCallCheckForHangHandler(
    _In_ PLOGICAL_ADAPTER Adapter);

/* mpoid.c */

struct _CORE_OID_REQUEST;

typedef VOID (NTAPI *CORE_OID_COMPLETION)(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ struct _CORE_OID_REQUEST *CoreRequest,
    _In_ NDIS_STATUS Status);

/*
 * An OID request on its way to a miniport. Whoever started it gets Completion
 * called when the request pends and later finishes.
 */
typedef struct _CORE_OID_REQUEST
{
    NDIS_OID_REQUEST Request;
    LIST_ENTRY QueueEntry;
    CORE_OID_COMPLETION Completion;
    PVOID Context;
} CORE_OID_REQUEST, *PCORE_OID_REQUEST;

NDIS_STATUS
NTAPI
CoreOidRequest(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PCORE_OID_REQUEST CoreRequest);

VOID
NTAPI
CoreOidRequestComplete(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_OID_REQUEST OidRequest,
    _In_ NDIS_STATUS Status);

NDIS_STATUS
NTAPI
CoreQueryInformation(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_OID Oid,
    _Out_writes_bytes_to_(Length, *BytesWritten) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG BytesWritten);

NDIS_STATUS
NTAPI
CoreQueryInformationEx(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_OID Oid,
    _Out_writes_bytes_to_opt_(Length, *BytesWritten) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG BytesWritten,
    _Out_ PULONG BytesNeeded);

NDIS_STATUS
NTAPI
CoreSetInformation(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_OID Oid,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length,
    _Out_ PULONG BytesRead);

/* mpdata.c */

VOID
NTAPI
CoreSendNetBufferLists(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ NDIS_PORT_NUMBER PortNumber,
    _In_ ULONG SendFlags);

VOID
NTAPI
CoreReturnNetBufferList(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferList);

VOID
NTAPI
CoreWaitForSends(
    _In_ PLOGICAL_ADAPTER Adapter);

PNDIS_PACKET
NTAPI
CorePacketFromNetBuffer(
    _In_ NDIS_HANDLE PacketPool,
    _In_ PNET_BUFFER_LIST NetBufferList,
    _In_ PNET_BUFFER NetBuffer);

VOID
NTAPI
CoreFreeNetBufferPacket(
    _In_ PNDIS_PACKET Packet);

/* mpif.c */

ULONG
NTAPI
CoreReadKeyUlong(
    _In_ HANDLE Key,
    _In_ PCWSTR Name,
    _In_ ULONG Default);

NTSTATUS
NTAPI
CoreRegisterInterface(
    _In_ PLOGICAL_ADAPTER Adapter);

VOID
NTAPI
CoreDeregisterInterface(
    _In_ PLOGICAL_ADAPTER Adapter);

/* mpwdf.c */

VOID
NTAPI
CoreWdfRevokeOpens(
    _In_ PLOGICAL_ADAPTER Adapter);

/* miniport.c */

NTSTATUS
NTAPI
MiniDeviceIoControl(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PIRP Irp);

/* mpport.c */

VOID
NTAPI
CoreFreePorts(
    _In_ PLOGICAL_ADAPTER Adapter);

NDIS_STATUS
NTAPI
CoreNotifyProtocols(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_PNP_EVENT Template);

/* mp5shim.c */

extern NDIS_MINIPORT_DRIVER_CHARACTERISTICS Mp5ShimCharacteristics;

VOID
NTAPI
Mp5IndicateLookahead(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_HANDLE MacReceiveContext,
    _In_reads_bytes_(HeaderBufferSize) PVOID HeaderBuffer,
    _In_ UINT HeaderBufferSize,
    _In_reads_bytes_(LookaheadBufferSize) PVOID LookaheadBuffer,
    _In_ UINT LookaheadBufferSize,
    _In_ UINT PacketSize);

NDIS_STATUS
Mp5SendPacket(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_PACKET Packet);

VOID
Mp5SendQueuedPacket(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNDIS_PACKET Packet);

VOID NTAPI
MiniSendComplete(
    _In_ NDIS_HANDLE MiniportAdapterHandle,
    _In_ PNDIS_PACKET Packet,
    _In_ NDIS_STATUS Status);

/* pro5shim.c */

VOID
NTAPI
Pro5IndicateReceive(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferLists,
    _In_ ULONG ReceiveFlags,
    _Out_ PNET_BUFFER_LIST *Unheld);

VOID
NTAPI
Pro5SendComplete(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ PNET_BUFFER_LIST NetBufferLists);

VOID
NTAPI
Pro5IndicateStatus(
    _In_ PLOGICAL_ADAPTER Adapter,
    _In_ NDIS_STATUS GeneralStatus,
    _In_reads_bytes_opt_(StatusBufferSize) PVOID StatusBuffer,
    _In_ UINT StatusBufferSize);

/* EOF */
