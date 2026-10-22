/*
 * PROJECT:     ReactOS PSDK
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Trace Data Helper API definitions
 * COPYRIGHT:   Copyright 2026 Justin Miller (justin.miller@reactos.org)
 */

#pragma once

#ifndef _TDH_H_
#define _TDH_H_

#include <evntcons.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef ULONG TDHSTATUS;
#define TDHAPI TDHSTATUS WINAPI

typedef HANDLE TDH_HANDLE, *PTDH_HANDLE;

typedef struct _EVENT_MAP_ENTRY
{
    ULONG OutputOffset;
    union
    {
        ULONG Value;
        ULONG InputOffset;
    } DUMMYUNIONNAME;
} EVENT_MAP_ENTRY, *PEVENT_MAP_ENTRY;

typedef enum _MAP_FLAGS
{
    EVENTMAP_INFO_FLAG_MANIFEST_VALUEMAP = 0x1,
    EVENTMAP_INFO_FLAG_MANIFEST_BITMAP = 0x2,
    EVENTMAP_INFO_FLAG_MANIFEST_PATTERNMAP = 0x4,
    EVENTMAP_INFO_FLAG_WBEM_VALUEMAP = 0x8,
    EVENTMAP_INFO_FLAG_WBEM_BITMAP = 0x10,
    EVENTMAP_INFO_FLAG_WBEM_FLAG = 0x20,
    EVENTMAP_INFO_FLAG_WBEM_NO_MAP = 0x40
} MAP_FLAGS;

typedef enum _MAP_VALUETYPE
{
    EVENTMAP_ENTRY_VALUETYPE_ULONG,
    EVENTMAP_ENTRY_VALUETYPE_STRING
} MAP_VALUETYPE;

typedef struct _EVENT_MAP_INFO
{
    ULONG NameOffset;
    MAP_FLAGS Flag;
    ULONG EntryCount;
    union
    {
        MAP_VALUETYPE MapEntryValueType;
        ULONG FormatStringOffset;
    } DUMMYUNIONNAME;
    EVENT_MAP_ENTRY MapEntryArray[ANYSIZE_ARRAY];
} EVENT_MAP_INFO, *PEVENT_MAP_INFO;

enum _TDH_IN_TYPE
{
    TDH_INTYPE_NULL,
    TDH_INTYPE_UNICODESTRING,
    TDH_INTYPE_ANSISTRING,
    TDH_INTYPE_INT8,
    TDH_INTYPE_UINT8,
    TDH_INTYPE_INT16,
    TDH_INTYPE_UINT16,
    TDH_INTYPE_INT32,
    TDH_INTYPE_UINT32,
    TDH_INTYPE_INT64,
    TDH_INTYPE_UINT64,
    TDH_INTYPE_FLOAT,
    TDH_INTYPE_DOUBLE,
    TDH_INTYPE_BOOLEAN,
    TDH_INTYPE_BINARY,
    TDH_INTYPE_GUID,
    TDH_INTYPE_POINTER,
    TDH_INTYPE_FILETIME,
    TDH_INTYPE_SYSTEMTIME,
    TDH_INTYPE_SID,
    TDH_INTYPE_HEXINT32,
    TDH_INTYPE_HEXINT64,
    TDH_INTYPE_MANIFEST_COUNTEDSTRING,
    TDH_INTYPE_MANIFEST_COUNTEDANSISTRING,
    TDH_INTYPE_RESERVED24,
    TDH_INTYPE_MANIFEST_COUNTEDBINARY,
    TDH_INTYPE_COUNTEDSTRING = 300,
    TDH_INTYPE_COUNTEDANSISTRING,
    TDH_INTYPE_REVERSEDCOUNTEDSTRING,
    TDH_INTYPE_REVERSEDCOUNTEDANSISTRING,
    TDH_INTYPE_NONNULLTERMINATEDSTRING,
    TDH_INTYPE_NONNULLTERMINATEDANSISTRING,
    TDH_INTYPE_UNICODECHAR,
    TDH_INTYPE_ANSICHAR,
    TDH_INTYPE_SIZET,
    TDH_INTYPE_HEXDUMP,
    TDH_INTYPE_WBEMSID
};

enum _TDH_OUT_TYPE
{
    TDH_OUTTYPE_NULL,
    TDH_OUTTYPE_STRING,
    TDH_OUTTYPE_DATETIME,
    TDH_OUTTYPE_BYTE,
    TDH_OUTTYPE_UNSIGNEDBYTE,
    TDH_OUTTYPE_SHORT,
    TDH_OUTTYPE_UNSIGNEDSHORT,
    TDH_OUTTYPE_INT,
    TDH_OUTTYPE_UNSIGNEDINT,
    TDH_OUTTYPE_LONG,
    TDH_OUTTYPE_UNSIGNEDLONG,
    TDH_OUTTYPE_FLOAT,
    TDH_OUTTYPE_DOUBLE,
    TDH_OUTTYPE_BOOLEAN,
    TDH_OUTTYPE_GUID,
    TDH_OUTTYPE_HEXBINARY,
    TDH_OUTTYPE_HEXINT8,
    TDH_OUTTYPE_HEXINT16,
    TDH_OUTTYPE_HEXINT32,
    TDH_OUTTYPE_HEXINT64,
    TDH_OUTTYPE_PID,
    TDH_OUTTYPE_TID,
    TDH_OUTTYPE_PORT,
    TDH_OUTTYPE_IPV4,
    TDH_OUTTYPE_IPV6,
    TDH_OUTTYPE_SOCKETADDRESS,
    TDH_OUTTYPE_CIMDATETIME,
    TDH_OUTTYPE_ETWTIME,
    TDH_OUTTYPE_XML,
    TDH_OUTTYPE_ERRORCODE,
    TDH_OUTTYPE_WIN32ERROR,
    TDH_OUTTYPE_NTSTATUS,
    TDH_OUTTYPE_HRESULT,
    TDH_OUTTYPE_CULTURE_INSENSITIVE_DATETIME,
    TDH_OUTTYPE_JSON,
    TDH_OUTTYPE_UTF8,
    TDH_OUTTYPE_PKCS7_WITH_TYPE_INFO,
    TDH_OUTTYPE_CODE_POINTER,
    TDH_OUTTYPE_DATETIME_UTC,
    TDH_OUTTYPE_REDUCEDSTRING = 300,
    TDH_OUTTYPE_NOPRINT
};

typedef enum _PROPERTY_FLAGS
{
    PropertyStruct = 0x1,
    PropertyParamLength = 0x2,
    PropertyParamCount = 0x4,
    PropertyWBEMXmlFragment = 0x8,
    PropertyParamFixedLength = 0x10,
    PropertyParamFixedCount = 0x20,
    PropertyHasTags = 0x40,
    PropertyHasCustomSchema = 0x80
} PROPERTY_FLAGS;

typedef struct _EVENT_PROPERTY_INFO
{
    PROPERTY_FLAGS Flags;
    ULONG NameOffset;
    union
    {
        struct _nonStructType
        {
            USHORT InType;
            USHORT OutType;
            ULONG MapNameOffset;
        } nonStructType;
        struct _structType
        {
            USHORT StructStartIndex;
            USHORT NumOfStructMembers;
            ULONG padding;
        } structType;
        struct _customSchemaType
        {
            USHORT InType;
            USHORT OutType;
            ULONG CustomSchemaOffset;
        } customSchemaType;
    } DUMMYUNIONNAME;
    union
    {
        USHORT count;
        USHORT countPropertyIndex;
    } DUMMYUNIONNAME2;
    union
    {
        USHORT length;
        USHORT lengthPropertyIndex;
    } DUMMYUNIONNAME3;
    union
    {
        ULONG Reserved;
        struct
        {
            ULONG Tags : 28;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME4;
} EVENT_PROPERTY_INFO, *PEVENT_PROPERTY_INFO;

typedef enum _DECODING_SOURCE
{
    DecodingSourceXMLFile,
    DecodingSourceWbem,
    DecodingSourceWPP,
    DecodingSourceTlg,
    DecodingSourceMax
} DECODING_SOURCE;

typedef enum _TEMPLATE_FLAGS
{
    TEMPLATE_EVENT_DATA = 1,
    TEMPLATE_USER_DATA = 2,
    TEMPLATE_CONTROL_GUID = 4
} TEMPLATE_FLAGS;

typedef struct _TRACE_EVENT_INFO
{
    GUID ProviderGuid;
    GUID EventGuid;
    EVENT_DESCRIPTOR EventDescriptor;
    DECODING_SOURCE DecodingSource;
    ULONG ProviderNameOffset;
    ULONG LevelNameOffset;
    ULONG ChannelNameOffset;
    ULONG KeywordsNameOffset;
    ULONG TaskNameOffset;
    ULONG OpcodeNameOffset;
    ULONG EventMessageOffset;
    ULONG ProviderMessageOffset;
    ULONG BinaryXMLOffset;
    ULONG BinaryXMLSize;
    union
    {
        ULONG EventNameOffset;
        ULONG ActivityIDNameOffset;
    } DUMMYUNIONNAME;
    union
    {
        ULONG EventAttributesOffset;
        ULONG RelatedActivityIDNameOffset;
    } DUMMYUNIONNAME2;
    ULONG PropertyCount;
    ULONG TopLevelPropertyCount;
    union
    {
        TEMPLATE_FLAGS Flags;
        struct
        {
            ULONG Reserved : 4;
            ULONG Tags : 28;
        } DUMMYSTRUCTNAME;
    } DUMMYUNIONNAME3;
    EVENT_PROPERTY_INFO EventPropertyInfoArray[ANYSIZE_ARRAY];
} TRACE_EVENT_INFO, *PTRACE_EVENT_INFO;

typedef struct _PROPERTY_DATA_DESCRIPTOR
{
    ULONGLONG PropertyName;
    ULONG ArrayIndex;
    ULONG Reserved;
} PROPERTY_DATA_DESCRIPTOR, *PPROPERTY_DATA_DESCRIPTOR;

typedef enum _TDH_CONTEXT_TYPE
{
    TDH_CONTEXT_WPP_TMFFILE,
    TDH_CONTEXT_WPP_TMFSEARCHPATH,
    TDH_CONTEXT_WPP_GMT,
    TDH_CONTEXT_POINTERSIZE,
    TDH_CONTEXT_PDB_PATH,
    TDH_CONTEXT_MAXIMUM
} TDH_CONTEXT_TYPE;

typedef struct _TDH_CONTEXT
{
    ULONGLONG ParameterValue;
    TDH_CONTEXT_TYPE ParameterType;
    ULONG ParameterSize;
} TDH_CONTEXT, *PTDH_CONTEXT;

TDHSTATUS
WINAPI
TdhGetEventInformation(
    _In_ PEVENT_RECORD Event,
    _In_ ULONG TdhContextCount,
    _In_reads_opt_(TdhContextCount) PTDH_CONTEXT TdhContext,
    _Out_writes_bytes_opt_(*BufferSize) PTRACE_EVENT_INFO Buffer,
    _Inout_ PULONG BufferSize);

TDHSTATUS
WINAPI
TdhGetEventMapInformation(
    _In_ PEVENT_RECORD pEvent,
    _In_ PWSTR pMapName,
    _Out_writes_bytes_opt_(*pBufferSize) PEVENT_MAP_INFO pBuffer,
    _Inout_ ULONG *pBufferSize);

TDHSTATUS
WINAPI
TdhGetPropertySize(
    _In_ PEVENT_RECORD pEvent,
    _In_ ULONG TdhContextCount,
    _In_reads_opt_(TdhContextCount) PTDH_CONTEXT pTdhContext,
    _In_ ULONG PropertyDataCount,
    _In_reads_(PropertyDataCount) PPROPERTY_DATA_DESCRIPTOR pPropertyData,
    _Out_ ULONG *pPropertySize);

TDHSTATUS
WINAPI
TdhGetProperty(
    _In_ PEVENT_RECORD pEvent,
    _In_ ULONG TdhContextCount,
    _In_reads_opt_(TdhContextCount) PTDH_CONTEXT pTdhContext,
    _In_ ULONG PropertyDataCount,
    _In_reads_(PropertyDataCount) PPROPERTY_DATA_DESCRIPTOR pPropertyData,
    _In_ ULONG BufferSize,
    _Out_writes_bytes_(BufferSize) PBYTE pBuffer);

#ifdef __cplusplus
}
#endif

#endif /* _TDH_H_ */
