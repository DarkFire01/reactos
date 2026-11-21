/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     WSP push and MMS binary encapsulation codec
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#pragma once

#define PH_MMS_TEXT_MAX             128
#define PH_MMS_URL_MAX              512
#define PH_MMS_TYPE_MAX             96
#define PH_MMS_MAX_PARTS            16

/* X-Mms-Message-Type */
#define PH_MMS_SEND_REQ             0x80
#define PH_MMS_SEND_CONF            0x81
#define PH_MMS_NOTIFICATION_IND     0x82
#define PH_MMS_NOTIFYRESP_IND       0x83
#define PH_MMS_RETRIEVE_CONF        0x84
#define PH_MMS_ACKNOWLEDGE_IND      0x85
#define PH_MMS_DELIVERY_IND         0x86

/* X-Mms-Status */
#define PH_MMS_STATUS_RETRIEVED     0x81
#define PH_MMS_STATUS_DEFERRED      0x83

/* X-Mms-MMS-Version 1.2 as a short integer */
#define PH_MMS_VERSION_1_2          0x92

/* X-Mms-Response-Status Ok */
#define PH_MMS_RESPONSE_OK          0x80

/* IANA MIBenum charsets */
#define PH_CHARSET_UNKNOWN          0
#define PH_CHARSET_US_ASCII         3
#define PH_CHARSET_LATIN1           4
#define PH_CHARSET_UTF8             106
#define PH_CHARSET_UCS2             1000
#define PH_CHARSET_UTF16BE          1013
#define PH_CHARSET_UTF16LE          1014
#define PH_CHARSET_UTF16            1015

/* Headers of one MMS PDU, only the fields this service uses */
typedef struct _PH_MMS_HEADERS
{
    BYTE MessageType;
    BYTE Version;
    BYTE Status;
    BYTE ResponseStatus;
    BOOL HaveSize;
    BOOL HaveExpiry;
    BOOL ExpiryRelative;
    BOOL HaveDate;
    ULONGLONG MessageSize;
    ULONGLONG Expiry;
    ULONGLONG Date;
    CHAR TransactionId[PH_MMS_TEXT_MAX];
    CHAR MessageId[PH_MMS_TEXT_MAX];
    WCHAR From[PH_MMS_TEXT_MAX];
    WCHAR Subject[PH_MMS_TEXT_MAX];
    CHAR ContentLocation[PH_MMS_URL_MAX];
    CHAR ContentType[PH_MMS_TYPE_MAX];
    const BYTE *Body;
    DWORD BodySize;
} PH_MMS_HEADERS, *PPH_MMS_HEADERS;

/* One entry of a WSP multipart body */
typedef struct _PH_MMS_PART
{
    CHAR ContentType[PH_MMS_TYPE_MAX];
    CHAR Name[PH_MMS_TEXT_MAX];
    CHAR ContentId[PH_MMS_TEXT_MAX];
    CHAR Location[PH_MMS_TEXT_MAX];
    DWORD Charset;
    const BYTE *Data;
    DWORD Size;
} PH_MMS_PART, *PPH_MMS_PART;

/* Content of a part that m-send-req carries */
typedef struct _PH_MMS_OUT_PART
{
    PCSTR ContentType;
    PCSTR Name;
    DWORD Charset;
    const BYTE *Data;
    DWORD Size;
} PH_MMS_OUT_PART, *PPH_MMS_OUT_PART;

BOOL
PhWspDecodePush(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size,
    _Out_writes_z_(TypeChars) PCHAR ContentType,
    _In_ DWORD TypeChars,
    _Outptr_result_bytebuffer_(*BodySize) const BYTE **Body,
    _Out_ PDWORD BodySize,
    _Out_ PCSTR *Failure);

BOOL
PhMmsDecodeHeaders(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size,
    _Out_ PPH_MMS_HEADERS Headers,
    _Out_ PCSTR *Failure);

BOOL
PhMmsDecodeMultipart(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size,
    _Out_writes_to_(MaxParts, *Count) PPH_MMS_PART Parts,
    _In_ DWORD MaxParts,
    _Out_ PDWORD Count,
    _Out_ PCSTR *Failure);

DWORD
PhMmsDecodeText(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size,
    _In_ DWORD Charset,
    _Out_writes_z_(Chars) PWCHAR Text,
    _In_ DWORD Chars);

DWORD
PhMmsEncodeNotifyResp(
    _In_z_ PCSTR TransactionId,
    _In_ BYTE Status,
    _Out_writes_bytes_to_(MaxSize, return) PBYTE Out,
    _In_ DWORD MaxSize);

DWORD
PhMmsEncodeSendReq(
    _In_z_ PCSTR TransactionId,
    _In_z_ PCSTR To,
    _In_reads_(PartCount) const PH_MMS_OUT_PART *Parts,
    _In_ DWORD PartCount,
    _Out_writes_bytes_to_opt_(MaxSize, return) PBYTE Out,
    _In_ DWORD MaxSize);

/* Seconds since 1970 to a FILETIME */
VOID
PhMmsUnixToFileTime(
    _In_ ULONGLONG Seconds,
    _Out_ PFILETIME Time);
