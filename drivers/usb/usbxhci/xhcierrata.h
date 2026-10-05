/*
 * PROJECT:     ReactOS xHCI Host Controller Driver
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Controller errata flag bit positions
 * COPYRIGHT:   Copyright 2026 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

/*
 * Bit positions of the 64 bit errata value. They match the layout the errata
 * database uses, so a value from there can be ORed in unchanged.
 */
enum class XhciErrata : ULONG
{
    Split64BitRegisterAccess = 0,
    NoStateSaveRestore = 1,
    IgnoreBiosHandoffFailure = 2,
    NoLinkTrbInsideTd = 3,
    ControllerNotSupported = 4,
    ContextErrorOnStopMeansStopped = 5,
    DataStageTrbMax512 = 6,
    NoChainedMdl = 7,
    UnusedBit8 = 8,
    LineInterruptsOnly = 9,
    UnusedBit10 = 10,
    ChainBitOnLinkTrb = 11,
    SingleInterrupter = 12,
    IgnoreCompletionCode199 = 13,
    DelayFirstStopEndpoint = 14,
    ResetByDisableEnableSlot = 15,
    EndpointIntervalMax7 = 16,
    NoImmediateData = 17,
    NoStreams = 18,
    U0BeforeU3 = 19,
    NoIdlePower = 20,
    ReconfigureStreamEndpointAfterStop = 21,
    NoUsb20Lpm = 22,
    PantherPointBandwidthLimit = 23,
    FirmwareOutdated = 24,
    Usb20PortDisableAsSuspend = 25,
    CanMissU3Request = 26,
    Usb20LpmOffBeforeU3 = 27,
    RecoverOnEventRingFull = 28,
    KeepPortWakeBitsInD0 = 29,
    ClearConnectChangeBeforeD0Exit = 30,
    OneCommandAtATime = 31,
    ClearTtBufferOnAsyncCancel = 32,
    NoIsochBei = 33,
    RoundSplitTransferToPacket = 34,
    StreamEdtlaAlternate1 = 35,
    StreamEdtlaAlternate2 = 36,
    ValidateTransferEventPointer = 37,
    DropDuplicateEd0Events = 38,
    SerializeBulkInterruptTds = 39,
    PartialStreamEdtla = 40,
    NoLateIsochTds = 41,
    MissedServiceEventsMayBeMissing = 42,
    ResetOnShutdownRestart = 43,
    StreamEdtlaReinit = 44,
    UseEdtlaValidBit = 45,
    PantherPointBulkBeforeInterruptFix = 46,
    NoStandaloneLinkTdFullSpeedIsoch = 47,
    EventRingSegmentsMax2 = 48,
    NoOpsAfterEventDataSuperSpeedBulkIn = 49,
    ZeroTrbSegments = 50,
    HideBadDebugPortStatus = 51,
    UnlimitedErrorRetries = 52,
    IgnoreEventDataOnRingEmpty = 53,
    AcpiCallBeforeInterruptsOff = 54,
    SaveVendorRegisterBits = 55,
    PulsePmeWakeBit = 56,
    SsicPortUnusedProgramming = 57,
    DsmEnableRtd3 = 58,
    IgnoreContiguousFrameId = 59,
    DsmHsicDisconnectInU3 = 60,
    MultiTtDuringConfigure = 61,
    Reserved62 = 62,
    StrictBiosHandoff = 63,

    /* Second 64 bit errata word, position 64 + bit */
    TunnelStateFromVendorPortRegister = 64 + 22,
    TunnelStateFromVendorStatusRegister = 64 + 23,
    TunnelStateFromAcpiDsm = 64 + 28
};

FORCEINLINE
ULONG64
NTAPI
XhciErrataBit(
    _In_ XhciErrata Bit)
{
    return 1ULL << static_cast<ULONG>(Bit);
}
