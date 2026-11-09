/*
 * PROJECT:     ReactOS Telephony
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     RIL command numbers, notification codes and payload layouts
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

typedef PVOID HRIL;

typedef enum _RIL_COMMAND_CODE
{
    RilCmdInitNotifications = 1,
    RilCmdGetNextNotification = 2,
    RilCmdEnableNotifications = 3,
    RilCmdDisableNotifications = 4,
    RilCmdGetDriverVersion = 5,
    RilCmdGetDevCaps = 6,
    RilCmdGetDeviceInfo = 7,
    RilCmdGetEquipmentState = 8,
    RilCmdSetEquipmentState = 9,
    RilCmdSetNotificationFilterState = 10,
    RilCmdGetNotificationFilterState = 11,
    RilCmdEnumerateSlots = 12,
    RilCmdGetCardInfo = 13,
    RilCmdSetSlotPower = 14,
    RilCmdGetUiccRecordStatus = 15,
    RilCmdSendRestrictedUiccCmd = 16,
    RilCmdWatchUiccFileChange = 17,
    RilCmdGetUiccPrlId = 18,
    RilCmdGetImsi = 19,
    RilCmdGetSubscriberNumbers = 20,
    RilCmdGetUiccLockState = 21,
    RilCmdGetUiccServiceLock = 22,
    RilCmdVerifyUiccLock = 23,
    RilCmdSetUiccLockEnabled = 24,
    RilCmdUnblockUiccLock = 25,
    RilCmdChangeUiccLockPassword = 26,
    RilCmdGetUiccAppPersoCheckState = 27,
    RilCmdGetPersoDeactivationState = 28,
    RilCmdDeactivatePerso = 29,
    RilCmdReadPhonebookEntries = 30,
    RilCmdWritePhonebookEntry = 31,
    RilCmdDeletePhonebookEntry = 32,
    RilCmdGetPhonebookOptions = 33,
    RilCmdGetAllAdditionalNumberStrings = 34,
    RilCmdGetAllEmergencyNumbers = 35,
    RilCmdSetRadioConfiguration = 36,
    RilCmdGetRadioConfiguration = 37,
    RilCmdSetExecutorConfig = 38,
    RilCmdGetExecutorConfig = 39,
    RilCmdSetSystemSelectionPrefs = 40,
    RilCmdGetSystemSelectionPrefs = 41,
    RilCmdGetOperatorList = 42,
    RilCmdGetPreferredOperatorList = 43,
    RilCmdGetCurrentRegStatus = 44,
    RilCmdGetSignalQuality = 45,
    RilCmdSendUiccToolkitCmdResponse = 47,
    RilCmdSendUiccToolkitEnvelope = 48,
    RilCmdDial = 49,
    RilCmdManageCalls = 50,
    RilCmdEmergencyModeControl = 51,
    RilCmdGetCallForwardingSettings = 52,
    RilCmdSetCallForwardingStatus = 53,
    RilCmdAddCallForwarding = 54,
    RilCmdRemoveCallForwarding = 55,
    RilCmdGetCallBarringStatus = 56,
    RilCmdSetCallBarringStatus = 57,
    RilCmdChangeCallBarringPassword = 58,
    RilCmdGetCallWaitingSettings = 59,
    RilCmdSetCallWaitingStatus = 60,
    RilCmdGetCallerIdSettings = 61,
    RilCmdGetDialedIdSettings = 62,
    RilCmdGetHideConnectedIdSettings = 63,
    RilCmdGetHideIdSettings = 64,
    RilCmdSendFlash = 65,
    RilCmdSendSupServiceData = 66,
    RilCmdSendDtmf = 67,
    RilCmdStartDtmf = 68,
    RilCmdStopDtmf = 69,
    RilCmdGetMsgServiceOptions = 70,
    RilCmdReadMsg = 71,
    RilCmdWriteMsg = 72,
    RilCmdDeleteMsg = 73,
    RilCmdGetCellBroadcastMsgConfig = 74,
    RilCmdSetCellBroadcastMsgConfig = 75,
    RilCmdGetMsgInUiccStatus = 76,
    RilCmdSetMsgInUiccStatus = 77,
    RilCmdSetMsgMemoryStatus = 78,
    RilCmdSendMsg = 79,
    RilCmdGetSmsc = 80,
    RilCmdSetSmsc = 81,
    RilCmdGetImsStatus = 82,
    RilCmdGetPositionInfo = 83,
    RilCmdGetRadioStateGroups = 84,
    RilCmdGetRadioStateDetails = 85,
    RilCmdSetRadioStateDetails = 86,
    RilCmdRadioStatePasswordCompare = 87,
    RilCmdRadioStateGetPasswordRetryCount = 88,
    RilCmdDevSpecific = 89,
    RilCmdSetRfState = 90,
    RilCmdGetRfState = 91,
    RilCmdGetDmProfileConfigInfo = 92,
    RilCmdSetDmProfileConfigInfo = 93,
    RilCmdWriteAdditionalNumberString = 94,
    RilCmdDeleteAdditionalNumberString = 95,
    RilCmdGetUiccAtr = 96,
    RilCmdOpenUiccLogicalChannel = 97,
    RilCmdCloseUiccLogicalChannel = 98,
    RilCmdExchangeUiccApdu = 99,
    RilCmdSendSupServiceDataResponse = 100,
    RilCmdCancelSupServiceDataSession = 101,
    RilCmdSetUiccToolkitProfile = 102,
    RilCmdGetUiccToolkitProfile = 103,
    RilCmdRegisterUiccToolkitService = 104,
    RilCmdSendMsgAck = 105,
    RilCmdCloseUiccLogicalChannelGroup = 106,
    RilCmdSetPreferredOperatorList = 107,
    RilCmdGetUiccServiceState = 108,
    RilCmdSetUiccServiceState = 109,
    RilCmdGetCallList = 110,
    RilCmdGetExecutorFocus = 111,
    RilCmdSetExecutorFocus = 112,
    RilCmdGetEmergencyMode = 113,
    RilCmdGetExecutorRfState = 114,
    RilCmdSetExecutorRfState = 115,
    RilCmdResetModem = 116,
    RilCmdCancelGetOperatorList = 117,
    RilCmdAvoidCdmaSystem = 118,
    RilCmdSetPsMediaConfiguration = 119,
    RilCmdGetPsMediaConfiguration = 120,
    RilCmdSetGeolocationData = 121,
    RilCmdSetTerminalCapability = 122,
    RilCmdGetTerminalCapability = 123,
    RilCmdSendRtt = 124,
    RilCmdEnableModemFilters = 1024,
    RilCmdDisableModemFilters = 1025,
    RilCmdStartModemLogs = 1026,
    RilCmdStopModemLogs = 1027,
    RilCmdDrainModemLogs = 1028
} RIL_COMMAND_CODE;

/* Result types carried in RIL_RESPONSE_RECORD.ResultType */
#define RIL_RESULT_OK               1
#define RIL_RESULT_ERROR            2

/* Payload of a RIL_RESULT_ERROR response */
typedef struct _RIL_ERROR_RESULT
{
    ULONG Size;
    ULONG Params;
    HRESULT Error;
    ULONG Reserved;
} RIL_ERROR_RESULT, *PRIL_ERROR_RESULT;

C_ASSERT(sizeof(RIL_ERROR_RESULT) == 16);

#define RIL_ERROR(n)                ((HRESULT)(0x81000000 | (n)))
#define RIL_E_OPNOTALLOWED          RIL_ERROR(0x04)
#define RIL_E_OPNOTSUPPORTED        RIL_ERROR(0x05)
#define RIL_E_UICCNOTINSERTED       RIL_ERROR(0x06)
#define RIL_E_NOTFOUND              RIL_ERROR(0x0D)
#define RIL_E_DIALSTRINGTOOLONG     RIL_ERROR(0x11)
#define RIL_E_INVALIDDIALSTRING     RIL_ERROR(0x12)
#define RIL_E_EMERGENCYONLY         RIL_ERROR(0x15)
#define RIL_E_RADIONOTAVAILABLE     RIL_ERROR(0x50)
#define RIL_E_RADIOOFF              RIL_ERROR(0x51)

/* Notification codes */
#define RIL_NOTIFY(n)               (0x10000000 | (n))
#define RIL_NOTIFY_RADIOPRESENCE    RIL_NOTIFY(0x101)
#define RIL_NOTIFY_EMERGENCYNUMBERS RIL_NOTIFY(0x10C)
#define RIL_NOTIFY_CALLMODIFIED     RIL_NOTIFY(0x117)
#define RIL_NOTIFY_CALLSTATE        RIL_NOTIFY(0x118)
#define RIL_NOTIFY_SUPSERVICEINFO   RIL_NOTIFY(0x120)
#define RIL_NOTIFY_DTMFSTART        RIL_NOTIFY(0x12A)
#define RIL_NOTIFY_DTMFSTOP         RIL_NOTIFY(0x12B)
#define RIL_NOTIFY_DTMFBURST        RIL_NOTIFY(0x12C)
#define RIL_NOTIFY_MESSAGE          RIL_NOTIFY(0x122)

/* RIL_NOTIFY_RADIOPRESENCE payload */
#define RIL_RADIO_ABSENT            1
#define RIL_RADIO_PRESENT           2

typedef VOID
(CALLBACK *PRIL_RESULT_CALLBACK)(
    _In_ HRIL hRil,
    _In_ DWORD ResultType,
    _In_ ULONGLONG Context,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize,
    _In_opt_ PVOID Param);

typedef VOID
(CALLBACK *PRIL_NOTIFY_CALLBACK)(
    _In_ HRIL hRil,
    _In_ DWORD Code,
    _In_reads_bytes_opt_(DataSize) const VOID *Data,
    _In_ DWORD DataSize,
    _In_opt_ PVOID Param);

/* Same layout as the Windows Mobile SDK RILADDRESS */
#define MAXLENGTH_ADDRESS           256

#define RILADDRESS_PARAM_TYPE       0x00000001
#define RILADDRESS_PARAM_NUMPLAN    0x00000002
#define RILADDRESS_PARAM_ADDRESS    0x00000004

#define RILADDRESSTYPE_UNKNOWN          0
#define RILADDRESSTYPE_INTERNATIONAL    1
#define RILADDRESSTYPE_NATIONAL         2
#define RILADDRESSTYPE_URI              9

typedef struct _RILADDRESS
{
    DWORD cbSize;
    DWORD dwParams;
    DWORD dwType;
    DWORD dwNumPlan;
    WCHAR wszAddress[MAXLENGTH_ADDRESS];
} RILADDRESS, *LPRILADDRESS;

C_ASSERT(sizeof(RILADDRESS) == 528);

/* Media description used by dial and call management */
#define RIL_MEDIA_MAX_ENTRIES       4

typedef struct _RIL_MEDIA_ENTRY
{
    DWORD Size;
    DWORD Params;
    DWORD MediaType;
    DWORD MediaId;
    DWORD Negotiation;
    BYTE Reserved1[76];
    DWORD Direction;
    BYTE Reserved2[68];
} RIL_MEDIA_ENTRY, *PRIL_MEDIA_ENTRY;

typedef struct _RIL_MEDIA_SET
{
    DWORD Size;
    DWORD Params;
    DWORD Count;
    RIL_MEDIA_ENTRY Entries[RIL_MEDIA_MAX_ENTRIES];
} RIL_MEDIA_SET, *PRIL_MEDIA_SET;

C_ASSERT(sizeof(RIL_MEDIA_ENTRY) == 168);
C_ASSERT(sizeof(RIL_MEDIA_SET) == 684);

/* Dial options */
#define RIL_DIAL_RESTRICT_CALLER_ID 0x00000001
#define RIL_DIAL_PRESENT_CALLER_ID  0x00000002
#define RIL_DIAL_EMERGENCY          0x00000004

/* Dial domain */
#define RIL_CALL_DOMAIN_AUTO        0
#define RIL_CALL_DOMAIN_CS          1
#define RIL_CALL_DOMAIN_IMS         8

/* Call management actions */
#define RIL_CALLACTION_RELEASE_ACTIVE_ACCEPT_HELD   2
#define RIL_CALLACTION_RELEASE_CALL                 3
#define RIL_CALLACTION_SWAP                         4
#define RIL_CALLACTION_SPLIT                        5
#define RIL_CALLACTION_CONFERENCE                   6
#define RIL_CALLACTION_TRANSFER                     7
#define RIL_CALLACTION_HANGUP_OUTGOING              10
#define RIL_CALLACTION_RELEASE_ALL                  11
#define RIL_CALLACTION_RELEASE_HELD                 12
#define RIL_CALLACTION_ANSWER                       13

/* Call status in RIL_CALL_RECORD.Status */
#define RIL_CALLSTATUS_IDLE         1
#define RIL_CALLSTATUS_OUTGOING     2
#define RIL_CALLSTATUS_INCOMING     3
#define RIL_CALLSTATUS_CONNECTED    4
#define RIL_CALLSTATUS_DISCONNECTED 5
#define RIL_CALLSTATUS_HELD         6

#define RIL_CALLDIR_INCOMING        1
#define RIL_CALLDIR_OUTGOING        2

#define RIL_CALLFLAG_EMERGENCY      0x00000002

typedef struct _RIL_CALL_RECORD
{
    DWORD Size;
    DWORD Params;
    DWORD Executor;
    DWORD CallId;
    DWORD Direction;
    DWORD Status;
    DWORD Domain;
    DWORD Multiparty;
    RILADDRESS Remote;
    BYTE SubAddress[524];
    WCHAR RemoteName[256];
    DWORD NumberPresentation;
    DWORD NamePresentation;
    DWORD Flags;
    DWORD DisconnectInitiator;
    DWORD DisconnectReason;
    DWORD DisconnectDetail[3];
    RIL_MEDIA_SET Media;
    BYTE Handover[24];
    DWORD ModifiedCause;
} RIL_CALL_RECORD, *PRIL_CALL_RECORD;

C_ASSERT(FIELD_OFFSET(RIL_CALL_RECORD, Remote) == 32);
C_ASSERT(FIELD_OFFSET(RIL_CALL_RECORD, RemoteName) == 1084);
C_ASSERT(FIELD_OFFSET(RIL_CALL_RECORD, Flags) == 1604);
C_ASSERT(FIELD_OFFSET(RIL_CALL_RECORD, Media) == 1628);
C_ASSERT(sizeof(RIL_CALL_RECORD) == 2340);

/* WMRil packs the call list with a 2336 byte stride, the last DWORD of each entry but the final one is clobbered */
#define RIL_CALLLIST_STRIDE         2336

/* Host interface of the OEM RIL library */
typedef DWORD (WINAPI *PFN_RIL_VERSION)(DWORD MinVersion, DWORD MaxVersion);
typedef DWORD (WINAPI *PFN_RIL_INIT)(DWORD Index);
typedef DWORD (WINAPI *PFN_RIL_OPEN)(DWORD Driver);
typedef BOOL (WINAPI *PFN_RIL_CLOSE)(DWORD Client);
typedef BOOL (WINAPI *PFN_RIL_IOCONTROL)(DWORD Client,
                                         DWORD Code,
                                         PBYTE InBuffer,
                                         DWORD InSize,
                                         PBYTE OutBuffer,
                                         DWORD OutSize,
                                         PDWORD ActualOut);

/* RilCmdGetNextNotification output header, version 1 and version 2 of the host interface */
typedef struct _RIL_HOST_QUEUED_V1
{
    DWORD Size;
    DWORD Required;
    DWORD Code;
    DWORD CommandId;
    DWORD DataSize;
} RIL_HOST_QUEUED_V1, *PRIL_HOST_QUEUED_V1;

typedef struct _RIL_HOST_QUEUED_V2
{
    DWORD Size;
    DWORD Required;
    DWORD Code;
    DWORD CommandId;
    DWORD Sequence;
    DWORD DataSize;
} RIL_HOST_QUEUED_V2, *PRIL_HOST_QUEUED_V2;

C_ASSERT(sizeof(RIL_HOST_QUEUED_V1) == 20);
C_ASSERT(sizeof(RIL_HOST_QUEUED_V2) == 24);

/* Equipment and executor radio state */
#define RIL_EQSTATE_LOWPOWER        1
#define RIL_EQSTATE_FULL            2
#define RIL_EQSTATE_RESET           3

/* Message types */
#define RIL_MSGTYPE_GW_DELIVER      0x00010001
#define RIL_MSGTYPE_GW_STATUS       0x00010002
#define RIL_MSGTYPE_GW_SUBMIT       0x00020001

/* RIL_MESSAGE.Params */
#define RIL_MSGPARAM_SERVICECENTER  0x00000001
#define RIL_MSGPARAM_TYPE           0x00000002
#define RIL_MSGPARAM_FLAGS          0x00000004
#define RIL_MSGPARAM_ADDRESS        0x00000008
#define RIL_MSGPARAM_TIMESTAMP      0x00000040
#define RIL_MSGPARAM_PROTOCOLID     0x00000200
#define RIL_MSGPARAM_DCS            0x00000800
#define RIL_MSGPARAM_HEADER         0x01000000
#define RIL_MSGPARAM_BODY           0x08000000

/* RIL_MESSAGE.Flags for submit */
#define RIL_MSGFLAG_REPLYPATH       0x00000002
#define RIL_MSGFLAG_REJECTDUPS      0x00000008
#define RIL_MSGFLAG_STATUSREPORT    0x00000020

/* RIL_MESSAGE.Flags for deliver */
#define RIL_MSGFLAG_MORE_FOLLOWS_CLEAR 0x00000001
#define RIL_MSGFLAG_HEADER_PRESENT  0x00000004

#define RIL_DCSPARAM_ALPHABET       0x00000008

#define RIL_ALPHABET_GSM7           1
#define RIL_ALPHABET_8BIT           2
#define RIL_ALPHABET_UCS2           3

#define RIL_MESSAGE_HEADER_MAX      256
#define RIL_MESSAGE_BODY_MAX        512

typedef struct _RIL_MESSAGE_DCS
{
    DWORD Size;
    DWORD Params;
    DWORD Type;
    DWORD Flags;
    DWORD MessageClass;
    DWORD Alphabet;
    DWORD Indication;
    DWORD Language;
} RIL_MESSAGE_DCS, *PRIL_MESSAGE_DCS;

/* GSM 7 bit bodies hold one unpacked GSM code per byte, UCS2 bodies hold WCHARs */
typedef struct _RIL_MESSAGE
{
    DWORD Size;
    DWORD Params;
    RILADDRESS ServiceCenter;
    DWORD Type;
    DWORD Flags;
    RILADDRESS Address;
    DWORD ProtocolId;
    RIL_MESSAGE_DCS Dcs;
    union
    {
        struct
        {
            SYSTEMTIME Timestamp;
            DWORD Reserved;
            DWORD HeaderLength;
            DWORD BodyLength;
            BYTE Header[RIL_MESSAGE_HEADER_MAX];
            BYTE Body[RIL_MESSAGE_BODY_MAX];
        } Deliver;
        struct
        {
            DWORD ValidityFormat;
            BYTE Validity[20];
            DWORD HeaderLength;
            DWORD BodyLength;
            BYTE Header[RIL_MESSAGE_HEADER_MAX];
            BYTE Body[RIL_MESSAGE_BODY_MAX];
        } Submit;
    } u;
    BYTE Reserved[744];
} RIL_MESSAGE, *PRIL_MESSAGE;

C_ASSERT(FIELD_OFFSET(RIL_MESSAGE, Type) == 536);
C_ASSERT(FIELD_OFFSET(RIL_MESSAGE, Address) == 544);
C_ASSERT(FIELD_OFFSET(RIL_MESSAGE, Dcs) == 1076);
C_ASSERT(FIELD_OFFSET(RIL_MESSAGE, u.Deliver.HeaderLength) == 1128);
C_ASSERT(FIELD_OFFSET(RIL_MESSAGE, u.Deliver.Body) == 1392);
C_ASSERT(FIELD_OFFSET(RIL_MESSAGE, u.Submit.HeaderLength) == 1132);
C_ASSERT(FIELD_OFFSET(RIL_MESSAGE, u.Submit.Body) == 1396);
C_ASSERT(sizeof(RIL_MESSAGE) == 2652);

/* RIL_NOTIFY_MESSAGE payload */
typedef struct _RIL_MESSAGE_NOTIFICATION
{
    DWORD Size;
    DWORD Executor;
    DWORD Params;
    DWORD AckId;
    RIL_MESSAGE Message;
    DWORD OverIms;
} RIL_MESSAGE_NOTIFICATION, *PRIL_MESSAGE_NOTIFICATION;

C_ASSERT(sizeof(RIL_MESSAGE_NOTIFICATION) == 2672);

/* RIL_SendMsgAck status and protocol */
#define RIL_MSGACK_OK               1
#define RIL_MSGACK_MEMORYFULL       2
#define RIL_MSGACK_FAILED           3
#define RIL_MSGACK_PROTOCOL_3GPP    1
