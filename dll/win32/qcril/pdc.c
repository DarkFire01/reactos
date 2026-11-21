/*
 * PROJECT:     ReactOS Qualcomm RIL
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Persistent device config, the carrier configuration the modem runs
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

/*
 * The modem runs one software configuration at a time, which decides features
 * such as IMS. Each SIM's home network maps to an MCFG file in the registry;
 * when that file is not the active configuration it is loaded, selected and
 * activated, and the modem restarts on it.
 */

#include "qcril.h"
#include <winreg.h>
#include <stdio.h>
#include <string.h>

#define QCRIL_PDC_GET_SELECTED          0x0022
#define QCRIL_PDC_SET_SELECTED          0x0023
#define QCRIL_PDC_LIST_CONFIGS          0x0024
#define QCRIL_PDC_LOAD_CONFIG           0x0026
#define QCRIL_PDC_ACTIVATE              0x0027
#define QCRIL_PDC_GET_INFO              0x0028

#define QCRIL_PDC_TLV_TYPE              0x01
#define QCRIL_PDC_TLV_RESULT            0x01
#define QCRIL_PDC_TLV_TOKEN             0x10
#define QCRIL_PDC_TLV_ACTIVE            0x11
#define QCRIL_PDC_TLV_PENDING           0x12
#define QCRIL_PDC_TLV_CONFIGS           0x11
#define QCRIL_PDC_TLV_LIST_TYPE         0x11
#define QCRIL_PDC_TLV_SIZE              0x11
#define QCRIL_PDC_TLV_DESCRIPTION       0x12
#define QCRIL_PDC_TLV_VERSION           0x13
#define QCRIL_PDC_TLV_REMAINING         0x12

#define QCRIL_PDC_SOFTWARE              1
#define QCRIL_PDC_CHUNK                 1024
#define QCRIL_PDC_TIMEOUT_MS            10000

#define QCRIL_CARRIER_KEY   L"SYSTEM\\CurrentControlSet\\Services\\RILAdaptation\\Parameters\\CarrierConfigs"

typedef struct _QC_PDC_ID
{
    UCHAR Length;
    UCHAR Bytes[QC_PDC_MAX_ID];
} QC_PDC_ID, *PQC_PDC_ID;

/* Renders a config id: printable ids as text, anything else as hex */
static
VOID
QcPdcIdText(
    _In_ const QC_PDC_ID *Id,
    _Out_writes_z_(OutSize) PSTR Out,
    _In_ ULONG OutSize)
{
    static const CHAR Digits[] = "0123456789abcdef";
    ULONG i, Written = 0;
    BOOL Printable = Id->Length != 0;

    for (i = 0; i < Id->Length; i++)
    {
        if (Id->Bytes[i] < 0x20 || Id->Bytes[i] >= 0x7F)
            Printable = FALSE;
    }

    for (i = 0; i < Id->Length && Written + 3 < OutSize; i++)
    {
        if (Printable)
        {
            Out[Written++] = (CHAR)Id->Bytes[i];
        }
        else
        {
            Out[Written++] = Digits[Id->Bytes[i] >> 4];
            Out[Written++] = Digits[Id->Bytes[i] & 0x0F];
        }
    }
    Out[Written] = '\0';
}

static
BOOL
QcPdcSameId(
    _In_ const QC_PDC_ID *A,
    _In_ const QC_PDC_ID *B)
{
    return A->Length == B->Length && memcmp(A->Bytes, B->Bytes, A->Length) == 0;
}

VOID
QcPdcIndication(
    _In_ PQC_MODEM Modem,
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    const VOID *Value;
    USHORT ValueLen;
    ULONG Token = 0;

    if (QmiFindTlv(Message, Length, QCRIL_PDC_TLV_TOKEN, &Value, &ValueLen) && ValueLen >= sizeof(Token))
        memcpy(&Token, Value, sizeof(Token));

    EnterCriticalSection(&Modem->StateLock);
    if (Modem->Pdc.WaitId == MessageId && Modem->Pdc.WaitToken == Token && Length <= sizeof(Modem->Pdc.Ind))
    {
        memcpy(Modem->Pdc.Ind, Message, Length);
        Modem->Pdc.IndLen = Length;
        Modem->Pdc.WaitId = 0;
        SetEvent(Modem->Pdc.Event);
        LeaveCriticalSection(&Modem->StateLock);
        return;
    }
    LeaveCriticalSection(&Modem->StateLock);

    QcTrace("ind PDC 0x%04X token %lu not waited for\n", MessageId, Token);
}

/*
 * Sends one PDC request carrying a token and waits for the indication with the
 * same token, which holds the real answer. The indication lands in Modem->Pdc.Ind.
 */
static
HRESULT
QcPdcCall(
    _In_ PQC_MODEM Modem,
    _In_ USHORT MessageId,
    _In_reads_bytes_opt_(BodyLen) const BYTE *Body,
    _In_ ULONG BodyLen,
    _Out_ PUSHORT IndResult)
{
    QMI_ENCODER Encoder;
    BYTE Message[QCRIL_PDC_CHUNK + 256];
    BYTE Response[128];
    ULONG Length, ResponseLen, Token;
    const VOID *Value;
    USHORT ValueLen;
    HRESULT hr;

    *IndResult = 0xFFFF;

    EnterCriticalSection(&Modem->StateLock);
    Token = ++Modem->Pdc.NextToken;
    Modem->Pdc.WaitId = MessageId;
    Modem->Pdc.WaitToken = Token;
    Modem->Pdc.IndLen = 0;
    ResetEvent(Modem->Pdc.Event);
    LeaveCriticalSection(&Modem->StateLock);

    QmiEncodeBegin(&Encoder, Message, sizeof(Message), QMI_TYPE_REQUEST, 0, MessageId);
    /* The callers build their TLVs whole, they go in before the token */
    if (BodyLen && Body && sizeof(Message) - Encoder.Offset < BodyLen)
        return E_INVALIDARG;
    if (BodyLen && Body)
    {
        memcpy(Message + Encoder.Offset, Body, BodyLen);
        Encoder.Offset += BodyLen;
    }
    if (!QmiEncodeTlv(&Encoder, QCRIL_PDC_TLV_TOKEN, &Token, sizeof(Token)))
        return E_FAIL;
    Length = QmiEncodeEnd(&Encoder);

    hr = QcQmiRequest(Modem, QcSvcPdc, Message, Length, Response, sizeof(Response), &ResponseLen);
    if (FAILED(hr))
    {
        EnterCriticalSection(&Modem->StateLock);
        Modem->Pdc.WaitId = 0;
        LeaveCriticalSection(&Modem->StateLock);
        return hr;
    }

    if (WaitForSingleObject(Modem->Pdc.Event, QCRIL_PDC_TIMEOUT_MS) != WAIT_OBJECT_0)
    {
        EnterCriticalSection(&Modem->StateLock);
        Modem->Pdc.WaitId = 0;
        LeaveCriticalSection(&Modem->StateLock);
        QcTrace("pdc: no indication for 0x%04X token %lu\n", MessageId, Token);
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    if (QmiFindTlv(Modem->Pdc.Ind, Modem->Pdc.IndLen, QCRIL_PDC_TLV_RESULT, &Value, &ValueLen) && ValueLen >= 2)
        memcpy(IndResult, Value, sizeof(*IndResult));

    return (*IndResult == 0) ? S_OK : E_FAIL;
}

/* Config type then a length prefixed id, the layout every by-id PDC request uses */
static
ULONG
QcPdcPutTypeAndId(
    _Out_writes_bytes_(OutSize) PBYTE Out,
    _In_ ULONG OutSize,
    _In_ BYTE TlvType,
    _In_ ULONG ConfigType,
    _In_ const QC_PDC_ID *Id)
{
    USHORT Len = (USHORT)(4 + 1 + Id->Length);

    if (OutSize < 3u + Len)
        return 0;

    Out[0] = TlvType;
    memcpy(Out + 1, &Len, sizeof(Len));
    memcpy(Out + 3, &ConfigType, sizeof(ConfigType));
    Out[7] = Id->Length;
    memcpy(Out + 8, Id->Bytes, Id->Length);
    return 3u + Len;
}

static
BOOL
QcPdcReadId(
    _In_ PQC_MODEM Modem,
    _In_ BYTE TlvType,
    _Out_ PQC_PDC_ID Id)
{
    const VOID *Value;
    USHORT ValueLen;
    const UCHAR *Bytes;

    Id->Length = 0;
    if (!QmiFindTlv(Modem->Pdc.Ind, Modem->Pdc.IndLen, TlvType, &Value, &ValueLen) || ValueLen < 1)
        return FALSE;

    Bytes = Value;
    Id->Length = (UCHAR)min((ULONG)Bytes[0], min((ULONG)ValueLen - 1, (ULONG)QC_PDC_MAX_ID));
    memcpy(Id->Bytes, Bytes + 1, Id->Length);
    return Id->Length != 0;
}

static
VOID
QcPdcLogInfo(
    _In_ PQC_MODEM Modem,
    _In_ const QC_PDC_ID *Id)
{
    BYTE Body[8 + QC_PDC_MAX_ID];
    CHAR Text[2 * QC_PDC_MAX_ID + 1];
    CHAR Description[128];
    const VOID *Value;
    USHORT ValueLen, Result;
    ULONG Size = 0, Version = 0, BodyLen, Count;

    QcPdcIdText(Id, Text, sizeof(Text));
    BodyLen = QcPdcPutTypeAndId(Body, sizeof(Body), QCRIL_PDC_TLV_TYPE, QCRIL_PDC_SOFTWARE, Id);
    if (!BodyLen || FAILED(QcPdcCall(Modem, QCRIL_PDC_GET_INFO, Body, BodyLen, &Result)))
    {
        QcTrace("pdc:   config '%s', no info\n", Text);
        return;
    }

    Description[0] = '\0';
    if (QmiFindTlv(Modem->Pdc.Ind, Modem->Pdc.IndLen, QCRIL_PDC_TLV_DESCRIPTION, &Value, &ValueLen) && ValueLen >= 1)
    {
        Count = min((ULONG)((const UCHAR *)Value)[0], min((ULONG)ValueLen - 1, (ULONG)sizeof(Description) - 1));
        memcpy(Description, (const UCHAR *)Value + 1, Count);
        Description[Count] = '\0';
    }
    if (QmiFindTlv(Modem->Pdc.Ind, Modem->Pdc.IndLen, QCRIL_PDC_TLV_SIZE, &Value, &ValueLen) && ValueLen >= 4)
        memcpy(&Size, Value, sizeof(Size));
    if (QmiFindTlv(Modem->Pdc.Ind, Modem->Pdc.IndLen, QCRIL_PDC_TLV_VERSION, &Value, &ValueLen) && ValueLen >= 4)
        memcpy(&Version, Value, sizeof(Version));

    QcTrace("pdc:   config '%s' '%s', %lu bytes, version 0x%08lx\n", Text, Description, Size, Version);
}

/* The MCFG file the registry maps the SIM's home network to, MCC+MNC with three then two MNC digits */
static
BOOL
QcPdcCarrierFile(
    _In_z_ PCSTR Imsi,
    _Out_writes_(PathSize) PWSTR Path,
    _In_ DWORD PathSize)
{
    WCHAR Raw[MAX_PATH], Name[8];
    DWORD Type, Size;
    HKEY Key;
    ULONG Digits, i;
    BOOL Found = FALSE;

    if (strlen(Imsi) < 6)
        return FALSE;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, QCRIL_CARRIER_KEY, 0, KEY_READ, &Key) != ERROR_SUCCESS)
        return FALSE;

    for (Digits = 6; Digits >= 5 && !Found; Digits--)
    {
        for (i = 0; i < Digits; i++)
            Name[i] = (WCHAR)Imsi[i];
        Name[Digits] = UNICODE_NULL;

        Size = sizeof(Raw) - sizeof(WCHAR);
        if (RegQueryValueExW(Key, Name, NULL, &Type, (LPBYTE)Raw, &Size) == ERROR_SUCCESS &&
            (Type == REG_SZ || Type == REG_EXPAND_SZ))
        {
            Raw[Size / sizeof(WCHAR)] = UNICODE_NULL;
            Found = ExpandEnvironmentStringsW(Raw, Path, PathSize) != 0;
        }
    }

    RegCloseKey(Key);
    return Found;
}

static
PBYTE
QcPdcReadFile(
    _In_z_ PCWSTR Path,
    _Out_ PULONG Size)
{
    HANDLE File;
    PBYTE Data;
    DWORD Read;
    LARGE_INTEGER Length;

    *Size = 0;
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return NULL;

    if (!GetFileSizeEx(File, &Length) || Length.QuadPart == 0 || Length.QuadPart > 4 * 1024 * 1024)
    {
        CloseHandle(File);
        return NULL;
    }

    Data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)Length.QuadPart);
    if (Data && (!ReadFile(File, Data, (DWORD)Length.QuadPart, &Read, NULL) || Read != (DWORD)Length.QuadPart))
    {
        HeapFree(GetProcessHeap(), 0, Data);
        Data = NULL;
    }

    CloseHandle(File);
    if (Data)
        *Size = (ULONG)Length.QuadPart;
    return Data;
}

/* Hands the file to the modem a chunk at a time, each chunk confirmed by an indication */
static
HRESULT
QcPdcLoad(
    _In_ PQC_MODEM Modem,
    _In_ const QC_PDC_ID *Id,
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ ULONG Size)
{
    BYTE Body[3 + 4 + 1 + QC_PDC_MAX_ID + 4 + 2 + QCRIL_PDC_CHUNK];
    ULONG Offset = 0, Chunk, Type = QCRIL_PDC_SOFTWARE, Remaining = Size;
    USHORT TlvLen, ChunkLen, Result;
    const VOID *Value;
    USHORT ValueLen;
    PBYTE At;
    HRESULT hr;

    while (Offset < Size)
    {
        Chunk = min(Size - Offset, (ULONG)QCRIL_PDC_CHUNK);
        ChunkLen = (USHORT)Chunk;
        TlvLen = (USHORT)(4 + 1 + Id->Length + 4 + 2 + Chunk);

        At = Body;
        *At++ = QCRIL_PDC_TLV_TYPE;
        memcpy(At, &TlvLen, sizeof(TlvLen));
        At += sizeof(TlvLen);
        memcpy(At, &Type, sizeof(Type));
        At += sizeof(Type);
        *At++ = Id->Length;
        memcpy(At, Id->Bytes, Id->Length);
        At += Id->Length;
        memcpy(At, &Size, sizeof(Size));
        At += sizeof(Size);
        memcpy(At, &ChunkLen, sizeof(ChunkLen));
        At += sizeof(ChunkLen);
        memcpy(At, Data + Offset, Chunk);
        At += Chunk;

        hr = QcPdcCall(Modem, QCRIL_PDC_LOAD_CONFIG, Body, (ULONG)(At - Body), &Result);
        if (FAILED(hr))
        {
            QcTrace("pdc: load chunk at %lu failed 0x%08lx, result %u\n", Offset, hr, Result);
            return hr;
        }

        Offset += Chunk;
        if (QmiFindTlv(Modem->Pdc.Ind, Modem->Pdc.IndLen, QCRIL_PDC_TLV_REMAINING, &Value, &ValueLen) && ValueLen >= 4)
            memcpy(&Remaining, Value, sizeof(Remaining));
    }

    QcTrace("pdc: loaded %lu bytes, modem reports %lu remaining\n", Size, Remaining);
    return S_OK;
}

VOID
QcPdcSetup(
    _In_ PQC_MODEM Modem)
{
    BYTE Body[16 + QC_PDC_MAX_ID];
    CHAR Imsi[20], ActiveText[2 * QC_PDC_MAX_ID + 1], WantText[2 * QC_PDC_MAX_ID + 1];
    WCHAR Path[MAX_PATH];
    QC_PDC_ID Active, Pending, Want, Loaded[QC_PDC_MAX_CONFIGS];
    ULONG LoadedCount = 0, BodyLen, Size, Type, i, Offset, Count;
    const VOID *Value;
    const UCHAR *List;
    USHORT ValueLen, Result;
    BOOL HaveWant;
    PBYTE Data;
    HRESULT hr;

    if (!Modem->Pdc.Event)
    {
        Modem->Pdc.Event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!Modem->Pdc.Event)
            return;
    }

    /* The configuration follows the card, so nothing happens until its IMSI reads.
       The device management IMSI request is not supported here, the card file is read */
    hr = QcUimReadImsi(Modem, Imsi, sizeof(Imsi));
    if (FAILED(hr))
    {
        if (hr != E_PENDING)
            QcTrace("pdc: reading the card IMSI failed 0x%08lx\n", hr);
        return;
    }

    /* Everything past the card read is a final outcome for this modem start */
    Modem->Pdc.Done = TRUE;

    Type = QCRIL_PDC_SOFTWARE;
    Body[0] = QCRIL_PDC_TLV_TYPE;
    Body[1] = sizeof(Type);
    Body[2] = 0;
    memcpy(Body + 3, &Type, sizeof(Type));
    hr = QcPdcCall(Modem, QCRIL_PDC_GET_SELECTED, Body, 3 + sizeof(Type), &Result);
    Active.Length = Pending.Length = 0;
    if (SUCCEEDED(hr))
    {
        QcPdcReadId(Modem, QCRIL_PDC_TLV_ACTIVE, &Active);
        QcPdcReadId(Modem, QCRIL_PDC_TLV_PENDING, &Pending);
    }
    QcPdcIdText(&Active, ActiveText, sizeof(ActiveText));
    QcPdcIdText(&Pending, WantText, sizeof(WantText));
    QcTrace("pdc: software config active '%s' pending '%s' (0x%08lx result %u)\n", ActiveText, WantText, hr, Result);

    /* List Configs takes its type in TLV 0x11 rather than 0x01 */
    Body[0] = QCRIL_PDC_TLV_LIST_TYPE;
    hr = QcPdcCall(Modem, QCRIL_PDC_LIST_CONFIGS, Body, 3 + sizeof(Type), &Result);
    if (SUCCEEDED(hr) &&
        QmiFindTlv(Modem->Pdc.Ind, Modem->Pdc.IndLen, QCRIL_PDC_TLV_CONFIGS, &Value, &ValueLen) && ValueLen >= 1)
    {
        /* Count, then per config a type and a length prefixed id */
        List = Value;
        Count = List[0];
        Offset = 1;
        for (i = 0; i < Count && Offset + 5 <= ValueLen && LoadedCount < QC_PDC_MAX_CONFIGS; i++)
        {
            UCHAR IdLen = List[Offset + 4];

            if (Offset + 5 + IdLen > ValueLen)
                break;
            Loaded[LoadedCount].Length = (UCHAR)min((ULONG)IdLen, (ULONG)QC_PDC_MAX_ID);
            memcpy(Loaded[LoadedCount].Bytes, List + Offset + 5, Loaded[LoadedCount].Length);
            LoadedCount++;
            Offset += 5 + IdLen;
        }
    }
    QcTrace("pdc: %lu software config(s) loaded\n", LoadedCount);
    for (i = 0; i < LoadedCount; i++)
        QcPdcLogInfo(Modem, &Loaded[i]);

    if (!QcPdcCarrierFile(Imsi, Path, RTL_NUMBER_OF(Path)))
    {
        QcTrace("pdc: no carrier configuration mapped for home network %.6s, keeping the active one\n", Imsi);
        return;
    }

    Data = QcPdcReadFile(Path, &Size);
    if (!Data)
    {
        QcTrace("pdc: carrier configuration file '%S' did not read (%lu)\n", Path, GetLastError());
        return;
    }

    /* The id is ours to pick for a loaded file, stable for the same file contents */
    Want.Length = (UCHAR)_snprintf((PCHAR)Want.Bytes, QC_PDC_MAX_ID, "ros-%08lx-%lu",
                                   RtlComputeCrc32(0, Data, Size), Size);
    QcPdcIdText(&Want, WantText, sizeof(WantText));
    QcTrace("pdc: home network %.6s wants '%S' as '%s'\n", Imsi, Path, WantText);

    if (QcPdcSameId(&Active, &Want))
    {
        QcTrace("pdc: the carrier configuration is already active\n");
        HeapFree(GetProcessHeap(), 0, Data);
        return;
    }

    /* One switch per run: a configuration the modem will not keep must not loop */
    if (Modem->Pdc.Switched)
    {
        QcTrace("pdc: already switched once, the modem kept '%s', leaving it\n", ActiveText);
        HeapFree(GetProcessHeap(), 0, Data);
        return;
    }

    HaveWant = FALSE;
    for (i = 0; i < LoadedCount; i++)
    {
        if (QcPdcSameId(&Loaded[i], &Want))
            HaveWant = TRUE;
    }

    hr = HaveWant ? S_OK : QcPdcLoad(Modem, &Want, Data, Size);
    HeapFree(GetProcessHeap(), 0, Data);
    if (FAILED(hr))
        return;

    BodyLen = QcPdcPutTypeAndId(Body, sizeof(Body), QCRIL_PDC_TLV_TYPE, QCRIL_PDC_SOFTWARE, &Want);
    hr = QcPdcCall(Modem, QCRIL_PDC_SET_SELECTED, Body, BodyLen, &Result);
    QcTrace("pdc: select '%s': 0x%08lx result %u\n", WantText, hr, Result);
    if (FAILED(hr))
        return;

    Modem->Pdc.Switched = TRUE;

    /* Activation restarts the modem; setup runs again once its services return */
    Body[0] = QCRIL_PDC_TLV_TYPE;
    Body[1] = sizeof(Type);
    Body[2] = 0;
    memcpy(Body + 3, &Type, sizeof(Type));
    hr = QcPdcCall(Modem, QCRIL_PDC_ACTIVATE, Body, 3 + sizeof(Type), &Result);
    QcTrace("pdc: activate: 0x%08lx result %u, the modem restarts on the new configuration\n", hr, Result);
}
