/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Multimedia message retrieval and sending
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include "phonesvc.h"
#include "mmspdu.h"
#include <iphlpapi.h>

#define NDEBUG
#include <debug.h>

#define PH_MMS_ATTEMPTS             3
#define PH_MMS_RETRY_MS             10000
#define PH_MMS_NETWORK_WAIT_MS      30000
#define PH_MMS_NETWORK_POLL_MS      500
#define PH_MMS_STOP_WAIT_MS         5000
#define PH_MMS_RECENT               16
#define PH_MMS_MAX_SEND_FILE        (1024 * 1024)
#define PH_MMS_FILE_NAME_CHARS      48
#define PH_MMS_ACK_MAX              (PH_MMS_TEXT_MAX + 16)

typedef enum _PH_MMS_JOB_KIND
{
    PhMmsJobPush = 1,
    PhMmsJobSend
} PH_MMS_JOB_KIND;

/* Work for the MMS thread, Data is the push or the UTF-8 text to send */
typedef struct _PH_MMS_JOB
{
    LIST_ENTRY Link;
    DWORD Kind;
    DWORD MessageId;
    FILETIME Received;
    WCHAR Address[PHONEOM_MAX_NUMBER];
    WCHAR Path[PHONEOM_MAX_PATH];
    DWORD Size;
    BYTE Data[ANYSIZE_ARRAY];
} PH_MMS_JOB, *PPH_MMS_JOB;

/* Notifications already handled, the relay repeats one until it sees our response */
typedef struct _PH_MMS_SEEN
{
    CHAR Location[PH_MMS_URL_MAX];
    BOOL Retrieved;
    BOOL Placeholder;
} PH_MMS_SEEN, *PPH_MMS_SEEN;

typedef struct _PH_MMS_EXTENSION
{
    PCSTR ContentType;
    PCSTR Extension;
} PH_MMS_EXTENSION;

static const PH_MMS_EXTENSION PhMmsExtensions[] =
{
    { "image/jpeg", ".jpg" },
    { "image/jpg", ".jpg" },
    { "image/png", ".png" },
    { "image/gif", ".gif" },
    { "image/bmp", ".bmp" },
    { "image/vnd.wap.wbmp", ".wbmp" },
    { "application/smil", ".smil" },
    { "video/3gpp", ".3gp" },
    { "video/mp4", ".mp4" },
    { "audio/amr", ".amr" },
    { "audio/mpeg", ".mp3" },
    { "text/x-vcard", ".vcf" },
    { "text/vcard", ".vcf" },
    { "text/plain", ".txt" }
};

static CRITICAL_SECTION PhMmsLock;
static LIST_ENTRY PhMmsQueue;
static HANDLE PhMmsWake;
static HANDLE PhMmsThread;

/* Only the MMS thread touches these */
static PH_MMS_SEEN PhMmsSeen[PH_MMS_RECENT];
static DWORD PhMmsSeenNext;

static
BOOL
PhMmsIsType(
    _In_z_ PCSTR ContentType,
    _In_z_ PCSTR Expected)
{
    size_t Length = strlen(Expected);

    /* Parameters are already split off, but some relays send trailing text anyway */
    return !_strnicmp(ContentType, Expected, Length) &&
           (ContentType[Length] == ANSI_NULL || ContentType[Length] == ';');
}

static
PPH_MMS_SEEN
PhMmsFindSeen(
    _In_z_ PCSTR Location)
{
    DWORD i;

    for (i = 0; i < PH_MMS_RECENT; i++)
    {
        if (PhMmsSeen[i].Location[0] && !strcmp(PhMmsSeen[i].Location, Location))
            return &PhMmsSeen[i];
    }

    return NULL;
}

static
PPH_MMS_SEEN
PhMmsRemember(
    _In_z_ PCSTR Location)
{
    PPH_MMS_SEEN Seen = PhMmsFindSeen(Location);

    if (Seen)
        return Seen;

    Seen = &PhMmsSeen[PhMmsSeenNext];
    PhMmsSeenNext = (PhMmsSeenNext + 1) % PH_MMS_RECENT;
    ZeroMemory(Seen, sizeof(*Seen));
    StringCchCopyA(Seen->Location, _countof(Seen->Location), Location);
    return Seen;
}

/* TRUE when the service is stopping */
static
BOOL
PhMmsPause(
    _In_ DWORD Milliseconds)
{
    return WaitForSingleObject(PhStopEvent, Milliseconds) == WAIT_OBJECT_0;
}

/* TRUE once an adapter has an address and a gateway and the resolver has a server */
static
BOOL
PhMmsNetworkReady(VOID)
{
    PIP_ADAPTER_INFO Adapters, Adapter;
    PFIXED_INFO Params;
    ULONG Size = 0;
    BOOL Routed = FALSE, Named = FALSE;

    if (GetAdaptersInfo(NULL, &Size) != ERROR_BUFFER_OVERFLOW || !Size)
        return FALSE;
    Adapters = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Adapters)
        return FALSE;
    if (GetAdaptersInfo(Adapters, &Size) == NO_ERROR)
    {
        for (Adapter = Adapters; Adapter && !Routed; Adapter = Adapter->Next)
        {
            Routed = strcmp(Adapter->IpAddressList.IpAddress.String, "0.0.0.0") != 0 &&
                     Adapter->GatewayList.IpAddress.String[0] &&
                     strcmp(Adapter->GatewayList.IpAddress.String, "0.0.0.0") != 0;
        }
    }
    HeapFree(GetProcessHeap(), 0, Adapters);
    if (!Routed)
        return FALSE;

    Size = 0;
    if (GetNetworkParams(NULL, &Size) != ERROR_BUFFER_OVERFLOW || !Size)
        return FALSE;
    Params = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Params)
        return FALSE;
    if (GetNetworkParams(Params, &Size) == NO_ERROR)
    {
        Named = Params->DnsServerList.IpAddress.String[0] &&
                strcmp(Params->DnsServerList.IpAddress.String, "0.0.0.0") != 0;
    }
    HeapFree(GetProcessHeap(), 0, Params);
    return Named;
}

/* TCP/IP and DHCP may still be coming up on a fresh boot, so wait for them */
static
BOOL
PhMmsWaitForNetwork(VOID)
{
    DWORD Waited;

    for (Waited = 0; Waited < PH_MMS_NETWORK_WAIT_MS; Waited += PH_MMS_NETWORK_POLL_MS)
    {
        if (PhMmsNetworkReady())
        {
            if (Waited)
                DPRINT1("MMS data: network ready after %lu ms\n", Waited);
            return TRUE;
        }
        if (PhMmsPause(PH_MMS_NETWORK_POLL_MS))
            return FALSE;
    }

    DPRINT1("MMS data: no address or DNS server after %lu ms, trying anyway\n", Waited);
    return TRUE;
}

static
BOOL
PhMmsBringUpData(
    _Out_ PBOOL Raised)
{
    PHONEOM_DATA_STATUS Status;
    HRESULT hr;

    *Raised = FALSE;

    hr = PhConnectData(PHONEOM_DATA_QUERY, &Status);
    if (SUCCEEDED(hr) && Status.Connected)
        return PhMmsWaitForNetwork();

    DPRINT1("MMS data: session is down, connecting\n");
    hr = PhConnectData(PHONEOM_DATA_CONNECT, &Status);
    if (FAILED(hr) || !Status.Connected)
    {
        DPRINT1("MMS data: connect failed 0x%lx, connected %lu\n", hr, Status.Connected);
        return FALSE;
    }

    *Raised = TRUE;
    return PhMmsWaitForNetwork();
}

/* Drops a session we raised ourselves unless the user keeps data on */
static
VOID
PhMmsReleaseData(
    _In_ BOOL Raised)
{
    PHONEOM_DATA_CONFIG Config;
    PHONEOM_DATA_STATUS Status;

    if (!Raised)
        return;

    PhGetDataConfig(&Config);
    if (Config.Enabled)
        return;

    PhConnectData(PHONEOM_DATA_DISCONNECT, &Status);
}

/* One HTTP exchange with retries on transport errors */
static
HRESULT
PhMmsTransfer(
    _In_ const PH_MMS_CONFIG *Config,
    _In_z_ LPCWSTR Url,
    _In_reads_bytes_opt_(BodySize) const BYTE *Body,
    _In_ DWORD BodySize,
    _In_z_ PCSTR Step,
    _Outptr_result_bytebuffer_(*ResponseSize) PBYTE *Response,
    _Out_ PDWORD ResponseSize,
    _Out_ PDWORD HttpStatus)
{
    DWORD Attempt;
    HRESULT hr = E_FAIL;

    for (Attempt = 1; Attempt <= PH_MMS_ATTEMPTS; Attempt++)
    {
        hr = PhMmsHttpRequest(Config, Url, Body, BodySize, Response, ResponseSize, HttpStatus);
        if (SUCCEEDED(hr))
            return hr;

        DPRINT1("MMS %s: attempt %lu failed 0x%lx\n", Step, Attempt, hr);
        if (Attempt < PH_MMS_ATTEMPTS && PhMmsPause(PH_MMS_RETRY_MS))
            break;
    }

    return hr;
}

static
VOID
PhMmsAppendText(
    _Inout_ PPHONEOM_MESSAGE Message,
    _In_z_ LPCWSTR Text)
{
    if (!Text[0])
        return;

    if (Message->Text[0])
        StringCchCatW(Message->Text, _countof(Message->Text), L"\n");
    StringCchCatW(Message->Text, _countof(Message->Text), Text);
}

/* Address of the sender, the number without the type suffix */
static
VOID
PhMmsPickAddress(
    _Out_writes_z_(Chars) PWSTR Address,
    _In_ DWORD Chars,
    _In_z_ LPCWSTR First,
    _In_z_ LPCWSTR Second,
    _In_z_ LPCWSTR Fallback)
{
    LPCWSTR Source = First[0] ? First : (Second[0] ? Second : Fallback);

    StringCchCopyW(Address, Chars, Source);
}

/* Order prefix, a cleaned up part name and an extension that matches the type */
static
VOID
PhMmsPartFileName(
    _In_ const PH_MMS_PART *Part,
    _In_ DWORD Index,
    _Out_writes_z_(Chars) PWSTR Name,
    _In_ DWORD Chars)
{
    CHAR Clean[PH_MMS_FILE_NAME_CHARS];
    PCSTR Source, Slash, Extension = ".bin";
    DWORD i, Out = 0;
    BOOL Dot = FALSE;

    if (Part->Name[0])
        Source = Part->Name;
    else if (Part->Location[0])
        Source = Part->Location;
    else if (Part->ContentId[0])
        Source = Part->ContentId;
    else
        Source = "part";

    Slash = strrchr(Source, '/');
    if (Slash)
        Source = Slash + 1;
    Slash = strrchr(Source, '\\');
    if (Slash)
        Source = Slash + 1;

    for (i = 0; Source[i] && Out + 1 < _countof(Clean); i++)
    {
        CHAR c = Source[i];

        if (c == '<' || c == '>')
            continue;

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            c = '_';

        /* Never start with a dot so the file is not hidden or relative */
        if (c == '.' && Out == 0)
            c = '_';
        if (c == '.')
            Dot = TRUE;

        Clean[Out++] = c;
    }

    if (Out == 0)
        Clean[Out++] = 'p';
    Clean[Out] = ANSI_NULL;

    for (i = 0; i < _countof(PhMmsExtensions); i++)
    {
        if (PhMmsIsType(Part->ContentType, PhMmsExtensions[i].ContentType))
        {
            Extension = PhMmsExtensions[i].Extension;
            break;
        }
    }

    StringCchPrintfW(Name, Chars, L"%02lu_%S%S", Index, Clean, Dot ? "" : Extension);
}

static
BOOL
PhMmsWriteFile(
    _In_z_ LPCWSTR Path,
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size)
{
    HANDLE File;
    DWORD Written = 0;
    BOOL Ok;

    File = CreateFileW(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;

    Ok = WriteFile(File, Data, Size, &Written, NULL) && Written == Size;
    CloseHandle(File);
    if (!Ok)
        DeleteFileW(Path);
    return Ok;
}

/* Saves everything but the text parts, returns how many are media other than SMIL */
static
DWORD
PhMmsSaveParts(
    _In_ DWORD MessageId,
    _In_reads_(Count) const PH_MMS_PART *Parts,
    _In_ DWORD Count)
{
    WCHAR Folder[MAX_PATH], Path[MAX_PATH], Name[PH_MMS_FILE_NAME_CHARS + 16];
    DWORD Media = 0, Saved = 0, i;

    /* A store that was reset can leave files under a reused id */
    PhStoreRemoveAttachments(MessageId);

    if (!PhStoreGetAttachmentFolder(MessageId, TRUE, Folder, _countof(Folder)))
    {
        DPRINT1("MMS attachments: no folder for message %lu, the store is not persistent\n", MessageId);
        return 0;
    }

    for (i = 0; i < Count; i++)
    {
        if (PhMmsIsType(Parts[i].ContentType, "text/plain"))
            continue;

        PhMmsPartFileName(&Parts[i], i, Name, _countof(Name));
        if (FAILED(StringCchPrintfW(Path, _countof(Path), L"%s\\%s", Folder, Name)))
            continue;

        if (!PhMmsWriteFile(Path, Parts[i].Data, Parts[i].Size))
        {
            DPRINT1("MMS attachments: writing %S failed (%lu)\n", Path, GetLastError());
            continue;
        }

        Saved++;
        if (!PhMmsIsType(Parts[i].ContentType, "application/smil"))
            Media++;
    }

    if (Saved == 0)
        RemoveDirectoryW(Folder);

    DPRINT1("MMS attachments: %lu saved for message %lu, %lu media\n", Saved, MessageId, Media);
    return Media;
}

/* Text in the record, the rest as files next to the store */
static
BOOL
PhMmsStoreRetrieved(
    _In_ const PH_MMS_HEADERS *Notification,
    _In_ const PH_MMS_HEADERS *Retrieved,
    _In_ const PH_MMS_JOB *Job)
{
    PPHONEOM_MESSAGE Message = NULL;
    PPH_MMS_PART Parts = NULL;
    PWCHAR Text = NULL;
    DWORD Count = 0, Id, i;
    PCSTR Failure = NULL;
    BOOL Ok = FALSE;

    Parts = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, PH_MMS_MAX_PARTS * sizeof(*Parts));
    Message = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Message));
    Text = HeapAlloc(GetProcessHeap(), 0, PHONEOM_MAX_TEXT * sizeof(WCHAR));
    if (!Parts || !Message || !Text)
    {
        DPRINT1("MMS store failed: out of memory\n");
        goto Exit;
    }

    if (PhMmsIsType(Retrieved->ContentType, "application/vnd.wap.multipart.related") ||
        PhMmsIsType(Retrieved->ContentType, "application/vnd.wap.multipart.mixed") ||
        PhMmsIsType(Retrieved->ContentType, "application/vnd.wap.multipart.alternative"))
    {
        if (!PhMmsDecodeMultipart(Retrieved->Body, Retrieved->BodySize, Parts, PH_MMS_MAX_PARTS, &Count, &Failure))
        {
            DPRINT1("MMS parts decode failed: %s\n", Failure);
            goto Exit;
        }
    }
    else
    {
        /* A single body, the message content type is the part type */
        StringCchCopyA(Parts[0].ContentType, _countof(Parts[0].ContentType), Retrieved->ContentType);
        Parts[0].Data = Retrieved->Body;
        Parts[0].Size = Retrieved->BodySize;
        Count = 1;
    }

    DPRINT1("MMS parts: %lu in %s\n", Count, Retrieved->ContentType);
    for (i = 0; i < Count; i++)
    {
        DPRINT1("MMS part %lu: %s name '%s' location '%s' %lu bytes\n",
                i, Parts[i].ContentType, Parts[i].Name, Parts[i].Location, Parts[i].Size);
    }

    Message->Flags = PHONEOM_MESSAGE_INCOMING | PHONEOM_MESSAGE_MMS;
    PhMmsPickAddress(Message->Address, _countof(Message->Address), Retrieved->From, Notification->From, Job->Address);

    if (Retrieved->HaveDate)
        PhMmsUnixToFileTime(Retrieved->Date, &Message->Timestamp);
    else
        Message->Timestamp = Job->Received;

    PhMmsAppendText(Message, Retrieved->Subject[0] ? Retrieved->Subject : Notification->Subject);
    for (i = 0; i < Count; i++)
    {
        if (!PhMmsIsType(Parts[i].ContentType, "text/plain"))
            continue;

        PhMmsDecodeText(Parts[i].Data, Parts[i].Size, Parts[i].Charset, Text, PHONEOM_MAX_TEXT);
        PhMmsAppendText(Message, Text);
    }

    Id = PhStoreAdd(Message);
    if (Id == 0)
    {
        DPRINT1("MMS store failed: the message store is full\n");
        goto Exit;
    }

    if (PhMmsSaveParts(Id, Parts, Count) != 0)
        PhStoreUpdateFlags(Id, PHONEOM_MESSAGE_ATTACHMENTS, 0, Message);

    PhWakeDevice("a picture message");
    PhBroadcast(PhoneEventMessage, Message, sizeof(*Message));
    DPRINT1("MMS stored as message %lu from %S\n", Id, Message->Address);
    Ok = TRUE;

Exit:
    if (Text)
        HeapFree(GetProcessHeap(), 0, Text);
    if (Message)
        HeapFree(GetProcessHeap(), 0, Message);
    if (Parts)
        HeapFree(GetProcessHeap(), 0, Parts);
    return Ok;
}

/* Tells the user a message is waiting that could not be fetched */
static
VOID
PhMmsStorePlaceholder(
    _In_ const PH_MMS_HEADERS *Notification,
    _In_ const PH_MMS_JOB *Job)
{
    PPHONEOM_MESSAGE Message;

    Message = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Message));
    if (!Message)
        return;

    Message->Flags = PHONEOM_MESSAGE_INCOMING | PHONEOM_MESSAGE_MMS | PHONEOM_MESSAGE_FAILED;
    Message->Timestamp = Job->Received;
    PhMmsPickAddress(Message->Address, _countof(Message->Address), Notification->From, L"", Job->Address);
    StringCchPrintfW(Message->Text,
                     _countof(Message->Text),
                     L"Picture message of %I64u KB could not be downloaded",
                     (Notification->MessageSize + 1023) / 1024);
    if (Notification->Subject[0])
    {
        StringCchCatW(Message->Text, _countof(Message->Text), L"\n");
        StringCchCatW(Message->Text, _countof(Message->Text), Notification->Subject);
    }

    if (PhStoreAdd(Message) != 0)
    {
        PhWakeDevice("a picture message");
        PhBroadcast(PhoneEventMessage, Message, sizeof(*Message));
    }

    HeapFree(GetProcessHeap(), 0, Message);
}

/* m-notifyresp-ind tells the relay the message is retrieved and may be dropped */
static
VOID
PhMmsAcknowledge(
    _In_ const PH_MMS_CONFIG *Config,
    _In_ const PH_MMS_HEADERS *Notification)
{
    BYTE Pdu[PH_MMS_ACK_MAX];
    PBYTE Response;
    DWORD Size, ResponseSize, Status;
    HRESULT hr;

    Size = PhMmsEncodeNotifyResp(Notification->TransactionId, PH_MMS_STATUS_RETRIEVED, Pdu, sizeof(Pdu));
    if (Size == 0)
    {
        DPRINT1("MMS ack failed: transaction id %s does not fit\n", Notification->TransactionId);
        return;
    }

    hr = PhMmsTransfer(Config, Config->Url, Pdu, Size, "ack", &Response, &ResponseSize, &Status);
    if (FAILED(hr))
    {
        DPRINT1("MMS ack failed: POST to %S 0x%lx\n", Config->Url, hr);
        return;
    }

    DPRINT1("MMS ack sent for %s: HTTP %lu\n", Notification->TransactionId, Status);
    HeapFree(GetProcessHeap(), 0, Response);
}

static
VOID
PhMmsRetrieve(
    _In_ const PH_MMS_HEADERS *Notification,
    _In_ const PH_MMS_JOB *Job,
    _Inout_ PPH_MMS_SEEN Seen)
{
    WCHAR Url[PH_MMS_URL_MAX];
    PPH_MMS_CONFIG Config = NULL;
    PPH_MMS_HEADERS Retrieved = NULL;
    PBYTE Response = NULL;
    DWORD ResponseSize = 0, Status = 0;
    PCSTR Failure;
    BOOL Raised = FALSE, Done = FALSE;
    HRESULT hr;

    Config = HeapAlloc(GetProcessHeap(), 0, sizeof(*Config));
    Retrieved = HeapAlloc(GetProcessHeap(), 0, sizeof(*Retrieved));
    if (!Config || !Retrieved)
    {
        DPRINT1("MMS fetch failed: out of memory\n");
        goto Exit;
    }

    PhGetMmsConfig(Config);
    if (!MultiByteToWideChar(CP_UTF8, 0, Notification->ContentLocation, -1, Url, _countof(Url)))
    {
        DPRINT1("MMS fetch failed: Content-Location does not convert (%lu)\n", GetLastError());
        goto Exit;
    }

    if (!PhMmsBringUpData(&Raised))
    {
        DPRINT1("MMS fetch failed: no data session\n");
        goto Exit;
    }

    DPRINT1("MMS fetch start: %S\n", Url);
    hr = PhMmsTransfer(Config, Url, NULL, 0, "fetch", &Response, &ResponseSize, &Status);
    if (FAILED(hr))
    {
        DPRINT1("MMS fetch failed: GET 0x%lx\n", hr);
        goto Exit;
    }

    DPRINT1("MMS fetch: HTTP %lu, %lu bytes\n", Status, ResponseSize);
    if (Status != 200)
    {
        DPRINT1("MMS fetch failed: relay answered HTTP %lu\n", Status);
        goto Exit;
    }

    if (!PhMmsDecodeHeaders(Response, ResponseSize, Retrieved, &Failure))
    {
        DPRINT1("MMS retrieve decode failed: %s\n", Failure);
        goto Exit;
    }

    if (Retrieved->MessageType != PH_MMS_RETRIEVE_CONF)
    {
        DPRINT1("MMS retrieve decode failed: PDU type 0x%02x is not m-retrieve-conf\n", Retrieved->MessageType);
        goto Exit;
    }

    /* Retrieve status Ok is 0x80, an absent status means Ok too */
    if (Retrieved->Status != 0 && Retrieved->Status != PH_MMS_RESPONSE_OK)
    {
        DPRINT1("MMS retrieve failed: retrieve status 0x%02x\n", Retrieved->Status);
        goto Exit;
    }

    if (!PhMmsStoreRetrieved(Notification, Retrieved, Job))
        goto Exit;

    Seen->Retrieved = TRUE;
    Done = TRUE;
    PhMmsAcknowledge(Config, Notification);

Exit:
    if (!Done && !Seen->Placeholder)
    {
        PhMmsStorePlaceholder(Notification, Job);
        Seen->Placeholder = TRUE;
    }

    PhMmsReleaseData(Raised);

    if (Response)
        HeapFree(GetProcessHeap(), 0, Response);
    if (Retrieved)
        HeapFree(GetProcessHeap(), 0, Retrieved);
    if (Config)
        HeapFree(GetProcessHeap(), 0, Config);
}

static
VOID
PhMmsHandlePush(
    _In_ const PH_MMS_JOB *Job)
{
    CHAR Type[PH_MMS_TYPE_MAX];
    PPH_MMS_HEADERS Notification;
    PPH_MMS_CONFIG Config;
    PPH_MMS_SEEN Seen;
    const BYTE *Body;
    DWORD BodySize;
    PCSTR Failure;

    if (!PhWspDecodePush(Job->Data, Job->Size, Type, _countof(Type), &Body, &BodySize, &Failure))
    {
        DPRINT1("MMS push decode failed: %s\n", Failure);
        return;
    }

    if (!PhMmsIsType(Type, "application/vnd.wap.mms-message"))
    {
        DPRINT1("MMS push decode: content type %s is not an MMS PDU, ignored\n", Type);
        return;
    }

    Notification = HeapAlloc(GetProcessHeap(), 0, sizeof(*Notification));
    if (!Notification)
    {
        DPRINT1("MMS notification decode failed: out of memory\n");
        return;
    }

    if (!PhMmsDecodeHeaders(Body, BodySize, Notification, &Failure))
    {
        DPRINT1("MMS notification decode failed: %s\n", Failure);
        goto Exit;
    }

    if (Notification->MessageType != PH_MMS_NOTIFICATION_IND)
    {
        /* Delivery and read reports for sent messages land here */
        DPRINT1("MMS push carries PDU type 0x%02x status 0x%02x, ignored\n",
                Notification->MessageType, Notification->Status);
        goto Exit;
    }

    if (!Notification->ContentLocation[0] || !Notification->TransactionId[0])
    {
        DPRINT1("MMS notification decode failed: no Content-Location or Transaction-ID\n");
        goto Exit;
    }

    DPRINT1("MMS notification: tid %s from %S size %I64u expiry %I64u%s url %s\n",
            Notification->TransactionId,
            Notification->From[0] ? Notification->From : Job->Address,
            Notification->MessageSize,
            Notification->Expiry,
            Notification->ExpiryRelative ? "s relative" : "",
            Notification->ContentLocation);

    Seen = PhMmsRemember(Notification->ContentLocation);
    if (Seen->Retrieved)
    {
        /* Repeated because our response was lost, answer it again */
        DPRINT1("MMS notification for %s already retrieved, acknowledging again\n", Notification->ContentLocation);
        Config = HeapAlloc(GetProcessHeap(), 0, sizeof(*Config));
        if (Config)
        {
            PhGetMmsConfig(Config);
            PhMmsAcknowledge(Config, Notification);
            HeapFree(GetProcessHeap(), 0, Config);
        }
        goto Exit;
    }

    PhMmsRetrieve(Notification, Job, Seen);

Exit:
    HeapFree(GetProcessHeap(), 0, Notification);
}

static
VOID
PhMmsFinishSend(
    _In_ DWORD MessageId,
    _In_ BOOL Sent)
{
    PHONEOM_MESSAGE *Message;

    Message = HeapAlloc(GetProcessHeap(), 0, sizeof(*Message));
    if (!Message)
        return;

    if (PhStoreUpdateFlags(MessageId, Sent ? PHONEOM_MESSAGE_SENT : PHONEOM_MESSAGE_FAILED, 0, Message))
        PhBroadcast(PhoneEventMessageStatus, Message, sizeof(*Message));

    HeapFree(GetProcessHeap(), 0, Message);
}

static
PBYTE
PhMmsReadFile(
    _In_z_ LPCWSTR Path,
    _Out_ PDWORD Size)
{
    LARGE_INTEGER Length;
    HANDLE File;
    PBYTE Data = NULL;
    DWORD Read;

    *Size = 0;
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return NULL;

    if (GetFileSizeEx(File, &Length) && Length.QuadPart > 0 && Length.QuadPart <= PH_MMS_MAX_SEND_FILE)
    {
        Data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)Length.QuadPart);
        if (Data && (!ReadFile(File, Data, (DWORD)Length.QuadPart, &Read, NULL) || Read != (DWORD)Length.QuadPart))
        {
            HeapFree(GetProcessHeap(), 0, Data);
            Data = NULL;
        }
        if (Data)
            *Size = (DWORD)Length.QuadPart;
    }

    CloseHandle(File);
    return Data;
}

static
PCSTR
PhMmsTypeFromPath(
    _In_z_ LPCWSTR Path)
{
    LPCWSTR Dot = wcsrchr(Path, L'.');
    CHAR Extension[16];
    DWORD i;

    if (!Dot || FAILED(StringCchPrintfA(Extension, _countof(Extension), "%S", Dot)))
        return "application/octet-stream";

    for (i = 0; i < _countof(PhMmsExtensions); i++)
    {
        if (!_stricmp(PhMmsExtensions[i].Extension, Extension))
            return PhMmsExtensions[i].ContentType;
    }

    if (!_stricmp(Extension, ".jpeg"))
        return "image/jpeg";
    return "application/octet-stream";
}

static
VOID
PhMmsHandleSend(
    _In_ const PH_MMS_JOB *Job)
{
    CHAR To[PHONEOM_MAX_NUMBER], TransactionId[32], FileName[PH_MMS_FILE_NAME_CHARS];
    PH_MMS_OUT_PART Parts[2];
    PPH_MMS_CONFIG Config = NULL;
    PPH_MMS_HEADERS Reply = NULL;
    PBYTE File = NULL, Pdu = NULL, Response = NULL;
    DWORD FileSize = 0, PartCount = 0, Size, ResponseSize = 0, Status = 0, Out = 0, i;
    LPCWSTR Base;
    PCSTR Failure;
    BOOL Raised = FALSE, Sent = FALSE;
    HRESULT hr;

    /* Dialable characters only, the relay wants a bare number */
    for (i = 0; Job->Address[i] && Out + 1 < _countof(To); i++)
    {
        if ((Job->Address[i] >= L'0' && Job->Address[i] <= L'9') || (Job->Address[i] == L'+' && Out == 0))
            To[Out++] = (CHAR)Job->Address[i];
    }
    To[Out] = ANSI_NULL;
    if (Out == 0)
    {
        DPRINT1("MMS send failed: %S is not a phone number\n", Job->Address);
        goto Exit;
    }

    if (Job->Size)
    {
        Parts[PartCount].ContentType = "text/plain";
        Parts[PartCount].Name = "text_0.txt";
        Parts[PartCount].Charset = PH_CHARSET_UTF8;
        Parts[PartCount].Data = Job->Data;
        Parts[PartCount].Size = Job->Size;
        PartCount++;
    }

    if (Job->Path[0])
    {
        File = PhMmsReadFile(Job->Path, &FileSize);
        if (!File)
        {
            DPRINT1("MMS send failed: reading %S (%lu)\n", Job->Path, GetLastError());
            goto Exit;
        }

        /* The stored copy has the order prefix, the recipient sees the plain name */
        Base = wcsrchr(Job->Path, L'\\');
        Base = Base ? Base + 1 : Job->Path;
        if (wcslen(Base) > 3 && Base[2] == L'_')
            Base += 3;
        StringCchPrintfA(FileName, _countof(FileName), "%S", Base);

        Parts[PartCount].ContentType = PhMmsTypeFromPath(Job->Path);
        Parts[PartCount].Name = FileName;
        Parts[PartCount].Charset = 0;
        Parts[PartCount].Data = File;
        Parts[PartCount].Size = FileSize;
        PartCount++;
    }

    StringCchPrintfA(TransactionId, _countof(TransactionId), "T%08lx%08lx", Job->MessageId, (DWORD)GetTickCount64());

    Size = PhMmsEncodeSendReq(TransactionId, To, Parts, PartCount, NULL, 0);
    Pdu = Size ? HeapAlloc(GetProcessHeap(), 0, Size) : NULL;
    Config = HeapAlloc(GetProcessHeap(), 0, sizeof(*Config));
    Reply = HeapAlloc(GetProcessHeap(), 0, sizeof(*Reply));
    if (!Pdu || !Config || !Reply || PhMmsEncodeSendReq(TransactionId, To, Parts, PartCount, Pdu, Size) != Size)
    {
        DPRINT1("MMS send failed: building m-send-req of %lu bytes\n", Size);
        goto Exit;
    }

    PhGetMmsConfig(Config);
    if (!PhMmsBringUpData(&Raised))
    {
        DPRINT1("MMS send failed: no data session\n");
        goto Exit;
    }

    DPRINT1("MMS send start: %lu bytes to %s, tid %s\n", Size, To, TransactionId);
    hr = PhMmsTransfer(Config, Config->Url, Pdu, Size, "send", &Response, &ResponseSize, &Status);
    if (FAILED(hr))
    {
        DPRINT1("MMS send failed: POST 0x%lx\n", hr);
        goto Exit;
    }

    DPRINT1("MMS send: HTTP %lu, %lu bytes\n", Status, ResponseSize);
    if (Status != 200)
    {
        DPRINT1("MMS send failed: relay answered HTTP %lu\n", Status);
        goto Exit;
    }

    if (!PhMmsDecodeHeaders(Response, ResponseSize, Reply, &Failure))
    {
        DPRINT1("MMS send failed: m-send-conf decode, %s\n", Failure);
        goto Exit;
    }

    if (Reply->MessageType != PH_MMS_SEND_CONF || Reply->ResponseStatus != PH_MMS_RESPONSE_OK)
    {
        DPRINT1("MMS send failed: PDU type 0x%02x response status 0x%02x\n", Reply->MessageType, Reply->ResponseStatus);
        goto Exit;
    }

    DPRINT1("MMS send accepted, message id %s\n", Reply->MessageId);
    Sent = TRUE;

Exit:
    PhMmsReleaseData(Raised);
    PhMmsFinishSend(Job->MessageId, Sent);

    if (Response)
        HeapFree(GetProcessHeap(), 0, Response);
    if (Reply)
        HeapFree(GetProcessHeap(), 0, Reply);
    if (Config)
        HeapFree(GetProcessHeap(), 0, Config);
    if (Pdu)
        HeapFree(GetProcessHeap(), 0, Pdu);
    if (File)
        HeapFree(GetProcessHeap(), 0, File);
}

static
DWORD
WINAPI
PhMmsWorker(
    _In_ LPVOID Parameter)
{
    HANDLE Waits[2];
    PLIST_ENTRY Entry;
    PPH_MMS_JOB Job;

    UNREFERENCED_PARAMETER(Parameter);

    Waits[0] = PhStopEvent;
    Waits[1] = PhMmsWake;

    while (WaitForMultipleObjects(_countof(Waits), Waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
    {
        for (;;)
        {
            EnterCriticalSection(&PhMmsLock);
            Entry = IsListEmpty(&PhMmsQueue) ? NULL : RemoveHeadList(&PhMmsQueue);
            LeaveCriticalSection(&PhMmsLock);

            if (!Entry)
                break;

            Job = CONTAINING_RECORD(Entry, PH_MMS_JOB, Link);
            if (Job->Kind == PhMmsJobPush)
                PhMmsHandlePush(Job);
            else
                PhMmsHandleSend(Job);
            HeapFree(GetProcessHeap(), 0, Job);

            if (WaitForSingleObject(PhStopEvent, 0) == WAIT_OBJECT_0)
                return 0;
        }
    }

    return 0;
}

static
BOOL
PhMmsEnqueue(
    _In_ PPH_MMS_JOB Job)
{
    if (!PhMmsThread)
        return FALSE;

    EnterCriticalSection(&PhMmsLock);
    InsertTailList(&PhMmsQueue, &Job->Link);
    LeaveCriticalSection(&PhMmsLock);

    SetEvent(PhMmsWake);
    return TRUE;
}

static
PPH_MMS_JOB
PhMmsAllocateJob(
    _In_ DWORD Kind,
    _In_ DWORD DataSize)
{
    PPH_MMS_JOB Job;

    Job = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, FIELD_OFFSET(PH_MMS_JOB, Data) + max(DataSize, 1));
    if (Job)
    {
        Job->Kind = Kind;
        Job->Size = DataSize;
    }

    return Job;
}

BOOL
PhMmsPushReceived(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size,
    _In_z_ LPCWSTR Origin,
    _In_ const FILETIME *Received)
{
    PPH_MMS_JOB Job;

    Job = PhMmsAllocateJob(PhMmsJobPush, Size);
    if (!Job)
    {
        DPRINT1("MMS push dropped: out of memory\n");
        return FALSE;
    }

    CopyMemory(Job->Data, Data, Size);
    Job->Received = *Received;
    StringCchCopyW(Job->Address, _countof(Job->Address), Origin);

    if (!PhMmsEnqueue(Job))
    {
        DPRINT1("MMS push dropped: no MMS worker\n");
        HeapFree(GetProcessHeap(), 0, Job);
        return FALSE;
    }

    DPRINT1("MMS push detected: %lu bytes from %S queued\n", Size, Origin);
    return TRUE;
}

HRESULT
PhMmsSend(
    _In_z_ LPCWSTR Address,
    _In_z_ LPCWSTR Text,
    _In_z_ LPCWSTR Path,
    _Out_ PDWORD MessageId)
{
    WCHAR Folder[MAX_PATH], Copy[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA Attributes;
    PPHONEOM_MESSAGE Stored = NULL;
    PPH_MMS_JOB Job = NULL;
    LPCWSTR Base;
    int TextSize = 0;
    HRESULT hr = S_OK;

    *MessageId = 0;

    if (!Address[0] || (!Text[0] && !Path[0]))
        return E_INVALIDARG;

    if (Path[0])
    {
        if (!GetFileAttributesExW(Path, GetFileExInfoStandard, &Attributes))
            return HRESULT_FROM_WIN32(GetLastError());
        if (Attributes.nFileSizeHigh || Attributes.nFileSizeLow == 0 || Attributes.nFileSizeLow > PH_MMS_MAX_SEND_FILE)
            return HRESULT_FROM_WIN32(ERROR_MESSAGE_EXCEEDS_MAX_SIZE);
    }

    if (Text[0])
    {
        TextSize = WideCharToMultiByte(CP_UTF8, 0, Text, -1, NULL, 0, NULL, NULL);
        if (TextSize <= 1)
            return E_INVALIDARG;
        TextSize--;
    }

    Stored = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Stored));
    Job = PhMmsAllocateJob(PhMmsJobSend, (DWORD)TextSize);
    if (!Stored || !Job)
    {
        hr = E_OUTOFMEMORY;
        goto Exit;
    }

    if (TextSize)
        WideCharToMultiByte(CP_UTF8, 0, Text, -1, (LPSTR)Job->Data, TextSize, NULL, NULL);

    /* The outgoing copy shows in the conversation while it is in flight */
    StringCchCopyW(Stored->Address, _countof(Stored->Address), Address);
    StringCchCopyW(Stored->Text, _countof(Stored->Text), Text);
    Stored->Flags = PHONEOM_MESSAGE_READ | PHONEOM_MESSAGE_MMS;
    GetSystemTimeAsFileTime(&Stored->Timestamp);

    *MessageId = PhStoreAdd(Stored);
    if (*MessageId == 0)
    {
        hr = HRESULT_FROM_WIN32(ERROR_DISK_FULL);
        goto Exit;
    }

    Job->MessageId = *MessageId;
    StringCchCopyW(Job->Address, _countof(Job->Address), Address);

    if (Path[0])
    {
        /* Sent from a copy kept with the message, the caller may delete its file */
        Base = wcsrchr(Path, L'\\');
        Base = Base ? Base + 1 : Path;
        PhStoreRemoveAttachments(*MessageId);
        if (!PhStoreGetAttachmentFolder(*MessageId, TRUE, Folder, _countof(Folder)) ||
            FAILED(StringCchPrintfW(Copy, _countof(Copy), L"%s\\00_%s", Folder, Base)) ||
            !CopyFileW(Path, Copy, FALSE))
        {
            DPRINT1("MMS send: keeping a copy of %S failed (%lu), sending the original\n", Path, GetLastError());
            StringCchCopyW(Job->Path, _countof(Job->Path), Path);
        }
        else
        {
            StringCchCopyW(Job->Path, _countof(Job->Path), Copy);
            PhStoreUpdateFlags(*MessageId, PHONEOM_MESSAGE_ATTACHMENTS, 0, NULL);
        }
    }

    if (!PhMmsEnqueue(Job))
    {
        PhMmsFinishSend(*MessageId, FALSE);
        hr = E_UNEXPECTED;
        goto Exit;
    }

    Job = NULL;
    DPRINT1("MMS send queued as message %lu to %S\n", *MessageId, Address);

Exit:
    if (Job)
        HeapFree(GetProcessHeap(), 0, Job);
    if (Stored)
        HeapFree(GetProcessHeap(), 0, Stored);
    return hr;
}

VOID
PhMmsInitialize(VOID)
{
    InitializeCriticalSection(&PhMmsLock);
    InitializeListHead(&PhMmsQueue);

    PhMmsWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (PhMmsWake)
        PhMmsThread = CreateThread(NULL, 0, PhMmsWorker, NULL, 0, NULL);

    if (!PhMmsThread)
        DPRINT1("MMS worker did not start (%lu), picture messages are unavailable\n", GetLastError());
}

/* PhStopEvent must already be set */
VOID
PhMmsStop(VOID)
{
    if (!PhMmsThread)
        return;

    /* A transfer in progress is left to the process exit */
    if (WaitForSingleObject(PhMmsThread, PH_MMS_STOP_WAIT_MS) != WAIT_OBJECT_0)
        DPRINT1("MMS worker still busy at stop\n");

    CloseHandle(PhMmsThread);
    PhMmsThread = NULL;
}
