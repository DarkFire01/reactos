/*
 * PROJECT:     ReactOS Phone Service
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     WSP push and MMS binary encapsulation codec
 * COPYRIGHT:   Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#ifdef PH_MMS_OFFLINE
#include <windows.h>
#include <strsafe.h>
#else
#include "phonesvc.h"
#endif
#include <string.h>
#include <wchar.h>

#include "mmspdu.h"

/* WSP PDU types */
#define PH_WSP_PUSH                 0x06
#define PH_WSP_CONFIRMED_PUSH       0x07

/* Value-length escape that is followed by a uintvar */
#define PH_WSP_LENGTH_QUOTE         31
#define PH_WSP_TEXT_QUOTE           0x7F
#define PH_WSP_STRING_QUOTE         0x22

/* MMS header field codes, without the high bit */
#define PH_MMSF_CONTENT_LOCATION    0x03
#define PH_MMSF_CONTENT_TYPE        0x04
#define PH_MMSF_DATE                0x05
#define PH_MMSF_EXPIRY              0x08
#define PH_MMSF_FROM                0x09
#define PH_MMSF_MESSAGE_ID          0x0B
#define PH_MMSF_MESSAGE_TYPE        0x0C
#define PH_MMSF_VERSION             0x0D
#define PH_MMSF_MESSAGE_SIZE        0x0E
#define PH_MMSF_RESPONSE_STATUS     0x12
#define PH_MMSF_STATUS              0x15
#define PH_MMSF_SUBJECT             0x16
#define PH_MMSF_TO                  0x17
#define PH_MMSF_TRANSACTION_ID      0x18
#define PH_MMSF_RETRIEVE_STATUS     0x19

/* From and Expiry tokens */
#define PH_MMS_ADDRESS_PRESENT      0x80
#define PH_MMS_INSERT_ADDRESS       0x81
#define PH_MMS_ABSOLUTE_TIME        0x80

/* WSP part header field codes, without the high bit */
#define PH_WSPF_CONTENT_LOCATION    0x0E
#define PH_WSPF_DISPOSITION         0x2E
#define PH_WSPF_CONTENT_ID          0x40
#define PH_WSPF_DISPOSITION_14      0x45

/* WSP content type parameters, without the high bit */
#define PH_WSPP_Q                   0x00
#define PH_WSPP_CHARSET             0x01
#define PH_WSPP_NAME                0x05
#define PH_WSPP_FILENAME            0x06
#define PH_WSPP_NAME_14             0x17
#define PH_WSPP_FILENAME_14         0x18

typedef struct _PH_WSP_CURSOR
{
    const BYTE *Data;
    DWORD Size;
    DWORD Offset;
    PCSTR Failure;
} PH_WSP_CURSOR, *PPH_WSP_CURSOR;

typedef struct _PH_WSP_MEDIA
{
    BYTE Code;
    PCSTR Name;
} PH_WSP_MEDIA;

/* Well-known media types from the WSP content type assignments */
static const PH_WSP_MEDIA PhWspMedia[] =
{
    { 0x00, "*/*" },
    { 0x01, "text/*" },
    { 0x02, "text/html" },
    { 0x03, "text/plain" },
    { 0x06, "text/x-vcalendar" },
    { 0x07, "text/x-vcard" },
    { 0x08, "text/vnd.wap.wml" },
    { 0x0B, "multipart/*" },
    { 0x0C, "multipart/mixed" },
    { 0x0F, "multipart/alternative" },
    { 0x10, "application/*" },
    { 0x1C, "image/*" },
    { 0x1D, "image/gif" },
    { 0x1E, "image/jpeg" },
    { 0x1F, "image/tiff" },
    { 0x20, "image/png" },
    { 0x21, "image/vnd.wap.wbmp" },
    { 0x22, "application/vnd.wap.multipart.*" },
    { 0x23, "application/vnd.wap.multipart.mixed" },
    { 0x26, "application/vnd.wap.multipart.alternative" },
    { 0x27, "application/xml" },
    { 0x28, "text/xml" },
    { 0x2D, "text/vnd.wap.si" },
    { 0x2E, "application/vnd.wap.sic" },
    { 0x33, "application/vnd.wap.multipart.related" },
    { 0x3B, "application/xhtml+xml" },
    { 0x3D, "text/css" },
    { 0x3E, "application/vnd.wap.mms-message" }
};

static
BOOL
PhWspFail(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _In_z_ PCSTR Reason)
{
    if (!Cursor->Failure)
        Cursor->Failure = Reason;
    return FALSE;
}

static
DWORD
PhWspLeft(
    _In_ const PH_WSP_CURSOR *Cursor)
{
    return Cursor->Size - Cursor->Offset;
}

static
BOOL
PhWspPeek(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _Out_ PBYTE Value)
{
    if (Cursor->Offset >= Cursor->Size)
        return PhWspFail(Cursor, "truncated value");

    *Value = Cursor->Data[Cursor->Offset];
    return TRUE;
}

static
BOOL
PhWspByte(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _Out_ PBYTE Value)
{
    if (!PhWspPeek(Cursor, Value))
        return FALSE;

    Cursor->Offset++;
    return TRUE;
}

static
BOOL
PhWspSkip(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _In_ DWORD Count)
{
    if (Count > PhWspLeft(Cursor))
        return PhWspFail(Cursor, "value runs past the end");

    Cursor->Offset += Count;
    return TRUE;
}

/* Seven bits per octet, high bit set on every octet but the last */
static
BOOL
PhWspUintvar(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _Out_ PDWORD Value)
{
    DWORD Result = 0, i;
    BYTE Octet;

    *Value = 0;
    for (i = 0; i < 5; i++)
    {
        if (!PhWspByte(Cursor, &Octet))
            return PhWspFail(Cursor, "truncated uintvar");

        Result = (Result << 7) | (Octet & 0x7F);
        if (!(Octet & 0x80))
        {
            *Value = Result;
            return TRUE;
        }
    }

    return PhWspFail(Cursor, "uintvar longer than 32 bits");
}

/* A short length of 0 to 30, or the quote byte and a uintvar */
static
BOOL
PhWspValueLength(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _Out_ PDWORD Length)
{
    BYTE Octet;

    *Length = 0;
    if (!PhWspByte(Cursor, &Octet))
        return FALSE;

    if (Octet < PH_WSP_LENGTH_QUOTE)
        *Length = Octet;
    else if (Octet != PH_WSP_LENGTH_QUOTE || !PhWspUintvar(Cursor, Length))
        return PhWspFail(Cursor, "bad value length");

    if (*Length > PhWspLeft(Cursor))
        return PhWspFail(Cursor, "value length runs past the end");

    return TRUE;
}

/* Short length followed by up to eight big endian octets */
static
BOOL
PhWspLongInteger(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _Out_ PULONGLONG Value)
{
    BYTE Count, Octet;

    *Value = 0;
    if (!PhWspByte(Cursor, &Count))
        return FALSE;

    if (Count == 0 || Count > 8)
        return PhWspFail(Cursor, "bad long integer length");

    while (Count--)
    {
        if (!PhWspByte(Cursor, &Octet))
            return FALSE;
        *Value = (*Value << 8) | Octet;
    }

    return TRUE;
}

/* Short integer when the high bit is set, otherwise a long integer */
static
BOOL
PhWspInteger(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _Out_ PULONGLONG Value)
{
    BYTE Octet;

    *Value = 0;
    if (!PhWspPeek(Cursor, &Octet))
        return FALSE;

    if (Octet & 0x80)
    {
        Cursor->Offset++;
        *Value = Octet & 0x7F;
        return TRUE;
    }

    return PhWspLongInteger(Cursor, Value);
}

/* Text or quoted string up to its terminator, too long text is cut */
static
BOOL
PhWspText(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _Out_writes_z_(Chars) PCHAR Text,
    _In_ DWORD Chars)
{
    DWORD Out = 0;
    BYTE Octet;

    Text[0] = ANSI_NULL;
    if (!PhWspPeek(Cursor, &Octet))
        return FALSE;

    if (Octet == PH_WSP_TEXT_QUOTE || Octet == PH_WSP_STRING_QUOTE)
        Cursor->Offset++;

    for (;;)
    {
        if (!PhWspByte(Cursor, &Octet))
            return PhWspFail(Cursor, "unterminated text");

        if (Octet == 0)
            break;

        if (Out + 1 < Chars)
            Text[Out++] = (CHAR)Octet;
    }

    Text[Out] = ANSI_NULL;
    return TRUE;
}

/* Generic skip of one header value whatever its encoding */
static
BOOL
PhWspSkipValue(
    _Inout_ PPH_WSP_CURSOR Cursor)
{
    DWORD Length;
    BYTE Octet;

    if (!PhWspPeek(Cursor, &Octet))
        return FALSE;

    if (Octet <= PH_WSP_LENGTH_QUOTE)
        return PhWspValueLength(Cursor, &Length) && PhWspSkip(Cursor, Length);

    if (Octet & 0x80)
        return PhWspSkip(Cursor, 1);

    for (;;)
    {
        if (!PhWspByte(Cursor, &Octet))
            return PhWspFail(Cursor, "unterminated text");
        if (Octet == 0)
            return TRUE;
    }
}

static
DWORD
PhMmsWiden(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size,
    _Out_writes_z_(Chars) PWCHAR Text,
    _In_ DWORD Chars)
{
    DWORD i;

    for (i = 0; i < Size && i + 1 < Chars; i++)
        Text[i] = Data[i];
    Text[i] = UNICODE_NULL;
    return i;
}

DWORD
PhMmsDecodeText(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size,
    _In_ DWORD Charset,
    _Out_writes_z_(Chars) PWCHAR Text,
    _In_ DWORD Chars)
{
    BOOL BigEndian = TRUE;
    DWORD Count, i;
    PWCHAR Wide;
    int Length;

    Text[0] = UNICODE_NULL;
    if (Chars == 0)
        return 0;

    switch (Charset)
    {
        case PH_CHARSET_UCS2:
        case PH_CHARSET_UTF16:
        case PH_CHARSET_UTF16BE:
        case PH_CHARSET_UTF16LE:
            /* A byte order mark wins over the declared order */
            if (Charset == PH_CHARSET_UTF16LE)
                BigEndian = FALSE;
            if (Size >= 2 && Data[0] == 0xFE && Data[1] == 0xFF)
            {
                BigEndian = TRUE;
                Data += 2;
                Size -= 2;
            }
            else if (Size >= 2 && Data[0] == 0xFF && Data[1] == 0xFE)
            {
                BigEndian = FALSE;
                Data += 2;
                Size -= 2;
            }

            Count = min(Size / 2, Chars - 1);
            for (i = 0; i < Count; i++)
            {
                Text[i] = BigEndian ? (WCHAR)((Data[2 * i] << 8) | Data[2 * i + 1])
                                    : (WCHAR)((Data[2 * i + 1] << 8) | Data[2 * i]);
                if (Text[i] == UNICODE_NULL)
                    break;
            }
            Text[i] = UNICODE_NULL;
            return i;

        case PH_CHARSET_US_ASCII:
        case PH_CHARSET_LATIN1:
            return PhMmsWiden(Data, Size, Text, Chars);

        default:
            break;
    }

    /* UTF-8, and anything unknown that is valid UTF-8 */
    if (Size && Data[Size - 1] == 0)
        Size--;
    if (Size == 0)
        return 0;

    /* Invalid UTF-8 is most likely Latin-1 */
    Length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCSTR)Data, (int)Size, NULL, 0);
    if (Length <= 0)
        return PhMmsWiden(Data, Size, Text, Chars);

    if ((DWORD)Length < Chars)
    {
        Length = MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)Data, (int)Size, Text, (int)Chars - 1);
        Text[max(Length, 0)] = UNICODE_NULL;
        return (DWORD)max(Length, 0);
    }

    /* Longer than the buffer, convert all of it and keep the head */
    Wide = HeapAlloc(GetProcessHeap(), 0, Length * sizeof(WCHAR));
    if (!Wide)
        return 0;

    MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)Data, (int)Size, Wide, Length);
    CopyMemory(Text, Wide, (Chars - 1) * sizeof(WCHAR));
    Text[Chars - 1] = UNICODE_NULL;
    HeapFree(GetProcessHeap(), 0, Wide);
    return Chars - 1;
}

/* Text-string or value length, charset and text */
static
BOOL
PhWspEncodedString(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _Out_writes_z_(Chars) PWCHAR Text,
    _In_ DWORD Chars)
{
    ULONGLONG Charset;
    DWORD Length, End, Start;
    BYTE Octet;

    Text[0] = UNICODE_NULL;
    if (!PhWspPeek(Cursor, &Octet))
        return FALSE;

    if (Octet > PH_WSP_LENGTH_QUOTE)
    {
        Start = Cursor->Offset;
        if (Octet == PH_WSP_TEXT_QUOTE)
            Start++;
        if (!PhWspSkipValue(Cursor))
            return FALSE;
        PhMmsDecodeText(Cursor->Data + Start, Cursor->Offset - Start, PH_CHARSET_UTF8, Text, Chars);
        return TRUE;
    }

    if (!PhWspValueLength(Cursor, &Length))
        return FALSE;
    End = Cursor->Offset + Length;

    if (!PhWspInteger(Cursor, &Charset) || Cursor->Offset > End)
        return PhWspFail(Cursor, "bad encoded string charset");

    Start = Cursor->Offset;
    if (Start < End && Cursor->Data[Start] == PH_WSP_TEXT_QUOTE)
        Start++;

    PhMmsDecodeText(Cursor->Data + Start, End - Start, (DWORD)Charset, Text, Chars);
    Cursor->Offset = End;
    return TRUE;
}

static
PCSTR
PhWspMediaName(
    _In_ ULONGLONG Code)
{
    DWORD i;

    for (i = 0; i < _countof(PhWspMedia); i++)
    {
        if (PhWspMedia[i].Code == Code)
            return PhWspMedia[i].Name;
    }

    return "application/octet-stream";
}

static
LONG
PhWspMediaCode(
    _In_z_ PCSTR Name)
{
    DWORD i;

    for (i = 0; i < _countof(PhWspMedia); i++)
    {
        if (!_stricmp(PhWspMedia[i].Name, Name))
            return PhWspMedia[i].Code;
    }

    return -1;
}

static
DWORD
PhWspCharsetFromName(
    _In_z_ PCSTR Name)
{
    if (!_stricmp(Name, "utf-8"))
        return PH_CHARSET_UTF8;
    if (!_stricmp(Name, "us-ascii"))
        return PH_CHARSET_US_ASCII;
    if (!_stricmp(Name, "iso-8859-1"))
        return PH_CHARSET_LATIN1;
    if (!_stricmp(Name, "utf-16"))
        return PH_CHARSET_UTF16;
    if (!_stricmp(Name, "utf-16be"))
        return PH_CHARSET_UTF16BE;
    if (!_stricmp(Name, "utf-16le"))
        return PH_CHARSET_UTF16LE;
    if (!_stricmp(Name, "iso-10646-ucs-2"))
        return PH_CHARSET_UCS2;
    return PH_CHARSET_UNKNOWN;
}

/* One typed or untyped parameter, picks out the charset and the name */
static
BOOL
PhWspParameter(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _Inout_opt_ PDWORD Charset,
    _Out_writes_opt_z_(NameChars) PCHAR Name,
    _In_ DWORD NameChars)
{
    CHAR Token[32], Value[PH_MMS_TEXT_MAX];
    ULONGLONG Code, Number;
    DWORD Ignored;
    BYTE Octet;

    if (!PhWspPeek(Cursor, &Octet))
        return FALSE;

    if (Octet >= 0x20 && Octet < 0x80)
    {
        /* Untyped, a token name and a text or integer value */
        if (!PhWspText(Cursor, Token, _countof(Token)) || !PhWspPeek(Cursor, &Octet))
            return FALSE;

        if ((Octet & 0x80) || Octet <= PH_WSP_LENGTH_QUOTE)
            return PhWspInteger(Cursor, &Number);

        if (!PhWspText(Cursor, Value, _countof(Value)))
            return FALSE;

        if (Charset && !_stricmp(Token, "charset"))
            *Charset = PhWspCharsetFromName(Value);
        else if (Name && (!_stricmp(Token, "name") || !_stricmp(Token, "filename")) && !Name[0])
            StringCchCopyA(Name, NameChars, Value);
        return TRUE;
    }

    if (!PhWspInteger(Cursor, &Code))
        return FALSE;

    switch (Code)
    {
        case PH_WSPP_Q:
            return PhWspUintvar(Cursor, &Ignored);

        case PH_WSPP_CHARSET:
            if (!PhWspPeek(Cursor, &Octet))
                return FALSE;

            /* Any-charset is short integer zero */
            if (Octet >= 0x20 && Octet < 0x80)
            {
                if (!PhWspText(Cursor, Value, _countof(Value)))
                    return FALSE;
                Number = PhWspCharsetFromName(Value);
            }
            else if (!PhWspInteger(Cursor, &Number))
            {
                return FALSE;
            }

            if (Charset)
                *Charset = (DWORD)Number;
            return TRUE;

        case PH_WSPP_NAME:
        case PH_WSPP_FILENAME:
        case PH_WSPP_NAME_14:
        case PH_WSPP_FILENAME_14:
            if (!PhWspText(Cursor, Value, _countof(Value)))
                return FALSE;
            if (Name && !Name[0])
                StringCchCopyA(Name, NameChars, Value);
            return TRUE;

        default:
            return PhWspSkipValue(Cursor);
    }
}

/* Constrained media or value length, media type and parameters */
static
BOOL
PhWspContentType(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _Out_writes_z_(TypeChars) PCHAR Type,
    _In_ DWORD TypeChars,
    _Inout_opt_ PDWORD Charset,
    _Out_writes_opt_z_(NameChars) PCHAR Name,
    _In_ DWORD NameChars)
{
    ULONGLONG Code;
    DWORD Length, End;
    BYTE Octet;

    Type[0] = ANSI_NULL;
    if (!PhWspPeek(Cursor, &Octet))
        return PhWspFail(Cursor, "missing content type");

    if (Octet & 0x80)
    {
        Cursor->Offset++;
        StringCchCopyA(Type, TypeChars, PhWspMediaName(Octet & 0x7F));
        return TRUE;
    }

    if (Octet > PH_WSP_LENGTH_QUOTE)
        return PhWspText(Cursor, Type, TypeChars);

    if (!PhWspValueLength(Cursor, &Length))
        return FALSE;
    End = Cursor->Offset + Length;

    if (!PhWspPeek(Cursor, &Octet))
        return FALSE;

    if (Octet >= 0x20 && Octet < 0x80)
    {
        if (!PhWspText(Cursor, Type, TypeChars))
            return FALSE;
    }
    else
    {
        if (!PhWspInteger(Cursor, &Code))
            return FALSE;
        StringCchCopyA(Type, TypeChars, PhWspMediaName(Code));
    }

    while (Cursor->Offset < End)
    {
        if (!PhWspParameter(Cursor, Charset, Name, NameChars))
            return FALSE;
    }

    if (Cursor->Offset != End)
        return PhWspFail(Cursor, "content type parameters overrun");

    return TRUE;
}

BOOL
PhWspDecodePush(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size,
    _Out_writes_z_(TypeChars) PCHAR ContentType,
    _In_ DWORD TypeChars,
    _Outptr_result_bytebuffer_(*BodySize) const BYTE **Body,
    _Out_ PDWORD BodySize,
    _Out_ PCSTR *Failure)
{
    PH_WSP_CURSOR Cursor = { Data, Size, 0, NULL };
    DWORD HeadersLength, HeadersEnd;
    BYTE TransactionId, PduType;

    ContentType[0] = ANSI_NULL;
    *Body = NULL;
    *BodySize = 0;

    if (!PhWspByte(&Cursor, &TransactionId) || !PhWspByte(&Cursor, &PduType))
    {
        *Failure = "push shorter than its fixed header";
        return FALSE;
    }

    if (PduType != PH_WSP_PUSH && PduType != PH_WSP_CONFIRMED_PUSH)
    {
        *Failure = "WSP PDU is not a push";
        return FALSE;
    }

    if (!PhWspUintvar(&Cursor, &HeadersLength) || HeadersLength > PhWspLeft(&Cursor))
    {
        *Failure = Cursor.Failure ? Cursor.Failure : "push headers run past the end";
        return FALSE;
    }
    HeadersEnd = Cursor.Offset + HeadersLength;

    /* The content type opens the header block, the rest of it is not needed */
    if (!PhWspContentType(&Cursor, ContentType, TypeChars, NULL, NULL, 0) || Cursor.Offset > HeadersEnd)
    {
        *Failure = Cursor.Failure ? Cursor.Failure : "push content type overruns the headers";
        return FALSE;
    }

    *Body = Data + HeadersEnd;
    *BodySize = Size - HeadersEnd;
    *Failure = NULL;
    return TRUE;
}

/* From is a value length, then an address or the insert address token */
static
BOOL
PhMmsFrom(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _Out_writes_z_(Chars) PWCHAR Text,
    _In_ DWORD Chars)
{
    DWORD Length, End;
    PWCHAR Type;
    BYTE Token;

    Text[0] = UNICODE_NULL;
    if (!PhWspValueLength(Cursor, &Length))
        return FALSE;
    End = Cursor->Offset + Length;

    if (Length == 0 || !PhWspByte(Cursor, &Token))
        return PhWspFail(Cursor, "empty From");

    if (Token == PH_MMS_ADDRESS_PRESENT)
    {
        if (!PhWspEncodedString(Cursor, Text, Chars))
            return FALSE;

        /* Drop the address type suffix the relay adds */
        Type = wcschr(Text, L'/');
        if (Type && !_wcsnicmp(Type, L"/TYPE=", 6))
            *Type = UNICODE_NULL;
    }
    else if (Token != PH_MMS_INSERT_ADDRESS)
    {
        return PhWspFail(Cursor, "unknown From token");
    }

    if (Cursor->Offset > End)
        return PhWspFail(Cursor, "From overruns its length");

    Cursor->Offset = End;
    return TRUE;
}

static
BOOL
PhMmsExpiry(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _Inout_ PPH_MMS_HEADERS Headers)
{
    DWORD Length, End;
    BYTE Token;

    if (!PhWspValueLength(Cursor, &Length))
        return FALSE;
    End = Cursor->Offset + Length;

    if (Length == 0 || !PhWspByte(Cursor, &Token))
        return PhWspFail(Cursor, "empty Expiry");

    /* Absolute is a date, relative is delta seconds */
    if (Token == PH_MMS_ABSOLUTE_TIME)
    {
        if (!PhWspLongInteger(Cursor, &Headers->Expiry))
            return FALSE;
    }
    else if (!PhWspInteger(Cursor, &Headers->Expiry))
    {
        return FALSE;
    }

    if (Cursor->Offset > End)
        return PhWspFail(Cursor, "Expiry overruns its length");

    Headers->HaveExpiry = TRUE;
    Headers->ExpiryRelative = (Token != PH_MMS_ABSOLUTE_TIME);
    Cursor->Offset = End;
    return TRUE;
}

static
BOOL
PhMmsShortValue(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _Out_ PBYTE Value)
{
    BYTE Octet;

    if (!PhWspPeek(Cursor, &Octet))
        return FALSE;

    if (!(Octet & 0x80))
    {
        *Value = 0;
        return PhWspSkipValue(Cursor);
    }

    return PhWspByte(Cursor, Value);
}

BOOL
PhMmsDecodeHeaders(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size,
    _Out_ PPH_MMS_HEADERS Headers,
    _Out_ PCSTR *Failure)
{
    PH_WSP_CURSOR Cursor = { Data, Size, 0, NULL };
    CHAR Name[PH_MMS_TEXT_MAX];
    BYTE Field;
    BOOL Ok;

    ZeroMemory(Headers, sizeof(*Headers));
    *Failure = NULL;

    while (Cursor.Offset < Cursor.Size)
    {
        if (!PhWspByte(&Cursor, &Field))
            break;

        if (!(Field & 0x80))
        {
            /* Application header, a token name and a text value */
            Cursor.Offset--;
            Ok = PhWspText(&Cursor, Name, _countof(Name)) && PhWspSkipValue(&Cursor);
            if (!Ok)
                break;
            continue;
        }

        switch (Field & 0x7F)
        {
            case PH_MMSF_MESSAGE_TYPE:
                Ok = PhWspByte(&Cursor, &Headers->MessageType);
                break;

            case PH_MMSF_VERSION:
                Ok = PhMmsShortValue(&Cursor, &Headers->Version);
                break;

            case PH_MMSF_TRANSACTION_ID:
                Ok = PhWspText(&Cursor, Headers->TransactionId, _countof(Headers->TransactionId));
                break;

            case PH_MMSF_MESSAGE_ID:
                Ok = PhWspText(&Cursor, Headers->MessageId, _countof(Headers->MessageId));
                break;

            case PH_MMSF_FROM:
                Ok = PhMmsFrom(&Cursor, Headers->From, _countof(Headers->From));
                break;

            case PH_MMSF_SUBJECT:
                Ok = PhWspEncodedString(&Cursor, Headers->Subject, _countof(Headers->Subject));
                break;

            case PH_MMSF_MESSAGE_SIZE:
                Ok = PhWspLongInteger(&Cursor, &Headers->MessageSize);
                Headers->HaveSize = Ok;
                break;

            case PH_MMSF_EXPIRY:
                Ok = PhMmsExpiry(&Cursor, Headers);
                break;

            case PH_MMSF_DATE:
                Ok = PhWspLongInteger(&Cursor, &Headers->Date);
                Headers->HaveDate = Ok;
                break;

            case PH_MMSF_CONTENT_LOCATION:
                Ok = PhWspText(&Cursor, Headers->ContentLocation, _countof(Headers->ContentLocation));
                break;

            case PH_MMSF_STATUS:
            case PH_MMSF_RETRIEVE_STATUS:
                Ok = PhMmsShortValue(&Cursor, &Headers->Status);
                break;

            case PH_MMSF_RESPONSE_STATUS:
                Ok = PhMmsShortValue(&Cursor, &Headers->ResponseStatus);
                break;

            case PH_MMSF_CONTENT_TYPE:
                /* Always the last header, the message body follows it */
                Ok = PhWspContentType(&Cursor, Headers->ContentType, _countof(Headers->ContentType), NULL, NULL, 0);
                if (Ok)
                {
                    Headers->Body = Data + Cursor.Offset;
                    Headers->BodySize = Size - Cursor.Offset;
                    Cursor.Offset = Cursor.Size;
                }
                break;

            default:
                Ok = PhWspSkipValue(&Cursor);
                break;
        }

        if (!Ok)
            break;
    }

    if (Cursor.Failure)
    {
        *Failure = Cursor.Failure;
        return FALSE;
    }

    if (Headers->MessageType == 0)
    {
        *Failure = "no X-Mms-Message-Type";
        return FALSE;
    }

    return TRUE;
}

/* Part headers after the content type, only the naming ones are kept */
static
BOOL
PhMmsPartHeaders(
    _Inout_ PPH_WSP_CURSOR Cursor,
    _In_ DWORD End,
    _Inout_ PPH_MMS_PART Part)
{
    CHAR Name[32];
    DWORD Length, Stop;
    BYTE Octet;
    BOOL Ok;

    while (Cursor->Offset < End)
    {
        if (!PhWspByte(Cursor, &Octet))
            return FALSE;

        if (Octet == PH_WSP_TEXT_QUOTE)
        {
            /* Shift delimiter and the page it selects */
            Ok = PhWspSkip(Cursor, 1);
        }
        else if (Octet < 0x20)
        {
            /* Short cut shift to a header code page */
            Ok = TRUE;
        }
        else if (!(Octet & 0x80))
        {
            Cursor->Offset--;
            Ok = PhWspText(Cursor, Name, _countof(Name));
            if (!Ok)
                break;

            if (!_stricmp(Name, "Content-ID") && PhWspPeek(Cursor, &Octet) && Octet > PH_WSP_LENGTH_QUOTE && !(Octet & 0x80))
                Ok = PhWspText(Cursor, Part->ContentId, _countof(Part->ContentId));
            else if (!_stricmp(Name, "Content-Location") && PhWspPeek(Cursor, &Octet) && Octet > PH_WSP_LENGTH_QUOTE && !(Octet & 0x80))
                Ok = PhWspText(Cursor, Part->Location, _countof(Part->Location));
            else
                Ok = PhWspSkipValue(Cursor);
        }
        else
        {
            switch (Octet & 0x7F)
            {
                case PH_WSPF_CONTENT_LOCATION:
                    Ok = PhWspText(Cursor, Part->Location, _countof(Part->Location));
                    break;

                case PH_WSPF_CONTENT_ID:
                    Ok = PhWspText(Cursor, Part->ContentId, _countof(Part->ContentId));
                    break;

                case PH_WSPF_DISPOSITION:
                case PH_WSPF_DISPOSITION_14:
                    /* Disposition token, then parameters that may carry a file name */
                    Ok = PhWspValueLength(Cursor, &Length);
                    if (!Ok)
                        break;
                    Stop = Cursor->Offset + Length;
                    Ok = PhWspSkipValue(Cursor);
                    while (Ok && Cursor->Offset < Stop)
                        Ok = PhWspParameter(Cursor, NULL, Part->Name, _countof(Part->Name));
                    if (Ok && Cursor->Offset != Stop)
                        Ok = PhWspFail(Cursor, "disposition parameters overrun");
                    break;

                default:
                    Ok = PhWspSkipValue(Cursor);
                    break;
            }
        }

        if (!Ok)
            return FALSE;
    }

    if (Cursor->Offset != End)
        return PhWspFail(Cursor, "part headers overrun their length");

    return TRUE;
}

BOOL
PhMmsDecodeMultipart(
    _In_reads_bytes_(Size) const BYTE *Data,
    _In_ DWORD Size,
    _Out_writes_to_(MaxParts, *Count) PPH_MMS_PART Parts,
    _In_ DWORD MaxParts,
    _Out_ PDWORD Count,
    _Out_ PCSTR *Failure)
{
    PH_WSP_CURSOR Cursor = { Data, Size, 0, NULL };
    DWORD Entries, HeadersLength, DataLength, HeadersEnd, i;
    PPH_MMS_PART Part;

    *Count = 0;
    *Failure = NULL;

    if (!PhWspUintvar(&Cursor, &Entries))
    {
        *Failure = "no multipart entry count";
        return FALSE;
    }

    for (i = 0; i < Entries; i++)
    {
        if (!PhWspUintvar(&Cursor, &HeadersLength) || !PhWspUintvar(&Cursor, &DataLength))
            break;

        if (HeadersLength > PhWspLeft(&Cursor) || DataLength > PhWspLeft(&Cursor) - HeadersLength)
        {
            PhWspFail(&Cursor, "part runs past the end of the body");
            break;
        }

        HeadersEnd = Cursor.Offset + HeadersLength;

        /* Parts past the limit are dropped but still walked */
        if (*Count >= MaxParts)
        {
            Cursor.Offset = HeadersEnd + DataLength;
            continue;
        }

        Part = &Parts[*Count];
        ZeroMemory(Part, sizeof(*Part));

        if (!PhWspContentType(&Cursor, Part->ContentType, _countof(Part->ContentType),
                              &Part->Charset, Part->Name, _countof(Part->Name)) ||
            Cursor.Offset > HeadersEnd ||
            !PhMmsPartHeaders(&Cursor, HeadersEnd, Part))
        {
            PhWspFail(&Cursor, "bad part headers");
            break;
        }

        Part->Data = Data + Cursor.Offset;
        Part->Size = DataLength;
        Cursor.Offset += DataLength;
        (*Count)++;
    }

    if (Cursor.Failure)
    {
        *Failure = Cursor.Failure;
        return FALSE;
    }

    return TRUE;
}

typedef struct _PH_WSP_WRITER
{
    PBYTE Data;
    DWORD Size;
    DWORD Offset;
} PH_WSP_WRITER, *PPH_WSP_WRITER;

/* Writes past the end only count, so a NULL buffer measures */
static
VOID
PhWspPut(
    _Inout_ PPH_WSP_WRITER Writer,
    _In_reads_bytes_(Count) const VOID *Bytes,
    _In_ DWORD Count)
{
    if (Writer->Data && Writer->Offset + Count <= Writer->Size)
        CopyMemory(Writer->Data + Writer->Offset, Bytes, Count);
    Writer->Offset += Count;
}

static
VOID
PhWspPutByte(
    _Inout_ PPH_WSP_WRITER Writer,
    _In_ BYTE Value)
{
    PhWspPut(Writer, &Value, 1);
}

static
VOID
PhWspPutUintvar(
    _Inout_ PPH_WSP_WRITER Writer,
    _In_ DWORD Value)
{
    BYTE Octets[5];
    int Count = 0;

    do
    {
        Octets[Count++] = (BYTE)(Value & 0x7F);
        Value >>= 7;
    } while (Value && Count < (int)sizeof(Octets));

    while (Count > 1)
        PhWspPutByte(Writer, Octets[--Count] | 0x80);
    PhWspPutByte(Writer, Octets[0]);
}

static
VOID
PhWspPutLength(
    _Inout_ PPH_WSP_WRITER Writer,
    _In_ DWORD Length)
{
    if (Length < PH_WSP_LENGTH_QUOTE)
    {
        PhWspPutByte(Writer, (BYTE)Length);
        return;
    }

    PhWspPutByte(Writer, PH_WSP_LENGTH_QUOTE);
    PhWspPutUintvar(Writer, Length);
}

static
VOID
PhWspPutText(
    _Inout_ PPH_WSP_WRITER Writer,
    _In_z_ PCSTR Text)
{
    /* Text that starts with a high octet needs the quote */
    if ((BYTE)Text[0] & 0x80)
        PhWspPutByte(Writer, PH_WSP_TEXT_QUOTE);
    PhWspPut(Writer, Text, (DWORD)strlen(Text) + 1);
}

static
VOID
PhWspPutInteger(
    _Inout_ PPH_WSP_WRITER Writer,
    _In_ DWORD Value)
{
    BYTE Octets[4];
    BYTE Count = 0;

    if (Value < 0x80)
    {
        PhWspPutByte(Writer, (BYTE)(Value | 0x80));
        return;
    }

    while (Value)
    {
        Octets[Count++] = (BYTE)Value;
        Value >>= 8;
    }

    PhWspPutByte(Writer, Count);
    while (Count)
        PhWspPutByte(Writer, Octets[--Count]);
}

static
VOID
PhWspPutMedia(
    _Inout_ PPH_WSP_WRITER Writer,
    _In_z_ PCSTR Type)
{
    LONG Code = PhWspMediaCode(Type);

    if (Code >= 0)
        PhWspPutByte(Writer, (BYTE)(Code | 0x80));
    else
        PhWspPutText(Writer, Type);
}

DWORD
PhMmsEncodeNotifyResp(
    _In_z_ PCSTR TransactionId,
    _In_ BYTE Status,
    _Out_writes_bytes_to_(MaxSize, return) PBYTE Out,
    _In_ DWORD MaxSize)
{
    PH_WSP_WRITER Writer = { Out, MaxSize, 0 };

    PhWspPutByte(&Writer, 0x80 | PH_MMSF_MESSAGE_TYPE);
    PhWspPutByte(&Writer, PH_MMS_NOTIFYRESP_IND);
    PhWspPutByte(&Writer, 0x80 | PH_MMSF_TRANSACTION_ID);
    PhWspPutText(&Writer, TransactionId);
    PhWspPutByte(&Writer, 0x80 | PH_MMSF_VERSION);
    PhWspPutByte(&Writer, PH_MMS_VERSION_1_2);
    PhWspPutByte(&Writer, 0x80 | PH_MMSF_STATUS);
    PhWspPutByte(&Writer, Status);

    return (Writer.Offset <= MaxSize) ? Writer.Offset : 0;
}

/* Content type of one part, with the charset and name parameters */
static
VOID
PhWspPutPartType(
    _Inout_ PPH_WSP_WRITER Writer,
    _In_ const PH_MMS_OUT_PART *Part)
{
    PH_WSP_WRITER Measure = { NULL, 0, 0 };
    PPH_WSP_WRITER Pass;
    DWORD Round;

    /* First round measures the value, the second writes it */
    for (Round = 0; Round < 2; Round++)
    {
        Pass = Round ? Writer : &Measure;
        if (Round)
            PhWspPutLength(Writer, Measure.Offset);

        PhWspPutMedia(Pass, Part->ContentType);
        if (Part->Charset)
        {
            PhWspPutByte(Pass, 0x80 | PH_WSPP_CHARSET);
            PhWspPutInteger(Pass, Part->Charset);
        }
        if (Part->Name)
        {
            PhWspPutByte(Pass, 0x80 | PH_WSPP_NAME);
            PhWspPutText(Pass, Part->Name);
        }
    }
}

DWORD
PhMmsEncodeSendReq(
    _In_z_ PCSTR TransactionId,
    _In_z_ PCSTR To,
    _In_reads_(PartCount) const PH_MMS_OUT_PART *Parts,
    _In_ DWORD PartCount,
    _Out_writes_bytes_to_opt_(MaxSize, return) PBYTE Out,
    _In_ DWORD MaxSize)
{
    PH_WSP_WRITER Writer = { Out, MaxSize, 0 };
    PH_WSP_WRITER Measure;
    CHAR Address[PH_MMS_TEXT_MAX];
    DWORD i;

    if (FAILED(StringCchPrintfA(Address, _countof(Address), "%s/TYPE=PLMN", To)))
        return 0;

    PhWspPutByte(&Writer, 0x80 | PH_MMSF_MESSAGE_TYPE);
    PhWspPutByte(&Writer, PH_MMS_SEND_REQ);
    PhWspPutByte(&Writer, 0x80 | PH_MMSF_TRANSACTION_ID);
    PhWspPutText(&Writer, TransactionId);
    PhWspPutByte(&Writer, 0x80 | PH_MMSF_VERSION);
    PhWspPutByte(&Writer, PH_MMS_VERSION_1_2);

    /* The relay fills in our own number */
    PhWspPutByte(&Writer, 0x80 | PH_MMSF_FROM);
    PhWspPutLength(&Writer, 1);
    PhWspPutByte(&Writer, PH_MMS_INSERT_ADDRESS);

    PhWspPutByte(&Writer, 0x80 | PH_MMSF_TO);
    PhWspPutText(&Writer, Address);

    PhWspPutByte(&Writer, 0x80 | PH_MMSF_CONTENT_TYPE);
    PhWspPutMedia(&Writer, "application/vnd.wap.multipart.mixed");

    PhWspPutUintvar(&Writer, PartCount);
    for (i = 0; i < PartCount; i++)
    {
        /* Headers length covers the content type and the location header */
        Measure.Data = NULL;
        Measure.Size = 0;
        Measure.Offset = 0;
        PhWspPutPartType(&Measure, &Parts[i]);
        if (Parts[i].Name)
        {
            PhWspPutByte(&Measure, 0x80 | PH_WSPF_CONTENT_LOCATION);
            PhWspPutText(&Measure, Parts[i].Name);
        }

        PhWspPutUintvar(&Writer, Measure.Offset);
        PhWspPutUintvar(&Writer, Parts[i].Size);
        PhWspPutPartType(&Writer, &Parts[i]);
        if (Parts[i].Name)
        {
            PhWspPutByte(&Writer, 0x80 | PH_WSPF_CONTENT_LOCATION);
            PhWspPutText(&Writer, Parts[i].Name);
        }
        PhWspPut(&Writer, Parts[i].Data, Parts[i].Size);
    }

    if (Out && Writer.Offset > MaxSize)
        return 0;
    return Writer.Offset;
}

VOID
PhMmsUnixToFileTime(
    _In_ ULONGLONG Seconds,
    _Out_ PFILETIME Time)
{
    ULARGE_INTEGER Value;

    /* 1601 to 1970 in seconds, then 100 ns units */
    Value.QuadPart = (Seconds + 11644473600ULL) * 10000000ULL;
    Time->dwLowDateTime = Value.LowPart;
    Time->dwHighDateTime = Value.HighPart;
}
