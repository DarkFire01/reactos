/*
 * PROJECT:     uACPI-NT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private declarations for the uACPI-NT ACPI bus driver
 */
#ifndef _ACPIPRIV_H_
#define _ACPIPRIV_H_

#include <ntifs.h>
#include <ntstrsafe.h>
#include <ndk/rtlfuncs.h>

#include <uacpi/uacpi.h>
#include <uacpi/namespace.h>
#include <uacpi/utilities.h>
#include <uacpi/resources.h>
#include <uacpi/event.h>
#include <uacpi/sleep.h>
#include <uacpi/status.h>
#include <uacpi/types.h>

#include "uacpi_host.h"   // build_support/include/acpi

#ifdef __cplusplus
extern "C"
{
#endif

#define UACPI_POOL_TAG   'IpcA'   // 'AcpI'

// ACPI _STA bits (ACPI spec 6.3.7): bit0 present, bit3 functioning.
#define UACPI_STA_PRESENT      0x00000001u
#define UACPI_STA_FUNCTIONING  0x00000008u

typedef enum _ACPI_EXT_TYPE
{
    UacpiExtFdo    = 0x4F444600,   // 'FDO'
    UacpiExtPdo    = 0x4F445000,   // 'PDO'
    UacpiExtFilter = 0x544C4600,   // 'FLT'
} UACPI_EXT_TYPE;

// Common prefix of every device extension; Type selects the dispatch path.
typedef struct _ACPI_COMMON
{
    UACPI_EXT_TYPE  Type;
    PDEVICE_OBJECT Self;
} UACPI_COMMON, *PUACPI_COMMON;

// Motherboard FDO (ACPI\PNP0C08). Owns uACPI bring-up and the child PDO list.
typedef struct _ACPI_FDO
{
    UACPI_COMMON            Common;
    PDEVICE_OBJECT         LowerDevice;          ///< IoAttachDeviceToDeviceStack
    PDEVICE_OBJECT         PhysicalDeviceObject; ///< the root PDO we sit over
    BOOLEAN                Started;
    BOOLEAN                UacpiUp;               ///< uACPI namespace initialized
    BOOLEAN                SciFound;
    UACPI_HOST_SCI_RESOURCE Sci;
    FAST_MUTEX             ChildLock;
    LIST_ENTRY             Children;             ///< UACPI_PDO.Link
    LIST_ENTRY             Filters;              ///< UACPI_FILTER.Link
} UACPI_FDO, *PUACPI_FDO;

// PowerResource (powerres.c), reference-counted by the devices holding it on.
#define UACPI_MAX_PR_PER_DEV 8

typedef struct _ACPI_POWER_RESOURCE
{
    LIST_ENTRY            Link;          ///< in the global power-resource list
    uacpi_namespace_node *Node;         ///< the PowerResource node (_ON/_OFF/_STA)
    uacpi_object         *Object;       ///< node object, for _PRx element identity match
    USHORT                ResourceOrder; ///< _PRx apply order (ascending on, desc off)
    UCHAR                 SystemLevel;
    LONG                  RefCount;      ///< devices currently holding it on
    BOOLEAN               On;            ///< last hardware state we drove
} UACPI_POWER_RESOURCE, *PUACPI_POWER_RESOURCE;

// Thermal zone data (drvs/thermal.c): the superset of every release's
// QUERY_INFORMATION layout. Temperatures are deci-Kelvin.
typedef struct _ACPI_THERMAL_INFO
{
    ULONG     ThermalStamp;           ///< bumped on every _TMP reading
    ULONG     ThermalConstant1;       ///< _TC1
    ULONG     ThermalConstant2;       ///< _TC2
    ULONG     SamplingPeriod;         ///< _TSP; Win10: ms from _TFP / _TSP
    ULONG     CurrentTemperature;     ///< _TMP
    ULONG     PassiveTripPoint;       ///< _PSV
    ULONG     StandbyTripPoint;       ///< _CR3 (Win10)
    ULONG     CriticalTripPoint;      ///< _CRT
    ULONG     S4TripPoint;            ///< _HOT (Win8+)
    ULONG     ActiveTripPointCount;   ///< _ACx present, contiguous from _AC0
    ULONG     ActiveTripPoint[10];    ///< _AC0.._AC9
    ULONG     MinimumThrottle;        ///< _MTL / _DSM 1 (Win10)
    ULONG     OverThrottleThreshold;  ///< _DSM 3 (Win10)
    KAFFINITY Processors;             ///< _PSL (Vista/Win7)
} UACPI_THERMAL_INFO;

// Thermal zone state machine (drvs/thermal.c). Lock guards the queue, the
// work/flag fields, the request values and Info.ThermalStamp; the rest of
// Info and the _DTI/_DSM/_STR state belong to the worker.
typedef struct _ACPI_THERMAL_ZONE
{
    KSPIN_LOCK         Lock;
    LIST_ENTRY         IrpQueue;      ///< pended thermal IOCTLs
    ULONG              Work;          ///< UACPI_TZ_* steps owed to the worker
    BOOLEAN            Active;        ///< started; thermal IOCTLs accepted
    BOOLEAN            Running;       ///< worker queued or running
    BOOLEAN            Delivered;     ///< a query has seen the data since it changed
    BOOLEAN            DtiArmed;      ///< _DTI evaluated at NotifyTemp (Win8+)
    UCHAR              CoolingPolicy; ///< _SCP argument
    UCHAR              CoolingLevel;  ///< _ALx at this index and above run
    UCHAR              ThrottleLimit; ///< percent, SET_PASSIVE_LIMIT (Win8+)
    ULONG              NotifyDelta;   ///< _NTT (Win8+)
    ULONG              NotifyTemp;    ///< temperature last passed to _DTI
    ULONG              DsmSupport;    ///< thermal _DSM function mask (Win10)
    UNICODE_STRING     Description;   ///< _STR (Win10)
    UACPI_THERMAL_INFO Info;
} UACPI_THERMAL_ZONE, *PUACPI_THERMAL_ZONE;

// Namespace device PDO (PCI root, buttons, EC, thermal zones, ...).
// WAIT_WAKE / _PRW GPE arming state. Embedded in both a PDO and a filter DO so
// the same arming code serves ACPI's own devnodes and the foreign PDOs we filter
// (on-board NIC/USB, the common S3 wake sources). Lock guards the cancel race.
typedef struct _UACPI_WAKE
{
    uacpi_namespace_node *Node;         ///< AML node providing _PRW/_PSW/_DSW
    CHAR                  Name[8];       ///< 4-char node name + NUL, for tracing
    KSPIN_LOCK            Lock;
    PIRP                  WaitWakeIrp;   ///< pended WAIT_WAKE, NULL if none
    uacpi_namespace_node *GpeDevice;     ///< GPE block device (NULL => \_GPE)
    UCHAR                 GpeIdx;        ///< GPE bit index within that block
    BOOLEAN               GpeParsed;     ///< _PRW[0] parse attempted
    BOOLEAN               GpeValid;      ///< _PRW[0] gave a usable (dev,idx)
    SYSTEM_POWER_STATE    SysWake;       ///< deepest wake S-state (_PRW[1]), Unspecified if none
    DEVICE_POWER_STATE    DevWake;       ///< wake D-state (_S<w>D else D3), for _DSW arg
    SYSTEM_POWER_STATE    ReqSysState;   ///< S-state the pending WAIT_WAKE asked for
    BOOLEAN               PswParsed;     ///< _DSW/_PSW presence resolved once
    BOOLEAN               HasDsw;        ///< _DSW present, so it wins over _PSW
    BOOLEAN               HasPsw;        ///< _PSW present
    BOOLEAN               SetupDone;     ///< uacpi_setup_gpe_for_wake done once
    BOOLEAN               Armed;         ///< enable_gpe(_for_wake) outstanding
    BOOLEAN               DepthSuspended; ///< wake mask dropped for a deeper sleep than requested
    WORK_QUEUE_ITEM       DisarmWork;    ///< deferred disarm from the DISPATCH cancel routine
    volatile LONG         DisarmQueued;  ///< DisarmWork is queued
    PUACPI_POWER_RESOURCE WakeRes[UACPI_MAX_PR_PER_DEV]; ///< _PRW[2..] rails held while armed
    ULONG                 WakeResCount;
} UACPI_WAKE, *PUACPI_WAKE;

// Embedded Controller (drvs/ec.c). A driver claims one query code instead of
// letting the matching _Qxx run. acpi.sys takes the registration through a
// METHOD_NEITHER IOCTL on the PNP0C09 PDO, so this layout is the caller's ABI.
typedef VOID (NTAPI *PACPI_EC_QUERY_ROUTINE)(ULONG QueryCode, PVOID Context);

typedef struct _ACPI_EC_QUERY_REGISTRATION
{
    UCHAR     QueryCode;
    PVOID     Handler;      ///< PACPI_EC_QUERY_ROUTINE
    PVOID     Context;
    ULONG_PTR Cookie;       ///< out: pass back to unregister
} ACPI_EC_QUERY_REGISTRATION, *PACPI_EC_QUERY_REGISTRATION;

#define IOCTL_ACPI_EC_REGISTER_QUERY_HANDLER     CTL_CODE(FILE_DEVICE_UNKNOWN, 5, METHOD_NEITHER, FILE_ANY_ACCESS)   // 0x220017
#define IOCTL_ACPI_EC_UNREGISTER_QUERY_HANDLER     CTL_CODE(FILE_DEVICE_UNKNOWN, 6, METHOD_NEITHER, FILE_ANY_ACCESS)   // 0x22001B

#define UACPI_EC_QUERY_CODES 256

// One controller; a board may carry more than one. The boot EC comes up before
// the namespace is ready and its PNP0C09 PDO adopts it at START.
typedef struct _ACPI_EC
{
    struct _ACPI_EC      *Next;            ///< UacpiEcList
    uacpi_namespace_node *Node;
    PVOID                 Pdo;             ///< PUACPI_PDO once started, else NULL
    USHORT                DataPort;        ///< _CRS/ECDT data register
    USHORT                CmdPort;         ///< command (write) / status (read)
    uacpi_namespace_node *GpeDevice;       ///< GPE block device (NULL => \_GPE)
    uacpi_u16             GpeIdx;
    BOOLEAN               RegionOn;        ///< address-space handler installed
    BOOLEAN               FromEcdt;        ///< named by the ECDT
    BOOLEAN               GlobalLock;      ///< _GLK: non-AML callers take the global lock
    FAST_MUTEX            Lock;            ///< serializes port transactions
    KSPIN_LOCK            QueryLock;       ///< guards the handler table
    PACPI_EC_QUERY_ROUTINE QueryRoutine[UACPI_EC_QUERY_CODES];
    PVOID                 QueryContext[UACPI_EC_QUERY_CODES];
    WORK_QUEUE_ITEM       Work;            ///< PASSIVE query drain
    KDPC                  Dpc;             ///< GPE (DIRQL) -> work item
    LONG                  WorkQueued;
} UACPI_EC, *PUACPI_EC;

typedef struct _ACPI_PDO
{
    UACPI_COMMON           Common;
    PUACPI_FDO             Parent;
    LIST_ENTRY            Link;                  ///< in Parent->Children
    uacpi_namespace_node *Node;                  ///< the uACPI namespace node
    uacpi_namespace_node *ParentNode;            ///< parent whose devnode reports this PDO
    BOOLEAN               Present;               ///< _STA present
    BOOLEAN               Reported;              ///< handed out in a BusRelations
    BOOLEAN               Started;
    BOOLEAN               HasAdr;
    BOOLEAN               IsThermalZone;
    BOOLEAN               IsProcessor;           ///< legacy Processor() object (IDs from CentralProcessor\0)
    ULONG64               Adr;                   ///< _ADR value (if HasAdr)
    CHAR                  Hid[16];               ///< primary _HID, "" if none
    CHAR                  Name[8];               ///< 4-char node name + NUL
    CHAR                  Instance[64];          ///< path-derived instance ID (see enum.c)
    UCHAR                 PciRootBaseBus;        ///< _BBN, cached so config access runs no AML
    // PCI roots only: _OSC result and _DSM fn 4 caps, evaluated at PASSIVE.
    struct
    {
        BOOLEAN OscEvaluated;                    ///< _OSC negotiation attempted
        ULONG   OscControlGranted;               ///< granted control field (0 if none)
        BOOLEAN DsmEvaluated;                    ///< _DSM fn4 attempted
        BOOLEAN BusCapsFound;                    ///< fn4 returned a capability record
        ULONG   CurrentSpeedAndMode;
        ULONG   SupportedSpeedsAndModes;
        ULONG   BusCapAttributes;                ///< bit0 = 64-bit, bit2 = DevID msg
        BOOLEAN IsExpress;                       ///< PNP0A08 in _HID/_CID
    } PciRoot;
    PDEVICE_NOTIFY_CALLBACK NotifyRoutine;       ///< registered driver Notify() cb
    PVOID                 NotifyContext;

    // WAIT_WAKE and _PRW GPE state (power.c).
    UACPI_WAKE            Wake;
    // Cached UacpiPowerBuildStateMap result; QUERY_CAPABILITIES repeats often.
    BOOLEAN               CapsMapDone;           ///< map below is valid
    DEVICE_CAPABILITIES   CapsMap;               ///< cached DeviceState[]/*Wake/WakeFromDx

    // SYS_BUTTON interface (drvs/button.c). ButtonCaps != 0 marks a button PDO.
    ULONG                 ButtonCaps;            ///< SYS_BUTTON_* mask (0 = not a button)
    ULONG                 ButtonEvents;          ///< latched pending event bits
    KSPIN_LOCK            ButtonLock;            ///< guards ButtonEvents + the queue
    LIST_ENTRY            ButtonIrpQueue;        ///< pended IOCTL_GET_SYS_BUTTON_EVENT
    KDPC                  ButtonDpc;             ///< fixed-event delivery out of the SCI ISR
    volatile LONG         ButtonDeferred;        ///< SYS_BUTTON_* bits awaiting ButtonDpc
    UNICODE_STRING        ButtonSymLink;         ///< registered interface symbolic link
    BOOLEAN               ButtonIfRegistered;    ///< interface created

    // Lid: \Callback\PowerState LidClose policy; None suppresses wake arming.
    PCALLBACK_OBJECT      LidPowerCallback;      ///< \Callback\PowerState object
    PVOID                 LidPowerCbReg;         ///< ExRegisterCallback handle
    BOOLEAN               LidCloseNoAction;      ///< current LidClose.Action is None
    BOOLEAN               LidInitialReported;    ///< first _LID report carries the initial-state bit

    // Class device interfaces (drvs/devif.c): processor, thermal zone, fan.
    UNICODE_STRING        ClassIfLink;           ///< registered class interface link
    BOOLEAN               ClassIfOn;             ///< interface enabled
    UNICODE_STRING        CoolingIfLink;         ///< GUID_DEVINTERFACE_THERMAL_COOLING
    BOOLEAN               CoolingIfOn;           ///< (fans / throttle CPUs, a 2nd class)
    PVOID                 ThermalWmi;            ///< PWMILIB_CONTEXT (thermal zones)
    UACPI_THERMAL_ZONE    Thermal;               ///< thermal IOCTL state (thermal zones)
    PVOID                 ResArb;                ///< PUACPI_RES_ARBITERS (resarb.c)
    PUACPI_EC             Ec;                    ///< PNP0C09 only (drvs/ec.c)

    // Power resources held for CurrentDState (powerres.c).
    PUACPI_POWER_RESOURCE  HeldRes[UACPI_MAX_PR_PER_DEV];
    ULONG                 HeldCount;
    DEVICE_POWER_STATE    CurrentDState;
    BOOLEAN               DStateKnown;

    // Paging/hibernate/dump usage; > 0 means PNP_DEVICE_NOT_DISABLEABLE.
    LONG                  UsageCount;
} UACPI_PDO, *PUACPI_PDO;

// Filter over a foreign bus driver's PDO whose device is in the namespace.
typedef struct _ACPI_FILTER
{
    UACPI_COMMON           Common;
    PUACPI_FDO             Fdo;
    LIST_ENTRY            Link;                  ///< in Fdo->Filters
    uacpi_namespace_node *Node;                  ///< the matched namespace node
    PDEVICE_OBJECT        ForeignPdo;            ///< the bus driver's PDO we sit over
    PDEVICE_OBJECT        LowerDevice;           ///< IoAttachDeviceToDeviceStack result
    LONG                  PagingCount;            ///< DeviceUsageTypePaging refs on this DO
    LONG                  HibernationCount;       ///< DeviceUsageTypeHibernation refs
    LONG                  DumpCount;              ///< DeviceUsageTypeDumpFile refs
    PDEVICE_NOTIFY_CALLBACK NotifyRoutine;        ///< ACPI_INTERFACE_STANDARD Notify() sink
    PVOID                 NotifyContext;          ///< its caller-supplied context
    UACPI_WAKE            Wake;                   ///< WAIT_WAKE on a filtered _PRW device
} UACPI_FILTER, *PUACPI_FILTER;

// The single motherboard FDO; set on start.
extern PUACPI_FDO g_AcpiFdo;

// This driver's object; tells our PDOs from foreign ones.
extern PDRIVER_OBJECT g_AcpiDriverObject;

// Service registry path saved in DriverEntry for WmiLib.
extern UNICODE_STRING UacpiDriverRegistryPath;

// driver.c

DRIVER_INITIALIZE DriverEntry;

// glue.c

// Initialize uACPI and load the namespace.
NTSTATUS UacpiBringUpInterpreter(PUACPI_FDO Fdo);

VOID     UacpiTearDownInterpreter(PUACPI_FDO Fdo);

// enum.c

// Build a child PDO per reportable namespace device.
NTSTATUS UacpiEnumerateNamespace(PUACPI_FDO Fdo);

// BusRelations on the root FDO: merge the \_SB and \_TZ children.
NTSTATUS UacpiBuildBusRelations(PUACPI_FDO Fdo, PIRP Irp);

NTSTATUS UacpiPdoQueryId(PUACPI_PDO Pdo, PIRP Irp);
VOID     UacpiInitProcessorString(VOID);

// Create the child PDOs for one namespace level.
VOID     UacpiBuildChildPdosForNode(PUACPI_FDO Fdo, uacpi_namespace_node *parent);

// Merge present PDOs under parent (and parent2) into BusRelations.
NTSTATUS UacpiMergeChildRelations(PUACPI_FDO Fdo, uacpi_namespace_node *parent,
                                 uacpi_namespace_node *parent2, PIRP Irp);

// TRUE when _STA reports present, or there is no _STA.
BOOLEAN  UacpiNodeIsPresentEx(uacpi_namespace_node *node);

// TRUE for PNP0A03 or PNP0A08.
BOOLEAN  UacpiHidIsPciRoot(const char *hid);

// TRUE when the node is present and has a _HID we do not keep internal.
BOOLEAN  UacpiNodeWillBecomePdo(uacpi_namespace_node *node);

// 0 = PIC, 1 = APIC, from the HAL PM handshake.
extern ULONG g_AcpiInterruptModel;

// Flatten legacy devices under PCI roots into root relations; default 0.
extern int UacpiFlatEnumEnabled;
// Gates the namespace inventory printed at FDO start; default 1.
extern int UacpiEnumDiagEnabled;
// Seconds to wait before the settled inventory; default 5.
extern int UacpiEnumDiagDelaySeconds;

// Print every ACPI device node and the verdict for it.
VOID
UacpiEnumDiagDump(PUACPI_FDO Fdo, BOOLEAN Settled);

// Schedule the settled inventory after this enumeration pass.
VOID
UacpiEnumDiagArm(PUACPI_FDO Fdo);
extern int UacpiIrqArbVerbose;
extern int UacpiIrqLibHalOverrides;

// filter.c

// Attach filters over foreign PDOs matched by _ADR under parent.
NTSTATUS UacpiDetectFilterDevices(PUACPI_FDO Fdo, uacpi_namespace_node *parent,
                                 PDEVICE_RELATIONS Relations);

NTSTATUS UacpiFilterPnp(PUACPI_FILTER Filter, PIRP Irp);

NTSTATUS UacpiFilterPower(PUACPI_FILTER Filter, PIRP Irp);

// Eval IOCTLs on a filter; unhandled codes go down the stack.
NTSTATUS UacpiFilterDeviceControl(PUACPI_FILTER Filter, PIRP Irp);

// resource.c

// _CRS to CM_RESOURCE_LIST; caller frees *out.
NTSTATUS UacpiCrsToCmList(uacpi_namespace_node *node, PDEVICE_OBJECT DeviceObject,
                          PCM_RESOURCE_LIST *out);
ULONG UacpiCrsEmitConnectionsCm(uacpi_namespace_node *node, PDEVICE_OBJECT DeviceObject,
                                PCM_PARTIAL_RESOURCE_DESCRIPTOR out);

// Settle a PNP0C0F link and return its GSIV (irqarb.c).
BOOLEAN UacpiIrqLinkDecide(uacpi_namespace_node *Node, PULONG Gsiv);

// Resume: re-run _SRS on every decided PCI link (PIC model only).
VOID UacpiIrqLinksResume(VOID);

// _PRS to IO_RESOURCE_REQUIREMENTS_LIST; caller frees *out.
NTSTATUS UacpiPrsToRequirements(uacpi_namespace_node *node, BOOLEAN Possible,
                                PDEVICE_OBJECT DeviceObject,
                               PIO_RESOURCE_REQUIREMENTS_LIST *out);

ULONG UacpiCrsConnectionCount(uacpi_namespace_node *node, PDEVICE_OBJECT DeviceObject);
ULONG UacpiCrsEmitConnections(uacpi_namespace_node *node, PDEVICE_OBJECT DeviceObject,
                              PIO_RESOURCE_DESCRIPTOR out);

// reshub.c: ACPI 5.0 Connection() descriptors, translated by acpiex
NTSTATUS UacpiConnectResourceHub(VOID);
NTSTATUS UacpiAddBiosNameDeviceAssociation(_In_ PCUNICODE_STRING BiosName,
                                           _In_ PDEVICE_OBJECT DeviceObject);
VOID     UacpiRegisterBiosNameForPdo(PUACPI_PDO Pdo);
NTSTATUS UacpiTranslateConnectionDescriptor(_In_ PDEVICE_OBJECT DeviceObject,
                                            _In_ PVOID Descriptor,
                                            _In_ ULONG DescriptorLength,
                                            _Out_ PIO_RESOURCE_DESCRIPTOR IoDescriptor);

// _CRS as a single-alternative requirements list; caller frees *out.
NTSTATUS UacpiCrsToRequirements(uacpi_namespace_node *node, PDEVICE_OBJECT DeviceObject,
                               PIO_RESOURCE_REQUIREMENTS_LIST *out);

// interfaces.c

// IRP_MN_QUERY_INTERFACE on a namespace PDO.
NTSTATUS UacpiPdoQueryInterface(PUACPI_PDO Pdo, PIRP Irp);

// PDO built for a namespace node, or NULL.
PUACPI_PDO UacpiFindPdoByNode(PUACPI_FDO Fdo, uacpi_namespace_node *node);

// Class << 8 | SubClass for tracing, or 0xFFFF.
ULONG    UacpiReadPciClassCode(PDEVICE_OBJECT Pdo);

// Route a namespace Notify to the owning PDO's callback.
VOID     UacpiRouteNotify(uacpi_namespace_node *node, ULONG value);

// GSIV to vector TRANSLATOR_INTERFACE; shared by the PDO and filter QI paths.
NTSTATUS UacpiBuildIrqTranslator(const char *TraceName, PIO_STACK_LOCATION sp);

NTSTATUS UacpiBuildDeviceResetInterface(uacpi_namespace_node *Node,
                                        const char *TraceName,
                                        PIO_STACK_LOCATION sp);

// ACPI_INTERFACE_STANDARD(2) for PDOs and filters; Context is the devnode's DO.
NTSTATUS UacpiBuildAcpiInterface(PDEVICE_OBJECT Context, const char *TraceName,
                                PIO_STACK_LOCATION sp);

// ioctl.c

// Eval-method IOCTLs on a PDO. AML never runs above PASSIVE_LEVEL.
NTSTATUS UacpiPdoDeviceControl(PUACPI_PDO Pdo, PIRP Irp);

// irqarb.c

// Interrupt arbiter on the FDO, with _PRT-aware placement.
NTSTATUS UacpiIrqArbiterInitialize(PUACPI_FDO Fdo);

// Interrupt ARBITER_INTERFACE if the GUID matches.
NTSTATUS UacpiQueryIrqArbiter(PIO_STACK_LOCATION sp);

// Gates the IRQ arbiter; default 1.
extern int UacpiIrqArbEnabled;

// One-shot timer dump of the MSI/MSI-X capability of every filtered PDO.
VOID UacpiMsiDiagArm(VOID);

// Cancel the readback timer if it has not fired.
VOID UacpiMsiDiagDisarm(VOID);

// Gates the MSI readback; default 1.
extern int UacpiMsiDiagEnabled;

// Delay before the readback fires; default 30.
extern int UacpiMsiDiagDelaySeconds;

// resarb.c

// PCI root memory/I/O/bus arbiter, seeded from the root's _CRS windows.
NTSTATUS UacpiQueryResArbiter(PUACPI_PDO Pdo, PIO_STACK_LOCATION sp);

// Release a PCI root's arbiters at remove.
VOID     UacpiResArbiterTeardown(PUACPI_PDO Pdo);

// Gates the resource arbiters; default 1.
extern int UacpiResArbEnabled;

// hal.c

// Pass \_Sx to the HAL, read back the interrupt model, evaluate \_PIC.
NTSTATUS UacpiHalPmHandshake(VOID);

// Snapshot the armed wake GPE set for the HAL's HIGH_LEVEL sleep callbacks.
// PASSIVE_LEVEL; call after arming wake GPEs on the sleep path.
VOID UacpiHalRefreshWakeCache(VOID);

// Non-zero from the sleep IRP until \_WAK on resume: a fixed-button event in
// this window is the wake press (report SYS_BUTTON_WAKE), not a fresh press.
extern volatile LONG UacpiSystemResuming;

// Cleared on the way into sleep; the first WAIT_WAKE to complete during the
// resume window claims it (0 -> 1) and is flagged the system wake source. Later
// completions and any S0 runtime wake do not set PoSetSystemWake.
extern volatile LONG UacpiWakeSourceClaimed;

// PCI config read via the HAL; must return Length or pci.sys bugchecks 0xC0.
ULONG UacpiHalPciReadConfig(PVOID Context, ULONG Bus, ULONG Slot, PVOID Buffer,
                           ULONG Offset, ULONG Length);

// PCI config write via the HAL; same Length rule as the read.
ULONG UacpiHalPciWriteConfig(PVOID Context, ULONG Bus, ULONG Slot, PVOID Buffer,
                            ULONG Offset, ULONG Length);

// irqlib.c

// Record the IDT vectors the HAL granted in the root's raw start resources;
// call before UacpiIrqLibInitialize.
VOID     UacpiIrqLibSetHalVectors(PCM_RESOURCE_LIST Resources);

// Vector subsystem for the interrupt model; call after the HAL handshake.
NTSTATUS UacpiIrqLibInitialize(VOID);

// Map a line GSIV to vector, IRQL, affinity, polarity and mode.
NTSTATUS UacpiIrqLibResolveVector(ULONG Gsiv, PULONG Vector, PKIRQL Irql,
                                 PKAFFINITY Affinity, PULONG Polarity, PULONG Mode);

// Publish INTERRUPT_CONNECTION_DATA; a line call keeps existing messages.
NTSTATUS UacpiIrqLibWriteConnectionData(PDEVICE_OBJECT Pdo, ULONG Gsiv, ULONG Count);

// IDT vector run for a message placement, allocated once per owner PDO.
NTSTATUS UacpiIrqLibResolveMessageVector(PVOID Owner, ULONG MsgGsiv, ULONG Count,
                                        PULONG BaseVector, PKIRQL Irql, PKAFFINITY Affinity);

// Mark a GSIV level-triggered, active-low.
VOID     UacpiIrqLibNoteLevelGsiv(ULONG Gsiv);
VOID     UacpiIrqLibNoteEdgeGsiv(ULONG Gsiv);

// TRUE when a MADT override pinned this GSIV edge-triggered.
BOOLEAN  UacpiIrqLibGsivForcedEdge(ULONG Gsiv);

// sleep.c

// IRP_MN_SET_POWER for a system power state on the FDO.
NTSTATUS UacpiSystemSetPower(PUACPI_FDO Fdo, PIRP Irp);

// power.c

// TRUE when node has a child object with the given 4-char name.
BOOLEAN  UacpiNodeHasChild(uacpi_namespace_node *node, const char *name);

// \_SB._OSC platform negotiation, once at bring-up.
VOID     UacpiPlatformOscNegotiate(void);

// Acquire power resources, evaluate _PSx, release the delta.
NTSTATUS UacpiPowerSetDeviceState(PUACPI_PDO Pdo, DEVICE_POWER_STATE DState);

// S-to-D map and wake capability, computed once per PDO.
VOID     UacpiPowerBuildStateMap(PUACPI_PDO Pdo, PDEVICE_CAPABILITIES Caps);

// Merge a node's _PRW/_SxD wake capability into caps a bus driver already filled
// (filtered foreign PDOs), so the power manager will issue a WAIT_WAKE we arm.
VOID     UacpiPowerMergeWakeCaps(uacpi_namespace_node *Node, PDEVICE_CAPABILITIES Caps);

// IRP_MN_SET_POWER for a device power state on a PDO.
NTSTATUS UacpiPdoSetPower(PUACPI_PDO Pdo, PIRP Irp);

// Initialise a wake block over an AML node (lock, cached name, node pointer).
VOID     UacpiWakeInit(PUACPI_WAKE Wake, uacpi_namespace_node *Node);

// Boot: mark every _PRW GPE for wake, disarm each device's circuit, and keep the
// fixed-function buttons/lid/RTC enabled in S0. Call before finalize_gpe_init.

// S4 resume: re-assert _PSW/_DSW for every device still armed for wake.
VOID     UacpiWakeReArmAfterHibernate(void);

// Sleep: drop the wake mask on armed devices that requested a shallower state
// than 'Target' (they cannot wake from this depth). Restore mirrors it on resume.
VOID     UacpiWakeSuspendShallow(SYSTEM_POWER_STATE Target);
VOID     UacpiWakeRestoreSuspended(void);

// TRUE if the node has a usable _PRW GPE (parsed and cached).
BOOLEAN  UacpiWakeHasPrw(PUACPI_WAKE Wake);

// Pend WAIT_WAKE and arm the node's _PRW GPE. Completes or pends the IRP.
NTSTATUS UacpiWakeArm(PUACPI_WAKE Wake, PIRP Irp);

// Complete a pended WAIT_WAKE after the wake GPE fired.
VOID     UacpiWakeComplete(PUACPI_WAKE Wake);

// Removal: complete a pended WAIT_WAKE with STATUS_NO_SUCH_DEVICE and disarm.
VOID     UacpiWakeTeardown(PUACPI_WAKE Wake);

// powerres.c

// Enumerate every PowerResource; once at FDO start.
VOID     UacpiPowerResInit(void);

// Re-assert _ON on held resources after wake; S4 leaves the rails off.
VOID     UacpiPowerResResume(void);

// Ref and _ON the resources DState needs; call before _PSx.
VOID     UacpiPowerResAcquireForState(PUACPI_PDO Pdo, DEVICE_POWER_STATE DState,
                                     PUACPI_POWER_RESOURCE *newList, PULONG newCount);

// Deref and _OFF resources not in newList, then commit it; call after _PSx.
VOID     UacpiPowerResReleaseDelta(PUACPI_PDO Pdo, PUACPI_POWER_RESOURCE *newList,
                                  ULONG newCount);

// _PRW[2..] wake power rails: on while a device is armed for wake, off at disarm.
ULONG    UacpiPowerResAcquireWake(uacpi_namespace_node *Dev,
                                 PUACPI_POWER_RESOURCE *Out, ULONG MaxOut);
VOID     UacpiPowerResReleaseWake(PUACPI_POWER_RESOURCE *List, ULONG Count);

// drvs/button.c

// Set ButtonCaps from _HID; TRUE for a button.
BOOLEAN  UacpiButtonClassify(PUACPI_PDO Pdo);

// Register GUID_DEVICE_SYS_BUTTON.
VOID     UacpiButtonStart(PUACPI_PDO Pdo);

// Arm the PoRequestPowerIrp(WAIT_WAKE) loop for a button.
VOID     UacpiButtonArmWaitWake(PUACPI_PDO Pdo);

// Deliver a SYS_BUTTON event, or latch it if no waiter is queued.
VOID     UacpiButtonDeliverEvent(PUACPI_PDO Pdo, ULONG Event);

// ACPI\FixedButton PDO and PM1 handlers, if PWRBTN_EN or SLPBTN_EN is set.
VOID     UacpiFixedButtonInit(PUACPI_FDO Fdo);

// Tear down the interface and pending IRPs at remove.
VOID     UacpiButtonRemove(PUACPI_PDO Pdo);

// IOCTL_GET_SYS_BUTTON_CAPS/_EVENT; *Handled set when consumed.
NTSTATUS UacpiButtonDeviceControl(PUACPI_PDO Pdo, PIRP Irp, PBOOLEAN Handled);

// ACPI Notify on a button node to a SYS_BUTTON event.
VOID     UacpiButtonNotify(PUACPI_PDO Pdo, ULONG NotifyValue);

// drvs/devif.c

// Register one class device interface on a PDO.
VOID     UacpiDevIfRegister(PUACPI_PDO Pdo, const GUID *Guid, const char *Label);

// Register GUID_DEVINTERFACE_THERMAL_COOLING.
VOID     UacpiCoolingIfRegister(PUACPI_PDO Pdo);

// Deregister class interfaces at remove.
VOID     UacpiDevIfRemove(PUACPI_PDO Pdo);

// Run every class driver's start hook.
VOID     UacpiDrvsStartDevice(PUACPI_PDO Pdo);

// Run every class driver's remove hook.
VOID     UacpiDrvsRemoveDevice(PUACPI_PDO Pdo);

// drvs/processor.c, thermal.c, fan.c

// GUID_DEVICE_PROCESSOR on an ACPI0007 / Processor PDO.
VOID     UacpiProcessorStart(PUACPI_PDO Pdo);

// GUID_DEVICE_THERMAL_ZONE and the thermal WMI provider.
VOID     UacpiThermalStart(PUACPI_PDO Pdo);

// Deregister the thermal WMI provider.
VOID     UacpiThermalRemove(PUACPI_PDO Pdo);

// IRP_MJ_SYSTEM_CONTROL for a thermal zone via WmiLib.
NTSTATUS UacpiThermalSystemControl(PUACPI_PDO Pdo, PIRP Irp, PBOOLEAN Handled);

// The running release's thermal IOCTLs; *Handled set when consumed.
NTSTATUS UacpiThermalDeviceControl(PUACPI_PDO Pdo, PIRP Irp, PBOOLEAN Handled);

// Notify 0x80 (temperature) and 0x81 (trip points) on a thermal zone.
VOID     UacpiThermalNotify(PUACPI_PDO Pdo, ULONG NotifyValue);

// GUID_DEVICE_FAN on a PNP0C0B PDO.
VOID     UacpiFanStart(PUACPI_PDO Pdo);

// GUID_DEVICE_APPLICATIONLAUNCH_BUTTON on a PNP0C32 PDO.
VOID     UacpiApplaunchStart(PUACPI_PDO Pdo);

// drvs/ec.c

// Bring up the boot EC before the namespace is initialized: the ECDT one if the
// table is present, else the first PNP0C09 in the namespace. _INI and _REG both
// run AML that reads EC fields, so a handler has to exist before either.
VOID UacpiEcInitialize(void);

// Adopt the EC brought up early, or install this one from its _CRS.
VOID UacpiEcStart(PUACPI_PDO Pdo);

// Detach the PDO; the controller itself stays up for AML.
VOID UacpiEcRemove(PUACPI_PDO Pdo);

// IRP_MJ_READ / IRP_MJ_WRITE against the EC address space (buffered).
NTSTATUS UacpiEcReadWrite(PUACPI_PDO Pdo, PIRP Irp);

// Register / unregister a _Qxx handler; *Handled set when consumed.
NTSTATUS UacpiEcDeviceControl(PUACPI_PDO Pdo, PIRP Irp, PBOOLEAN Handled);

// TRUE if the node's _HID/_CID is PNP0C09.
BOOLEAN UacpiEcIsEcNode(uacpi_namespace_node *Node);

// fdo.c, pdo.c

NTSTATUS UacpiFdoPnp(PUACPI_FDO Fdo, PIRP Irp);

NTSTATUS UacpiFdoPower(PUACPI_FDO Fdo, PIRP Irp);

NTSTATUS UacpiPdoPnp(PUACPI_PDO Pdo, PIRP Irp);

NTSTATUS UacpiPdoPower(PUACPI_PDO Pdo, PIRP Irp);

// driver.c: IRP helpers

// Complete Irp; returns Status.
NTSTATUS UacpiCompleteIrp(PIRP Irp, NTSTATUS Status, ULONG_PTR Information);

NTSTATUS UacpiForwardAndForget(PDEVICE_OBJECT Lower, PIRP Irp);

// Forward and wait for completion; PASSIVE_LEVEL only.
NTSTATUS UacpiForwardAndWait(PDEVICE_OBJECT Lower, PIRP Irp);

// Gates UacpiTrace; default 1 (driver.c).
extern int UacpiTraceEnabled;

// Trace to the kernel debugger when UacpiTraceEnabled is set.
#define UacpiTrace(...)                                                        \
    do {                                                                      \
        if (UacpiTraceEnabled) {                                               \
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, __VA_ARGS__); \
        }                                                                     \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif // _ACPIPRIV_H_