/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Device, subscriber and network queries and the data profile
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "phonesvc.h"
#include <telephony/rilapi.h>
#include <telephony/rilinfo.h>

#define NDEBUG
#include <debug.h>

#define PH_DATA_KEY L"SYSTEM\\CurrentControlSet\\Services\\PhoneSvc\\Data"

static
VOID
PhWiden(
    _Out_writes_z_(Chars) PWSTR Out,
    _In_ DWORD Chars,
    _In_ PCSTR In)
{
    DWORD i;

    for (i = 0; i + 1 < Chars && In[i]; i++)
        Out[i] = (WCHAR)(UCHAR)In[i];
    Out[i] = UNICODE_NULL;
}

static
VOID
PhNarrow(
    _Out_writes_z_(Bytes) PSTR Out,
    _In_ DWORD Bytes,
    _In_ PCWSTR In)
{
    DWORD i;

    for (i = 0; i + 1 < Bytes && In[i]; i++)
        Out[i] = (CHAR)In[i];
    Out[i] = '\0';
}

/* Runs one informational RIL query and copies the raw record the library returns */
static
HRESULT
PhQuery(
    _In_ HRESULT SendStatus,
    _In_ PPH_REQUEST Request,
    _Out_writes_bytes_(Size) PVOID Record,
    _In_ DWORD Size)
{
    PH_RIL_RESULT Result;
    HRESULT hr;

    hr = PhFinishRequest(Request, SendStatus, &Result);
    if (FAILED(hr))
        return hr;

    if (Result.DataSize < Size)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

    CopyMemory(Record, Result.Data, Size);
    return S_OK;
}

/* Reads one device information class, the library answers each as a UTF-16 string */
static
HRESULT
PhGetDeviceString(
    _In_ DWORD Class,
    _Out_writes_z_(Chars) PWSTR Out,
    _In_ DWORD Chars)
{
    PH_RIL_RESULT Result;
    PPH_REQUEST Request;
    DWORD Copy;
    HRESULT hr;

    Out[0] = UNICODE_NULL;

    Request = PhBeginRequest();
    if (!Request)
        return E_OUTOFMEMORY;

    hr = PhFinishRequest(Request, RIL_GetDeviceInfo(PhRil, Request->Id, PHONE_EXECUTOR, Class), &Result);
    if (FAILED(hr))
    {
        DPRINT1("Device information class %lu failed 0x%lx\n", Class, hr);
        return hr;
    }

    Copy = min(Result.DataSize / sizeof(WCHAR), Chars - 1);
    CopyMemory(Out, Result.Data, Copy * sizeof(WCHAR));
    Out[Copy] = UNICODE_NULL;
    return S_OK;
}

HRESULT
PhGetDeviceInfo(
    _Out_ PPHONEOM_DEVICE_INFO Info)
{
    HRESULT hr;

    ZeroMemory(Info, sizeof(*Info));
    if (!PhRil)
        return RIL_E_RADIONOTAVAILABLE;

    /* The IMEI is the identity that matters, the rest is best effort */
    hr = PhGetDeviceString(RIL_DEVICEINFO_SERIALNUMBER, Info->Imei, _countof(Info->Imei));
    if (FAILED(hr))
        return hr;

    PhGetDeviceString(RIL_DEVICEINFO_MANUFACTURER, Info->Manufacturer, _countof(Info->Manufacturer));
    PhGetDeviceString(RIL_DEVICEINFO_MODEL, Info->Model, _countof(Info->Model));
    PhGetDeviceString(RIL_DEVICEINFO_REVISION, Info->Revision, _countof(Info->Revision));
    return S_OK;
}

HRESULT
PhGetSubscriber(
    _Out_ PPHONEOM_SUBSCRIBER Subscriber)
{
    RIL_SUBSCRIBER_INFO Record;
    PPH_REQUEST Request;
    HRESULT hr;

    ZeroMemory(Subscriber, sizeof(*Subscriber));
    if (!PhRil)
        return RIL_E_RADIONOTAVAILABLE;

    Request = PhBeginRequest();
    if (!Request)
        return E_OUTOFMEMORY;

    hr = PhQuery(RIL_GetCardInfo(PhRil, Request->Id, 0), Request, &Record, sizeof(Record));
    if (FAILED(hr))
        return hr;

    DPRINT1("SIM state %lu\n", Record.SimState);

    Subscriber->SimState = Record.SimState;
    PhWiden(Subscriber->Imsi, _countof(Subscriber->Imsi), Record.Imsi);
    PhWiden(Subscriber->Number, _countof(Subscriber->Number), Record.Msisdn);
    return S_OK;
}

HRESULT
PhGetNetwork(
    _Out_ PPHONEOM_NETWORK Network)
{
    RIL_NETWORK_INFO Record;
    PPH_REQUEST Request;
    HRESULT hr;

    ZeroMemory(Network, sizeof(*Network));
    if (!PhRil)
        return RIL_E_RADIONOTAVAILABLE;

    Request = PhBeginRequest();
    if (!Request)
        return E_OUTOFMEMORY;

    hr = PhQuery(RIL_GetCurrentRegStatus(PhRil, Request->Id, 0), Request, &Record, FIELD_OFFSET(RIL_NETWORK_INFO, Limited));
    if (FAILED(hr))
        return hr;

    Network->RegState = Record.RegState;
    Network->Rat = Record.Rat;
    Network->SignalDbm = Record.SignalDbm;
    Network->SignalBars = Record.SignalBars;
    Network->Mcc = Record.Mcc;
    Network->Mnc = Record.Mnc;
    Network->Roaming = Record.Roaming;
    PhWiden(Network->Operator, _countof(Network->Operator), Record.Operator);
    return S_OK;
}

VOID
PhGetDataConfig(
    _Out_ PPHONEOM_DATA_CONFIG Config)
{
    HKEY Key;
    DWORD Size, Type, Value;

    ZeroMemory(Config, sizeof(*Config));

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, PH_DATA_KEY, 0, KEY_READ, &Key) != ERROR_SUCCESS)
        return;

    Size = sizeof(Value);
    if (RegQueryValueExW(Key, L"Enabled", NULL, &Type, (PBYTE)&Value, &Size) == ERROR_SUCCESS && Type == REG_DWORD)
        Config->Enabled = Value;

    Size = sizeof(Value);
    if (RegQueryValueExW(Key, L"AuthType", NULL, &Type, (PBYTE)&Value, &Size) == ERROR_SUCCESS && Type == REG_DWORD)
        Config->AuthType = Value;

    Size = sizeof(Config->Apn);
    RegQueryValueExW(Key, L"Apn", NULL, &Type, (PBYTE)Config->Apn, &Size);
    Size = sizeof(Config->Username);
    RegQueryValueExW(Key, L"Username", NULL, &Type, (PBYTE)Config->Username, &Size);
    Size = sizeof(Config->Password);
    RegQueryValueExW(Key, L"Password", NULL, &Type, (PBYTE)Config->Password, &Size);

    Config->Apn[_countof(Config->Apn) - 1] = UNICODE_NULL;
    Config->Username[_countof(Config->Username) - 1] = UNICODE_NULL;
    Config->Password[_countof(Config->Password) - 1] = UNICODE_NULL;
    RegCloseKey(Key);
}

HRESULT
PhSetDataConfig(
    _In_ const PHONEOM_DATA_CONFIG *Config)
{
    HKEY Key;
    DWORD Value;
    LSTATUS Status;

    Status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, PH_DATA_KEY, 0, NULL, 0, KEY_WRITE, NULL, &Key, NULL);
    if (Status != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Status);

    Value = Config->Enabled;
    RegSetValueExW(Key, L"Enabled", 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
    Value = Config->AuthType;
    RegSetValueExW(Key, L"AuthType", 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
    RegSetValueExW(Key, L"Apn", 0, REG_SZ, (const BYTE *)Config->Apn,
                   (DWORD)((wcslen(Config->Apn) + 1) * sizeof(WCHAR)));
    RegSetValueExW(Key, L"Username", 0, REG_SZ, (const BYTE *)Config->Username,
                   (DWORD)((wcslen(Config->Username) + 1) * sizeof(WCHAR)));
    RegSetValueExW(Key, L"Password", 0, REG_SZ, (const BYTE *)Config->Password,
                   (DWORD)((wcslen(Config->Password) + 1) * sizeof(WCHAR)));

    RegCloseKey(Key);
    return S_OK;
}

HRESULT
PhConnectData(
    _In_ BOOL Connect,
    _Out_ PPHONEOM_DATA_STATUS Status)
{
    PHONEOM_DATA_CONFIG Config;
    RIL_DATA_REQUEST Request;
    RIL_DATA_STATUS Record;
    PPH_REQUEST RilRequest;
    HRESULT hr;

    ZeroMemory(Status, sizeof(*Status));
    if (!PhRil)
        return RIL_E_RADIONOTAVAILABLE;

    PhGetDataConfig(&Config);

    ZeroMemory(&Request, sizeof(Request));
    Request.Size = sizeof(Request);
    Request.Op = Connect ? RIL_DATA_OP_CONNECT : RIL_DATA_OP_DISCONNECT;
    Request.AuthType = Config.AuthType;
    PhNarrow(Request.Apn, sizeof(Request.Apn), Config.Apn);
    PhNarrow(Request.Username, sizeof(Request.Username), Config.Username);
    PhNarrow(Request.Password, sizeof(Request.Password), Config.Password);

    RilRequest = PhBeginRequest();
    if (!RilRequest)
        return E_OUTOFMEMORY;

    hr = PhQuery(RIL_DevSpecific(PhRil, RilRequest->Id, (const BYTE *)&Request, sizeof(Request)),
                 RilRequest, &Record, sizeof(Record));
    if (FAILED(hr))
        return hr;

    Status->Connected = Record.Connected;
    return S_OK;
}
