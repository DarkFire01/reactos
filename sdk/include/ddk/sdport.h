/*
 * PROJECT:     ReactOS Storage Stack / SD bus
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     SD host controller port driver miniport interface
 * COPYRIGHT:   Justin Miller (justinmiller100@gmail.com)
 */

#ifndef _SDPORT_H_
#define _SDPORT_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Submit interface exposed by the port driver PDO to the class driver above.
   CTL_CODE(FILE_DEVICE_CONTROLLER, 0xC1C, METHOD_NEITHER, FILE_ANY_ACCESS). */
#define IOCTL_SDPORT_SUBMIT_REQUEST \
    CTL_CODE(FILE_DEVICE_CONTROLLER, 0xC1C, METHOD_NEITHER, FILE_ANY_ACCESS)

/* Minimum size of the request block handed through the submit interface. */
#define SDPORT_REQUEST_LENGTH 104

typedef enum _SDPORT_REQUEST_TYPE
{
    SdRequestGetProperty = 0,
    SdRequestSetProperty = 1,
    SdRequestCommand = 2,
    SdRequestCommandWithTransfer = 3
} SDPORT_REQUEST_TYPE;

typedef enum _SDPORT_PROPERTY
{
    SdPropertySlotCount = 0,
    SdPropertyPresent = 1,
    SdPropertyCardDetect = 2,
    SdPropertyWriteProtect = 3,
    SdPropertyBusMode = 4,
    SdPropertyMaxBlockCount = 5,
    SdPropertyBusWidth = 6,
    SdPropertyClock = 7,
    SdPropertyCardStatus = 8,
    SdPropertyCapabilities0 = 9,
    SdPropertyCapabilities1 = 10,
    SdPropertyCapabilities2 = 11,
    SdPropertySignalingVoltage = 12,
    SdPropertyFlags = 15,
    SdPropertyHighSpeed = 16,
    SdPropertyCid = 17,
    SdPropertyCsd = 18,
    SdPropertyHighCapacity = 19,
    SdPropertyRca = 20
} SDPORT_PROPERTY;

typedef enum _SDPORT_BUS_OPERATION_TYPE
{
    SdResetHost = 1,
    SdResetHw = 2,
    SdSetClock = 3,
    SdSetVoltage = 4,
    SdSetBusWidth = 5,
    SdSetBusSpeed = 6,
    SdSetSignalingVoltage = 7,
    SdSetDriveStrength = 8,
    SdSetDriverType = 9,
    SdSetPresetValue = 10,
    SdSetBlockGapInterrupt = 11,
    SdExecuteTuning = 12
} SDPORT_BUS_OPERATION_TYPE;

typedef enum _SDPORT_TRANSFER_TYPE
{
    SdTransferTypeNone = 0,
    SdTransferTypeSingleBlock = 1,
    SdTransferTypeMultiBlock = 2,
    SdTransferTypeMultiBlockNoStop = 3,
    SdTransferTypeCommandOnly = 4
} SDPORT_TRANSFER_TYPE;

typedef enum _SDPORT_TRANSFER_DIRECTION
{
    SdTransferDirectionUndefined = 0,
    SdTransferDirectionRead = 1,
    SdTransferDirectionWrite = 2
} SDPORT_TRANSFER_DIRECTION;

typedef enum _SDPORT_TRANSFER_METHOD
{
    SdTransferMethodUndefined = 0,
    SdTransferMethodPio = 1,
    SdTransferMethodSgDma = 2,
    SdTransferMethodAdma2 = 3
} SDPORT_TRANSFER_METHOD;

typedef enum _SDPORT_RESPONSE_TYPE
{
    SdResponseTypeNone = 0,
    SdResponseTypeR1 = 1,
    SdResponseTypeR1B = 2,
    SdResponseTypeR2 = 3,
    SdResponseTypeR3 = 4,
    SdResponseTypeR4 = 5,
    SdResponseTypeR5 = 6,
    SdResponseTypeR5B = 7,
    SdResponseTypeR6 = 8
} SDPORT_RESPONSE_TYPE;

typedef enum _SDPORT_BUS_WIDTH
{
    SdBusWidth1Bit = 1,
    SdBusWidth4Bit = 4,
    SdBusWidth8Bit = 8
} SDPORT_BUS_WIDTH;

typedef enum _SDPORT_BUS_SPEED
{
    SdBusSpeedNormal = 1,
    SdBusSpeedHigh = 2,
    SdBusSpeedSDR12 = 3,
    SdBusSpeedSDR25 = 4,
    SdBusSpeedSDR50 = 5,
    SdBusSpeedDDR50 = 6,
    SdBusSpeedSDR104 = 7,
    SdBusSpeedHS200 = 8,
    SdBusSpeedHS400 = 9
} SDPORT_BUS_SPEED;

typedef enum _SDPORT_SIGNALING_VOLTAGE
{
    SdSignalingVoltage33 = 0,
    SdSignalingVoltage18 = 1
} SDPORT_SIGNALING_VOLTAGE;

/* Slot capability descriptor filled by the miniport GetSlotCapabilities callback. */
typedef struct _SDPORT_CAPABILITIES
{
    ULONG Version;
    ULONG MaximumOutstandingRequests;
    ULONG MaximumBlockSize;
    ULONG MaximumBlockCount;
    ULONG BaseClockFrequencyKhz;
    ULONG MaximumCurrent;
    struct
    {
        ULONG ScatterGatherDma : 1;
        ULONG Address64Bit : 1;
        ULONG BusWidth8Bit : 1;
        ULONG HighSpeed : 1;
        ULONG Timing18V : 1;
        ULONG SDR50 : 1;
        ULONG SDR104 : 1;
        ULONG DDR50 : 1;
        ULONG DriverTypeA : 1;
        ULONG DriverTypeC : 1;
        ULONG DriverTypeD : 1;
        ULONG TuningForSDR50 : 1;
        ULONG SoftwareTuning : 1;
        ULONG AutoCmd12 : 1;
        ULONG AutoCmd23 : 1;
        ULONG Voltage33V : 1;
        ULONG Voltage30V : 1;
        ULONG Voltage18V : 1;
        ULONG SlotPowerControl : 1;
        ULONG Reserved : 13;
    } Flags;
    ULONG DmaDescriptorSize;
    ULONG AlignmentRequirement;
    ULONG PioTransferMaxThreshold;
    ULONG Reserved[4];
} SDPORT_CAPABILITIES, *PSDPORT_CAPABILITIES;

/* An SD/MMC command to be issued by the miniport. */
typedef struct _SDPORT_COMMAND
{
    UCHAR Class;
    UCHAR Index;
    ULONG Argument;
    SDPORT_TRANSFER_TYPE TransferType;
    SDPORT_TRANSFER_DIRECTION TransferDirection;
    SDPORT_RESPONSE_TYPE ResponseType;
    SDPORT_TRANSFER_METHOD TransferMethod;
    ULONG BlockSize;
    ULONG BlockCount;
    ULONG Length;
} SDPORT_COMMAND, *PSDPORT_COMMAND;

typedef struct _SDPORT_BUS_OPERATION
{
    SDPORT_BUS_OPERATION_TYPE Type;
    union
    {
        ULONG FrequencyKhz;
        SDPORT_BUS_WIDTH BusWidth;
        SDPORT_BUS_SPEED BusSpeed;
        SDPORT_SIGNALING_VOLTAGE SignalingVoltage;
        ULONG Voltage;
        ULONG Parameter;
    } Parameters;
} SDPORT_BUS_OPERATION, *PSDPORT_BUS_OPERATION;

/* The request block exchanged through IOCTL_SDPORT_SUBMIT_REQUEST and passed to
   the miniport IssueRequest callback.  The class driver fills in the command or
   property, the MDL/data describing the payload, and reads back Status,
   BytesTransferred and ResponseBuffer on completion.  The port driver tracks its
   own per-request timer, event and DMA state in a private wrapper, so none of
   that appears here. */
typedef struct _SDPORT_REQUEST
{
    SDPORT_REQUEST_TYPE Type;
    ULONG RequiredEvents;
    NTSTATUS Status;
    ULONG BytesTransferred;
    union
    {
        SDPORT_COMMAND Command;
        SDPORT_BUS_OPERATION BusOperation;
        struct
        {
            SDPORT_PROPERTY PropertyId;
            PVOID OutputBuffer;
            ULONG OutputBufferLength;
            ULONG Parameter;
        } Property;
    } Data;
    PMDL Mdl;
    PVOID DataBuffer;
    ULONG DataLength;
    ULONG ResponseBuffer[4];
} SDPORT_REQUEST, *PSDPORT_REQUEST;

/* Miniport callback prototypes.  The private extension pointer is the per-slot
   context whose size the miniport declared in PrivateExtensionSize. */
typedef NTSTATUS (NTAPI *PSDPORT_GET_SLOT_COUNT)(
    _In_ PVOID Argument,
    _Out_ PUCHAR SlotCount);

typedef VOID (NTAPI *PSDPORT_GET_SLOT_CAPABILITIES)(
    _In_ PVOID PrivateExtension,
    _Out_ PSDPORT_CAPABILITIES Capabilities);

typedef NTSTATUS (NTAPI *PSDPORT_INITIALIZE)(
    _In_ PVOID PrivateExtension,
    _In_ PHYSICAL_ADDRESS PhysicalBase,
    _In_ PVOID VirtualBase,
    _In_ ULONG Length,
    _In_ BOOLEAN CrashdumpMode);

typedef NTSTATUS (NTAPI *PSDPORT_ISSUE_BUS_OPERATION)(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_BUS_OPERATION BusOperation);

typedef BOOLEAN (NTAPI *PSDPORT_GET_CARD_DETECT_STATE)(
    _In_ PVOID PrivateExtension);

typedef BOOLEAN (NTAPI *PSDPORT_GET_WRITE_PROTECT_STATE)(
    _In_ PVOID PrivateExtension);

typedef BOOLEAN (NTAPI *PSDPORT_INTERRUPT)(
    _In_ PVOID PrivateExtension,
    _Out_ PULONG Events,
    _Out_ PULONG Errors,
    _Out_ PBOOLEAN CardChange,
    _Out_ PBOOLEAN SdioInterrupt,
    _Out_ PBOOLEAN Tuning);

typedef NTSTATUS (NTAPI *PSDPORT_ISSUE_REQUEST)(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_REQUEST Request);

typedef VOID (NTAPI *PSDPORT_GET_RESPONSE)(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_COMMAND Command,
    _Out_ PVOID ResponseBuffer);

typedef VOID (NTAPI *PSDPORT_TOGGLE_EVENTS)(
    _In_ PVOID PrivateExtension,
    _In_ ULONG EventMask,
    _In_ BOOLEAN Enable);

typedef VOID (NTAPI *PSDPORT_CLEAR_EVENTS)(
    _In_ PVOID PrivateExtension,
    _In_ ULONG EventMask);

typedef VOID (NTAPI *PSDPORT_REQUEST_DPC)(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_REQUEST Request,
    _In_ ULONG Events,
    _In_ ULONG Errors);

typedef VOID (NTAPI *PSDPORT_SAVE_CONTEXT)(
    _In_ PVOID PrivateExtension);

typedef VOID (NTAPI *PSDPORT_RESTORE_CONTEXT)(
    _In_ PVOID PrivateExtension);

typedef NTSTATUS (NTAPI *PSDPORT_PO_FX_POWER_CONTROL_CALLBACK)(
    _In_ PVOID PrivateExtension,
    _In_ LPCGUID PowerControlCode,
    _In_opt_ PVOID InputBuffer,
    _In_ SIZE_T InputBufferSize,
    _Out_opt_ PVOID OutputBuffer,
    _In_ SIZE_T OutputBufferSize,
    _Out_opt_ PSIZE_T BytesReturned);

typedef VOID (NTAPI *PSDPORT_CLEANUP)(
    _In_ PVOID PrivateExtension);

typedef struct _SDPORT_CRASHDUMP_FUNCTIONS *PSDPORT_CRASHDUMP_FUNCTIONS;

/* Initialization table passed by the miniport to SdPortInitialize.  The field
   order and offsets are the fixed port/miniport ABI. */
typedef struct _SDPORT_INITIALIZATION_DATA
{
    USHORT StructureSize;
    PSDPORT_GET_SLOT_COUNT GetSlotCount;
    PSDPORT_GET_SLOT_CAPABILITIES GetSlotCapabilities;
    PSDPORT_INITIALIZE Initialize;
    PSDPORT_ISSUE_BUS_OPERATION IssueBusOperation;
    PSDPORT_GET_CARD_DETECT_STATE GetCardDetectState;
    PSDPORT_GET_WRITE_PROTECT_STATE GetWriteProtectState;
    PSDPORT_INTERRUPT Interrupt;
    PSDPORT_ISSUE_REQUEST IssueRequest;
    PSDPORT_GET_RESPONSE GetResponse;
    PSDPORT_TOGGLE_EVENTS ToggleEvents;
    PSDPORT_CLEAR_EVENTS ClearEvents;
    PSDPORT_REQUEST_DPC RequestDpc;
    PSDPORT_SAVE_CONTEXT SaveContext;
    PSDPORT_RESTORE_CONTEXT RestoreContext;
    PSDPORT_PO_FX_POWER_CONTROL_CALLBACK PowerControlCallback;
    PSDPORT_CLEANUP Cleanup;
    ULONG PrivateExtensionSize;
    PSDPORT_CRASHDUMP_FUNCTIONS CrashdumpFunctions;
} SDPORT_INITIALIZATION_DATA, *PSDPORT_INITIALIZATION_DATA;

/* Exported port library routines called by miniports. */

NTSTATUS
NTAPI
SdPortInitialize(
    _In_ PVOID DriverObject,
    _In_ PUNICODE_STRING RegistryPath,
    _In_ PSDPORT_INITIALIZATION_DATA InitializationData);

VOID
NTAPI
SdPortCompleteRequest(
    _In_ PSDPORT_REQUEST Request,
    _In_ NTSTATUS Status);

VOID
NTAPI
SdPortWait(
    _In_ ULONG Microseconds);

UCHAR
NTAPI
SdPortReadRegisterUchar(
    _In_ PVOID Base,
    _In_ ULONG Offset);

USHORT
NTAPI
SdPortReadRegisterUshort(
    _In_ PVOID Base,
    _In_ ULONG Offset);

ULONG
NTAPI
SdPortReadRegisterUlong(
    _In_ PVOID Base,
    _In_ ULONG Offset);

VOID
NTAPI
SdPortReadRegisterBufferUchar(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _Out_writes_(Count) PUCHAR Buffer,
    _In_ ULONG Count);

VOID
NTAPI
SdPortReadRegisterBufferUshort(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _Out_writes_(Count) PUSHORT Buffer,
    _In_ ULONG Count);

VOID
NTAPI
SdPortReadRegisterBufferUlong(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _Out_writes_(Count) PULONG Buffer,
    _In_ ULONG Count);

VOID
NTAPI
SdPortWriteRegisterUchar(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _In_ UCHAR Value);

VOID
NTAPI
SdPortWriteRegisterUshort(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _In_ USHORT Value);

VOID
NTAPI
SdPortWriteRegisterUlong(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _In_ ULONG Value);

VOID
NTAPI
SdPortWriteRegisterBufferUchar(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _In_reads_(Count) PUCHAR Buffer,
    _In_ ULONG Count);

VOID
NTAPI
SdPortWriteRegisterBufferUshort(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _In_reads_(Count) PUSHORT Buffer,
    _In_ ULONG Count);

VOID
NTAPI
SdPortWriteRegisterBufferUlong(
    _In_ PVOID Base,
    _In_ ULONG Offset,
    _In_reads_(Count) PULONG Buffer,
    _In_ ULONG Count);

NTSTATUS
NTAPI
SdPortGetPciConfigSpace(
    _In_ PVOID Fdo,
    _In_ UCHAR Offset,
    _Out_writes_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length);

NTSTATUS
NTAPI
SdPortSetPciConfigSpace(
    _In_ PVOID Fdo,
    _In_ UCHAR Offset,
    _In_reads_bytes_(Length) PVOID Buffer,
    _In_ ULONG Length);

NTSTATUS
NTAPI
SdPortPoFxPowerControl(
    _In_ PVOID PrivateExtension,
    _In_ LPCGUID PowerControlCode,
    _In_opt_ PVOID InputBuffer,
    _In_ SIZE_T InputBufferSize,
    _Out_opt_ PVOID OutputBuffer,
    _In_ SIZE_T OutputBufferSize,
    _Out_opt_ PSIZE_T BytesReturned);

#ifdef __cplusplus
}
#endif

#endif /* _SDPORT_H_ */
