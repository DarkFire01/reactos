/*
 * PROJECT:     ReactOS RIL Proxy
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client lifetime and variable or structured RIL requests
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "rilproxy.h"
#include <strsafe.h>

#define RIL_LOCK_CREDENTIAL_SIZE    264
#define RIL_OPERATOR_ENTRY_SIZE     156
#define RIL_RADIOSTATE_ENTRY_SIZE   652
#define RIL_FILEWATCH_ENTRY_SIZE    24
#define RIL_PHONEBOOK_MIN_SIZE      0x660
#define RIL_APDU_MAX_SIZE           261

#define RIL_ADAPTATION_PARAMETERS   L"SYSTEM\\CurrentControlSet\\Services\\RILAdaptation\\Parameters"
#define RIL_CELLULAR_DISABLE        L"SYSTEM\\CurrentControlSet\\Services\\Wwansvc\\Parameters\\DisableCellular"

static
BOOL
RilpBlockSize(
    _In_ ULONGLONG Fixed,
    _In_ ULONGLONG Count,
    _In_ ULONGLONG Unit,
    _Out_ PDWORD Size)
{
    ULONGLONG Variable = Count * Unit;

    if (Variable > MAXDWORD || Fixed + Variable > MAXDWORD)
        return FALSE;

    *Size = (DWORD)(Fixed + Variable);
    return TRUE;
}

static
PBYTE
RilpAllocBlock(
    _In_ DWORD Size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size);
}

static
HRESULT
RilpSendBlockAndFree(
    _In_ HRIL hRil,
    _In_ DWORD Command,
    _In_ ULONGLONG Context,
    _In_ _Post_invalid_ PBYTE Block,
    _In_ DWORD Size)
{
    HRESULT hr = RilpSend(hRil, Command, Context, Block, Size);

    HeapFree(GetProcessHeap(), 0, Block);
    return hr;
}

HRESULT
WINAPI
RIL_Initialize(
    _In_ DWORD Index,
    _In_opt_ PRIL_RESULT_CALLBACK ResultCallback,
    _In_opt_ PRIL_NOTIFY_CALLBACK NotifyCallback,
    _In_reads_opt_(ClassCount) const DWORD *NotificationClasses,
    _In_ DWORD ClassCount,
    _In_opt_ PVOID Param,
    _In_opt_ LPCWSTR ClientName,
    _Out_ HRIL *phRil)
{
    /* Classes and name are accepted for compatibility, clients enable notifications explicitly */
    UNREFERENCED_PARAMETER(NotificationClasses);
    UNREFERENCED_PARAMETER(ClassCount);
    UNREFERENCED_PARAMETER(ClientName);

    return RilpCreateClient(Index, ResultCallback, NotifyCallback, Param, (PRIL_CLIENT *)phRil);
}

HRESULT
WINAPI
RIL_Deinitialize(
    _In_ HRIL hRil)
{
    RilpDestroyClient(hRil);
    return S_OK;
}

HRESULT
WINAPI
RIL_GetNumberOfModems(
    _Out_ PDWORD Count)
{
    DWORD Instances = 0, Disabled = 0, Size;
    LSTATUS Status;

    if (!Count)
        return E_POINTER;

    Size = sizeof(Instances);
    Status = RegGetValueW(HKEY_LOCAL_MACHINE,
                          RIL_ADAPTATION_PARAMETERS,
                          L"MSRilInstances",
                          RRF_RT_DWORD,
                          NULL,
                          &Instances,
                          &Size);
    if (Status != ERROR_SUCCESS)
        Instances = 0;

    Size = sizeof(Disabled);
    RegGetValueW(HKEY_LOCAL_MACHINE,
                 RIL_CELLULAR_DISABLE,
                 L"MSSoftwareModemDisable",
                 RRF_RT_DWORD,
                 NULL,
                 &Disabled,
                 &Size);

    *Count = (Disabled == 1) ? 0 : Instances;
    return HRESULT_FROM_WIN32(Status);
}

HRESULT
WINAPI
RIL_EnableNotifications(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_reads_(Count) const DWORD *Classes,
    _In_ DWORD Count)
{
    DWORD Prefix = Count;

    if (Count && !Classes)
        return E_INVALIDARG;
    if ((ULONGLONG)Count * sizeof(DWORD) + 20 > MAXDWORD)
        return E_INVALIDARG;

    return RilpSendVariable(hRil, RilCmdEnableNotifications, Context,
                            &Prefix, sizeof(Prefix),
                            Classes, Count * sizeof(DWORD));
}

HRESULT
WINAPI
RIL_DisableNotifications(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_reads_(Count) const DWORD *Classes,
    _In_ DWORD Count)
{
    UNREFERENCED_PARAMETER(hRil);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Classes);
    UNREFERENCED_PARAMETER(Count);

    return E_NOTIMPL;
}

HRESULT
WINAPI
RIL_GetEquipmentState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context)
{
    return RilpSendHeaderOnly(hRil, RilCmdGetEquipmentState, Context);
}

HRESULT
WINAPI
RIL_StartModemLogs(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context)
{
    return RilpSendHeaderOnly(hRil, RilCmdStartModemLogs, Context);
}

HRESULT
WINAPI
RIL_StopModemLogs(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context)
{
    return RilpSendHeaderOnly(hRil, RilCmdStopModemLogs, Context);
}

HRESULT
WINAPI
RIL_DrainModemLogs(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context)
{
    return RilpSendHeaderOnly(hRil, RilCmdDrainModemLogs, Context);
}

HRESULT
WINAPI
RIL_SetDMProfileConfigInfo(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Profile,
    _In_reads_bytes_(sizeof(RILADDRESS)) const VOID *Info)
{
    BYTE Block[552];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Executor;
    *(PDWORD)&Block[20] = Profile;
    CopyMemory(&Block[24], Info, sizeof(RILADDRESS));
    return RilpSend(hRil, RilCmdSetDmProfileConfigInfo, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetGeolocationData(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_reads_bytes_(1400) const VOID *Location)
{
    BYTE Block[1416];

    ZeroMemory(Block, sizeof(Block));
    CopyMemory(&Block[16], Location, 1400);
    return RilpSend(hRil, RilCmdSetGeolocationData, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetExecutorConfig(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_reads_bytes_(20) const VOID *Config)
{
    BYTE Block[40];

    if (!Config)
        return E_INVALIDARG;

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Executor;
    CopyMemory(&Block[20], Config, 20);
    return RilpSend(hRil, RilCmdSetExecutorConfig, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetSystemSelectionPrefs_V1(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_reads_bytes_(180) const VOID *Prefs)
{
    BYTE Block[200];

    if (!Prefs)
        return E_INVALIDARG;

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Executor;
    CopyMemory(&Block[20], Prefs, 180);
    return RilpSend(hRil, RilCmdSetSystemSelectionPrefs, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetSystemSelectionPrefs(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_reads_bytes_(248) const VOID *Prefs)
{
    BYTE Block[268];

    if (!Prefs)
        return E_INVALIDARG;

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Executor;
    CopyMemory(&Block[20], Prefs, 248);
    return RilpSend(hRil, RilCmdSetSystemSelectionPrefs, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetPreferredOperatorList(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Count,
    _In_reads_bytes_(Count * RIL_OPERATOR_ENTRY_SIZE) const VOID *Operators)
{
    DWORD Size;
    PBYTE Block;

    if (!RilpBlockSize(24, Count, RIL_OPERATOR_ENTRY_SIZE, &Size))
        return E_INVALIDARG;

    Block = RilpAllocBlock(Size);
    if (!Block)
        return E_OUTOFMEMORY;

    *(PDWORD)&Block[16] = Executor;
    *(PDWORD)&Block[20] = Count;
    if (Count && Operators)
        CopyMemory(&Block[24], Operators, Size - 24);

    return RilpSendBlockAndFree(hRil, RilCmdSetPreferredOperatorList, Context, Block, Size);
}

HRESULT
WINAPI
RIL_SetPSMediaConfiguration(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ const VOID *Config)
{
    const DWORD *Header = Config;

    if (!Config)
        return E_INVALIDARG;

    /* Layout is {cbSize, ?, count, count * 8 bytes} */
    if ((ULONGLONG)Header[2] * 8 + 12 != Header[0])
        return E_INVALIDARG;

    return RilpSendVariable(hRil, RilCmdSetPsMediaConfiguration, Context, NULL, 0, Config, Header[0]);
}

HRESULT
WINAPI
RIL_SetRadioStateDetails(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ const VOID *Details)
{
    const DWORD *Header = Details;
    DWORD Size;
    PBYTE Block;

    if (!Details || !RilpBlockSize(24, Header[1], RIL_RADIOSTATE_ENTRY_SIZE, &Size))
        return E_INVALIDARG;

    Block = RilpAllocBlock(Size);
    if (!Block)
        return E_OUTOFMEMORY;

    *(PDWORD)&Block[16] = Header[0];
    *(PDWORD)&Block[20] = Header[1];
    CopyMemory(&Block[24], &Header[2], Size - 24);

    return RilpSendBlockAndFree(hRil, RilCmdSetRadioStateDetails, Context, Block, Size);
}

HRESULT
WINAPI
RIL_DevSpecific(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size)
{
    if (!Data || Size > MAXDWORD - sizeof(RIL_COMMAND_RECORD))
        return E_INVALIDARG;

    return RilpSendVariable(hRil, RilCmdDevSpecific, Context, NULL, 0, Data, Size);
}

HRESULT
WINAPI
RIL_SetRFState_V1(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD State)
{
    BYTE Block[20];

    *(PDWORD)&Block[16] = State;
    return RilpSend(hRil, RilCmdSetRfState, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetRFState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ const VOID *State)
{
    const BYTE *Source = State;
    DWORD Flags, DataLength, Length, Size;
    PBYTE Block;

    if (!State)
        return E_INVALIDARG;

    Flags = *(const DWORD *)&Source[4];
    DataLength = *(const DWORD *)&Source[12];
    Length = (Flags & 2) ? DataLength : 0;
    if (!RilpBlockSize(32, Length, 1, &Size))
        return E_INVALIDARG;

    Block = RilpAllocBlock(Size);
    if (!Block)
        return E_OUTOFMEMORY;

    *(PDWORD)&Block[16] = 16 + Length;
    *(PDWORD)&Block[20] = Flags;
    *(PDWORD)&Block[24] = *(const DWORD *)&Source[8];
    *(PDWORD)&Block[28] = DataLength;
    if (Length)
        CopyMemory(&Block[32], &Source[16], Length);

    return RilpSendBlockAndFree(hRil, RilCmdSetRfState, Context, Block, Size);
}

HRESULT
WINAPI
RIL_RadioStatePasswordCompare(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ const VOID *Password)
{
    BYTE Block[84];
    HRESULT hr;

    if (!Password)
        return E_INVALIDARG;

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = *(const DWORD *)Password;
    hr = StringCchCopyW((PWSTR)&Block[20], 32, (PCWSTR)((const BYTE *)Password + 4));
    if (FAILED(hr))
        return hr;

    return RilpSend(hRil, RilCmdRadioStatePasswordCompare, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_Dial(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ const RILADDRESS *Address,
    _In_ DWORD Options,
    _In_ DWORD Domain,
    _In_opt_ const RIL_MEDIA_SET *Media)
{
    BYTE Block[1244];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Executor;
    CopyMemory(&Block[20], Address, sizeof(*Address));
    *(PDWORD)&Block[548] = Options;
    *(PDWORD)&Block[552] = Domain;
    if (Media)
    {
        *(PDWORD)&Block[556] = TRUE;
        CopyMemory(&Block[560], Media, sizeof(*Media));
    }

    return RilpSend(hRil, RilCmdDial, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_ManageCalls_V2(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Action,
    _In_ DWORD CallId,
    _In_opt_ const RIL_MEDIA_SET *Media)
{
    BYTE Block[716];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Executor;
    *(PDWORD)&Block[20] = Action;
    *(PDWORD)&Block[24] = CallId;
    if (Media)
    {
        *(PDWORD)&Block[28] = TRUE;
        CopyMemory(&Block[32], Media, sizeof(*Media));
    }

    return RilpSend(hRil, RilCmdManageCalls, Context, Block, sizeof(Block));
}

static
HRESULT
RilpManageCallsExtended(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Action,
    _In_ DWORD CallId,
    _In_opt_ const RIL_MEDIA_SET *Media,
    _In_opt_ const RILADDRESS *Address,
    _In_opt_ const VOID *Extra,
    _In_ DWORD Size)
{
    BYTE Block[1272];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Executor;
    *(PDWORD)&Block[20] = Action;
    *(PDWORD)&Block[24] = CallId;
    if (Media)
    {
        *(PDWORD)&Block[28] = TRUE;
        CopyMemory(&Block[32], Media, sizeof(*Media));
    }
    if (Address)
        CopyMemory(&Block[716], Address, sizeof(*Address));
    if (Extra)
        CopyMemory(&Block[1244], Extra, 28);

    return RilpSend(hRil, RilCmdManageCalls, Context, Block, Size);
}

HRESULT
WINAPI
RIL_ManageCalls_V3(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Action,
    _In_ DWORD CallId,
    _In_opt_ const RIL_MEDIA_SET *Media,
    _In_opt_ const RILADDRESS *Address)
{
    return RilpManageCallsExtended(hRil, Context, Executor, Action, CallId, Media, Address, NULL, 1244);
}

HRESULT
WINAPI
RIL_ManageCalls(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Action,
    _In_ DWORD CallId,
    _In_opt_ const RIL_MEDIA_SET *Media,
    _In_opt_ const RILADDRESS *Address,
    _In_reads_bytes_opt_(28) const VOID *Extra)
{
    return RilpManageCallsExtended(hRil, Context, Executor, Action, CallId, Media, Address, Extra, 1272);
}

HRESULT
WINAPI
RIL_SendFlash(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_opt_ const RILADDRESS *Address)
{
    BYTE Block[548];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Executor;
    if (Address)
        CopyMemory(&Block[20], Address, sizeof(*Address));

    return RilpSend(hRil, RilCmdSendFlash, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetCallBarringStatus(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Type,
    _In_ DWORD Status,
    _In_ DWORD InfoClasses,
    _In_opt_ LPCSTR Password,
    _In_ DWORD ServiceClass)
{
    BYTE Block[296];
    HRESULT hr;

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Executor;
    *(PDWORD)&Block[20] = Type;
    *(PDWORD)&Block[24] = Status;
    *(PDWORD)&Block[28] = InfoClasses;
    if (Password)
    {
        *(PDWORD)&Block[32] = TRUE;
        hr = StringCchCopyA((PSTR)&Block[36], 256, Password);
        if (FAILED(hr))
            return hr;
    }
    *(PDWORD)&Block[292] = ServiceClass;

    return RilpSend(hRil, RilCmdSetCallBarringStatus, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_ChangeCallBarringPassword(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Type,
    _In_ LPCSTR OldPassword,
    _In_ LPCSTR NewPassword,
    _In_ LPCSTR ConfirmPassword)
{
    BYTE Block[792];
    HRESULT hr;

    if (!OldPassword || !NewPassword || !ConfirmPassword)
        return E_INVALIDARG;

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Executor;
    *(PDWORD)&Block[20] = Type;

    hr = StringCchCopyA((PSTR)&Block[24], 256, OldPassword);
    if (SUCCEEDED(hr))
        hr = StringCchCopyA((PSTR)&Block[280], 256, NewPassword);
    if (SUCCEEDED(hr))
        hr = StringCchCopyA((PSTR)&Block[536], 256, ConfirmPassword);
    if (FAILED(hr))
        return hr;

    return RilpSend(hRil, RilCmdChangeCallBarringPassword, Context, Block, sizeof(Block));
}

static
HRESULT
RilpSendSupServiceString(
    _In_ HRIL hRil,
    _In_ DWORD Command,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ LPCWSTR Data)
{
    SIZE_T Chars;
    DWORD Size;
    PBYTE Block;
    HRESULT hr;

    if (!Data)
        return E_INVALIDARG;

    Chars = wcslen(Data) + 1;
    if (!RilpBlockSize(24, Chars, sizeof(WCHAR), &Size))
        return E_INVALIDARG;

    Block = RilpAllocBlock(Size);
    if (!Block)
        return E_OUTOFMEMORY;

    *(PDWORD)&Block[16] = Executor;
    *(PDWORD)&Block[20] = (DWORD)Chars;
    hr = StringCchCopyW((PWSTR)&Block[24], Chars, Data);
    if (FAILED(hr))
    {
        HeapFree(GetProcessHeap(), 0, Block);
        return hr;
    }

    return RilpSendBlockAndFree(hRil, Command, Context, Block, Size);
}

HRESULT
WINAPI
RIL_SendSupServiceData(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ LPCWSTR Data)
{
    return RilpSendSupServiceString(hRil, RilCmdSendSupServiceData, Context, Executor, Data);
}

HRESULT
WINAPI
RIL_SendSupServiceDataResponse(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ LPCWSTR Data)
{
    return RilpSendSupServiceString(hRil, RilCmdSendSupServiceDataResponse, Context, Executor, Data);
}

HRESULT
WINAPI
RIL_SendRTT(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD CallId,
    _In_ LPCWSTR Text)
{
    BYTE Block[284];
    HRESULT hr;

    if (!Text)
        return E_INVALIDARG;

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[20] = Executor;
    *(PDWORD)&Block[24] = CallId;
    hr = StringCchCopyW((PWSTR)&Block[28], 128, Text);
    if (FAILED(hr))
        return hr;

    return RilpSend(hRil, RilCmdSendRtt, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SendDTMF(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_opt_ LPCSTR Digits,
    _In_ DWORD OnMs,
    _In_ DWORD OffMs)
{
    BYTE Block[284];
    HRESULT hr;

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Executor;
    if (Digits)
    {
        hr = StringCchCopyA((PSTR)&Block[20], 256, Digits);
        if (FAILED(hr))
            return hr;
    }
    *(PDWORD)&Block[276] = OnMs;
    *(PDWORD)&Block[280] = OffMs;

    return RilpSend(hRil, RilCmdSendDtmf, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SetExecutorFocus(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_reads_(Count) const DWORD *Executors,
    _In_ DWORD Count)
{
    BYTE Block[28];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Count;
    if (Count && Executors && Count <= 2)
        CopyMemory(&Block[20], Executors, Count * sizeof(DWORD));

    return RilpSend(hRil, RilCmdSetExecutorFocus, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetUiccRecordStatus(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_reads_bytes_(24) const VOID *Record)
{
    BYTE Block[40];

    if (!Record)
        return E_INVALIDARG;

    CopyMemory(&Block[16], Record, 24);
    return RilpSend(hRil, RilCmdGetUiccRecordStatus, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_SendRestrictedUiccCmd(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Command,
    _In_reads_bytes_(44) const VOID *Parameters,
    _In_reads_bytes_opt_(DataSize) const BYTE *Data,
    _In_ DWORD DataSize,
    _In_reads_bytes_opt_(RIL_LOCK_CREDENTIAL_SIZE) const VOID *Credential)
{
    DWORD Size;
    PBYTE Block;

    if (!Parameters || !RilpBlockSize(340, DataSize, 1, &Size))
        return E_INVALIDARG;

    Block = RilpAllocBlock(Size);
    if (!Block)
        return E_OUTOFMEMORY;

    *(PDWORD)&Block[16] = Size - 16;
    *(PDWORD)&Block[20] = Command;
    CopyMemory(&Block[24], Parameters, 44);
    if (Credential)
    {
        *(PDWORD)&Block[68] = TRUE;
        CopyMemory(&Block[72], Credential, RIL_LOCK_CREDENTIAL_SIZE);
    }
    *(PDWORD)&Block[336] = DataSize;
    if (Data && DataSize)
        CopyMemory(&Block[340], Data, DataSize);

    return RilpSendBlockAndFree(hRil, RilCmdSendRestrictedUiccCmd, Context, Block, Size);
}

HRESULT
WINAPI
RIL_WatchUiccFileChange(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ const VOID *Files)
{
    const DWORD *Header = Files;
    DWORD Size;

    if (!Files || !RilpBlockSize(8, Header[1], RIL_FILEWATCH_ENTRY_SIZE, &Size) || Size != Header[0])
        return E_INVALIDARG;

    return RilpSendVariable(hRil, RilCmdWatchUiccFileChange, Context, NULL, 0, Files, Size);
}

HRESULT
WINAPI
RIL_OpenUiccLogicalChannel(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Application,
    _In_ DWORD AidLength,
    _In_reads_bytes_(AidLength) const BYTE *Aid,
    _In_ DWORD Flags)
{
    BYTE Block[64];

    if (!Aid)
        return E_INVALIDARG;

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Executor;
    *(PDWORD)&Block[20] = Application;
    *(PDWORD)&Block[24] = AidLength;
    if (AidLength <= 32)
        CopyMemory(&Block[28], Aid, AidLength);
    *(PDWORD)&Block[60] = Flags;

    return RilpSend(hRil, RilCmdOpenUiccLogicalChannel, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_ExchangeUiccAPDU(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Channel,
    _In_ DWORD ApduSize,
    _In_reads_bytes_(ApduSize) const BYTE *Apdu)
{
    DWORD Prefix[3];

    if (!Apdu || ApduSize == 0 || ApduSize > RIL_APDU_MAX_SIZE)
        return E_INVALIDARG;

    Prefix[0] = Executor;
    Prefix[1] = Channel;
    Prefix[2] = ApduSize;
    return RilpSendVariable(hRil, RilCmdExchangeUiccApdu, Context, Prefix, sizeof(Prefix), Apdu, ApduSize);
}

static
HRESULT
RilpSendLockQuery(
    _In_ HRIL hRil,
    _In_ DWORD Command,
    _In_ ULONGLONG Context,
    _In_reads_bytes_(8) const VOID *Lock)
{
    BYTE Block[24];

    if (!Lock)
        return E_INVALIDARG;

    CopyMemory(&Block[16], Lock, 8);
    return RilpSend(hRil, Command, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_GetUiccLockState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_reads_bytes_(8) const VOID *Lock)
{
    return RilpSendLockQuery(hRil, RilCmdGetUiccLockState, Context, Lock);
}

HRESULT
WINAPI
RIL_GetUiccServiceLock(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_reads_bytes_(8) const VOID *Lock)
{
    return RilpSendLockQuery(hRil, RilCmdGetUiccServiceLock, Context, Lock);
}

static
HRESULT
RilpSendLockPassword(
    _In_ HRIL hRil,
    _In_ DWORD Command,
    _In_ ULONGLONG Context,
    _In_reads_bytes_(RIL_LOCK_CREDENTIAL_SIZE) const VOID *Credential,
    _In_ LPCSTR Password)
{
    BYTE Block[536];

    if (!Credential || !Password)
        return E_INVALIDARG;

    ZeroMemory(Block, sizeof(Block));
    CopyMemory(&Block[16], Credential, RIL_LOCK_CREDENTIAL_SIZE);
    if (FAILED(StringCchCopyA((PSTR)&Block[280], 256, Password)))
        return E_POINTER;

    return RilpSend(hRil, Command, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_UnblockUiccLock(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_reads_bytes_(RIL_LOCK_CREDENTIAL_SIZE) const VOID *Credential,
    _In_ LPCSTR NewPassword)
{
    return RilpSendLockPassword(hRil, RilCmdUnblockUiccLock, Context, Credential, NewPassword);
}

HRESULT
WINAPI
RIL_ChangeUiccLockPassword(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_reads_bytes_(RIL_LOCK_CREDENTIAL_SIZE) const VOID *Credential,
    _In_ LPCSTR NewPassword)
{
    return RilpSendLockPassword(hRil, RilCmdChangeUiccLockPassword, Context, Credential, NewPassword);
}

HRESULT
WINAPI
RIL_DeactivatePerso(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Feature,
    _In_ LPCSTR Password)
{
    BYTE Block[276];

    if (!Password)
        return E_INVALIDARG;

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Feature;
    if (FAILED(StringCchCopyA((PSTR)&Block[20], 256, Password)))
        return E_POINTER;

    return RilpSend(hRil, RilCmdDeactivatePerso, Context, Block, sizeof(Block));
}

HRESULT
WINAPI
RIL_WritePhonebookEntry(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Storage,
    _In_ const VOID *Entry,
    _In_reads_bytes_opt_(RIL_LOCK_CREDENTIAL_SIZE) const VOID *Credential)
{
    DWORD EntrySize, Size;
    PBYTE Block;

    if (!Entry)
        return E_INVALIDARG;

    EntrySize = *(const DWORD *)Entry;
    if (EntrySize < RIL_PHONEBOOK_MIN_SIZE || !RilpBlockSize(292, EntrySize, 1, &Size))
        return E_INVALIDARG;

    Block = RilpAllocBlock(Size);
    if (!Block)
        return E_OUTOFMEMORY;

    *(PDWORD)&Block[16] = Executor;
    *(PDWORD)&Block[20] = Storage;
    if (Credential)
    {
        *(PDWORD)&Block[24] = TRUE;
        CopyMemory(&Block[28], Credential, RIL_LOCK_CREDENTIAL_SIZE);
    }
    CopyMemory(&Block[292], Entry, EntrySize);

    return RilpSendBlockAndFree(hRil, RilCmdWritePhonebookEntry, Context, Block, Size);
}

HRESULT
WINAPI
RIL_SetUiccToolkitProfile(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ const VOID *Profile)
{
    const DWORD *Header = Profile;
    DWORD Size;

    if (!Profile || !RilpBlockSize(8, Header[1], 1, &Size) || Size != Header[0])
        return E_INVALIDARG;

    return RilpSendVariable(hRil, RilCmdSetUiccToolkitProfile, Context, NULL, 0, Profile, Size);
}

static
HRESULT
RilpSendToolkitData(
    _In_ HRIL hRil,
    _In_ DWORD Command,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size)
{
    DWORD Prefix[2];

    if (!Data || Size > MAXDWORD - 24)
        return E_INVALIDARG;

    Prefix[0] = Executor;
    Prefix[1] = Size;
    return RilpSendVariable(hRil, Command, Context, Prefix, sizeof(Prefix), Data, Size);
}

HRESULT
WINAPI
RIL_SendUiccToolkitCmdResponse(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_reads_bytes_(Size) const BYTE *Response,
    _In_ DWORD Size)
{
    return RilpSendToolkitData(hRil, RilCmdSendUiccToolkitCmdResponse, Context, Executor, Response, Size);
}

HRESULT
WINAPI
RIL_SendUiccToolkitEnvelope(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_reads_bytes_(Size) const BYTE *Envelope,
    _In_ DWORD Size)
{
    return RilpSendToolkitData(hRil, RilCmdSendUiccToolkitEnvelope, Context, Executor, Envelope, Size);
}

HRESULT
WINAPI
RIL_SetUiccServiceState(
    _In_ HRIL hRil,
    _In_ ULONGLONG Context,
    _In_ DWORD Executor,
    _In_ DWORD Service,
    _In_reads_bytes_opt_(RIL_LOCK_CREDENTIAL_SIZE) const VOID *Credential,
    _In_ DWORD State)
{
    BYTE Block[296];

    ZeroMemory(Block, sizeof(Block));
    *(PDWORD)&Block[16] = Executor;
    *(PDWORD)&Block[20] = Service;
    if (Credential)
    {
        *(PDWORD)&Block[24] = TRUE;
        CopyMemory(&Block[28], Credential, RIL_LOCK_CREDENTIAL_SIZE);
    }
    *(PDWORD)&Block[292] = State;

    return RilpSend(hRil, RilCmdSetUiccServiceState, Context, Block, sizeof(Block));
}
