/*
 * PROJECT:     ReactOS GNSS Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Client of the modem QMI location service
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "gnsssvc.h"
#include <string.h>

#define NDEBUG
#include <debug.h>

/* The location service, announced by the modem as service 0x10 version 2 */
#define GS_LOC_SERVICE              0x10
#define GS_LOC_ROUTER_NAME          0x02

/* Requests */
#define GS_LOC_REGISTER_EVENTS      0x0021
#define GS_LOC_START                0x0022
#define GS_LOC_STOP                 0x0023
#define GS_LOC_SERVICE_REVISION     0x0032
#define GS_LOC_INJECT_UTC_TIME      0x0038

/* Indications */
#define GS_LOC_IND_POSITION         0x0024
#define GS_LOC_IND_SV_INFO          0x0025
#define GS_LOC_IND_TIME_WANTED      0x0028
#define GS_LOC_IND_ENGINE_STATE     0x002B
#define GS_LOC_IND_SESSION_STATE    0x002C

/* Event mask bits for GS_LOC_REGISTER_EVENTS */
#define GS_LOC_EVENT_POSITION       0x00000001
#define GS_LOC_EVENT_SV_INFO        0x00000002
#define GS_LOC_EVENT_TIME_WANTED    0x00000010
#define GS_LOC_EVENT_ENGINE_STATE   0x00000080
#define GS_LOC_EVENT_SESSION_STATE  0x00000100

#define GS_LOC_EVENTS               (GS_LOC_EVENT_POSITION | GS_LOC_EVENT_SV_INFO | GS_LOC_EVENT_TIME_WANTED | \
                                     GS_LOC_EVENT_ENGINE_STATE | GS_LOC_EVENT_SESSION_STATE)

/* Start request values */
#define GS_LOC_SESSION_ID           1
#define GS_LOC_RECUR_PERIODIC       1
#define GS_LOC_ACCURACY_HIGH        3
#define GS_LOC_INTERMEDIATE_ON      1

/* Position report session status */
#define GS_LOC_SESSION_OK           0
#define GS_LOC_SESSION_RUNNING      1
#define GS_LOC_SESSION_LOCKED       7

/* SV info entry, packed on the wire */
#define GS_SV_ENTRY_SIZE            28
#define GS_SV_VALID_SYSTEM          0x00000001
#define GS_SV_VALID_STATUS          0x00000008
#define GS_SV_VALID_SNR             0x00000080
#define GS_SV_STATUS_TRACK          3
#define GS_SV_SYSTEMS               8

#define GS_REQUEST_TIMEOUT_MS       5000
#define GS_RETRY_FAST_MS            3000
#define GS_RETRY_SLOW_MS            30000
#define GS_RETRY_FAST_TRIES         40
#define GS_RESTART_DELAY_MS         5000
#define GS_SV_LOG_MS                15000
#define GS_TIME_UNCERTAINTY_MS      10000

/* 1601 to 1970 in FILETIME units */
#define GS_UNIX_EPOCH_FILETIME      116444736000000000ULL

typedef struct _GS_LOC
{
    CRITICAL_SECTION Lock;
    HANDLE Worker;
    HANDLE Wake;

    BOOL RouterOpen;
    ULONG RouterTries;
    BOOL ServerKnown;
    GS_ADDR Server;
    BOOL Registered;
    BOOL SetupLogged;

    BOOL SessionActive;
    DWORD SessionInterval;
    ULONGLONG RestartAfter;
    BOOL TimeWanted;

    /* One request in flight, the worker thread is the only sender */
    USHORT NextTransaction;
    USHORT PendingTransaction;
    BOOL PendingGot;
    HANDLE PendingDone;
    ULONG PendingLength;
    UCHAR PendingResponse[GS_MESSAGE_MAX];

    NATIVEGNSS_FIX Fix;
    NATIVEGNSS_STATUS Status;
    BOOL LastHadPosition;
    BOOL FixDirty;
    BOOL StateDirty;
    DWORD LastState;

    ULONG Reports;
    ULONG SvReports;
    ULONGLONG SvLoggedAt;
    DWORD SvTrackedLogged;
} GS_LOC;

static GS_LOC GsLoc;

static const PCSTR GsSessionStatusNames[] =
{
    "fix", "in progress", "failure", "timeout", "user end", "bad parameter", "phone offline", "engine locked"
};

static
PCSTR
GsSessionStatusName(
    _In_ DWORD Status)
{
    return (Status < _countof(GsSessionStatusNames)) ? GsSessionStatusNames[Status] : "unknown";
}

/* Copies a record of at least Size bytes, FALSE when it is missing or short */
static
BOOL
GsTlvRead(
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length,
    _In_ UCHAR Type,
    _Out_writes_bytes_(Size) PVOID Out,
    _In_ USHORT Size)
{
    const VOID *Value;
    USHORT ValueLength;

    if (!QmiFindTlv(Message, Length, Type, &Value, &ValueLength) || ValueLength < Size)
        return FALSE;

    memcpy(Out, Value, Size);
    return TRUE;
}

static
DWORD
GsCurrentState(VOID)
{
    if (!GsLoc.Registered)
        return NativeGnssNoService;
    if (!GsLoc.SessionActive)
        return NativeGnssIdle;
    return GsLoc.LastHadPosition ? NativeGnssFixed : NativeGnssSearching;
}

/* Called with the lock held, marks a state event when the state moved */
static
VOID
GsNoteState(VOID)
{
    DWORD State = GsCurrentState();

    if (State != GsLoc.LastState)
    {
        GsLoc.LastState = State;
        GsLoc.StateDirty = TRUE;
        SetEvent(GsLoc.Wake);
    }
}

/* Sends one request and waits for its response, TRUE when the service took it */
static
BOOL
GsLocRequest(
    _Inout_ PQMI_ENCODER Encoder)
{
    QMI_RESULT Result;
    GS_ADDR Server;
    ULONG Length;
    USHORT MessageId = Encoder->Header->MessageId;
    BOOL Got;

    EnterCriticalSection(&GsLoc.Lock);
    if (!GsLoc.ServerKnown)
    {
        LeaveCriticalSection(&GsLoc.Lock);
        return FALSE;
    }
    Server = GsLoc.Server;
    if (++GsLoc.NextTransaction == 0)
        GsLoc.NextTransaction = 1;
    GsLoc.PendingTransaction = GsLoc.NextTransaction;
    GsLoc.PendingGot = FALSE;
    ResetEvent(GsLoc.PendingDone);
    Encoder->Header->Type = QMI_TYPE_REQUEST;
    Encoder->Header->TransactionId = GsLoc.PendingTransaction;
    LeaveCriticalSection(&GsLoc.Lock);

    Length = QmiEncodeEnd(Encoder);
    if (!GsRouterSend(&Server, Encoder->Buffer, Length))
    {
        DPRINT1("GNSS: LOC 0x%04X send failed (%lu)\n", MessageId, GetLastError());
        EnterCriticalSection(&GsLoc.Lock);
        GsLoc.PendingTransaction = 0;
        LeaveCriticalSection(&GsLoc.Lock);
        return FALSE;
    }

    WaitForSingleObject(GsLoc.PendingDone, GS_REQUEST_TIMEOUT_MS);

    EnterCriticalSection(&GsLoc.Lock);
    Got = GsLoc.PendingGot;
    GsLoc.PendingTransaction = 0;
    LeaveCriticalSection(&GsLoc.Lock);

    if (!Got)
    {
        DPRINT1("GNSS: LOC 0x%04X got no response\n", MessageId);
        return FALSE;
    }

    if (!QmiGetResult(GsLoc.PendingResponse, GsLoc.PendingLength, &Result))
    {
        DPRINT1("GNSS: LOC 0x%04X response has no result\n", MessageId);
        return FALSE;
    }

    if (Result.Result != QMI_RESULT_SUCCESS)
    {
        DPRINT1("GNSS: LOC 0x%04X refused, QMI error 0x%x\n", MessageId, Result.Error);
        return FALSE;
    }

    return TRUE;
}

static
BOOL
GsLocRegister(VOID)
{
    UCHAR Buffer[64];
    QMI_ENCODER Encoder;
    ULONGLONG Mask = GS_LOC_EVENTS;

    QmiEncodeBegin(&Encoder, Buffer, sizeof(Buffer), QMI_TYPE_REQUEST, 0, GS_LOC_REGISTER_EVENTS);
    QmiEncodeTlv(&Encoder, 0x01, &Mask, sizeof(Mask));
    if (!GsLocRequest(&Encoder))
        return FALSE;

    DPRINT1("GNSS: LOC accepted event registration, mask 0x%I64x\n", Mask);

    /* The revision comes back as an indication, it is only logged */
    QmiEncodeBegin(&Encoder, Buffer, sizeof(Buffer), QMI_TYPE_REQUEST, 0, GS_LOC_SERVICE_REVISION);
    GsLocRequest(&Encoder);
    return TRUE;
}

static
BOOL
GsLocStartSession(
    _In_ DWORD IntervalMs)
{
    UCHAR Buffer[64];
    QMI_ENCODER Encoder;
    UCHAR SessionId = GS_LOC_SESSION_ID;
    DWORD Recurrence = GS_LOC_RECUR_PERIODIC;
    DWORD Accuracy = GS_LOC_ACCURACY_HIGH;
    DWORD Intermediate = GS_LOC_INTERMEDIATE_ON;

    QmiEncodeBegin(&Encoder, Buffer, sizeof(Buffer), QMI_TYPE_REQUEST, 0, GS_LOC_START);
    QmiEncodeTlv(&Encoder, 0x01, &SessionId, sizeof(SessionId));
    QmiEncodeTlv(&Encoder, 0x10, &Recurrence, sizeof(Recurrence));
    QmiEncodeTlv(&Encoder, 0x11, &Accuracy, sizeof(Accuracy));
    QmiEncodeTlv(&Encoder, 0x12, &Intermediate, sizeof(Intermediate));
    QmiEncodeTlv(&Encoder, 0x13, &IntervalMs, sizeof(IntervalMs));
    if (!GsLocRequest(&Encoder))
    {
        DPRINT1("GNSS: LOC refused to start a session\n");
        return FALSE;
    }

    DPRINT1("GNSS: LOC accepted start, session %u every %lu ms\n", SessionId, IntervalMs);
    return TRUE;
}

static
VOID
GsLocStopSession(VOID)
{
    UCHAR Buffer[32];
    QMI_ENCODER Encoder;
    UCHAR SessionId = GS_LOC_SESSION_ID;

    QmiEncodeBegin(&Encoder, Buffer, sizeof(Buffer), QMI_TYPE_REQUEST, 0, GS_LOC_STOP);
    QmiEncodeTlv(&Encoder, 0x01, &SessionId, sizeof(SessionId));
    if (GsLocRequest(&Encoder))
        DPRINT1("GNSS: LOC accepted stop, engine may go idle\n");
}

/* The engine asked for time, give it the system clock when that looks set */
static
VOID
GsLocInjectTime(VOID)
{
    UCHAR Buffer[48];
    QMI_ENCODER Encoder;
    SYSTEMTIME Now;
    ULARGE_INTEGER FileTime;
    ULONGLONG UnixMs;
    DWORD Uncertainty = GS_TIME_UNCERTAINTY_MS;

    GetSystemTime(&Now);
    if (Now.wYear < 2024)
    {
        DPRINT1("GNSS: engine wants time, system clock not set (%u), skipped\n", Now.wYear);
        return;
    }

    GetSystemTimeAsFileTime((LPFILETIME)&FileTime);
    UnixMs = (FileTime.QuadPart - GS_UNIX_EPOCH_FILETIME) / 10000;

    QmiEncodeBegin(&Encoder, Buffer, sizeof(Buffer), QMI_TYPE_REQUEST, 0, GS_LOC_INJECT_UTC_TIME);
    QmiEncodeTlv(&Encoder, 0x01, &UnixMs, sizeof(UnixMs));
    QmiEncodeTlv(&Encoder, 0x02, &Uncertainty, sizeof(Uncertainty));
    if (GsLocRequest(&Encoder))
        DPRINT1("GNSS: injected system time, %lu ms uncertainty\n", Uncertainty);
}

static
VOID
GsLocPosition(
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    NATIVEGNSS_FIX Fix;
    DWORD Session = MAXDWORD;
    FLOAT Value, Dop[3];
    ULONGLONG UtcMs;
    const UCHAR *Used;
    USHORT UsedLength;
    ULONG i, Count, Report;
    BOOL Log;

    ZeroMemory(&Fix, sizeof(Fix));
    GsTlvRead(Message, Length, 0x01, &Session, sizeof(Session));

    if (GsTlvRead(Message, Length, 0x10, &Fix.Latitude, sizeof(Fix.Latitude)) &&
        GsTlvRead(Message, Length, 0x11, &Fix.Longitude, sizeof(Fix.Longitude)))
    {
        Fix.Flags |= NATIVEGNSS_FIX_POSITION;
    }

    if (GsTlvRead(Message, Length, 0x12, &Fix.HorizontalAccuracy, sizeof(Fix.HorizontalAccuracy)))
        Fix.Flags |= NATIVEGNSS_FIX_ACCURACY;
    if (GsTlvRead(Message, Length, 0x18, &Fix.Speed, sizeof(Fix.Speed)))
        Fix.Flags |= NATIVEGNSS_FIX_SPEED;

    if (GsTlvRead(Message, Length, 0x1B, &Value, sizeof(Value)))
    {
        Fix.Altitude = Value;
        Fix.Flags |= NATIVEGNSS_FIX_ALTITUDE;
    }
    else if (GsTlvRead(Message, Length, 0x1A, &Value, sizeof(Value)))
    {
        Fix.Altitude = Value;
        Fix.Flags |= NATIVEGNSS_FIX_ALTITUDE | NATIVEGNSS_FIX_ALTITUDE_WGS84;
    }

    if (GsTlvRead(Message, Length, 0x1C, &Fix.VerticalAccuracy, sizeof(Fix.VerticalAccuracy)))
        Fix.Flags |= NATIVEGNSS_FIX_ALT_ACCURACY;
    if (GsTlvRead(Message, Length, 0x20, &Fix.Heading, sizeof(Fix.Heading)))
        Fix.Flags |= NATIVEGNSS_FIX_HEADING;

    if (GsTlvRead(Message, Length, 0x24, Dop, sizeof(Dop)))
    {
        Fix.Pdop = Dop[0];
        Fix.Hdop = Dop[1];
        Fix.Vdop = Dop[2];
        Fix.Flags |= NATIVEGNSS_FIX_DOP;
    }

    if (GsTlvRead(Message, Length, 0x25, &UtcMs, sizeof(UtcMs)))
    {
        ULARGE_INTEGER Time;

        Time.QuadPart = UtcMs * 10000 + GS_UNIX_EPOCH_FILETIME;
        Fix.UtcTime.dwLowDateTime = Time.LowPart;
        Fix.UtcTime.dwHighDateTime = Time.HighPart;
        Fix.Flags |= NATIVEGNSS_FIX_TIME;
    }

    /* Satellites used: a count byte then that many 16 bit ids */
    if (QmiFindTlv(Message, Length, 0x2C, (const VOID **)&Used, &UsedLength) && UsedLength >= 1)
    {
        Count = min(Used[0], (UsedLength - 1) / sizeof(WORD));
        Fix.SatellitesUsed = Count;
        for (i = 0; i < Count && i < NATIVEGNSS_MAX_USED_SV; i++)
            memcpy(&Fix.UsedSv[i], Used + 1 + i * sizeof(WORD), sizeof(WORD));
        Fix.Flags |= NATIVEGNSS_FIX_SATELLITES;
    }

    if (Session != GS_LOC_SESSION_OK)
        Fix.Flags |= NATIVEGNSS_FIX_INTERMEDIATE;

    GetSystemTimeAsFileTime(&Fix.Received);

    EnterCriticalSection(&GsLoc.Lock);
    Report = ++GsLoc.Reports;
    GsLoc.Status.LastSessionStatus = Session;
    Log = (Report <= 3) || (Report % 30) == 0;

    if (Fix.Flags & NATIVEGNSS_FIX_POSITION)
    {
        if (!GsLoc.LastHadPosition)
            Log = TRUE;

        Fix.FixId = GsLoc.Fix.FixId + 1;
        GsLoc.Fix = Fix;
        GsLoc.LastHadPosition = TRUE;
        if (Session == GS_LOC_SESSION_OK)
            GsLoc.Status.LastFix = Fix.Received;
        GsLoc.FixDirty = TRUE;
        SetEvent(GsLoc.Wake);
    }
    else
    {
        GsLoc.LastHadPosition = FALSE;
    }
    GsNoteState();
    LeaveCriticalSection(&GsLoc.Lock);

    if (Session == GS_LOC_SESSION_LOCKED && Report <= 3)
        DPRINT1("GNSS: the modem reports the GPS engine locked\n");

    if (!Log)
        return;

    if (Fix.Flags & NATIVEGNSS_FIX_POSITION)
    {
        DPRINT1("GNSS: report %lu %s: lat %ld lon %ld (1e-6 deg) alt %ld m acc %ld m sats %lu\n",
                Report, GsSessionStatusName(Session),
                (LONG)(Fix.Latitude * 1000000.0), (LONG)(Fix.Longitude * 1000000.0),
                (LONG)Fix.Altitude, (LONG)Fix.HorizontalAccuracy, Fix.SatellitesUsed);
    }
    else
    {
        DPRINT1("GNSS: report %lu %s (%lu), no position yet\n", Report, GsSessionStatusName(Session), Session);
    }
}

static
VOID
GsLocSvInfo(
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    const UCHAR *List, *Entry;
    USHORT ListLength;
    ULONG i, Count, Tracked = 0;
    ULONG PerSystem[GS_SV_SYSTEMS];
    DWORD Valid, System, State;
    FLOAT Snr, Best = 0.0f;
    ULONGLONG Now;
    BOOL Log;

    ZeroMemory(PerSystem, sizeof(PerSystem));

    if (!QmiFindTlv(Message, Length, 0x10, (const VOID **)&List, &ListLength) || ListLength < 1)
        return;

    Count = List[0];
    if (ListLength < 1 + Count * GS_SV_ENTRY_SIZE)
    {
        DPRINT1("GNSS: SV list of %lu entries in %u bytes, layout differs\n", Count, ListLength);
        return;
    }

    for (i = 0; i < Count; i++)
    {
        Entry = List + 1 + i * GS_SV_ENTRY_SIZE;
        memcpy(&Valid, Entry, sizeof(Valid));
        memcpy(&System, Entry + 4, sizeof(System));
        memcpy(&State, Entry + 11, sizeof(State));
        memcpy(&Snr, Entry + 24, sizeof(Snr));

        if (!(Valid & GS_SV_VALID_STATUS) || State != GS_SV_STATUS_TRACK)
            continue;

        Tracked++;
        if ((Valid & GS_SV_VALID_SYSTEM) && System < GS_SV_SYSTEMS)
            PerSystem[System]++;
        if ((Valid & GS_SV_VALID_SNR) && Snr > Best)
            Best = Snr;
    }

    Now = GetTickCount64();

    EnterCriticalSection(&GsLoc.Lock);
    GsLoc.Status.SatellitesInView = Count;
    GsLoc.Status.SatellitesTracked = Tracked;
    GsLoc.Status.BestSignal = Best;
    GsLoc.SvReports++;
    Log = GsLoc.SvReports == 1 ||
          (Tracked != 0) != (GsLoc.SvTrackedLogged != 0) ||
          Now - GsLoc.SvLoggedAt >= GS_SV_LOG_MS;
    if (Log)
    {
        GsLoc.SvLoggedAt = Now;
        GsLoc.SvTrackedLogged = Tracked;
    }
    LeaveCriticalSection(&GsLoc.Lock);

    if (!Log)
        return;

    DPRINT1("GNSS: SV in view %lu tracked %lu best %ld dB-Hz (GPS %lu GLO %lu GAL %lu BDS %lu QZSS %lu SBAS %lu)\n",
            Count, Tracked, (LONG)Best,
            PerSystem[1], PerSystem[5], PerSystem[2], PerSystem[4] + PerSystem[6], PerSystem[7], PerSystem[3]);
}

static
VOID
GsLocIndication(
    _In_ USHORT MessageId,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    DWORD Value = 0, Status = 0;
    const CHAR *Text;
    USHORT TextLength;

    switch (MessageId)
    {
        case GS_LOC_IND_POSITION:
            GsLocPosition(Message, Length);
            break;

        case GS_LOC_IND_SV_INFO:
            GsLocSvInfo(Message, Length);
            break;

        case GS_LOC_IND_TIME_WANTED:
            EnterCriticalSection(&GsLoc.Lock);
            GsLoc.TimeWanted = TRUE;
            LeaveCriticalSection(&GsLoc.Lock);
            SetEvent(GsLoc.Wake);
            break;

        case GS_LOC_IND_ENGINE_STATE:
            GsTlvRead(Message, Length, 0x01, &Value, sizeof(Value));
            EnterCriticalSection(&GsLoc.Lock);
            GsLoc.Status.EngineOn = (Value == 1);
            LeaveCriticalSection(&GsLoc.Lock);
            DPRINT1("GNSS: engine %s\n", Value == 1 ? "on" : "off");
            break;

        case GS_LOC_IND_SESSION_STATE:
            GsTlvRead(Message, Length, 0x01, &Value, sizeof(Value));
            DPRINT1("GNSS: fix session %s\n", Value == 1 ? "started" : "finished");

            /* The engine ended a session we still want, the worker starts it again */
            EnterCriticalSection(&GsLoc.Lock);
            if (Value == 2 && GsLoc.SessionActive)
            {
                GsLoc.SessionActive = FALSE;
                GsLoc.LastHadPosition = FALSE;
                GsLoc.RestartAfter = GetTickCount64() + GS_RESTART_DELAY_MS;
                GsNoteState();
            }
            LeaveCriticalSection(&GsLoc.Lock);
            break;

        case GS_LOC_SERVICE_REVISION:
            GsTlvRead(Message, Length, 0x01, &Status, sizeof(Status));
            GsTlvRead(Message, Length, 0x02, &Value, sizeof(Value));
            EnterCriticalSection(&GsLoc.Lock);
            GsLoc.Status.ServiceRevision = Value;
            LeaveCriticalSection(&GsLoc.Lock);
            DPRINT1("GNSS: LOC service revision %lu (status %lu)\n", Value, Status);
            if (QmiFindTlv(Message, Length, 0x12, (const VOID **)&Text, &TextLength) && TextLength)
                DPRINT1("GNSS: engine software %.*s\n", (int)TextLength, Text);
            break;

        case GS_LOC_INJECT_UTC_TIME:
            GsTlvRead(Message, Length, 0x01, &Status, sizeof(Status));
            DPRINT1("GNSS: time injection status %lu\n", Status);
            break;

        default:
            break;
    }
}

VOID
GsLocMessage(
    _In_ const GS_ADDR *Source,
    _In_reads_bytes_(Length) const VOID *Message,
    _In_ ULONG Length)
{
    const QMI_HEADER *Header = Message;
    BOOL Ours;

    if (!QmiMessageValid(Message, Length))
        return;

    EnterCriticalSection(&GsLoc.Lock);
    Ours = GsLoc.ServerKnown &&
           GsLoc.Server.Node == Source->Node &&
           GsLoc.Server.Port == Source->Port;

    if (Ours &&
        Header->Type == QMI_TYPE_RESPONSE &&
        GsLoc.PendingTransaction != 0 &&
        Header->TransactionId == GsLoc.PendingTransaction &&
        !GsLoc.PendingGot)
    {
        GsLoc.PendingLength = min(Length, sizeof(GsLoc.PendingResponse));
        memcpy(GsLoc.PendingResponse, Message, GsLoc.PendingLength);
        GsLoc.PendingGot = TRUE;
        SetEvent(GsLoc.PendingDone);
        LeaveCriticalSection(&GsLoc.Lock);
        return;
    }
    LeaveCriticalSection(&GsLoc.Lock);

    if (Ours && Header->Type == QMI_TYPE_INDICATION)
        GsLocIndication(Header->MessageId, Message, Length);
}

VOID
GsLocServerEvent(
    _In_ BOOL Up,
    _In_ ULONG ServiceId,
    _In_ ULONG Instance,
    _In_ const GS_ADDR *Addr)
{
    if (ServiceId != GS_LOC_SERVICE)
        return;

    EnterCriticalSection(&GsLoc.Lock);
    if (Up)
    {
        if (!GsLoc.ServerKnown && Instance == GS_LOC_ROUTER_NAME)
        {
            GsLoc.Server = *Addr;
            GsLoc.ServerKnown = TRUE;
            DPRINT1("GNSS: LOC service up at %lu:%lu\n", Addr->Node, Addr->Port);
        }
    }
    else if (GsLoc.ServerKnown && GsLoc.Server.Node == Addr->Node && GsLoc.Server.Port == Addr->Port)
    {
        DPRINT1("GNSS: LOC service at %lu:%lu went away\n", Addr->Node, Addr->Port);
        GsLoc.ServerKnown = FALSE;
        GsLoc.Registered = FALSE;
        GsLoc.SessionActive = FALSE;
        GsLoc.LastHadPosition = FALSE;
        GsLoc.Status.EngineOn = FALSE;
        GsNoteState();

        /* A request in flight will not be answered now */
        SetEvent(GsLoc.PendingDone);
    }
    LeaveCriticalSection(&GsLoc.Lock);

    SetEvent(GsLoc.Wake);
}

/* One pass of the worker, returns how long to sleep before the next */
static
DWORD
GsLocStep(VOID)
{
    NATIVEGNSS_STATUS Status;
    NATIVEGNSS_FIX Fix;
    GS_ADDR Addr;
    DWORD Demand, Clients, Retry;
    BOOL Active, FixDirty, StateDirty, TimeWanted;
    DWORD Interval;
    ULONGLONG RestartAfter;

    Retry = (GsLoc.RouterTries < GS_RETRY_FAST_TRIES) ? GS_RETRY_FAST_MS : GS_RETRY_SLOW_MS;

    if (!GsLoc.RouterOpen)
    {
        if (!GsRouterOpen())
        {
            if (GsLoc.RouterTries++ == 0)
                DPRINT1("GNSS: IPC router not available yet (%lu), retrying\n", GetLastError());
            return Retry;
        }

        if (!GsRouterStartThreads())
        {
            DPRINT1("GNSS: router threads failed (%lu)\n", GetLastError());
            GsRouterStopThreads();
            GsRouterClose();
            GsLoc.RouterTries++;
            return Retry;
        }

        GsLoc.RouterOpen = TRUE;
        GsLoc.RouterTries = 0;
        DPRINT1("GNSS: IPC router port open\n");
    }

    EnterCriticalSection(&GsLoc.Lock);
    Active = GsLoc.ServerKnown;
    LeaveCriticalSection(&GsLoc.Lock);

    if (!Active)
    {
        if (!GsRouterFindServer(GS_LOC_SERVICE, GS_LOC_ROUTER_NAME, &Addr))
        {
            if (GsLoc.RouterTries++ == 0)
                DPRINT1("GNSS: waiting for the modem LOC service (0x%x)\n", GS_LOC_SERVICE);
            return Retry;
        }

        GsLocServerEvent(TRUE, GS_LOC_SERVICE, GS_LOC_ROUTER_NAME, &Addr);
        GsLoc.RouterTries = 0;
    }

    if (!GsLoc.Registered)
    {
        if (!GsLocRegister())
        {
            GsLoc.RouterTries++;
            return Retry;
        }

        EnterCriticalSection(&GsLoc.Lock);
        GsLoc.Registered = TRUE;
        GsNoteState();
        LeaveCriticalSection(&GsLoc.Lock);
    }

    Demand = GsPipeDemand(&Clients);

    EnterCriticalSection(&GsLoc.Lock);
    Active = GsLoc.SessionActive;
    Interval = GsLoc.SessionInterval;
    RestartAfter = GsLoc.RestartAfter;
    TimeWanted = GsLoc.TimeWanted;
    GsLoc.TimeWanted = FALSE;
    GsLoc.Status.Clients = Clients;
    LeaveCriticalSection(&GsLoc.Lock);

    Retry = INFINITE;

    if (Demand && (!Active || Interval != Demand))
    {
        if (!Active && GetTickCount64() < RestartAfter)
        {
            Retry = (DWORD)(RestartAfter - GetTickCount64());
        }
        else if (GsLocStartSession(Demand))
        {
            EnterCriticalSection(&GsLoc.Lock);
            GsLoc.SessionActive = TRUE;
            GsLoc.SessionInterval = Demand;
            GsLoc.Status.IntervalMs = Demand;
            GsNoteState();
            LeaveCriticalSection(&GsLoc.Lock);
        }
        else
        {
            Retry = GS_RESTART_DELAY_MS;
        }
    }
    else if (!Demand && Active)
    {
        /* Nobody wants location, let the engine sleep */
        EnterCriticalSection(&GsLoc.Lock);
        GsLoc.SessionActive = FALSE;
        GsLoc.LastHadPosition = FALSE;
        GsLoc.Status.IntervalMs = 0;
        GsNoteState();
        LeaveCriticalSection(&GsLoc.Lock);

        GsLocStopSession();
    }

    if (TimeWanted)
        GsLocInjectTime();

    EnterCriticalSection(&GsLoc.Lock);
    FixDirty = GsLoc.FixDirty;
    StateDirty = GsLoc.StateDirty;
    GsLoc.FixDirty = FALSE;
    GsLoc.StateDirty = FALSE;
    Fix = GsLoc.Fix;
    LeaveCriticalSection(&GsLoc.Lock);

    if (StateDirty || FixDirty)
    {
        GsLocGetStatus(&Status);
        if (StateDirty)
        {
            DPRINT1("GNSS: state %lu, %lu client(s)\n", Status.State, Status.Clients);
            GsPipeBroadcast(NativeGnssEventState, &Status, &Fix);
        }
        if (FixDirty)
            GsPipeBroadcast(NativeGnssEventFix, &Status, &Fix);
    }

    return Retry;
}

static
DWORD
WINAPI
GsLocWorker(
    _In_ LPVOID Parameter)
{
    HANDLE Waits[2];
    DWORD Timeout = 0;
    BOOL Active;

    UNREFERENCED_PARAMETER(Parameter);

    Waits[0] = GsStopEvent;
    Waits[1] = GsLoc.Wake;

    while (WaitForMultipleObjects(_countof(Waits), Waits, FALSE, Timeout) != WAIT_OBJECT_0)
        Timeout = GsLocStep();

    /* Leave the engine off behind us, the router runs until GsLocStop */
    EnterCriticalSection(&GsLoc.Lock);
    Active = GsLoc.SessionActive;
    GsLoc.SessionActive = FALSE;
    LeaveCriticalSection(&GsLoc.Lock);

    if (Active)
        GsLocStopSession();

    return 0;
}

/* Runs before the pipe opens, clients read the state from the start */
BOOL
GsLocInit(VOID)
{
    InitializeCriticalSection(&GsLoc.Lock);
    GsLoc.LastState = NativeGnssNoService;

    GsLoc.Wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    GsLoc.PendingDone = CreateEventW(NULL, TRUE, FALSE, NULL);
    return GsLoc.Wake != NULL && GsLoc.PendingDone != NULL;
}

BOOL
GsLocStart(VOID)
{
    GsLoc.Worker = CreateThread(NULL, 0, GsLocWorker, NULL, 0, NULL);
    return GsLoc.Worker != NULL;
}

/* GsStopEvent must already be set */
VOID
GsLocStop(VOID)
{
    if (GsLoc.Worker)
    {
        WaitForSingleObject(GsLoc.Worker, INFINITE);
        CloseHandle(GsLoc.Worker);
        GsLoc.Worker = NULL;
    }

    if (GsLoc.RouterOpen)
    {
        GsRouterStopThreads();
        GsRouterClose();
        GsLoc.RouterOpen = FALSE;
    }
}

VOID
GsLocDemandChanged(VOID)
{
    if (GsLoc.Wake)
        SetEvent(GsLoc.Wake);
}

VOID
GsLocGetFix(
    _Out_ PNATIVEGNSS_FIX Fix)
{
    EnterCriticalSection(&GsLoc.Lock);
    *Fix = GsLoc.Fix;
    LeaveCriticalSection(&GsLoc.Lock);
}

VOID
GsLocGetStatus(
    _Out_ PNATIVEGNSS_STATUS Status)
{
    EnterCriticalSection(&GsLoc.Lock);
    *Status = GsLoc.Status;
    Status->State = GsCurrentState();
    LeaveCriticalSection(&GsLoc.Lock);
}
