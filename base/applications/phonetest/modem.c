/*
 * PROJECT:     ReactOS Phone Test
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     QMI console straight on the Qualcomm IPC router, next to the RIL
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "phonetest.h"
#include <winioctl.h>
#include <wchar.h>
#include <qcom/qmi.h>

/* The IPC router control codes, FILE_DEVICE_NETWORK functions from 0x800 */
#define PT_IPC_DEVICE               L"\\\\.\\IPC"
#define PT_IPC_FIND_SERVERS         CTL_CODE(0x12, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define PT_IPC_PORT_OPEN            CTL_CODE(0x12, 0x807, METHOD_NEITHER, FILE_ANY_ACCESS)
#define PT_IPC_PORT_CLOSE           CTL_CODE(0x12, 0x808, METHOD_NEITHER, FILE_ANY_ACCESS)

#define PT_IPC_TIMEOUT_MS           3000
#define PT_QMI_MAX                  2048
#define PT_MAX_SERVICE_ID           0x50
#define PT_MAX_PENDING              32

#define PT_SVC_DMS                  0x02
#define PT_SVC_NAS                  0x03
#define PT_SVC_UIM                  0x0B

typedef struct _PT_ADDR
{
    ULONG Node;
    ULONG Port;
} PT_ADDR;

typedef struct _PT_FIND
{
    ULONG ServiceId;
    ULONG Instance;
    ULONG InstanceMask;
    ULONG Capacity;
} PT_FIND;

typedef struct _PT_FOUND
{
    ULONG Total;
    ULONG Returned;
    ULONG ServiceId;
    ULONG Instance;
    PT_ADDR Addr;
} PT_FOUND;

typedef struct _PT_SERVICE
{
    BOOL Found;
    ULONG Instance;
    PT_ADDR Addr;
} PT_SERVICE;

typedef struct _PT_PENDING
{
    USHORT Transaction;
    UCHAR Service;
    USHORT MessageId;
} PT_PENDING;

static HWND PtModemWindow;
static HANDLE PtIpc = INVALID_HANDLE_VALUE;
static HANDLE PtRxThread;
static volatile LONG PtRxStop;
static PT_SERVICE PtServices[PT_MAX_SERVICE_ID + 1];
static PT_PENDING PtPending[PT_MAX_PENDING];
static ULONG PtPendingNext;
static USHORT PtTransaction;

static const struct
{
    UCHAR Id;
    LPCWSTR Name;
} PtServiceNames[] =
{
    { 0x01, L"WDS" }, { 0x02, L"DMS" }, { 0x03, L"NAS" }, { 0x04, L"QOS" },
    { 0x05, L"WMS" }, { 0x06, L"PDS" }, { 0x07, L"AUTH" }, { 0x08, L"AT" },
    { 0x09, L"VOICE" }, { 0x0A, L"CAT" }, { 0x0B, L"UIM" }, { 0x0C, L"PBM" },
    { 0x10, L"LOC" }, { 0x11, L"SAR" }, { 0x12, L"IMSS" }, { 0x1A, L"WDA" },
    { 0x22, L"COEX" }, { 0x24, L"PDC" }, { 0x2A, L"DSD" }
};

static const LPCWSTR PtModeNames[] =
{
    L"online", L"low power", L"factory test", L"offline", L"resetting",
    L"shutting down", L"persistent low power", L"mode only low power"
};

static const LPCWSTR PtRegNames[] =
{
    L"not registered", L"registered", L"searching", L"denied", L"unknown"
};

static const LPCWSTR PtCardNames[] =
{
    L"absent", L"present", L"error"
};

static const LPCWSTR PtAppTypeNames[] =
{
    L"unknown", L"SIM", L"USIM", L"RUIM", L"CSIM", L"ISIM"
};

static const LPCWSTR PtAppStateNames[] =
{
    L"unknown", L"detected", L"PIN required", L"PUK required", L"perso check",
    L"PIN blocked", L"illegal", L"ready"
};

BOOL
PtModemIsCommand(
    _In_ UINT Id)
{
    return Id >= IDC_MODEM_STATUS && Id <= IDC_MODEM_LOG;
}

static
LPCWSTR
PtTableName(
    _In_reads_(Count) const LPCWSTR *Table,
    _In_ ULONG Count,
    _In_ ULONG Index)
{
    return (Index < Count) ? Table[Index] : L"?";
}

static
LPCWSTR
PtServiceName(
    _In_ ULONG Id)
{
    ULONG i;

    for (i = 0; i < _countof(PtServiceNames); i++)
    {
        if (PtServiceNames[i].Id == Id)
            return PtServiceNames[i].Name;
    }
    return L"svc";
}

static
LPCWSTR
PtRadioName(
    _In_ UCHAR Radio)
{
    switch (Radio)
    {
        case 0x00: return L"none";
        case 0x01: return L"CDMA 1x";
        case 0x02: return L"EV-DO";
        case 0x04: return L"GSM";
        case 0x05: return L"UMTS";
        case 0x08: return L"LTE";
        case 0x09: return L"TD-SCDMA";
        case 0x0C: return L"NR5G";
        default: return L"?";
    }
}

/* Every log line also goes to the debug port, the serial log keeps what the screen scrolls away */
static
VOID
PtModemLog(
    _In_ LPCWSTR Format,
    ...)
{
    WCHAR Line[600];
    va_list Args;
    HWND Log;
    LRESULT Index;

    va_start(Args, Format);
    StringCchVPrintfW(Line, _countof(Line), Format, Args);
    va_end(Args);

    OutputDebugStringW(L"[modem] ");
    OutputDebugStringW(Line);
    OutputDebugStringW(L"\n");

    Log = GetDlgItem(PtModemWindow, IDC_MODEM_LOG);
    if (!Log)
        return;

    Index = SendMessageW(Log, LB_ADDSTRING, 0, (LPARAM)Line);
    SendMessageW(Log, LB_SETCURSEL, Index, 0);
}

static
VOID
PtModemStatus(VOID)
{
    WCHAR Text[128];
    ULONG Count = 0, i;

    for (i = 0; i <= PT_MAX_SERVICE_ID; i++)
    {
        if (PtServices[i].Found)
            Count++;
    }

    if (PtIpc == INVALID_HANDLE_VALUE)
        StringCchCopyW(Text, _countof(Text), L"IPC router closed");
    else
        StringCchPrintfW(Text, _countof(Text), L"IPC router open  \x2022  %lu services", Count);

    SetDlgItemTextW(PtModemWindow, IDC_MODEM_STATUS, Text);
}

/* Runs one router request, giving up after a while so a stuck router cannot freeze the page */
static
BOOL
PtIpcCall(
    _In_ DWORD Code,
    _In_reads_bytes_opt_(InSize) PVOID In,
    _In_ DWORD InSize,
    _Out_writes_bytes_opt_(OutSize) PVOID Out,
    _In_ DWORD OutSize,
    _Out_opt_ PDWORD Returned)
{
    OVERLAPPED Overlapped;
    DWORD Transferred = 0;
    BOOL Ok;

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Overlapped.hEvent)
        return FALSE;

    Ok = DeviceIoControl(PtIpc, Code, In, InSize, Out, OutSize, NULL, &Overlapped);
    if (!Ok && GetLastError() == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject(Overlapped.hEvent, PT_IPC_TIMEOUT_MS) != WAIT_OBJECT_0)
        {
            CancelIo(PtIpc);
            WaitForSingleObject(Overlapped.hEvent, INFINITE);
            SetLastError(ERROR_TIMEOUT);
            CloseHandle(Overlapped.hEvent);
            return FALSE;
        }
        Ok = GetOverlappedResult(PtIpc, &Overlapped, &Transferred, FALSE);
    }

    if (Returned)
        *Returned = Transferred;
    CloseHandle(Overlapped.hEvent);
    return Ok;
}

static
DWORD
WINAPI
PtRxMain(
    _In_ LPVOID Parameter)
{
    OVERLAPPED Overlapped;
    PPT_MODEM_PACKET Packet;
    BYTE Buffer[8 + PT_QMI_MAX];
    DWORD Read;
    BOOL Ok;

    UNREFERENCED_PARAMETER(Parameter);

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Overlapped.hEvent)
        return 0;

    while (!PtRxStop)
    {
        ResetEvent(Overlapped.hEvent);
        Ok = ReadFile(PtIpc, Buffer, sizeof(Buffer), NULL, &Overlapped);
        if (!Ok && GetLastError() != ERROR_IO_PENDING)
            break;
        if (!GetOverlappedResult(PtIpc, &Overlapped, &Read, TRUE) || PtRxStop)
            break;
        if (Read < 8)
            continue;

        Packet = HeapAlloc(GetProcessHeap(), 0, FIELD_OFFSET(PT_MODEM_PACKET, Data) + Read - 8);
        if (!Packet)
            continue;

        Packet->Node = ((PULONG)Buffer)[0];
        Packet->Port = ((PULONG)Buffer)[1];
        Packet->Length = Read - 8;
        CopyMemory(Packet->Data, Buffer + 8, Packet->Length);

        if (!PostMessageW(PtModemWindow, WM_MODEM_PACKET, 0, (LPARAM)Packet))
            HeapFree(GetProcessHeap(), 0, Packet);
    }

    CloseHandle(Overlapped.hEvent);
    return 0;
}

VOID
PtModemOpen(
    _In_ HWND Window)
{
    PtModemWindow = Window;
    if (PtIpc != INVALID_HANDLE_VALUE)
        return;

    PtIpc = CreateFileW(PT_IPC_DEVICE,
                        GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL,
                        OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED,
                        NULL);
    if (PtIpc == INVALID_HANDLE_VALUE)
    {
        PtModemLog(L"Cannot open %s (%lu)", PT_IPC_DEVICE, GetLastError());
        PtModemStatus();
        return;
    }

    if (!PtIpcCall(PT_IPC_PORT_OPEN, NULL, 0, NULL, 0, NULL))
    {
        PtModemLog(L"Router refused a port (%lu)", GetLastError());
        CloseHandle(PtIpc);
        PtIpc = INVALID_HANDLE_VALUE;
        PtModemStatus();
        return;
    }

    PtRxStop = FALSE;
    PtRxThread = CreateThread(NULL, 0, PtRxMain, NULL, 0, NULL);
    PtModemLog(L"Router port open");
    PtModemStatus();
}

VOID
PtModemClose(VOID)
{
    if (PtIpc == INVALID_HANDLE_VALUE)
        return;

    PtRxStop = TRUE;
    PtIpcCall(PT_IPC_PORT_CLOSE, NULL, 0, NULL, 0, NULL);
    CancelIo(PtIpc);
    CloseHandle(PtIpc);
    PtIpc = INVALID_HANDLE_VALUE;

    if (PtRxThread)
    {
        WaitForSingleObject(PtRxThread, PT_IPC_TIMEOUT_MS);
        CloseHandle(PtRxThread);
        PtRxThread = NULL;
    }
}

/* Asks the router who serves one service id; any version or instance is taken first */
static
BOOL
PtLookup(
    _In_ ULONG ServiceId,
    _In_ BOOL Quiet)
{
    static const ULONG Masks[] = { 0, 0xFFFFFFFF, 0xFFFFFFFF };
    static const ULONG Names[] = { 0, 1, 2 };
    PT_FIND Find;
    PT_FOUND Found;
    DWORD Returned;
    ULONG i;

    if (ServiceId > PT_MAX_SERVICE_ID)
        return FALSE;

    for (i = 0; i < _countof(Masks); i++)
    {
        Find.ServiceId = ServiceId;
        Find.Instance = Names[i];
        Find.InstanceMask = Masks[i];
        Find.Capacity = 1;
        ZeroMemory(&Found, sizeof(Found));

        if (PtIpcCall(PT_IPC_FIND_SERVERS, &Find, sizeof(Find), &Found, sizeof(Found), &Returned) &&
            Found.Returned)
        {
            PtServices[ServiceId].Found = TRUE;
            PtServices[ServiceId].Instance = Found.Instance;
            PtServices[ServiceId].Addr = Found.Addr;
            if (!Quiet)
            {
                PtModemLog(L"%s (0x%02lx) at %lu:%lu, version %lu instance %lu, %lu server(s)",
                           PtServiceName(ServiceId), ServiceId, Found.Addr.Node, Found.Addr.Port,
                           Found.Instance & 0xFF, Found.Instance >> 8, Found.Total);
            }
            return TRUE;
        }
    }

    PtServices[ServiceId].Found = FALSE;
    if (!Quiet)
        PtModemLog(L"%s (0x%02lx) not registered (%lu)", PtServiceName(ServiceId), ServiceId, GetLastError());
    return FALSE;
}

static
VOID
PtScanServices(VOID)
{
    ULONG Id, Count = 0;

    PtModemLog(L"Scanning service ids 1 to 0x%x", PT_MAX_SERVICE_ID);
    for (Id = 1; Id <= PT_MAX_SERVICE_ID; Id++)
    {
        if (PtLookup(Id, TRUE))
        {
            Count++;
            PtModemLog(L"  %s (0x%02lx) at %lu:%lu, version %lu instance %lu",
                       PtServiceName(Id), Id, PtServices[Id].Addr.Node, PtServices[Id].Addr.Port,
                       PtServices[Id].Instance & 0xFF, PtServices[Id].Instance >> 8);
        }
    }

    PtModemLog(L"%lu service(s) registered", Count);
    if (!PtServices[PT_SVC_DMS].Found)
        PtLookup(PT_SVC_DMS, FALSE);
    PtModemStatus();
}

/* Sends one request; the answer is printed when the receive thread hands it over */
static
VOID
PtSend(
    _In_ UCHAR Service,
    _In_ USHORT MessageId,
    _In_reads_bytes_opt_(TlvLength) const BYTE *Tlvs,
    _In_ USHORT TlvLength)
{
    BYTE Frame[8 + PT_QMI_MAX];
    PQMI_HEADER Header = (PQMI_HEADER)(Frame + 8);
    OVERLAPPED Overlapped;
    DWORD Written;
    BOOL Ok;

    if (PtIpc == INVALID_HANDLE_VALUE)
    {
        PtModemLog(L"Router is not open");
        return;
    }

    if (TlvLength > PT_QMI_MAX - sizeof(QMI_HEADER))
        return;

    if (!PtServices[Service].Found && !PtLookup(Service, FALSE))
        return;

    if (++PtTransaction == 0)
        PtTransaction = 1;

    ((PULONG)Frame)[0] = PtServices[Service].Addr.Node;
    ((PULONG)Frame)[1] = PtServices[Service].Addr.Port;
    Header->Type = QMI_TYPE_REQUEST;
    Header->TransactionId = PtTransaction;
    Header->MessageId = MessageId;
    Header->Length = TlvLength;
    if (TlvLength)
        CopyMemory(Header + 1, Tlvs, TlvLength);

    PtPending[PtPendingNext].Transaction = PtTransaction;
    PtPending[PtPendingNext].Service = Service;
    PtPending[PtPendingNext].MessageId = MessageId;
    PtPendingNext = (PtPendingNext + 1) % PT_MAX_PENDING;

    ZeroMemory(&Overlapped, sizeof(Overlapped));
    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Overlapped.hEvent)
        return;

    Ok = WriteFile(PtIpc, Frame, 8 + sizeof(QMI_HEADER) + TlvLength, NULL, &Overlapped);
    if (!Ok && GetLastError() == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject(Overlapped.hEvent, PT_IPC_TIMEOUT_MS) == WAIT_OBJECT_0)
            Ok = GetOverlappedResult(PtIpc, &Overlapped, &Written, FALSE);
        else
        {
            CancelIo(PtIpc);
            WaitForSingleObject(Overlapped.hEvent, INFINITE);
            SetLastError(ERROR_TIMEOUT);
        }
    }
    CloseHandle(Overlapped.hEvent);

    if (Ok)
        PtModemLog(L"-> %s 0x%04x, txn %u, %u TLV bytes", PtServiceName(Service), MessageId, PtTransaction, TlvLength);
    else
        PtModemLog(L"-> %s 0x%04x send failed (%lu)", PtServiceName(Service), MessageId, GetLastError());
}

static
VOID
PtSendByte(
    _In_ UCHAR Service,
    _In_ USHORT MessageId,
    _In_ UCHAR Type,
    _In_ UCHAR Value)
{
    BYTE Tlv[4];

    Tlv[0] = Type;
    Tlv[1] = 1;
    Tlv[2] = 0;
    Tlv[3] = Value;
    PtSend(Service, MessageId, Tlv, sizeof(Tlv));
}

/* Finds one record of a message body, the body being everything past the QMI header */
static
BOOL
PtTlv(
    _In_reads_bytes_(Length) const BYTE *Body,
    _In_ ULONG Length,
    _In_ UCHAR Type,
    _Outptr_ const BYTE **Value,
    _Out_ PUSHORT ValueLength)
{
    ULONG Offset = 0;

    while (Offset + sizeof(QMI_TLV_HEADER) <= Length)
    {
        const QMI_TLV_HEADER *Tlv = (const QMI_TLV_HEADER *)(Body + Offset);
        ULONG Next = Offset + sizeof(QMI_TLV_HEADER) + Tlv->Length;

        if (Next > Length)
            break;
        if (Tlv->Type == Type)
        {
            *Value = (const BYTE *)(Tlv + 1);
            *ValueLength = Tlv->Length;
            return TRUE;
        }
        Offset = Next;
    }

    return FALSE;
}

static
VOID
PtDumpTlvs(
    _In_reads_bytes_(Length) const BYTE *Body,
    _In_ ULONG Length)
{
    WCHAR Line[600];
    ULONG Offset = 0, i;

    while (Offset + sizeof(QMI_TLV_HEADER) <= Length)
    {
        const QMI_TLV_HEADER *Tlv = (const QMI_TLV_HEADER *)(Body + Offset);
        const BYTE *Value = (const BYTE *)(Tlv + 1);
        ULONG Next = Offset + sizeof(QMI_TLV_HEADER) + Tlv->Length;

        if (Next > Length)
        {
            PtModemLog(L"   truncated record at %lu", Offset);
            break;
        }

        StringCchPrintfW(Line, _countof(Line), L"   tlv 0x%02x [%u]:", Tlv->Type, Tlv->Length);
        for (i = 0; i < Tlv->Length && i < 64; i++)
        {
            WCHAR Byte[4];
            StringCchPrintfW(Byte, _countof(Byte), L" %02x", Value[i]);
            StringCchCatW(Line, _countof(Line), Byte);
        }
        if (Tlv->Length > 64)
            StringCchCatW(Line, _countof(Line), L" ...");
        PtModemLog(L"%s", Line);

        Offset = Next;
    }
}

static
VOID
PtLogString(
    _In_ LPCWSTR What,
    _In_reads_bytes_(Length) const BYTE *Value,
    _In_ USHORT Length)
{
    WCHAR Text[256];
    ULONG i;

    for (i = 0; i < Length && i < _countof(Text) - 1; i++)
        Text[i] = (WCHAR)Value[i];
    Text[i] = UNICODE_NULL;
    PtModemLog(L"   %s %s", What, Text);
}

static
VOID
PtDecodeDms(
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const BYTE *Body,
    _In_ ULONG Length)
{
    const BYTE *Value;
    USHORT Size;

    switch (MessageId)
    {
        case 0x23:
            if (PtTlv(Body, Length, 0x01, &Value, &Size))
                PtLogString(L"revision", Value, Size);
            break;

        case 0x25:
            if (PtTlv(Body, Length, 0x11, &Value, &Size))
                PtLogString(L"IMEI", Value, Size);
            if (PtTlv(Body, Length, 0x10, &Value, &Size))
                PtLogString(L"ESN", Value, Size);
            if (PtTlv(Body, Length, 0x12, &Value, &Size))
                PtLogString(L"MEID", Value, Size);
            break;

        case 0x2D:
            if (PtTlv(Body, Length, 0x01, &Value, &Size) && Size >= 1)
                PtModemLog(L"   operating mode %u (%s)", Value[0], PtTableName(PtModeNames, _countof(PtModeNames), Value[0]));
            if (PtTlv(Body, Length, 0x10, &Value, &Size) && Size >= 2)
                PtModemLog(L"   offline reason 0x%04x", *(const USHORT *)Value);
            if (PtTlv(Body, Length, 0x11, &Value, &Size) && Size >= 1)
                PtModemLog(L"   hardware restricted %u", Value[0]);
            break;
    }
}

static
VOID
PtDecodeNas(
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const BYTE *Body,
    _In_ ULONG Length)
{
    const BYTE *Value;
    USHORT Size;
    ULONG i;

    switch (MessageId)
    {
        case 0x20:
            if (PtTlv(Body, Length, 0x01, &Value, &Size) && Size >= 2)
                PtModemLog(L"   signal %d dBm on %s", (CHAR)Value[0], PtRadioName(Value[1]));
            break;

        case 0x24:
            if (PtTlv(Body, Length, 0x01, &Value, &Size) && Size >= 5)
            {
                PtModemLog(L"   %s, CS attach %u, PS attach %u, network %u",
                           PtTableName(PtRegNames, _countof(PtRegNames), Value[0]), Value[1], Value[2], Value[3]);
                for (i = 0; i < Value[4] && 5 + i < Size; i++)
                    PtModemLog(L"   radio %s", PtRadioName(Value[5 + i]));
            }
            if (PtTlv(Body, Length, 0x12, &Value, &Size) && Size >= 5)
            {
                WCHAR Name[64];
                ULONG Count = min(Value[4], (ULONG)(Size - 5));

                for (i = 0; i < Count && i < _countof(Name) - 1; i++)
                    Name[i] = (WCHAR)Value[5 + i];
                Name[i] = UNICODE_NULL;
                PtModemLog(L"   PLMN %03u/%02u %s", *(const USHORT *)Value, *(const USHORT *)(Value + 2), Name);
            }
            break;
    }
}

static
VOID
PtDecodeUim(
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const BYTE *Body,
    _In_ ULONG Length)
{
    const BYTE *Value;
    USHORT Size;
    ULONG Offset, Slot, App, Slots, Apps;

    if (MessageId != 0x2F || !PtTlv(Body, Length, 0x10, &Value, &Size) || Size < 9)
        return;

    Slots = Value[8];
    Offset = 9;
    for (Slot = 0; Slot < Slots && Offset + 6 <= Size; Slot++)
    {
        PtModemLog(L"   slot %lu card %s, UPIN state %u, error %u",
                   Slot, PtTableName(PtCardNames, _countof(PtCardNames), Value[Offset]),
                   Value[Offset + 1], Value[Offset + 4]);
        Apps = Value[Offset + 5];
        Offset += 6;

        for (App = 0; App < Apps && Offset + 7 <= Size; App++)
        {
            ULONG AidLength = Value[Offset + 6];

            PtModemLog(L"   app %lu %s %s, perso %u",
                       App, PtTableName(PtAppTypeNames, _countof(PtAppTypeNames), Value[Offset]),
                       PtTableName(PtAppStateNames, _countof(PtAppStateNames), Value[Offset + 1]),
                       Value[Offset + 2]);
            Offset += 7 + AidLength;
            if (Offset + 7 <= Size)
                PtModemLog(L"   PIN1 state %u, %u tries, PUK1 %u tries", Value[Offset + 1], Value[Offset + 2], Value[Offset + 3]);
            Offset += 7;
        }
    }
}

VOID
PtModemPacket(
    _In_ const PT_MODEM_PACKET *Packet)
{
    const QMI_HEADER *Header = (const QMI_HEADER *)Packet->Data;
    const BYTE *Body;
    const BYTE *Value;
    USHORT Size;
    ULONG BodyLength, i;
    UCHAR Service = 0;

    if (Packet->Length < sizeof(QMI_HEADER))
    {
        PtModemLog(L"<- %lu:%lu short packet of %lu bytes", Packet->Node, Packet->Port, Packet->Length);
        return;
    }

    Body = (const BYTE *)(Header + 1);
    BodyLength = min((ULONG)Header->Length, Packet->Length - (ULONG)sizeof(QMI_HEADER));

    for (i = 0; i <= PT_MAX_SERVICE_ID; i++)
    {
        if (PtServices[i].Found &&
            PtServices[i].Addr.Node == Packet->Node &&
            PtServices[i].Addr.Port == Packet->Port)
        {
            Service = (UCHAR)i;
            break;
        }
    }

    if (Header->Type == QMI_TYPE_RESPONSE)
    {
        for (i = 0; i < PT_MAX_PENDING; i++)
        {
            if (PtPending[i].Transaction == Header->TransactionId && PtPending[i].MessageId == Header->MessageId)
            {
                Service = PtPending[i].Service;
                break;
            }
        }

        if (PtTlv(Body, BodyLength, QMI_TLV_RESULT, &Value, &Size) && Size >= sizeof(QMI_RESULT))
        {
            const QMI_RESULT *Result = (const QMI_RESULT *)Value;

            if (Result->Result == QMI_RESULT_SUCCESS)
                PtModemLog(L"<- %s 0x%04x txn %u ok", PtServiceName(Service), Header->MessageId, Header->TransactionId);
            else
                PtModemLog(L"<- %s 0x%04x txn %u failed, QMI error 0x%04x",
                           PtServiceName(Service), Header->MessageId, Header->TransactionId, Result->Error);
        }
        else
        {
            PtModemLog(L"<- %s 0x%04x txn %u without a result", PtServiceName(Service), Header->MessageId, Header->TransactionId);
        }
    }
    else if (Header->Type == QMI_TYPE_INDICATION)
    {
        PtModemLog(L"<- indication %s 0x%04x from %lu:%lu", PtServiceName(Service), Header->MessageId, Packet->Node, Packet->Port);
    }
    else
    {
        PtModemLog(L"<- type %u message 0x%04x from %lu:%lu", Header->Type, Header->MessageId, Packet->Node, Packet->Port);
    }

    switch (Service)
    {
        case PT_SVC_DMS: PtDecodeDms(Header->MessageId, Body, BodyLength); break;
        case PT_SVC_NAS: PtDecodeNas(Header->MessageId, Body, BodyLength); break;
        case PT_SVC_UIM: PtDecodeUim(Header->MessageId, Body, BodyLength); break;
    }

    PtDumpTlvs(Body, BodyLength);
}

/* Reads "svc msg [type:hexbytes ...]", every number in hex */
static
VOID
PtSendRaw(VOID)
{
    WCHAR Text[512];
    BYTE Tlvs[PT_QMI_MAX];
    USHORT TlvLength = 0;
    PWCHAR Cursor, End;
    ULONG Service, MessageId;

    GetDlgItemTextW(PtModemWindow, IDC_MODEM_RAW, Text, _countof(Text));

    Service = wcstoul(Text, &End, 16);
    if (End == Text || Service == 0 || Service > PT_MAX_SERVICE_ID)
    {
        PtModemLog(L"Raw: give the service id first, then the message id, all in hex");
        return;
    }

    Cursor = End;
    MessageId = wcstoul(Cursor, &End, 16);
    if (End == Cursor || MessageId > 0xFFFF)
    {
        PtModemLog(L"Raw: the message id is missing");
        return;
    }
    Cursor = End;

    for (;;)
    {
        ULONG Type;
        USHORT LengthAt;

        while (*Cursor == L' ')
            Cursor++;
        if (*Cursor == UNICODE_NULL)
            break;

        Type = wcstoul(Cursor, &End, 16);
        if (End == Cursor || *End != L':' || Type > 0xFF || TlvLength + 3 > sizeof(Tlvs))
        {
            PtModemLog(L"Raw: records are type:hexbytes, like 01:00");
            return;
        }

        Tlvs[TlvLength] = (BYTE)Type;
        LengthAt = TlvLength + 1;
        TlvLength += 3;
        Cursor = End + 1;

        while (iswxdigit(Cursor[0]) && iswxdigit(Cursor[1]) && TlvLength < sizeof(Tlvs))
        {
            WCHAR Pair[3] = { Cursor[0], Cursor[1], UNICODE_NULL };
            Tlvs[TlvLength++] = (BYTE)wcstoul(Pair, NULL, 16);
            Cursor += 2;
        }

        *(UNALIGNED USHORT *)&Tlvs[LengthAt] = (USHORT)(TlvLength - LengthAt - 2);
    }

    PtSend((UCHAR)Service, (USHORT)MessageId, TlvLength ? Tlvs : NULL, TlvLength);
}

VOID
PtModemCommand(
    _In_ UINT Id)
{
    if (Id != IDC_MODEM_CLEAR && PtIpc == INVALID_HANDLE_VALUE)
        PtModemOpen(PtModemWindow);

    switch (Id)
    {
        case IDC_MODEM_SERVICES:
            PtScanServices();
            break;

        case IDC_MODEM_MODE:
            PtSend(PT_SVC_DMS, 0x2D, NULL, 0);
            break;

        case IDC_MODEM_ONLINE:
            PtSendByte(PT_SVC_DMS, 0x2E, 0x01, 0);
            break;

        case IDC_MODEM_LOW_POWER:
            PtSendByte(PT_SVC_DMS, 0x2E, 0x01, 1);
            break;

        case IDC_MODEM_OFFLINE:
            PtSendByte(PT_SVC_DMS, 0x2E, 0x01, 3);
            break;

        case IDC_MODEM_RESET:
            PtSendByte(PT_SVC_DMS, 0x2E, 0x01, 4);
            break;

        case IDC_MODEM_SIGNAL:
            PtSend(PT_SVC_NAS, 0x20, NULL, 0);
            break;

        case IDC_MODEM_SERVING:
            PtSend(PT_SVC_NAS, 0x24, NULL, 0);
            break;

        case IDC_MODEM_SIM:
            PtSend(PT_SVC_UIM, 0x2F, NULL, 0);
            break;

        case IDC_MODEM_IDENTITY:
            PtSend(PT_SVC_DMS, 0x23, NULL, 0);
            PtSend(PT_SVC_DMS, 0x25, NULL, 0);
            break;

        case IDC_MODEM_SEND:
            PtSendRaw();
            break;

        case IDC_MODEM_CLEAR:
            SendDlgItemMessageW(PtModemWindow, IDC_MODEM_LOG, LB_RESETCONTENT, 0, 0);
            break;
    }

    PtModemStatus();
}
