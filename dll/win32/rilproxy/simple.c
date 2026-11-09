/*
 * PROJECT:     ReactOS RIL Proxy
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Fixed size RIL requests
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "rilproxy.h"

HRESULT
WINAPI
RIL_GetDriverVersion(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdGetDriverVersion, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetDevCaps(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetDevCaps, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetDeviceInfo(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdGetDeviceInfo, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetDMProfileConfigInfo(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdGetDmProfileConfigInfo, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetEquipmentState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdSetEquipmentState, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetNotificationFilterState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdSetNotificationFilterState, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetNotificationFilterState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context)
{
    return RilpSendHeaderOnly(hRil, RilCmdGetNotificationFilterState, Context);
}

HRESULT
WINAPI
RIL_SetRadioConfiguration(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdSetRadioConfiguration, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetRadioConfiguration(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context)
{
    return RilpSendHeaderOnly(hRil, RilCmdGetRadioConfiguration, Context);
}

HRESULT
WINAPI
RIL_GetExecutorConfig(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetExecutorConfig, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetSystemSelectionPrefs(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetSystemSelectionPrefs, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetOperatorList(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdGetOperatorList, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetPreferredOperatorList(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdGetPreferredOperatorList, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetCurrentRegStatus(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetCurrentRegStatus, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_CancelGetOperatorList(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdCancelGetOperatorList, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetPSMediaConfiguration(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetPsMediaConfiguration, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetSignalQuality(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetSignalQuality, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetPositionInfo(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetPositionInfo, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetRadioStateGroups(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetRadioStateGroups, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetRadioStateDetails(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdGetRadioStateDetails, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_RadioStateGetPasswordRetryCount(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdRadioStateGetPasswordRetryCount, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetRFState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context)
{
    return RilpSendHeaderOnly(hRil, RilCmdGetRfState, Context);
}

HRESULT
WINAPI
RIL_ResetModem(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdResetModem, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_AvoidCDMASystem(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdAvoidCdmaSystem, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetMsgServiceOptions(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetMsgServiceOptions, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_ReadMsg(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdReadMsg, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_WriteMsg(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_reads_bytes_(2652) const VOID *Arg2,
    _In_ DWORD Arg3)
{
    BYTE Block[2676];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    CopyMemory(&Block[20], Arg2, 2652);
    *(PDWORD)&Block[2672] = Arg3;
    return RilpSend(hRil, RilCmdWriteMsg, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_DeleteMsg(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdDeleteMsg, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetCellBroadcastMsgConfig(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetCellBroadcastMsgConfig, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetCellBroadcastMsgConfig(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_reads_bytes_(1216) const VOID *Arg2)
{
    BYTE Block[1236];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    CopyMemory(&Block[20], Arg2, 1216);
    return RilpSend(hRil, RilCmdSetCellBroadcastMsgConfig, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetMsgInUiccStatus(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdGetMsgInUiccStatus, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetMsgInUiccStatus(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2,
    _In_ DWORD Arg3)
{
    BYTE Block[28];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    *(PDWORD)&Block[24] = Arg3;
    return RilpSend(hRil, RilCmdSetMsgInUiccStatus, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetMsgMemoryStatus(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdSetMsgMemoryStatus, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SendMsg(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2,
    _In_reads_bytes_(2652) const VOID *Arg3,
    _In_ DWORD Arg4)
{
    BYTE Block[2680];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    CopyMemory(&Block[24], Arg3, 2652);
    *(PDWORD)&Block[2676] = Arg4;
    return RilpSend(hRil, RilCmdSendMsg, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SendMsgAck_V1(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2,
    _In_ DWORD Arg3,
    _In_ DWORD Arg4)
{
    BYTE Block[36];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = 20;
    *(PDWORD)&Block[20] = Arg1;
    *(PDWORD)&Block[24] = Arg2;
    *(PDWORD)&Block[28] = Arg3;
    *(PDWORD)&Block[32] = Arg4;
    return RilpSend(hRil, RilCmdSendMsgAck, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SendMsgAck(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2,
    _In_ DWORD Arg3,
    _In_ DWORD Arg4,
    _In_ DWORD Arg5,
    _In_ DWORD Arg6)
{
    BYTE Block[44];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = 28;
    *(PDWORD)&Block[20] = Arg1;
    *(PDWORD)&Block[24] = Arg2;
    *(PDWORD)&Block[28] = Arg3;
    *(PDWORD)&Block[32] = Arg4;
    *(PDWORD)&Block[36] = Arg5;
    *(PDWORD)&Block[40] = Arg6;
    return RilpSend(hRil, RilCmdSendMsgAck, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetSMSC(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetSmsc, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetSMSC(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_reads_bytes_(528) const VOID *Arg2)
{
    BYTE Block[548];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    CopyMemory(&Block[20], Arg2, 528);
    return RilpSend(hRil, RilCmdSetSmsc, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetIMSStatus(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetImsStatus, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_Dial_V1(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_reads_bytes_(528) const VOID *Arg2,
    _In_ DWORD Arg3)
{
    BYTE Block[552];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    CopyMemory(&Block[20], Arg2, 528);
    *(PDWORD)&Block[548] = Arg3;
    return RilpSend(hRil, RilCmdDial, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_ManageCalls_V1(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2,
    _In_ DWORD Arg3)
{
    BYTE Block[28];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    *(PDWORD)&Block[24] = Arg3;
    return RilpSend(hRil, RilCmdManageCalls, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_EmergencyModeControl(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdEmergencyModeControl, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetCallForwardingSettings(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2,
    _In_ DWORD Arg3,
    _In_ DWORD Arg4)
{
    BYTE Block[32];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    *(PDWORD)&Block[24] = Arg3;
    *(PDWORD)&Block[28] = Arg4;
    return RilpSend(hRil, RilCmdGetCallForwardingSettings, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetCallForwardingStatus(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2,
    _In_ DWORD Arg3,
    _In_ DWORD Arg4,
    _In_ DWORD Arg5)
{
    BYTE Block[36];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    *(PDWORD)&Block[24] = Arg3;
    *(PDWORD)&Block[28] = Arg4;
    *(PDWORD)&Block[32] = Arg5;
    return RilpSend(hRil, RilCmdSetCallForwardingStatus, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_AddCallForwarding(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2,
    _In_reads_bytes_(1072) const VOID *Arg3)
{
    BYTE Block[1096];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    CopyMemory(&Block[24], Arg3, 1072);
    return RilpSend(hRil, RilCmdAddCallForwarding, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_RemoveCallForwarding(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2,
    _In_ DWORD Arg3)
{
    BYTE Block[28];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    *(PDWORD)&Block[24] = Arg3;
    return RilpSend(hRil, RilCmdRemoveCallForwarding, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetCallBarringStatus(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2,
    _In_ DWORD Arg3,
    _In_ DWORD Arg4)
{
    BYTE Block[32];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    *(PDWORD)&Block[24] = Arg3;
    *(PDWORD)&Block[28] = Arg4;
    return RilpSend(hRil, RilCmdGetCallBarringStatus, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetCallWaitingSettings(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2,
    _In_ DWORD Arg3)
{
    BYTE Block[28];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    *(PDWORD)&Block[24] = Arg3;
    return RilpSend(hRil, RilCmdGetCallWaitingSettings, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetCallWaitingStatus(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2,
    _In_ DWORD Arg3,
    _In_ DWORD Arg4)
{
    BYTE Block[32];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    *(PDWORD)&Block[24] = Arg3;
    *(PDWORD)&Block[28] = Arg4;
    return RilpSend(hRil, RilCmdSetCallWaitingStatus, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetCallerIdSettings(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetCallerIdSettings, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetDialedIdSettings(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetDialedIdSettings, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetHideConnectedIdSettings(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetHideConnectedIdSettings, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetHideIdSettings(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetHideIdSettings, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_CancelSupServiceDataSession(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdCancelSupServiceDataSession, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_StartDTMF(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ CHAR Digit)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    Block[20] = (BYTE)Digit;
    return RilpSend(hRil, RilCmdStartDtmf, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_StopDTMF(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdStopDtmf, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetExecutorFocus(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context)
{
    return RilpSendHeaderOnly(hRil, RilCmdGetExecutorFocus, Context);
}

HRESULT
WINAPI
RIL_GetEmergencyMode(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetEmergencyMode, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetExecutorRFState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetExecutorRfState, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetExecutorRFState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdSetExecutorRfState, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_EnumerateSlots(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context)
{
    return RilpSendHeaderOnly(hRil, RilCmdEnumerateSlots, Context);
}

HRESULT
WINAPI
RIL_GetCardInfo(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetCardInfo, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetSlotPower(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdSetSlotPower, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetUiccPRLID(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetUiccPrlId, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetIMSI(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetImsi, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetSubscriberNumbers(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetSubscriberNumbers, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetUiccATR(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetUiccAtr, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_CloseUiccLogicalChannel(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdCloseUiccLogicalChannel, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_CloseUiccLogicalChannelGroup(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdCloseUiccLogicalChannelGroup, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetTerminalCapability(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_reads_bytes_(260) const VOID *Arg1)
{
    BYTE Block[276];

    ZeroMemory(Block, sizeof(Block));
    CopyMemory(&Block[16], Arg1, 260);
    return RilpSend(hRil, RilCmdSetTerminalCapability, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetTerminalCapability(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context)
{
    return RilpSendHeaderOnly(hRil, RilCmdGetTerminalCapability, Context);
}

HRESULT
WINAPI
RIL_VerifyUiccLock(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_reads_bytes_(264) const VOID *Arg1)
{
    BYTE Block[280];

    ZeroMemory(Block, sizeof(Block));
    CopyMemory(&Block[16], Arg1, 264);
    return RilpSend(hRil, RilCmdVerifyUiccLock, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetUiccLockEnabled(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_reads_bytes_(264) const VOID *Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[284];

    ZeroMemory(Block, sizeof(Block));
    CopyMemory(&Block[16], Arg1, 264);
    *(PDWORD)&Block[280] = Arg2;
    return RilpSend(hRil, RilCmdSetUiccLockEnabled, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetUiccAppPersoCheckState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetUiccAppPersoCheckState, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetPersoDeactivationState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetPersoDeactivationState, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_ReadPhonebookEntries(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2,
    _In_ DWORD Arg3,
    _In_ DWORD Arg4)
{
    BYTE Block[32];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    *(PDWORD)&Block[24] = Arg3;
    *(PDWORD)&Block[28] = Arg4;
    return RilpSend(hRil, RilCmdReadPhonebookEntries, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_DeletePhonebookEntry(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2,
    _In_ DWORD Arg3,
    _In_reads_bytes_(264) const VOID *Arg4)
{
    BYTE Block[296];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    *(PDWORD)&Block[24] = Arg3;
    *(PDWORD)&Block[28] = 0;
    *(PDWORD)&Block[28] = 1;
    CopyMemory(&Block[32], Arg4, 264);
    return RilpSend(hRil, RilCmdDeletePhonebookEntry, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetPhonebookOptions(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdGetPhonebookOptions, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetAllAdditionalNumberStrings(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdGetAllAdditionalNumberStrings, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_WriteAdditionalNumberString(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_reads_bytes_(520) const VOID *Arg2)
{
    BYTE Block[540];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    CopyMemory(&Block[20], Arg2, 520);
    return RilpSend(hRil, RilCmdWriteAdditionalNumberString, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_DeleteAdditionalNumberString(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdDeleteAdditionalNumberString, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetAllEmergencyNumbers(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context)
{
    return RilpSendHeaderOnly(hRil, RilCmdGetAllEmergencyNumbers, Context);
}

HRESULT
WINAPI
RIL_RegisterUiccToolkitService(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdRegisterUiccToolkitService, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetUiccToolkitProfile(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context)
{
    return RilpSendHeaderOnly(hRil, RilCmdGetUiccToolkitProfile, Context);
}

HRESULT
WINAPI
RIL_GetUiccServiceState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1,
    _In_ DWORD Arg2)
{
    BYTE Block[24];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    *(PDWORD)&Block[20] = Arg2;
    return RilpSend(hRil, RilCmdGetUiccServiceState, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_EnableModemFilters(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdEnableModemFilters, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_DisableModemFilters(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Arg1)
{
    BYTE Block[20];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Arg1;
    return RilpSend(hRil, RilCmdDisableModemFilters, Context, Block, sizeof(Block));
}
